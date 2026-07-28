// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/grpc/grpc.hpp>

// C++ port of github.com/SoundMatt/go-DDS bridge/grpc/grpc.go. See
// include/dds/bridge/grpc/grpc.hpp for scope notes and deliberate
// deviations from a literal line-for-line port.

#include <chrono>
#include <cctype>
#include <cstdio>
#include <thread>

namespace dds::bridge::grpc {

namespace {

// ── JSON string escaping (Go encoding/json default escapeHTML=true) ──────────
//
// Byte-for-byte identical algorithm to src/xtypes/xtypes.cpp's
// append_json_string — kept as its own copy here rather than shared,
// matching this repo's existing convention of each module owning its own
// minimal JSON codec (xtypes, tsn, cli/json_lite.hpp all do the same).
// Verified against real go-DDS json.Marshal output — see
// tests/test_bridge_grpc.cpp's "JSON reference vectors" section, including
// the '<'/'>'/'&' HTML-escape and non-ASCII pass-through cases.
void append_json_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '<':  out += "\\u003c"; break;
        case '>':  out += "\\u003e"; break;
        case '&':  out += "\\u0026"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

// base64_encode: standard (padded) alphabet, matching Go's
// base64.StdEncoding used by encoding/json for []byte fields.
std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    std::size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out += "=";
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// base64_decode returns std::nullopt on malformed input.
std::optional<std::vector<uint8_t>> base64_decode(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        s.push_back(c);
    }
    if (s.empty()) return std::vector<uint8_t>{};
    if (s.size() % 4 != 0) return std::nullopt;

    std::size_t pad = 0;
    if (s.size() >= 1 && s[s.size() - 1] == '=') pad++;
    if (s.size() >= 2 && s[s.size() - 2] == '=') pad++;

    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k) {
            char c = s[i + static_cast<std::size_t>(k)];
            if (c == '=') { v[k] = 0; continue; }
            v[k] = b64_val(c);
            if (v[k] < 0) return std::nullopt;
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        bool last_block = (i + 4 == s.size());
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        if (!(last_block && pad >= 2)) out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        if (!(last_block && pad >= 1)) out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    return out;
}

// ── Minimal recursive-descent JSON reader ─────────────────────────────────────
//
// Scoped to exactly what this module's four message types need: a flat
// object of string / uint64 / int64 / uint32 / base64-string fields.
// Not a general-purpose parser (see grpc.hpp's file-level scope note on
// this repo's per-module JSON codec convention).
class JsonReader {
public:
    explicit JsonReader(const std::string& text) : s_(text) {}

    bool parse_object(std::unordered_map<std::string, std::string>& fields) {
        skip_ws();
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            std::string raw;
            if (!parse_raw_value(raw)) return false;
            fields[key] = raw;
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return false;
        }
        return true;
    }

    // parse_string parses a JSON string literal (with escapes decoded).
    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (i_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char esc = s_[i_++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    // Encode as UTF-8. Surrogate pairs are not specially
                    // handled (no test vector in this module needs one);
                    // encoded verbatim as a 3-byte UTF-8 sequence like any
                    // other BMP code point, matching what any \uXXXX
                    // outside the surrogate range needs.
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return false; // unterminated string
    }

    bool at_end() const { skip_ws_const(); return i_ >= s_.size(); }

private:
    const std::string& s_;
    std::size_t         i_{0};

    void skip_ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    void skip_ws_const() const {
        std::size_t j = i_;
        while (j < s_.size() && std::isspace(static_cast<unsigned char>(s_[j]))) ++j;
        const_cast<JsonReader*>(this)->i_ = j;
    }
    bool consume(char c) {
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }

    // parse_raw_value captures the exact source text of the next JSON
    // value (string / number / true / false / null / object / array)
    // without decoding it — callers decode only the fields they care
    // about, matching this bridge's flat-schema needs.
    bool parse_raw_value(std::string& raw) {
        std::size_t start = i_;
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '"') {
            std::string tmp;
            if (!parse_string(tmp)) return false;
        } else if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']';
            int depth = 0;
            bool in_str = false;
            for (; i_ < s_.size(); ++i_) {
                char cc = s_[i_];
                if (in_str) {
                    if (cc == '\\') { ++i_; continue; }
                    if (cc == '"') in_str = false;
                    continue;
                }
                if (cc == '"') { in_str = true; continue; }
                if (cc == open) ++depth;
                else if (cc == close) {
                    --depth;
                    if (depth == 0) { ++i_; break; }
                }
            }
            if (depth != 0) return false;
        } else {
            // number / true / false / null: read until a structural char.
            while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
                   !std::isspace(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
            }
        }
        raw = s_.substr(start, i_ - start);
        return !raw.empty();
    }
};

