// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// json_lite.hpp — minimal JSON reader/writer for the cpp-dds CLI.
//
// Scoped to exactly what the RELAY spec §11.2 `convert` command needs: parsing
// a `dds.Sample` value (spec/schemas/dds-sample.json) and emitting a
// `relay.Message` value (spec/schemas/relay-message.json). Not a general
// purpose JSON library — CLI-only, not part of the cppdds_lib public API.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cli::json {

// ── error ─────────────────────────────────────────────────────────────────────

struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// ── value ─────────────────────────────────────────────────────────────────────

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() : type_(Type::Null) {}
    static Value make_bool(bool b)              { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
    static Value make_number(double n)          { Value v; v.type_ = Type::Number; v.num_ = n; return v; }
    static Value make_string(std::string s)      { Value v; v.type_ = Type::String; v.str_ = std::move(s); return v; }
    static Value make_array()                   { Value v; v.type_ = Type::Array; return v; }
    static Value make_object()                  { Value v; v.type_ = Type::Object; return v; }

    Type type() const noexcept { return type_; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_array()  const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    const std::string& as_string() const {
        if (type_ != Type::String) throw ParseError("expected JSON string");
        return str_;
    }
    double as_number() const {
        if (type_ != Type::Number) throw ParseError("expected JSON number");
        return num_;
    }
    const std::vector<Value>& as_array() const {
        if (type_ != Type::Array) throw ParseError("expected JSON array");
        return arr_;
    }
    std::vector<Value>& array_mut() { type_ = Type::Array; return arr_; }
    std::map<std::string, Value>& object_mut() { type_ = Type::Object; return obj_; }

    // find returns nullptr if this is not an object or key is absent.
    const Value* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = obj_.find(key);
        return it == obj_.end() ? nullptr : &it->second;
    }

    void set(const std::string& key, Value v) {
        type_ = Type::Object;
        obj_[key] = std::move(v);
    }
    void push_back(Value v) {
        type_ = Type::Array;
        arr_.push_back(std::move(v));
    }

private:
    Type                          type_;
    bool                          bool_{false};
    double                        num_{0};
    std::string                   str_;
    std::vector<Value>            arr_;
    std::map<std::string, Value>  obj_;
};

// ── parser ────────────────────────────────────────────────────────────────────

namespace detail {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), i_(0) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (i_ != s_.size()) throw ParseError("trailing data after JSON value");
        return v;
    }

private:
    // parse_value/parse_object/parse_array recurse into each other for every
    // nesting level. Untrusted input (this parser only ever reads `convert`'s
    // stdin — spec §11.2) could otherwise nest deeply enough to exhaust the
    // stack (CWE-674) before hitting a length or well-formedness error.
    static constexpr int kMaxDepth = 128;

    const std::string& s_;
    std::size_t         i_;
    int                 depth_{0};

    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) {
            if (++d > kMaxDepth) throw ParseError("JSON nesting too deep (max 128 levels)");
        }
        ~DepthGuard() { --d; }
    };

    char peek() const {
        if (i_ >= s_.size()) throw ParseError("unexpected end of JSON input");
        return s_[i_];
    }

    void skip_ws() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    void expect(char c) {
        if (i_ >= s_.size() || s_[i_] != c)
            throw ParseError(std::string("expected '") + c + "'");
        ++i_;
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
        case '{': { DepthGuard g(depth_); return parse_object(); }
        case '[': { DepthGuard g(depth_); return parse_array(); }
        case '"': return Value::make_string(parse_string());
        case 't':
        case 'f': return parse_bool();
        case 'n': return parse_null();
        default:  return parse_number();
        }
    }

    Value parse_object() {
        expect('{');
        Value v = Value::make_object();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            Value val = parse_value();
            v.object_mut()[key] = std::move(val);
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            break;
        }
        skip_ws();
        expect('}');
        return v;
    }

    Value parse_array() {
        expect('[');
        Value v = Value::make_array();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        while (true) {
            Value item = parse_value();
            v.array_mut().push_back(std::move(item));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            break;
        }
        skip_ws();
        expect(']');
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= s_.size()) throw ParseError("unterminated JSON string");
            char c = s_[i_++];
            if (c == '"') break;
            if (c == '\\') {
                if (i_ >= s_.size()) throw ParseError("unterminated JSON escape");
                char e = s_[i_++];
                switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) throw ParseError("truncated \\u escape");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else throw ParseError("invalid \\u escape");
                    }
                    // Minimal UTF-8 encoding (BMP only — sufficient for topic/id strings).
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
                default: throw ParseError("invalid JSON escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Value parse_bool() {
        if (s_.compare(i_, 4, "true") == 0)  { i_ += 4; return Value::make_bool(true); }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; return Value::make_bool(false); }
        throw ParseError("invalid JSON literal");
    }

    Value parse_null() {
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return Value(); }
        throw ParseError("invalid JSON literal");
    }

    Value parse_number() {
        std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        if (i_ == start) throw ParseError("invalid JSON number");
        try {
            return Value::make_number(std::stod(s_.substr(start, i_ - start)));
        } catch (...) {
            throw ParseError("invalid JSON number");
        }
    }
};

} // namespace detail

inline Value parse(const std::string& text) {
    detail::Parser p(text);
    return p.parse();
}

// ── string escaping for output ───────────────────────────────────────────────

inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
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
    return out;
}

} // namespace cli::json
