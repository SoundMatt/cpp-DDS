// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// C++ port of github.com/SoundMatt/go-DDS's tsn/tsn.go. See
// include/dds/tsn/tsn.hpp for the file-level scope note, including why
// JSON parsing here is a small internal recursive-descent reader rather
// than a general-purpose library dependency.

#include <dds/tsn/tsn.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <variant>

namespace dds::tsn {

// fusa:req REQ-TSN-001

int Stream::max_frag_payload() const noexcept {
    constexpr int kRtpsHeaderOverhead = 48;
    if (max_frame_size <= kRtpsHeaderOverhead) return 0;
    return max_frame_size - kRtpsHeaderOverhead;
}

const Stream* StreamConfig::stream_for_topic(const std::string& topic) const noexcept {
    for (const auto& s : streams) {
        if (s.topic == topic) return &s;
    }
    return nullptr;
}

std::vector<std::string> StreamConfig::topics() const {
    std::vector<std::string> names;
    names.reserve(streams.size());
    for (const auto& s : streams) names.push_back(s.topic);
    return names;
}

// ── Minimal internal JSON reader ────────────────────────────────────────
//
// Scoped to exactly the tsn_streams.json shape: a top-level object with a
// "streams" array of flat objects (string/number fields only). Not a
// general-purpose JSON library — matches this repo's convention of small,
// purpose-built parsers (dds::idl's lexer/parser, cli/json_lite.hpp).

namespace {

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::monostate, bool, double, std::string, JsonArray, JsonObject> v;

    bool is_object() const noexcept { return std::holds_alternative<JsonObject>(v); }
    bool is_array() const noexcept { return std::holds_alternative<JsonArray>(v); }