bool decode_string_field(const std::unordered_map<std::string, std::string>& fields,
                          const char* key, std::string& out) {
    auto it = fields.find(key);
    if (it == fields.end()) { out.clear(); return true; }
    if (it->second == "null") { out.clear(); return true; }
    JsonReader r(it->second);
    return r.parse_string(out);
}

// decode_bytes_field decodes a base64 JSON string field ("" or null both
// map to an empty vector, matching Go's Unmarshal-into-[]byte for both).
bool decode_bytes_field(const std::unordered_map<std::string, std::string>& fields,
                         const char* key, std::vector<uint8_t>& out) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second == "null") { out.clear(); return true; }
    std::string b64;
    JsonReader r(it->second);
    if (!r.parse_string(b64)) return false;
    auto decoded = base64_decode(b64);
    if (!decoded) return false;
    out = std::move(*decoded);
    return true;
}

bool decode_uint_field(const std::unordered_map<std::string, std::string>& fields,
                        const char* key, uint64_t& out) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second == "null") { out = 0; return true; }
    try {
        std::size_t consumed = 0;
        long long v = std::stoll(it->second, &consumed);
        if (consumed != it->second.size() || v < 0) return false;
        out = static_cast<uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool decode_int_field(const std::unordered_map<std::string, std::string>& fields,
                       const char* key, int64_t& out) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second == "null") { out = 0; return true; }
    try {
        std::size_t consumed = 0;
        long long v = std::stoll(it->second, &consumed);
        if (consumed != it->second.size()) return false;
        out = static_cast<int64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

// ── JSON codec (public) ────────────────────────────────────────────────────

std::string to_json(const SubscribeRequest& v) {
    std::string out = "{\"topic\":";
    append_json_string(out, v.topic);
    out += "}";
    return out;
}

std::string to_json(const PublishRequest& v) {
    std::string out = "{\"topic\":";
    append_json_string(out, v.topic);
    out += ",\"payload\":\"";
    out += base64_encode(v.payload);
    out += "\"}";
    return out;
}

std::string to_json(const Sample& v) {
    std::string out = "{\"topic\":";
    append_json_string(out, v.topic);
    out += ",\"payload\":\"";
    out += base64_encode(v.payload);
    out += "\",\"seq_num\":";
    out += std::to_string(v.sequence_number);
    out += ",\"timestamp_ns\":";
    out += std::to_string(v.timestamp_ns);
    out += ",\"writer_guid\":\"";
    out += base64_encode(v.writer_guid);
    out += "\"}";
    return out;
}

std::string to_json(const PublishAck& v) {
    return "{\"count\":" + std::to_string(v.count) + "}";
}

bool from_json(const std::string& text, SubscribeRequest& out) {
    JsonReader r(text);
    std::unordered_map<std::string, std::string> fields;
    if (!r.parse_object(fields)) return false;
    return decode_string_field(fields, "topic", out.topic);
}

bool from_json(const std::string& text, PublishRequest& out) {
    JsonReader r(text);
    std::unordered_map<std::string, std::string> fields;
    if (!r.parse_object(fields)) return false;
    return decode_string_field(fields, "topic", out.topic) &&
           decode_bytes_field(fields, "payload", out.payload);
}

bool from_json(const std::string& text, Sample& out) {
    JsonReader r(text);
    std::unordered_map<std::string, std::string> fields;
    if (!r.parse_object(fields)) return false;
    return decode_string_field(fields, "topic", out.topic) &&
           decode_bytes_field(fields, "payload", out.payload) &&
           decode_uint_field(fields, "seq_num", out.sequence_number) &&
           decode_int_field(fields, "timestamp_ns", out.timestamp_ns) &&
           decode_bytes_field(fields, "writer_guid", out.writer_guid);
}

bool from_json(const std::string& text, PublishAck& out) {
    JsonReader r(text);
    std::unordered_map<std::string, std::string> fields;
    if (!r.parse_object(fields)) return false;
    uint64_t count = 0;
    if (!decode_uint_field(fields, "count", count)) return false;
    out.count = static_cast<uint32_t>(count);
    return true;
}

// ── Bridge ──────────────────────────────────────────────────────────────────

Bridge::Bridge(std::shared_ptr<dds::IParticipant> participant, Options opts)
    : p_(std::move(participant)), opts_(std::move(opts)) {}

Bridge::~Bridge() { close(); }

std::error_code Bridge::close() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [topic, sub] : subs_) { (void)topic; sub->close(); }
    for (auto& [topic, pub] : pubs_) { (void)topic; pub->close(); }
    subs_.clear();
    pubs_.clear();
    return {};
}

