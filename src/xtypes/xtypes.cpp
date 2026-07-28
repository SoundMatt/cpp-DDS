// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/xtypes/xtypes.hpp>

#include "detail/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

// fusa:req REQ-XTYPE-001 REQ-XTYPE-002 REQ-XTYPE-003 REQ-XTYPE-004
// fusa:req REQ-XTYPE-005 REQ-XTYPE-006 REQ-TREG-001 REQ-TREG-002 REQ-TREG-003

namespace dds::xtypes {

// ── Error category ───────────────────────────────────────────────────────────

namespace {

class XTypesErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "dds.xtypes"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::unknown_field: return "xtypes: unknown field in type";
        case Errc::invalid_json:  return "xtypes: invalid JSON input";
        case Errc::type_mismatch: return "xtypes: type name already registered with different structure";
        default:                  return "xtypes: unknown error";
        }
    }
};

} // namespace

const std::error_category& error_category() noexcept {
    static XTypesErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

// ── TypeKind ──────────────────────────────────────────────────────────────────

std::string to_string(TypeKind k) {
    switch (k) {
    case TypeKind::Bool:    return "bool";
    case TypeKind::Int32:   return "int32";
    case TypeKind::Int64:   return "int64";
    case TypeKind::Float64: return "float64";
    case TypeKind::String:  return "string";
    case TypeKind::Bytes:   return "bytes";
    case TypeKind::Struct:  return "struct";
    case TypeKind::Seq:     return "seq";
    default: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "TypeKind(%u)", static_cast<unsigned>(k));
        return buf;
    }
    }
}

// ── JSON string escaping (Go encoding/json default escapeHTML=true) ──────────
//
// Matches go-DDS's actual output byte-for-byte for the ASCII range: quote
// and backslash are backslash-escaped, newline/carriage-return/tab use
// their short escapes, other control characters (below 0x20) use a
// 4-hex-digit escape, and the three characters '<', '>', '&' are each
// escaped to a 4-hex-digit form too (Go's default HTML-safe string
// encoding — see the switch statement below for the exact escape text of
// each). Bytes >= 0x80 (UTF-8 continuation/lead bytes of ordinary runes)
// pass through unescaped, matching Go for every rune except U+2028/U+2029
// and invalid UTF-8 — see xtypes.hpp's file-level scope note for that
// documented gap.
namespace {

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

// ── Canonical JSON (for TypeIdentifier hashing) ──────────────────────────────

std::vector<const FieldDescriptor*> sorted_field_ptrs(const std::vector<FieldDescriptor>& fields) {
    std::vector<const FieldDescriptor*> out;
    out.reserve(fields.size());
    for (const auto& f : fields) out.push_back(&f);
    std::stable_sort(out.begin(), out.end(),
                      [](const FieldDescriptor* a, const FieldDescriptor* b) { return a->name < b->name; });
    return out;
}

void append_canonical_field(std::string& out, const FieldDescriptor& f);

void append_canonical_field_array(std::string& out, const std::vector<FieldDescriptor>& fields) {
    out.push_back('[');
    auto sorted = sorted_field_ptrs(fields);
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) out.push_back(',');
        append_canonical_field(out, *sorted[i]);
    }
    out.push_back(']');
}

// append_canonical_field mirrors go-DDS's canonicalField(): keys emitted in
// lexicographic order — "element" (if f.element set), "fields" (if
// !f.fields.empty()), "kind", "name", "optional" — matching the order
// verified against real go-DDS output (see tests/test_xtypes.cpp).
void append_canonical_field(std::string& out, const FieldDescriptor& f) {
    out.push_back('{');
    bool first = true;
    auto comma = [&] {
        if (!first) out.push_back(',');
        first = false;
    };
    if (f.element) {
        comma();
        out += "\"element\":";
        append_canonical_field(out, *f.element);
    }
    if (!f.fields.empty()) {
        comma();
        out += "\"fields\":";
        append_canonical_field_array(out, f.fields);
    }
    comma();
    out += "\"kind\":";
    append_json_string(out, to_string(f.kind));
    comma();
    out += "\"name\":";
    append_json_string(out, f.name);
    comma();
    out += "\"optional\":";
    out += f.optional ? "true" : "false";
    out.push_back('}');
}

} // namespace

