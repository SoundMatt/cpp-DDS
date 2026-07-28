// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::idl — a C++ port of github.com/SoundMatt/go-DDS's
// `tools/idl` package (idl.go/parser.go/gen.go/idl_test.go), the
// correctness oracle named in cpp-DDS's ROADMAP.md ("Tier 3 — xtypes,
// tsn, idl, cdr", `idl`).
//
// fusa:test REQ-IDL-001 REQ-IDL-002 REQ-IDL-003 REQ-IDL-004 REQ-IDL-005
// fusa:test REQ-IDL-006 REQ-IDL-007 REQ-IDL-008
//
// Behavioral parity (parser test matrix, generator string-content
// assertions) mirrors go-DDS's tools/idl/idl_test.go line-for-line where
// meaningful for C++. Two behaviors were independently confirmed against a
// fresh go-DDS clone (never committed there) before porting, since neither
// is stated in idl_test.go's existing matrix:
//
//   1. `Generate`'s package/namespace name is derived from the *root*
//      Module's Name, which parse_string()/parse_file() always leave
//      empty (a top-level `module X { ... }` declaration becomes a
//      *sub*-module of the root, never the root itself) -- so the
//      generated namespace is always "idlgen" unless a caller explicitly
//      overrides Module::name first (mirrored here as `m.name = "..."`,
//      matching go-DDS's own `ddstool idl -package` flag / this repo's
//      `ddstool idl -namespace` flag).
//   2. A parenthesized annotation argument (`@id(5)`) is NOT supported and
//      produces a parse error -- go-DDS's lexer has no token for `(`/`)`,
//      so the `(...)`-skip branch in its parseStruct annotation loop never
//      actually triggers. Faithfully preserved, not "fixed" -- see
//      dds/idl/idl.hpp's module-level doc comment.
//
// Both were verified with a scratch _test.go file inside a fresh go-DDS
// clone (`go test ./tools/idl -run TestScratchProbe... -v`), 2026-07-27,
// commit 1691286d7857885f0fb8aab0d5303e945ec144fd; never committed to
// go-DDS, never pushed anywhere.

#include "dds/idl/idl.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dds::idl;

namespace {

const char* kVehicleIDL = R"IDL(
// Vehicle telemetry IDL
module VehicleData {
    struct Speed {
        string vehicle_id;
        double kph;
        long long timestamp_ns;
        boolean valid;
    };

    struct EngineStatus {
        boolean running;
        unsigned long rpm;
        float temperature;
        unsigned short gear;
        short error_code;
    };
};
)IDL";

} // namespace

// ── Parser: modules/structs ──────────────────────────────────────────────────

TEST_CASE("idl: parse_string finds nested module with two structs", "[idl][parser]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    REQUIRE(pr.module->modules.size() == 1);
    const Module& sub = *pr.module->modules[0];
    REQUIRE(sub.name == "VehicleData");
    REQUIRE(sub.structs.size() == 2);
}

TEST_CASE("idl: parse_string reads struct fields in order with correct kinds", "[idl][parser]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    const Struct& speed = pr.module->modules[0]->structs[0];
    REQUIRE(speed.name == "Speed");
    REQUIRE(speed.fields.size() == 4);
    REQUIRE(speed.fields[0].name == "vehicle_id");
    REQUIRE(speed.fields[0].type.kind == TypeKind::string);
    REQUIRE(speed.fields[1].type.kind == TypeKind::double_);
    REQUIRE(speed.fields[2].type.kind == TypeKind::long_long);
    REQUIRE(speed.fields[3].type.kind == TypeKind::boolean);
}

TEST_CASE("idl: parse_string reads unsigned/short variants correctly", "[idl][parser]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    const Struct& eng = pr.module->modules[0]->structs[1];
    REQUIRE(eng.fields[1].type.kind == TypeKind::ulong);
    REQUIRE(eng.fields[3].type.kind == TypeKind::ushort);
    REQUIRE(eng.fields[4].type.kind == TypeKind::short_);
}