Status Bridge::check_auth(const std::optional<std::string>& authorization) const {
    if (opts_.auth_token.empty()) return Status::make_ok();
    const std::string want = "Bearer " + opts_.auth_token;
    if (!authorization.has_value() || *authorization != want) {
        return Status::unauthenticated("invalid token");
    }
    return Status::make_ok();
}

std::pair<std::shared_ptr<dds::ISubscriber>, std::error_code>
Bridge::get_or_create_sub(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subs_.find(topic);
    if (it != subs_.end()) return {it->second, std::error_code{}};
    auto [sub, err] = p_->new_subscriber(topic, opts_.qos);
    if (err) return {nullptr, err};
    subs_[topic] = sub;
    return {sub, std::error_code{}};
}

std::pair<std::shared_ptr<dds::IPublisher>, std::error_code>
Bridge::get_or_create_pub(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pubs_.find(topic);
    if (it != pubs_.end()) return {it->second, std::error_code{}};
    auto [pub, err] = p_->new_publisher(topic, opts_.qos);
    if (err) return {nullptr, err};
    pubs_[topic] = pub;
    return {pub, std::error_code{}};
}

Status Bridge::subscribe(const SubscribeRequest& req, SampleSender& sender) {
    if (req.topic.empty()) return Status::invalid_argument("topic must not be empty");
    auto [sub, err] = get_or_create_sub(req.topic);
    if (err) return Status::internal_error("subscriber: " + err.message());

    auto chan = sub->channel();
    // Poll loop: relay::Channel<T> has no native multi-channel select, so
    // this polls with a short deadline rather than blocking indefinitely
    // (see grpc.hpp's file-level scope note; the same "synchronous
    // primitive polled from a loop" pattern used by rtps/transport.cpp's
    // recv()).
    constexpr auto kPollInterval = std::chrono::milliseconds(20);
    while (true) {
        if (sender.cancelled()) return Status::cancelled("context canceled");

        auto item = chan->recv_until(std::chrono::steady_clock::now() + kPollInterval);
        if (!item) {
            if (chan->is_closed()) return Status::make_ok();
            continue;
        }

        const dds::Sample& s = *item;
        std::vector<uint8_t> payload = s.payload;
        if (opts_.filter && !opts_.filter(req.topic, payload)) continue;
        if (opts_.transform) {
            auto out = opts_.transform(req.topic, payload);
            if (!out) continue; // drop on transform error
            payload = std::move(*out);
        }

        Sample msg;
        msg.topic           = req.topic;
        msg.payload          = std::move(payload);
        msg.sequence_number = s.sequence_number;
        msg.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                s.timestamp.time_since_epoch())
                                .count();
        msg.writer_guid.assign(s.writer_guid.begin(), s.writer_guid.end());

        if (auto send_err = sender.send(msg); send_err) {
            return Status::internal_error(send_err.message());
        }
    }
}

std::pair<PublishAck, Status> Bridge::publish(const PublishRequest& req) {
    if (req.topic.empty()) return {PublishAck{}, Status::invalid_argument("topic must not be empty")};
    auto [pub, err] = get_or_create_pub(req.topic);
    if (err) return {PublishAck{}, Status::internal_error("publisher: " + err.message())};
    if (auto werr = pub->write(relay::Context::background(), req.payload); werr) {
        return {PublishAck{}, Status::internal_error("write: " + werr.message())};
    }
    return {PublishAck{1}, Status::make_ok()};
}

std::pair<PublishAck, Status> Bridge::stream_publish(PublishReceiver& receiver) {
    uint32_t count = 0;
    while (true) {
        PublishRequest req;
        Status         recv_err;
        if (!receiver.recv(req, recv_err)) {
            if (recv_err.ok()) return {PublishAck{count}, Status::make_ok()}; // clean EOF
            return {PublishAck{}, recv_err};
        }
        if (req.topic.empty()) return {PublishAck{}, Status::invalid_argument("topic must not be empty")};
        auto [pub, err] = get_or_create_pub(req.topic);
        if (err) return {PublishAck{}, Status::internal_error("publisher: " + err.message())};
        if (auto werr = pub->write(relay::Context::background(), req.payload); werr) {
            return {PublishAck{}, Status::internal_error("write: " + werr.message())};
        }
        ++count;
    }
}

} // namespace dds::bridge::grpc
