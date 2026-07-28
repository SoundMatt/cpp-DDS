// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/rest/rest.hpp>

// C++ port of github.com/SoundMatt/go-DDS bridge/rest/rest.go's business
// logic (routing, JSON/SSE codec, Bridge). See include/dds/bridge/rest/
// rest.hpp for scope notes and deliberate deviations from a literal
// line-for-line port. No networking lives in this file — see
// transport.cpp for the real HTTP/1.1-over-TCP wire layer.

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace dds::bridge::rest {

// ── Options ────────────────────────────────────────────────────────────────

std::chrono::nanoseconds effective_keepalive(const Options& opts) noexcept {
    if (opts.sse_keepalive.count() == 0) return std::chrono::seconds(15);
    return opts.sse_keepalive;
}

// ── JSON codec (own copy — see rest.hpp's file-level scope note) ─────────────

namespace {

// append_json_string: HTML-safe string escaping matching Go's
// encoding/json default escapeHTML=true (<,>,& become </>/
// &), byte-for-byte identical to dds::bridge::wan's own copy of the
// same escaper (each module owns its own per this repo's established
// convention — see wan.hpp's file-level scope note).
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

} // anonymous namespace

std::string topics_to_json(const std::vector<std::string>& topics) {
    std::string out = "[";
    for (std::size_t i = 0; i < topics.size(); ++i) {
        if (i != 0) out += ",";
        append_json_string(out, topics[i]);
    }
    out += "]\n"; // trailing "\n": matches json.Encoder.Encode (not json.Marshal)
    return out;
}

// ── SSE wire-format helpers ───────────────────────────────────────────────────

std::string sse_message_event(uint64_t id, const std::vector<uint8_t>& payload) {
    std::string out = "id: ";
    out += std::to_string(id);
    out += "\nevent: message\ndata: ";
    out += base64_encode(payload);
    out += "\n\n";
    return out;
}

std::string sse_keepalive_comment() { return ": keepalive\n\n"; }

// ── Routing ────────────────────────────────────────────────────────────────

RouteResult classify_request(const std::string& method, const std::string& raw_path) {
    static const std::string kPrefix = "/topics";

    std::string path = raw_path;
    if (path.rfind(kPrefix, 0) == 0) path = path.substr(kPrefix.size());
    // else: path left unchanged, matching strings.TrimPrefix's no-op when
    // the prefix isn't present.

    if (path.empty() || path == "/") {
        if (method == "GET") return RouteResult{Route::list, ""};
        return RouteResult{Route::method_not_allowed, ""};
    }

    std::string topic = path;
    if (!topic.empty() && topic.front() == '/') topic.erase(topic.begin());
    if (topic.empty()) return RouteResult{Route::bad_request, ""};

    if (method == "GET") return RouteResult{Route::subscribe, topic};
    if (method == "POST") return RouteResult{Route::publish, topic};
    return RouteResult{Route::method_not_allowed, topic};
}

// ── Bridge ─────────────────────────────────────────────────────────────────

Bridge::Bridge(std::shared_ptr<dds::IParticipant> participant, Options opts)
    : p_(std::move(participant)), opts_(std::move(opts)) {}

Bridge::~Bridge() { close(); }

std::error_code Bridge::close() {
    std::unordered_map<std::string, std::shared_ptr<dds::ISubscriber>> subs;
    std::unordered_map<std::string, std::shared_ptr<dds::IPublisher>>  pubs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs.swap(subs_);
        pubs.swap(pubs_);
    }
    for (auto& [topic, sub] : subs) {
        (void)topic;
        sub->close();
    }
    for (auto& [topic, pub] : pubs) {
        (void)topic;
        pub->close();
    }
    return {};
}

bool Bridge::authorize(const std::optional<std::string>& authorization) const {
    if (opts_.auth_token.empty()) return true;
    return authorization.has_value() && *authorization == "Bearer " + opts_.auth_token;
}

std::vector<std::string> Bridge::list_topics() const {
    std::vector<std::string> topics;
    {
        std::lock_guard<std::mutex> lk(mu_);
        topics.reserve(subs_.size());
        for (auto& [topic, sub] : subs_) {
            (void)sub;
            topics.push_back(topic);
        }
    }
    std::sort(topics.begin(), topics.end());
    return topics;
}

std::pair<std::shared_ptr<dds::ISubscriber>, std::error_code>
Bridge::get_subscriber(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = subs_.find(topic); it != subs_.end()) return {it->second, {}};
    auto [sub, err] = p_->new_subscriber(topic, opts_.qos);
    if (err) return {nullptr, err};
    subs_.emplace(topic, sub);
    return {sub, {}};
}

std::pair<std::shared_ptr<dds::IPublisher>, std::error_code>
Bridge::get_publisher(const std::string& topic) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = pubs_.find(topic); it != pubs_.end()) return {it->second, {}};
    auto [pub, err] = p_->new_publisher(topic, opts_.qos);
    if (err) return {nullptr, err};
    pubs_.emplace(topic, pub);
    return {pub, {}};
}

Result Bridge::run_sse_loop(const std::shared_ptr<dds::ISubscriber>& sub, SseSink& sink) {
    auto chan = sub->channel();

    const auto keepalive_period               = effective_keepalive(opts_);
    std::chrono::steady_clock::time_point next_keepalive = std::chrono::steady_clock::now() + keepalive_period;
    // Polling granularity for the "wait for a sample or a keepalive
    // deadline or cancellation, whichever comes first" three-way wait —
    // relay::Channel<T> has no native multi-channel select, so this polls
    // a short interval and checks cancellation/keepalive between waits,
    // exactly like dds::bridge::wan::Bridge's per-topic sender loop (see
    // wan.hpp's file-level scope note for the established rationale).
    constexpr auto kPollInterval = std::chrono::milliseconds(20);

    while (true) {
        if (sink.cancelled()) return Result::success();

        auto now      = std::chrono::steady_clock::now();
        auto deadline = std::min(next_keepalive, now + kPollInterval);
        auto item     = chan->recv_until(deadline);

        if (item) {
            uint64_t id = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (auto werr = sink.send_message(id, item->payload); werr) {
                return Result::failure("write failed: " + werr.message());
            }
            continue;
        }

        if (chan->is_closed()) return Result::success();

        if (std::chrono::steady_clock::now() >= next_keepalive) {
            if (auto werr = sink.send_keepalive(); werr) {
                return Result::failure("keepalive write failed: " + werr.message());
            }
            next_keepalive = std::chrono::steady_clock::now() + keepalive_period;
        }
    }
}

Result Bridge::handle_publish(const std::string& topic, const std::vector<uint8_t>& payload) {
    auto [pub, err] = get_publisher(topic);
    if (err) return Result::failure("publisher: " + err.message());
    if (auto werr = pub->write(payload); werr) return Result::failure("publish: " + werr.message());
    return Result::success();
}

} // namespace dds::bridge::rest