// ── Parser: sequences, bounded types, arrays, qualified names ───────────────

TEST_CASE("idl: parse_string handles unbounded sequence", "[idl][parser]") {
    ParseResult pr = parse_string("struct Batch { sequence<double> samples; };");
    REQUIRE(pr.ok());
    REQUIRE(pr.module->structs.size() == 1);
    const Field& f = pr.module->structs[0].fields[0];
    REQUIRE(f.type.kind == TypeKind::sequence);
    REQUIRE(f.type.elem_type != nullptr);
    REQUIRE(f.type.elem_type->kind == TypeKind::double_);
}

TEST_CASE("idl: parse_string handles octet sequence", "[idl][parser]") {
    ParseResult pr = parse_string("struct Blob { sequence<octet> data; };");
    REQUIRE(pr.ok());
    const Field& f = pr.module->structs[0].fields[0];
    REQUIRE(f.type.kind == TypeKind::sequence);
    REQUIRE(f.type.elem_type->kind == TypeKind::octet);
}

TEST_CASE("idl: parse_string handles bounded sequence (bound discarded)", "[idl][parser]") {
    ParseResult pr = parse_string("struct Batch { sequence<double, 10> samples; };");
    REQUIRE(pr.ok());
    const Field& f = pr.module->structs[0].fields[0];
    REQUIRE(f.type.kind == TypeKind::sequence);
    REQUIRE(f.type.elem_type->kind == TypeKind::double_);
}

TEST_CASE("idl: parse_string handles bounded string (bound discarded)", "[idl][parser]") {
    ParseResult pr = parse_string("struct Named { string<32> name; };");
    REQUIRE(pr.ok());
    REQUIRE(pr.module->structs[0].fields[0].type.kind == TypeKind::string);
}

TEST_CASE("idl: parse_string handles fixed-size array", "[idl][parser]") {
    ParseResult pr = parse_string("struct Grid { long cells[3]; };");
    REQUIRE(pr.ok());
    const Field& f = pr.module->structs[0].fields[0];
    REQUIRE(f.type.kind == TypeKind::array);
    REQUIRE(f.type.array_size == 3);
    REQUIRE(f.type.elem_type->kind == TypeKind::long_);
}

TEST_CASE("idl: parse_string handles qualified Module::Name references", "[idl][parser]") {
    const char* src = R"(
module M {
  struct Inner { long a; };
  struct Outer { M::Inner inner; };
};
)";
    ParseResult pr = parse_string(src);
    REQUIRE(pr.ok());
    const Module& m = *pr.module->modules[0];
    const Struct& outer = m.structs[1];
    REQUIRE(outer.fields[0].type.kind == TypeKind::struct_);
    REQUIRE(outer.fields[0].type.ref_name == "M::Inner");
}

// ── Parser: @key annotation, typedef, enum ───────────────────────────────────

TEST_CASE("idl: parse_string records @key annotation", "[idl][parser]") {
    ParseResult pr = parse_string("struct Foo { @key string id; long x; };");
    REQUIRE(pr.ok());
    const Struct& s = pr.module->structs[0];
    REQUIRE(s.fields[0].key);
    REQUIRE_FALSE(s.fields[1].key);
}

TEST_CASE("idl: parse_string parses typedef alias", "[idl][parser]") {
    ParseResult pr = parse_string("typedef unsigned long TopicID;");
    REQUIRE(pr.ok());
    REQUIRE(pr.module->typedefs.size() == 1);
    REQUIRE(pr.module->typedefs[0].name == "TopicID");
    REQUIRE(pr.module->typedefs[0].type.kind == TypeKind::ulong);
}