namespace detail {

// canonical_json mirrors go-DDS's canonical(): top-level keys emitted in
// lexicographic order — "fields", "name", "version".
std::string canonical_json(const TypeDescriptor& td) {
    std::string out;
    out.push_back('{');
    out += "\"fields\":";
    append_canonical_field_array(out, td.fields);
    out += ",\"name\":";
    append_json_string(out, td.name);
    out += ",\"version\":";
    out += std::to_string(td.version);
    out.push_back('}');
    return out;
}

} // namespace detail

// ── Type identity ─────────────────────────────────────────────────────────────

TypeIdentifier identify(const TypeDescriptor& td) {
    std::string json = detail::canonical_json(td);
    auto digest = detail::Sha256::hash(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    TypeIdentifier id;
    id.name = td.name;
    std::copy(digest.begin(), digest.begin() + 8, id.hash.begin());
    return id;
}

std::shared_ptr<TypeObject> new_type_object(TypeDescriptor td) {
    auto id = identify(td);
    auto to = std::make_shared<TypeObject>();
    to->id         = id;
    to->descriptor = std::move(td);
    return to;
}

// ── Value ─────────────────────────────────────────────────────────────────────

Value Value::of_bool(bool b) {
    Value v;
    v.kind_ = Kind::Bool;
    v.bool_ = b;
    return v;
}

Value Value::of_int64(int64_t i) {
    Value v;
    v.kind_ = Kind::Int64;
    v.int_  = i;
    return v;
}

Value Value::of_double(double d) {
    Value v;
    v.kind_   = Kind::Double;
    v.double_ = d;
    return v;
}

Value Value::of_string(std::string s) {
    Value v;
    v.kind_ = Kind::String;
    v.str_  = std::move(s);
    return v;
}

Value Value::of_bytes(std::vector<uint8_t> b) {
    Value v;
    v.kind_  = Kind::Bytes;
    v.bytes_ = std::move(b);
    return v;
}

std::optional<bool> Value::as_bool() const {
    if (kind_ != Kind::Bool) return std::nullopt;
    return bool_;
}

std::optional<int64_t> Value::as_int64() const {
    if (kind_ != Kind::Int64) return std::nullopt;
    return int_;
}

std::optional<double> Value::as_double() const {
    if (kind_ != Kind::Double) return std::nullopt;
    return double_;
}

const std::string* Value::as_string() const noexcept {
    return kind_ == Kind::String ? &str_ : nullptr;
}

const std::vector<uint8_t>* Value::as_bytes() const noexcept {
    return kind_ == Kind::Bytes ? &bytes_ : nullptr;
}

bool Value::operator==(const Value& other) const noexcept {
    if (kind_ != other.kind_) return false;
    switch (kind_) {
    case Kind::Null:   return true;
    case Kind::Bool:   return bool_ == other.bool_;
    case Kind::Int64:  return int_ == other.int_;
    case Kind::Double: return double_ == other.double_;
    case Kind::String: return str_ == other.str_;
    case Kind::Bytes:  return bytes_ == other.bytes_;
    }
    return false;
}

// ── Value <-> JSON (DynamicData::to_json/from_json) ──────────────────────────
//
// Distinct from canonical_json() above: this is the general per-field value
// encoder/decoder, not the fixed-shape TypeIdentifier hash input. It shares
// append_json_string() for consistent (Go-matching) string escaping.

namespace {

// base64_encode: standard (padded) alphabet, matching Go's encoding/json
// special-casing of []byte values on marshal (base64.StdEncoding).
std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
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

std::string format_double(double d) {
    // 17 significant decimal digits is sufficient to round-trip any IEEE-754
    // double uniquely (Go's own shortest-round-trip formatter is not
    // reproduced here — behavioral round-trip correctness is what go-DDS's
    // own test suite actually checks, not textual byte-exactness; see
    // xtypes.hpp's file-level scope note).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    return buf;
}

std::string value_to_json(const Value& v) {
    switch (v.kind()) {
    case Value::Kind::Null: return "null";
    case Value::Kind::Bool: return v.as_bool().value() ? "true" : "false";
    case Value::Kind::Int64: return std::to_string(v.as_int64().value());
    case Value::Kind::Double: return format_double(v.as_double().value());
    case Value::Kind::String: {
        std::string out;
        append_json_string(out, *v.as_string());
        return out;
    }
    case Value::Kind::Bytes: {
        std::string out;
        append_json_string(out, base64_encode(*v.as_bytes()));
        return out;
    }
    }
    return "null";
}

// ── Minimal recursive-descent JSON reader for from_json() ────────────────────

constexpr int kMaxJsonDepth = 128;

void skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// parse_json_string decodes a quoted JSON string starting at s[i] == '"'.
bool parse_json_string(const std::string& s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (true) {
        if (i >= s.size()) return false;
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
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
                if (i + 4 > s.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                // Minimal UTF-8 encoding (BMP only — sufficient for field names/values).
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
        } else if (static_cast<unsigned char>(c) < 0x20) {
            return false; // raw control chars are not permitted unescaped in strict JSON.
        } else {
            out.push_back(c);
        }
    }
}

bool scan_number(const std::string& s, std::size_t& i) {
    std::size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    bool any_digit = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
        any_digit = true;
    }
    if (!any_digit) return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        bool frac_digit = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            frac_digit = true;
        }
        if (!frac_digit) return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        bool exp_digit = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            exp_digit = true;
        }
        if (!exp_digit) return false;
    }
    return i > start;
}

