// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/xtypes/xtypes.hpp — DDS-XTypes Dynamic Data support.
//
// C++ port of github.com/SoundMatt/go-DDS's `xtypes` package
// (tools/xtypes/xtypes.go, 367 non-test LOC): a runtime type system that
// lets distributed systems evolve their data schemas without lock-step
// software updates. Loosely aligned with the OMG DDS-XTypes 1.3
// specification but simplified, exactly as go-DDS's own package doc
// states. See ROADMAP.md, "Tier 3 — xtypes, tsn, idl, cdr", `xtypes`.
//
// # Type system
//
// A TypeDescriptor describes the structure of a named type: its fields,
// their primitive TypeKind, and optional/required status. A
// TypeIdentifier is a compact, content-addressed fingerprint of a
// descriptor derived from a stable canonical hash (identify()).
//
// # Dynamic Data
//
// DynamicData is a schema-validated property map: values may only be set
// on fields declared in the associated TypeDescriptor. It serialises
// transparently to/from JSON.
//
// # Type registry
//
// TypeRegistry is a thread-safe store of TypeObject values (descriptor +
// identifier pairs). Participants use the registry to announce their
// types and to resolve types received from peers.
//
// # Compatibility checking
//
// check_compatibility() implements the standard forward/backward type
// evolution rules:
//   - New optional fields added to the writer are invisible but harmless
//     to an older reader (forward compatibility).
//   - New required fields expected by the reader but absent from the
//     writer are incompatible (the reader would receive data it cannot
//     interpret).
//   - Renamed or type-changed fields are always incompatible.
//
// # Wire-visible encoding: TypeIdentifier's canonical-JSON hash
//
// TypeIdentifier::hash is SHA-256(canonical JSON representation of the
// descriptor)[0:8], exactly mirroring go-DDS's identify()/canonical().
// Even though this port is not yet wired into RTPS discovery (no item in
// ROADMAP.md's Tier 3 scope for this bullet calls for that), the hash
// itself is a cross-implementation type-identity fingerprint — the same
// reasoning that makes RTPS wire bytes and the safety/security wire
// headers byte-exact ports applies here too. detail::canonical_json()
// reproduces go-DDS's actual `encoding/json.Marshal` output byte-for-byte
// for the shapes this package produces: compact (no whitespace), map keys
// sorted lexicographically ("fields" < "name" < "version" at the
// TypeDescriptor level; "element" < "fields" < "kind" < "name" <
// "optional" at the FieldDescriptor level — each key present only when
// go-DDS's own canonicalField() would include it), integers as plain
// decimal digits, and Go's default `escapeHTML: true` string-escaping
// (control chars and the quote/backslash characters get the usual JSON
// escapes; the three characters '<', '>', '&' additionally each get a
// 4-hex-digit escape of their own — see xtypes.cpp's escaping helper for
// the exact text). All of this — including the escaping rule — was
// verified against real output from a fresh go-DDS clone; see tests/test_xtypes.cpp
// for the exact reference-vector derivation steps. Known, documented,
// low-risk gap: Go's encoder additionally escapes U+2028/U+2029 (line/
// paragraph separator) and replaces invalid UTF-8 with U+FFFD; this port
// passes non-ASCII bytes through unescaped instead. Type and field names
// are simple identifiers in every realistic use of this package (go-DDS's
// own test suite never exercises non-ASCII names either), so this is not
// expected to matter in practice.
//
// # Deliberate scope decisions vs. go-DDS
//
//   - DynamicData::from_json fully parses (and syntactically validates)
//     the entire input document before applying any field, matching
//     go-DDS's `json.Unmarshal(data, &raw map[string]json.RawMessage)`
//     up-front-parse behavior exactly (a value error anywhere in the
//     document fails the whole call, even under an unknown/skipped key).
//     Nested JSON object/array values are skipped rather than stored, even
//     under a declared field name, since dds::xtypes::Value (unlike Go's
//     `any`) does not model arbitrary nested structures — go-DDS's own
//     xtypes_test.go never exercises nested DynamicData field values
//     either.
//   - Value::of_bytes()'s JSON representation follows Go's asymmetric
//     []byte quirk faithfully: to_json() emits a byte slice as a
//     base64-encoded JSON string (matching Go's `encoding/json`
//     special-casing of []byte on *marshal*), but from_json() has no
//     special decode path for it — exactly as go-DDS's own
//     `json.Unmarshal(v, &val)` into `any` never re-derives []byte from a
//     base64 string either (it always yields a plain Go string). A caller
//     reading back a Bytes-kind field after a JSON round trip therefore
//     gets Value::as_string() (base64 text), not Value::as_bytes() — this
//     is go-DDS's own real, documented-here behavior, not an oversight.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <vector>