TEST_CASE("idl: parse_string parses enum values in declaration order", "[idl][parser]") {
    ParseResult pr = parse_string("enum Priority { LOW, MEDIUM, HIGH, CRITICAL };");
    REQUIRE(pr.ok());
    REQUIRE(pr.module->enums.size() == 1);
    const std::vector<std::string>& vals = pr.module->enums[0].values;
    REQUIRE(vals == std::vector<std::string>{"LOW", "MEDIUM", "HIGH", "CRITICAL"});
}

// ── Parser: error paths ──────────────────────────────────────────────────────

TEST_CASE("idl: parenthesized annotation argument is a parse error (go-DDS parity)",
          "[idl][parser][error]") {
    ParseResult pr = parse_string("struct Foo { @id(5) long x; };");
    REQUIRE_FALSE(pr.ok());
    REQUIRE(pr.error.has_value());
    REQUIRE(pr.error->message.find("expected type name") != std::string::npos);
}

TEST_CASE("idl: invalid array size is a parse error", "[idl][parser][error]") {
    ParseResult pr = parse_string("struct Foo { long x[abc]; };");
    REQUIRE_FALSE(pr.ok());
}

TEST_CASE("idl: unknown type after 'unsigned' is a parse error", "[idl][parser][error]") {
    ParseResult pr = parse_string("struct Foo { unsigned potato x; };");
    REQUIRE_FALSE(pr.ok());
}

TEST_CASE("idl: missing struct closing brace is a parse error, not a crash",
          "[idl][parser][error]") {
    ParseResult pr = parse_string("struct Foo { long x;");
    REQUIRE_FALSE(pr.ok());
    REQUIRE(pr.error.has_value());
}

TEST_CASE("idl: unterminated sequence is a parse error, not a crash", "[idl][parser][error]") {
    ParseResult pr = parse_string("struct Foo { sequence<long x; };");
    REQUIRE_FALSE(pr.ok());
}

TEST_CASE("idl: parse_file reports an error for a missing file", "[idl][parser][error]") {
    ParseResult pr = parse_file("/nonexistent/path/does-not-exist.idl");
    REQUIRE_FALSE(pr.ok());
    REQUIRE(pr.error.has_value());
}

// ── Generator: struct / enum / typedef / codec emission ─────────────────────

TEST_CASE("idl: generate emits a struct per IDL struct", "[idl][gen]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("struct Speed {") != std::string::npos);
    REQUIRE(gr.source->find("struct EngineStatus {") != std::string::npos);
}

TEST_CASE("idl: generate emits a codec with marshal/unmarshal per struct", "[idl][gen]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("struct SpeedCodec {") != std::string::npos);
    REQUIRE(gr.source->find("static std::vector<uint8_t> marshal(const Speed& v)") !=
            std::string::npos);
    REQUIRE(gr.source->find("static std::optional<Speed> unmarshal(") != std::string::npos);
}

TEST_CASE("idl: generate uses namespace idlgen when the root module has no name", "[idl][gen]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    // parse_string()/parse_file() always leave the *root* Module::name
    // empty -- "module VehicleData { ... }" becomes a sub-module, never
    // the root itself -- so the emitted namespace is always "idlgen"
    // unless the caller overrides it first (confirmed against go-DDS;
    // see file-level doc comment above).
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("namespace idlgen {") != std::string::npos);
}

TEST_CASE("idl: generate honors an explicit root module-name override", "[idl][gen]") {
    ParseResult pr = parse_string("struct A { long x; };");
    REQUIRE(pr.ok());
    pr.module->name = "MyPkg";
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("namespace mypkg {") != std::string::npos);
}

TEST_CASE("idl: generate keeps field names verbatim (no PascalCase conversion)", "[idl][gen]") {
    ParseResult pr = parse_string(kVehicleIDL);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("vehicle_id") != std::string::npos);
    REQUIRE(gr.source->find("timestamp_ns") != std::string::npos);
}