    const JsonObject* as_object() const noexcept {
        return std::holds_alternative<JsonObject>(v) ? &std::get<JsonObject>(v) : nullptr;
    }
    const JsonArray* as_array() const noexcept {
        return std::holds_alternative<JsonArray>(v) ? &std::get<JsonArray>(v) : nullptr;
    }
    const std::string* as_string() const noexcept {
        return std::holds_alternative<std::string>(v) ? &std::get<std::string>(v) : nullptr;
    }
    const double* as_number() const noexcept {
        return std::holds_alternative<double>(v) ? &std::get<double>(v) : nullptr;
    }
};

const JsonValue* object_field(const JsonObject& obj, const std::string& key) {
    for (const auto& [k, val] : obj) {
        if (k == key) return &val;
    }
    return nullptr;
}

std::string field_string(const JsonObject& obj, const std::string& key) {
    if (const auto* val = object_field(obj, key)) {
        if (const auto* s = val->as_string()) return *s;
    }
    return {};
}

int64_t field_int(const JsonObject& obj, const std::string& key) {
    if (const auto* val = object_field(obj, key)) {
        if (const auto* n = val->as_number()) return static_cast<int64_t>(*n);
    }
    return 0;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    // Parses a single JSON value, consuming leading/trailing whitespace.
    // Returns nullopt on malformed input, with a diagnostic in err_.
    std::optional<JsonValue> parse() {
        skip_ws();
        auto val = parse_value();
        if (!val) return std::nullopt;
        skip_ws();
        if (pos_ != s_.size()) {
            err_ = "unexpected trailing data at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        return val;
    }

    const std::string& error() const noexcept { return err_; }

private:
    const std::string& s_;
    std::size_t pos_{0};
    std::string err_;

    bool eof() const noexcept { return pos_ >= s_.size(); }
    char peek() const noexcept { return eof() ? '\0' : s_[pos_]; }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool consume(char c) {
        if (eof() || s_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    std::optional<JsonValue> parse_value() {
        skip_ws();
        if (eof()) {
            err_ = "unexpected end of input";
            return std::nullopt;
        }
        switch (peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return parse_string_value();
        case 't': return parse_literal("true", JsonValue{true});
        case 'f': return parse_literal("false", JsonValue{false});
        case 'n': return parse_literal("null", JsonValue{});
        default:  return parse_number();
        }
    }

    std::optional<JsonValue> parse_literal(const char* lit, JsonValue result) {
        const std::size_t len = std::char_traits<char>::length(lit);
        if (s_.compare(pos_, len, lit) != 0) {
            err_ = "invalid literal at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        pos_ += len;
        return result;
    }

    std::optional<JsonValue> parse_object() {
        if (!consume('{')) { err_ = "expected '{'"; return std::nullopt; }
        JsonObject obj;
        skip_ws();
        if (consume('}')) { JsonValue jv; jv.v = std::move(obj); return jv; }
        for (;;) {
            skip_ws();
            if (peek() != '"') { err_ = "expected string key at offset " + std::to_string(pos_); return std::nullopt; }
            auto key = parse_string_raw();
            if (!key) return std::nullopt;
            skip_ws();
            if (!consume(':')) { err_ = "expected ':' at offset " + std::to_string(pos_); return std::nullopt; }
            auto val = parse_value();
            if (!val) return std::nullopt;
            obj.emplace_back(std::move(*key), std::move(*val));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            err_ = "expected ',' or '}' at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        JsonValue jv;
        jv.v = std::move(obj);
        return jv;
    }

    std::optional<JsonValue> parse_array() {
        if (!consume('[')) { err_ = "expected '['"; return std::nullopt; }
        JsonArray arr;
        skip_ws();
        if (consume(']')) { JsonValue jv; jv.v = std::move(arr); return jv; }
        for (;;) {
            auto val = parse_value();
            if (!val) return std::nullopt;
            arr.push_back(std::move(*val));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            err_ = "expected ',' or ']' at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        JsonValue jv;
        jv.v = std::move(arr);
        return jv;
    }

    std::optional<std::string> parse_string_raw() {
        if (!consume('"')) { err_ = "expected '\"' at offset " + std::to_string(pos_); return std::nullopt; }
        std::string out;
        while (true) {
            if (eof()) { err_ = "unterminated string"; return std::nullopt; }
            char c = s_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (eof()) { err_ = "unterminated escape"; return std::nullopt; }
                char e = s_[pos_++];
                switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    // Minimal \uXXXX support: emit the raw UTF-8 for the
                    // BMP code point (no surrogate-pair handling — this
                    // config format has no need for it).
                    if (pos_ + 4 > s_.size()) { err_ = "invalid \\u escape"; return std::nullopt; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else { err_ = "invalid \\u escape digit"; return std::nullopt; }
                    }
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
                default:
                    err_ = "invalid escape character";
                    return std::nullopt;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    std::optional<JsonValue> parse_string_value() {
        auto s = parse_string_raw();
        if (!s) return std::nullopt;
        JsonValue jv;
        jv.v = std::move(*s);
        return jv;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (!eof() && (peek() == '-' || peek() == '+')) ++pos_;
        bool saw_digit = false;
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) { ++pos_; saw_digit = true; }
        if (!eof() && peek() == '.') {
            ++pos_;
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) { ++pos_; saw_digit = true; }
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '-' || peek() == '+')) ++pos_;
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (!saw_digit) { err_ = "invalid number at offset " + std::to_string(start); return std::nullopt; }
        JsonValue jv;
        jv.v = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return jv;
    }
};

// json_escape produces a minimally-escaped JSON string literal (quotes
// included) — enough for round-tripping to_json() output back through
// parse_config().
std::string json_escape(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        default:   out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::optional<std::string> validate(const StreamConfig& cfg) {
    for (std::size_t i = 0; i < cfg.streams.size(); ++i) {
        const auto& s = cfg.streams[i];
        const std::string prefix = "stream[" + std::to_string(i) + "] (" + s.topic + "): ";
        if (s.topic.empty())
            return "stream[" + std::to_string(i) + "]: topic must not be empty";
        if (s.pcp > 7)
            return prefix + "PCP " + std::to_string(s.pcp) + " out of range 0-7";
        if (s.dscp > 63)
            return prefix + "DSCP " + std::to_string(s.dscp) + " out of range 0-63";
        if (s.max_frame_size < 0)
            return prefix + "MaxFrameSize must be >= 0";
        if (s.max_interval_frames < 0)
            return prefix + "MaxIntervalFrames must be >= 0";
        if (s.interval_us < 0)
            return prefix + "IntervalUS must be >= 0";
        if (s.tx_offset_us < 0)
            return prefix + "TxOffsetUS must be >= 0";
    }
    return std::nullopt;
}

} // namespace

// fusa:req REQ-TSN-001
LoadResult parse_config(const std::string& json_text) {
    LoadResult res;
    JsonParser parser(json_text);
    auto root = parser.parse();
    if (!root) {
        res.error = "tsn: parse: " + parser.error();
        return res;
    }
    const JsonObject* obj = root->as_object();
    if (!obj) {
        res.error = "tsn: parse: top-level value must be a JSON object";
        return res;
    }

    StreamConfig cfg;
    if (const auto* streams_val = object_field(*obj, "streams")) {
        const JsonArray* arr = streams_val->as_array();
        if (!arr) {
            res.error = "tsn: parse: \"streams\" must be an array";
            return res;
        }
        for (const auto& item : *arr) {
            const JsonObject* sobj = item.as_object();
            if (!sobj) {
                res.error = "tsn: parse: stream entries must be objects";
                return res;
            }
            Stream s;
            s.topic               = field_string(*sobj, "topic");
            s.vid                  = static_cast<uint16_t>(field_int(*sobj, "vid"));
            s.pcp                  = static_cast<uint8_t>(field_int(*sobj, "pcp"));
            s.dscp                 = static_cast<uint8_t>(field_int(*sobj, "dscp"));
            s.max_frame_size       = static_cast<int>(field_int(*sobj, "max_frame_size"));
            s.max_interval_frames  = static_cast<int>(field_int(*sobj, "max_interval_frames"));
            s.interval_us          = field_int(*sobj, "interval_us");
            s.tx_offset_us         = field_int(*sobj, "tx_offset_us");
            s.talker_id            = field_string(*sobj, "talker_id");
            cfg.streams.push_back(std::move(s));
        }
    }

    if (auto verr = validate(cfg)) {
        res.error = "tsn: " + *verr;
        return res;
    }
    res.config = std::move(cfg);
    return res;
}

// fusa:req REQ-TSN-001
LoadResult load_config(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LoadResult res;
        res.error = "tsn: open " + path + ": file not found or unreadable";
        return res;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    auto res = parse_config(ss.str());
    if (!res.ok() && res.error) {
        res.error = "tsn: " + path + ": " + *res.error;
    }
    return res;
}

std::string StreamConfig::to_json() const {
    std::ostringstream out;
    out << "{\"streams\":[";
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (i > 0) out << ',';
        const auto& s = streams[i];
        out << "{"
            << "\"topic\":" << json_escape(s.topic) << ','
            << "\"vid\":" << s.vid << ','
            << "\"pcp\":" << static_cast<int>(s.pcp) << ','
            << "\"dscp\":" << static_cast<int>(s.dscp) << ','
            << "\"max_frame_size\":" << s.max_frame_size << ','
            << "\"max_interval_frames\":" << s.max_interval_frames << ','
            << "\"interval_us\":" << s.interval_us << ','
            << "\"tx_offset_us\":" << s.tx_offset_us << ','
            << "\"talker_id\":" << json_escape(s.talker_id)
            << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace dds::tsn