namespace dds::xtypes {

// ── Error codes ───────────────────────────────────────────────────────────────

// fusa:req REQ-XTYPE-003 REQ-TREG-002
enum class Errc : int {
    unknown_field = 1, // DynamicData::set() on a field not declared in the TypeDescriptor.
    invalid_json  = 2, // DynamicData::from_json(): malformed JSON input.
    type_mismatch = 3, // TypeRegistry::register_type(): name collision with a different hash
                        // (go-DDS: xtypes.ErrTypeMismatch).
};

const std::error_category& error_category() noexcept;
std::error_code             make_error_code(Errc e) noexcept;

inline std::error_code ErrUnknownField() noexcept { return make_error_code(Errc::unknown_field); }
inline std::error_code ErrInvalidJSON() noexcept { return make_error_code(Errc::invalid_json); }
// ErrTypeMismatch mirrors go-DDS's exported `xtypes.ErrTypeMismatch` sentinel.
inline std::error_code ErrTypeMismatch() noexcept { return make_error_code(Errc::type_mismatch); }

// ── Type kinds ────────────────────────────────────────────────────────────────

// TypeKind identifies the primitive kind of a FieldDescriptor
// (go-DDS: xtypes.TypeKind).
// fusa:req REQ-XTYPE-001
enum class TypeKind : uint8_t {
    Bool    = 0, // bool
    Int32   = 1, // int32
    Int64   = 2, // int64
    Float64 = 3, // float64
    String  = 4, // string
    Bytes   = 5, // bytes
    Struct  = 6, // nested struct; FieldDescriptor::fields contains sub-descriptors
    Seq     = 7, // variable-length sequence; FieldDescriptor::element points to the element type
};

// to_string returns a human-readable name for k, matching go-DDS's
// TypeKind.String() exactly, including its fallback for out-of-range
// values ("TypeKind(N)").
std::string to_string(TypeKind k);

// ── Descriptors ───────────────────────────────────────────────────────────────

// FieldDescriptor describes one field within a TypeDescriptor
// (go-DDS: xtypes.FieldDescriptor).
// fusa:req REQ-XTYPE-001
struct FieldDescriptor {
    std::string name;               // field name; must be unique within its parent.
    TypeKind    kind{TypeKind::Bool};
    bool        optional{false};    // if false, the field is required.
    std::vector<FieldDescriptor> fields; // for TypeKind::Struct: nested field descriptors.
    std::shared_ptr<FieldDescriptor> element; // for TypeKind::Seq: element type descriptor.
};

// TypeDescriptor describes a complete named schema
// (go-DDS: xtypes.TypeDescriptor).
// fusa:req REQ-XTYPE-001
struct TypeDescriptor {
    std::string name;                    // type name; must be unique within a TypeRegistry.
    uint32_t    version{0};              // schema version; 0 = unversioned.
    std::vector<FieldDescriptor> fields; // top-level fields.
};

// ── Type identity ─────────────────────────────────────────────────────────────

// TypeIdentifier is a compact, content-addressed reference to a
// TypeDescriptor. Two descriptors with identical structure produce the
// same TypeIdentifier regardless of the order in which they were created
// (go-DDS: xtypes.TypeIdentifier).
// fusa:req REQ-XTYPE-002
struct TypeIdentifier {
    std::string             name; // type name.
    std::array<uint8_t, 8>  hash{}; // first 8 bytes of SHA-256(canonical JSON representation).