bool skip_json_value(const std::string& s, std::size_t& i, int depth = 0) {
    if (depth > kMaxJsonDepth) return false;
    skip_ws(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
        ++i;
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skip_ws(s, i);
            std::string key;
            if (!parse_json_string(s, i, key)) return false;
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            if (!skip_json_value(s, i, depth + 1)) return false;
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            break;
        }
        skip_ws(s, i);
        if (i >= s.size() || s[i] != '}') return false;
        ++i;
        return true;
    }
    if (c == '[') {
        ++i;
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            if (!skip_json_value(s, i, depth + 1)) return false;
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            break;
        }
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ']') return false;
        ++i;
        return true;
    }
    if (c == '"') {
        std::string tmp;
        return parse_json_string(s, i, tmp);
    }
    if (c == 't') {
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            return true;
        }
        return false;
    }
    if (c == 'f') {
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return true;
        }
        return false;
    }
    if (c == 'n') {
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return true;
        }
        return false;
    }
    return scan_number(s, i);
}

enum class FieldParseOutcome { Scalar, NestedSkipped, Error };

// parse_field_value extracts a scalar Value (null/bool/number/string) at
// s[i], advancing i past it. Numbers always become Value::Kind::Double,
// matching Go's json.Unmarshal-into-`any` behavior exactly (JSON numbers
// are always float64, never int, regardless of textual shape). Arrays and
// objects are syntactically validated and skipped (NestedSkipped) rather
// than stored — see xtypes.hpp's file-level scope note.
FieldParseOutcome parse_field_value(const std::string& s, std::size_t& i, Value& out) {
    skip_ws(s, i);
    if (i >= s.size()) return FieldParseOutcome::Error;
    char c = s[i];
    if (c == '{' || c == '[') {
        return skip_json_value(s, i) ? FieldParseOutcome::NestedSkipped : FieldParseOutcome::Error;
    }
    if (c == '"') {
        std::string str;
        if (!parse_json_string(s, i, str)) return FieldParseOutcome::Error;
        out = Value::of_string(std::move(str));
        return FieldParseOutcome::Scalar;
    }
    if (c == 't') {
        if (s.compare(i, 4, "true") != 0) return FieldParseOutcome::Error;
        i += 4;
        out = Value::of_bool(true);
        return FieldParseOutcome::Scalar;
    }
    if (c == 'f') {
        if (s.compare(i, 5, "false") != 0) return FieldParseOutcome::Error;
        i += 5;
        out = Value::of_bool(false);
        return FieldParseOutcome::Scalar;
    }
    if (c == 'n') {
        if (s.compare(i, 4, "null") != 0) return FieldParseOutcome::Error;
        i += 4;
        out = Value(); // Null
        return FieldParseOutcome::Scalar;
    }
    std::size_t start = i;
    if (!scan_number(s, i)) return FieldParseOutcome::Error;
    try {
        out = Value::of_double(std::stod(s.substr(start, i - start)));
    } catch (...) {
        return FieldParseOutcome::Error;
    }
    return FieldParseOutcome::Scalar;
}

} // namespace

// ── DynamicData ───────────────────────────────────────────────────────────────

bool DynamicData::has_field(const std::string& name) const {
    if (type_desc_ == nullptr) return false;
    for (const auto& f : type_desc_->fields) {
        if (f.name == name) return true;
    }
    return false;
}

std::error_code DynamicData::set(const std::string& name, Value value) {
    if (!has_field(name)) return ErrUnknownField();
    fields_[name] = std::move(value);
    return {};
}