TEST_CASE("idl: generate emits key_fields() with @key field names", "[idl][gen]") {
    ParseResult pr = parse_string("struct Foo { @key string id; long x; };");
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find(R"(return { "id" };)") != std::string::npos);
}

TEST_CASE("idl: generate emits empty key_fields() when no field is @key", "[idl][gen]") {
    ParseResult pr = parse_string("struct Foo { long x; };");
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("return {};") != std::string::npos);
}

TEST_CASE("idl: generate emits an enum class with sequential int32 values", "[idl][gen]") {
    ParseResult pr = parse_string("enum Priority { LOW, MEDIUM, HIGH, CRITICAL };");
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("enum class Priority : int32_t {") != std::string::npos);
    REQUIRE(gr.source->find("LOW = 0,") != std::string::npos);
    REQUIRE(gr.source->find("CRITICAL = 3,") != std::string::npos);
}

TEST_CASE("idl: generate emits a using-alias for typedef", "[idl][gen]") {
    ParseResult pr = parse_string("typedef unsigned long TopicID;");
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("using TopicID = uint32_t;") != std::string::npos);
}

TEST_CASE("idl: generate inlines nested struct fields with no wrapper header", "[idl][gen]") {
    const char* src = R"(
struct Point { double x; double y; };
struct Path { sequence<Point> points; Point corners[4]; };
)";
    ParseResult pr = parse_string(src);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    // Nested struct fields are expanded at the call site (matching CDR's
    // "no wrapper for nested structs" wire semantics) -- e.g. an element
    // reference's `.x`/`.y` sub-fields appear directly, not via a nested
    // codec call.
    REQUIRE(gr.source->find(".x") != std::string::npos);
    REQUIRE(gr.source->find(".y") != std::string::npos);
    REQUIRE(gr.source->find("std::vector<Point>") != std::string::npos);
    REQUIRE(gr.source->find("std::array<Point, 4>") != std::string::npos);
}

TEST_CASE("idl: generate resolves qualified Module::Name struct references", "[idl][gen]") {
    const char* src = R"(
module M {
  struct Inner { long a; };
  struct Outer { M::Inner inner; };
};
)";
    ParseResult pr = parse_string(src);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("Inner inner{};") != std::string::npos);
    REQUIRE(gr.source->find("v.inner.a") != std::string::npos);
}

TEST_CASE("idl: generate emits a cycle-guard comment for self-referential structs, "
          "not infinite recursion",
          "[idl][gen]") {
    // A directly self-referential struct field is not itself valid IDL
    // (there is no indirection/pointer concept in this grammar subset),
    // but a struct referencing itself via a sequence is syntactically
    // legal and must not hang the generator.
    //
    // NOTE on a confirmed, deliberate divergence from go-DDS here: go-DDS's
    // *own* Generate() actually returns an error for this exact input
    // (confirmed against a fresh clone) -- its generator joins a struct's
    // field-expansion statements with `"; "` onto a single Go source line,
    // and a `// TODO: ... (cyclic struct Node)` comment landing mid-line
    // comments out everything after it on that line (including the `for`
    // loop's closing `}`), leaving unbalanced braces that gofmt rejects.
    // This C++ port emits one statement per source line (see gen.cpp's
    // write_lines()), so the analogous TODO comment sits on its own line
    // and never swallows subsequent code -- generation succeeds here,
    // producing valid (if functionally incomplete for the guarded field)
    // C++. This is treated as an accidental go-DDS formatting bug rather
    // than a semantic the port must reproduce; not exercised by any real
    // DDS topic-type IDL (self-referential-via-container types are rare
    // outside recursive tree-shaped data).
    const char* src = "struct Node { sequence<Node> children; };";
    ParseResult pr = parse_string(src);
    REQUIRE(pr.ok());
    GenerateResult gr = generate(*pr.module);
    REQUIRE(gr.ok());
    REQUIRE(gr.source->find("cyclic struct") != std::string::npos);
}