    bool operator==(const TypeIdentifier& other) const noexcept {
        return name == other.name && hash == other.hash;
    }
    bool operator!=(const TypeIdentifier& other) const noexcept { return !(*this == other); }
};

// identify computes the TypeIdentifier for td. The hash is derived from a
// sorted, canonical JSON encoding of the descriptor so that structurally
// identical types hash identically regardless of field declaration order
// (go-DDS: xtypes.Identify()).
// fusa:req REQ-XTYPE-002
TypeIdentifier identify(const TypeDescriptor& td);

namespace detail {
// canonical_json returns the exact canonical JSON byte sequence that
// identify() hashes — exposed for byte-for-byte verification against
// go-DDS reference vectors (see tests/test_xtypes.cpp and this header's
// file-level "Wire-visible encoding" note). Not part of the stable public
// surface beyond that verification role.
std::string canonical_json(const TypeDescriptor& td);
} // namespace detail

// TypeObject pairs a TypeDescriptor with its content-addressed
// TypeIdentifier (go-DDS: xtypes.TypeObject).
// fusa:req REQ-XTYPE-005
struct TypeObject {
    TypeIdentifier id;
    TypeDescriptor descriptor;
};

// new_type_object builds a TypeObject from td, computing the identifier
// automatically (go-DDS: xtypes.NewTypeObject()).
// fusa:req REQ-XTYPE-005
std::shared_ptr<TypeObject> new_type_object(TypeDescriptor td);

// ── Dynamic value ─────────────────────────────────────────────────────────────

// Value is a schema-agnostic field value held by DynamicData, mirroring
// what a Go `any` can hold for the leaf JSON kinds this package's own
// test suite exercises (bool / number / string / bytes / null). Not part
// of go-DDS's own exported surface (Go just uses `any` directly) — this
// type exists only because C++ has no equivalent implicit dynamic type.
class Value {
public:
    enum class Kind { Null, Bool, Int64, Double, String, Bytes };

    Value() noexcept : kind_(Kind::Null) {}

    static Value of_bool(bool b);
    static Value of_int64(int64_t i);
    static Value of_double(double d);
    static Value of_string(std::string s);
    static Value of_bytes(std::vector<uint8_t> b);

    Kind kind() const noexcept { return kind_; }
    bool is_null() const noexcept { return kind_ == Kind::Null; }

    std::optional<bool>    as_bool() const;
    std::optional<int64_t> as_int64() const;
    std::optional<double>  as_double() const;
    const std::string*     as_string() const noexcept;
    const std::vector<uint8_t>* as_bytes() const noexcept;

    bool operator==(const Value& other) const noexcept;
    bool operator!=(const Value& other) const noexcept { return !(*this == other); }

private:
    Kind                  kind_;
    bool                  bool_{false};
    int64_t               int_{0};
    double                double_{0.0};
    std::string           str_;
    std::vector<uint8_t>  bytes_;
};

// ── Dynamic Data ──────────────────────────────────────────────────────────────

// DynamicData is a schema-validated property map: values may only be set
// on fields declared in the associated TypeDescriptor. It serialises
// transparently to/from JSON (go-DDS: xtypes.DynamicData).
//
// DynamicData does not own the TypeDescriptor it is constructed with
// (matching go-DDS's *TypeDescriptor pointer field exactly) — the caller
// must keep it alive for the DynamicData's lifetime.
//
// DynamicData is NOT safe for concurrent use from multiple threads
// (matching go-DDS's own documented non-concurrency-safety).
// fusa:req REQ-XTYPE-003 REQ-XTYPE-004
class DynamicData {
public:
    explicit DynamicData(const TypeDescriptor* td) noexcept : type_desc_(td) {}