std::optional<Value> DynamicData::get(const std::string& name) const {
    auto it = fields_.find(name);
    if (it == fields_.end()) return std::nullopt;
    return it->second;
}

std::string DynamicData::to_json() const {
    std::string out = "{";
    bool        first = true;
    // std::map<std::string, Value> iterates in ascending key order already,
    // matching Go's map[string]any -> encoding/json.Marshal key sorting.
    for (const auto& [k, v] : fields_) {
        if (!first) out.push_back(',');
        first = false;
        append_json_string(out, k);
        out.push_back(':');
        out += value_to_json(v);
    }
    out.push_back('}');
    return out;
}

std::error_code DynamicData::from_json(const std::string& json_text) {
    std::size_t i = 0;
    skip_ws(json_text, i);
    if (i >= json_text.size() || json_text[i] != '{') return ErrInvalidJSON();
    ++i;
    skip_ws(json_text, i);
    if (i < json_text.size() && json_text[i] == '}') {
        ++i;
        skip_ws(json_text, i);
        return i == json_text.size() ? std::error_code{} : ErrInvalidJSON();
    }
    while (true) {
        skip_ws(json_text, i);
        std::string key;
        if (!parse_json_string(json_text, i, key)) return ErrInvalidJSON();
        skip_ws(json_text, i);
        if (i >= json_text.size() || json_text[i] != ':') return ErrInvalidJSON();
        ++i;
        skip_ws(json_text, i);
        if (has_field(key)) {
            Value              v;
            FieldParseOutcome r = parse_field_value(json_text, i, v);
            if (r == FieldParseOutcome::Error) return ErrInvalidJSON();
            if (r == FieldParseOutcome::Scalar) fields_[key] = std::move(v);
            // NestedSkipped: forward-compat, not stored — see file-level scope note.
        } else {
            if (!skip_json_value(json_text, i)) return ErrInvalidJSON();
        }
        skip_ws(json_text, i);
        if (i < json_text.size() && json_text[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    skip_ws(json_text, i);
    if (i >= json_text.size() || json_text[i] != '}') return ErrInvalidJSON();
    ++i;
    skip_ws(json_text, i);
    return i == json_text.size() ? std::error_code{} : ErrInvalidJSON();
}

// ── TypeRegistry ──────────────────────────────────────────────────────────────

std::error_code TypeRegistry::register_type(std::shared_ptr<TypeObject> to) {
    std::unique_lock lock(mu_);
    auto              it = types_.find(to->id.name);
    if (it != types_.end()) {
        if (it->second->id.hash != to->id.hash) return ErrTypeMismatch();
        return {}; // identical re-registration is fine.
    }
    types_[to->id.name] = std::move(to);
    return {};
}

std::shared_ptr<TypeObject> TypeRegistry::lookup(const std::string& name) const {
    std::shared_lock lock(mu_);
    auto              it = types_.find(name);
    return it == types_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<TypeObject>> TypeRegistry::all() const {
    std::shared_lock lock(mu_);
    std::vector<std::shared_ptr<TypeObject>> out;
    out.reserve(types_.size());
    // std::map<std::string, ...> iterates in ascending key order already.
    for (const auto& [name, to] : types_) {
        (void)name;
        out.push_back(to);
    }
    return out;
}

// ── Compatibility ─────────────────────────────────────────────────────────────

namespace {
std::map<std::string, const FieldDescriptor*> field_map(const std::vector<FieldDescriptor>& fields) {
    std::map<std::string, const FieldDescriptor*> m;
    for (const auto& f : fields) m[f.name] = &f;
    return m;
}
} // namespace

CompatibilityResult check_compatibility(const TypeDescriptor& writer_td, const TypeDescriptor& reader_td) {
    auto writer_fields = field_map(writer_td.fields);
    for (const auto& rf : reader_td.fields) {
        auto it = writer_fields.find(rf.name);
        if (it == writer_fields.end()) {
            if (!rf.optional) {
                return {false, "required field \"" + rf.name +
                                    "\" expected by reader is absent from writer"};
            }
            continue;
        }
        if (it->second->kind != rf.kind) {
            return {false, "field \"" + rf.name + "\": reader expects " + to_string(rf.kind) +
                                ", writer provides " + to_string(it->second->kind)};
        }
    }
    return {true, ""};
}

} // namespace dds::xtypes
