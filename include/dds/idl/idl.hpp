// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/idl/idl.hpp — OMG IDL parser plus a C++ code generator, for DDS topic
// types defined in `.idl` files.
//
// C++ port of github.com/SoundMatt/go-DDS's `tools/idl` package (1,382 LOC
// incl. tests) and its `tools/cmd/ddstool` `idl` subcommand. See
// ROADMAP.md, "Tier 3 — xtypes, tsn, idl, cdr", `idl`.
//
// # Scope
//
// parse_string()/parse_file() parse a subset of OMG IDL 4.x sufficient for
// DDS topic-type definitions:
//   - module declarations (may be nested)
//   - struct declarations, with fields referencing other structs/enums
//     (including qualified `Module::Name` references)
//   - enum declarations
//   - typedef aliases
//   - basic types: boolean, octet, short, unsigned short, long,
//     unsigned long, long long, unsigned long long, float, double, string
//   - bounded strings (`string<N>` — the bound is parsed and discarded,
//     matching go-DDS's own unbounded-`std::string`/`string` field type)
//   - sequences, bounded and unbounded (`sequence<T>`, `sequence<T, N>` —
//     the bound is parsed and discarded)
//   - fixed-size arrays (`T name[N]`)
//   - the `@key` field annotation; other annotations (`@id(N)`,
//     `@optional`, ...) are lexically invisible the same way go-DDS's own
//     parser leaves them: the lexer has no token for `(`/`)`, so a
//     *parenthesized-argument* annotation such as `@id(5)` is NOT
//     supported and yields a parse error (the `(...)`-skipping branch in
//     go-DDS's parseStruct never actually triggers, since parens never
//     become a `{` token) — confirmed against a fresh go-DDS clone and
//     faithfully preserved here rather than "fixed", per this repo's
//     byte-for-byte/behavior-parity porting convention. Only bare
//     (argument-less) annotations like `@key` are supported.
//
// generate() produces a self-contained C++ header (as a string) containing,
// for every IDL struct/enum/typedef in the module tree:
//   - a C++ struct/`enum class`/type alias, fields and enumerators in
//     declaration order, using the field/type names exactly as written in
//     the IDL source (unlike go-DDS's PascalCase-exported-field
//     convention, C++ has no public/private-by-case rule to satisfy, so
//     this is a deliberate, documented scope difference — see
//     ROADMAP.md's `idl` "Done" bullet)
//   - a `<Name>Codec` struct with `static marshal()`/`static unmarshal()`
//     built on `dds::cdr::Encoder`/`dds::cdr::Decoder` (Tier 3, `cdr`,
//     already landed) and `static key_fields()` returning the `@key`
//     field names — nested struct/enum/typedef references are inlined
//     recursively at the call site (matching CDR's own "no wrapper
//     header for nested structs" wire semantics, and matching go-DDS's
//     own recursive-expansion generator exactly), with a cycle guard for
//     self-referential struct graphs
//
// Unlike go-DDS's generator, no `TypedPublisher`/`TypedSubscriber`
// factory functions are emitted: cpp-DDS has no `dds::Codec<T>`/
// `TypedPublisher<T>` abstraction yet (go-DDS-only, not yet ported) — this
// is a second deliberate, documented scope difference from the go-DDS
// reference generator.
//
// # Usage
//
//   auto pr = dds::idl::parse_file("vehicle.idl");
//   if (!pr.module) { std::cerr << pr.error->message << "\n"; return 1; }
//   auto gr = dds::idl::generate(*pr.module);
//   if (!gr.source) { std::cerr << *gr.error << "\n"; return 1; }
//   std::ofstream("vehicle_gen.hpp") << *gr.source;

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dds::idl {

// ── AST ───────────────────────────────────────────────────────────────────────

// TypeKind identifies a primitive or compound IDL type.
// fusa:req REQ-IDL-002 REQ-IDL-003
enum class TypeKind {
    boolean,     // IDL: boolean            -> C++: bool
    octet,       // IDL: octet              -> C++: uint8_t
    short_,      // IDL: short              -> C++: int16_t
    ushort,      // IDL: unsigned short     -> C++: uint16_t
    long_,       // IDL: long               -> C++: int32_t
    ulong,       // IDL: unsigned long      -> C++: uint32_t
    long_long,   // IDL: long long          -> C++: int64_t
    ulong_long,  // IDL: unsigned long long -> C++: uint64_t
    float_,      // IDL: float              -> C++: float
    double_,     // IDL: double             -> C++: double
    string,      // IDL: string / string<N> -> C++: std::string
    sequence,    // IDL: sequence<T>[, N]   -> C++: std::vector<ElemType>
    struct_,     // IDL: struct T (cross-reference by name)
    array,       // IDL: T name[N]          -> C++: std::array<T, N>
    enum_,       // IDL: enum E { A, B }    -> C++: enum class E : int32_t
};

// TypeSpec describes the type of a field or typedef.
struct TypeSpec {
    TypeKind kind{TypeKind::boolean};
    std::shared_ptr<TypeSpec> elem_type;  // non-null for sequence and array
    int array_size{0};                    // non-zero for array: element count
    std::string ref_name;                 // non-empty for struct_ and enum_:
                                           // IDL name (may be Module::Name)
};

// Field is one field within an IDL struct.
struct Field {
    std::string name;    // IDL identifier (original case)
    TypeSpec type;        // field type
    bool key{false};       // true when the field carries an @key annotation
};

// Struct is an IDL struct declaration.
struct Struct {
    std::string name;         // struct name
    std::vector<Field> fields; // ordered fields
};

// Enum is an IDL enum declaration.
struct Enum {
    std::string name;               // enum name
    std::vector<std::string> values; // enumerator names in declaration order
};

// Typedef is an IDL typedef declaration: typedef BaseType AliasName.
struct Typedef {
    std::string name; // alias name
    TypeSpec type;    // underlying type
};

// Module is the top-level container returned by parse_string()/parse_file().
// It may contain both structs defined directly at module scope and
// sub-modules.
// fusa:req REQ-IDL-001
struct Module {
    std::string name;                              // module name (empty at file scope)
    std::vector<Typedef> typedefs;                  // typedefs defined in this module
    std::vector<Enum> enums;                        // enums defined in this module
    std::vector<Struct> structs;                     // structs defined in this module
    std::vector<std::shared_ptr<Module>> modules;    // nested sub-modules
};

// ── Errors ────────────────────────────────────────────────────────────────────

// ParseError carries a human-readable, line-numbered diagnostic. C++ analog
// of go-DDS's `fmt.Errorf("idl: line %d: ...", ...)` strings — a plain
// struct rather than std::error_code, since (unlike dds::xtypes's/
// dds::cdr's small closed error sets) IDL parse failures need free-form,
// per-occurrence diagnostic text, not a fixed enumerable condition.
// fusa:req REQ-IDL-005
struct ParseError {
    int line{0};
    std::string message;
};

// ParseResult is returned by parse_string()/parse_file(): exactly one of
// `module`/`error` is engaged.
struct ParseResult {
    std::optional<Module> module;
    std::optional<ParseError> error;

    bool ok() const noexcept { return module.has_value(); }
};

// GenerateResult is returned by generate(): exactly one of `source`/`error`
// is engaged.
struct GenerateResult {
    std::optional<std::string> source;
    std::optional<std::string> error;

    bool ok() const noexcept { return source.has_value(); }
};

// ── Entry points ──────────────────────────────────────────────────────────────

// parse_string parses src as IDL source and returns the top-level Module.
// fusa:req REQ-IDL-001 REQ-IDL-002 REQ-IDL-003 REQ-IDL-004 REQ-IDL-005
ParseResult parse_string(const std::string& src);

// parse_file reads path and parses it as an IDL file.
// fusa:req REQ-IDL-001 REQ-IDL-005
ParseResult parse_file(const std::string& path);

// generate produces a complete, self-contained C++ header (as a string)
// from m: a struct/enum/typedef for every IDL declaration plus a CDR codec
// for every struct.
// fusa:req REQ-IDL-006 REQ-IDL-007 REQ-IDL-008
GenerateResult generate(const Module& m);

} // namespace dds::idl