    // type_descriptor returns the schema backing this DynamicData.
    const TypeDescriptor* type_descriptor() const noexcept { return type_desc_; }

    // set sets the named field to value. Returns ErrUnknownField if name
    // does not exist in the descriptor's top-level fields.
    // fusa:req REQ-XTYPE-003
    std::error_code set(const std::string& name, Value value);

    // get returns the value for name, or std::nullopt if the field has
    // not been set (even if it is declared in the descriptor).
    std::optional<Value> get(const std::string& name) const;

    // to_json serialises the set fields to a compact JSON object, with
    // keys emitted in lexicographic order (matching Go's
    // map[string]any -> encoding/json.Marshal key-sorting behavior).
    // fusa:req REQ-XTYPE-004
    std::string to_json() const;

    // from_json populates set fields from a JSON object. Only keys that
    // match declared fields in the descriptor are stored; unknown keys
    // are silently ignored (forward compatibility: old code reads new
    // data). Returns ErrInvalidJSON for malformed input.
    // fusa:req REQ-XTYPE-004
    std::error_code from_json(const std::string& json_text);

private:
    bool has_field(const std::string& name) const;

    const TypeDescriptor*        type_desc_;
    std::map<std::string, Value> fields_;
};

// ── Type Registry ─────────────────────────────────────────────────────────────

// TypeRegistry is a thread-safe store for TypeObject values
// (go-DDS: xtypes.TypeRegistry).
// fusa:req REQ-TREG-001 REQ-TREG-002 REQ-TREG-003
class TypeRegistry {
public:
    TypeRegistry() = default;

    TypeRegistry(const TypeRegistry&)            = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    // register_type stores to. Returns ErrTypeMismatch if a type with the
    // same name but a different hash is already registered. Registering
    // the same TypeObject twice (same name and hash) is a no-op.
    // (Named register_type, not Register, since `register` is a reserved
    // C++ keyword — go-DDS: xtypes.TypeRegistry.Register().)
    // fusa:req REQ-TREG-001 REQ-TREG-002
    std::error_code register_type(std::shared_ptr<TypeObject> to);

    // lookup returns the TypeObject registered under name, if any.
    // fusa:req REQ-TREG-001
    std::shared_ptr<TypeObject> lookup(const std::string& name) const;

    // all returns a snapshot of all registered TypeObjects in
    // name-sorted order.
    // fusa:req REQ-TREG-003
    std::vector<std::shared_ptr<TypeObject>> all() const;

private:
    mutable std::shared_mutex                          mu_;
    std::map<std::string, std::shared_ptr<TypeObject>> types_;
};

// ── Compatibility ─────────────────────────────────────────────────────────────

// CompatibilityResult describes whether a reader using one type
// descriptor can safely consume data produced by a writer using another
// (go-DDS: xtypes.CompatibilityResult).
struct CompatibilityResult {
    bool        compatible{false}; // true if reader and writer are compatible.
    std::string reason;            // human-readable explanation when compatible is false.
};

// check_compatibility reports whether a reader using reader_td can safely
// consume data written by a writer using writer_td.
//
// The rules follow standard schema-evolution conventions:
//   - Fields present in writer but absent in reader: always OK (forward compat).
//   - Fields present in reader but absent in writer: OK only if Optional in reader.
//   - Fields present in both: compatible only if they have the same TypeKind.
// (go-DDS: xtypes.CheckCompatibility())
// fusa:req REQ-XTYPE-006
CompatibilityResult check_compatibility(const TypeDescriptor& writer_td,
                                         const TypeDescriptor& reader_td);

} // namespace dds::xtypes

namespace std {
template <>
struct is_error_code_enum<dds::xtypes::Errc> : true_type {};
} // namespace std
