// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// End-to-end round-trip test for dds::idl's generated C++ output.
//
// Mirrors go-DDS's tools/idl/roundtrip/ package exactly in structure: a
// checked-in `schema.idl` fixture, a checked-in *pre-generated* header
// (schema_gen.hpp, produced by actually running the ddstool CLI target
// this same PR adds — `ddstool idl -namespace idlgen -out
// tests/idl_roundtrip/schema_gen.hpp tests/idl_roundtrip/schema.idl` — not
// hand-written), and a test file that #includes the generated header and
// exercises it exactly like ordinary hand-written code would. This is the
// only test in this PR that actually compiles and runs the generator's
// *output* end-to-end (test_idl.cpp otherwise only asserts on generated
// source *text*), giving genuine confidence the emitted C++ is both valid
// and correct.
//
// schema.idl's field/type shapes are identical to go-DDS's own
// tools/idl/roundtrip/schema.idl, so the two reference implementations
// can be compared field-for-field. The HEADER_VEC/TELEMETRY_VEC/
// TELEMETRY_EMPTY_VEC hex reference vectors below were derived by calling
// go-DDS's *actual* generated HeaderCodec{}.Marshal/TelemetryCodec{}.Marshal
// from a scratch _test.go file placed temporarily inside
// tools/idl/roundtrip/ of a fresh go-DDS clone (never committed there,
// never pushed anywhere) — the same derivation convention used throughout
// this repo's test suite (test_cdr.cpp, test_xtypes.cpp, ...). Reproduced
// 2026-07-27 against go-DDS commit 1691286d7857885f0fb8aab0d5303e945ec144fd:
//
//   HEADER_VEC = 010000000f00000073656e736f72732f656e67696e650000009cc7009001000002000000
//   TELEMETRY_VEC = 010000000d00000073656e736f72732f74656d7000000000ffe30b540200000003000000000000009a9999999999f13f9a999999999901406666666666660a409a999999999911403333c542010000000300000000000000000000000000244000000000000034400000000000003e40
//   TELEMETRY_EMPTY_VEC = 01000000020000007800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
//
// fusa:test REQ-IDL-006 REQ-IDL-007 REQ-IDL-008

#include "schema_gen.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace idlgen;

namespace {

std::string to_hex(const std::vector<uint8_t>& b) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (uint8_t byte : b) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

} // namespace

// ── Enum / typedef sanity ────────────────────────────────────────────────────

TEST_CASE("idl roundtrip: Priority enum values are sequential from 0", "[idl][roundtrip]") {
    REQUIRE(static_cast<int32_t>(Priority::LOW) == 0);
    REQUIRE(static_cast<int32_t>(Priority::MEDIUM) == 1);
    REQUIRE(static_cast<int32_t>(Priority::HIGH) == 2);
    REQUIRE(static_cast<int32_t>(Priority::CRITICAL) == 3);
}

TEST_CASE("idl roundtrip: TopicID is a uint32_t alias", "[idl][roundtrip]") {
    TopicID id = 42;
    static_assert(std::is_same<TopicID, uint32_t>::value, "TopicID must alias uint32_t");
    REQUIRE(id == 42);
}

// ── Header: round trip, key_fields, byte-exact vector ────────────────────────

TEST_CASE("idl roundtrip: Header marshal is byte-exact against the go-DDS reference vector",
          "[idl][roundtrip][vectors]") {
    Header v;
    v.topic_id = "sensors/engine";
    v.timestamp_ns = 1'718'000'000'000LL;
    v.priority = Priority::HIGH;

    auto data = HeaderCodec::marshal(v);
    REQUIRE(to_hex(data) ==
            "010000000f00000073656e736f72732f656e67696e650000009cc7009001000002000000");
}

TEST_CASE("idl roundtrip: Header round-trips through marshal/unmarshal", "[idl][roundtrip]") {
    Header v;
    v.topic_id = "sensors/engine";
    v.timestamp_ns = 1'718'000'000'000LL;
    v.priority = Priority::HIGH;

    auto data = HeaderCodec::marshal(v);
    auto got = HeaderCodec::unmarshal(data);
    REQUIRE(got.has_value());
    REQUIRE(got->topic_id == v.topic_id);
    REQUIRE(got->timestamp_ns == v.timestamp_ns);
    REQUIRE(got->priority == v.priority);
}

TEST_CASE("idl roundtrip: HeaderCodec::key_fields reports topic_id", "[idl][roundtrip]") {
    auto keys = HeaderCodec::key_fields();
    REQUIRE(keys == std::vector<std::string>{"topic_id"});
}

TEST_CASE("idl roundtrip: HeaderCodec::unmarshal rejects a too-short CDR buffer",
          "[idl][roundtrip][error]") {
    auto got = HeaderCodec::unmarshal({0x00});
    REQUIRE_FALSE(got.has_value());
}

TEST_CASE("idl roundtrip: HeaderCodec::unmarshal rejects truncated string/int64/int32 fields",
          "[idl][roundtrip][error]") {
    Header v;
    v.topic_id = "x";
    v.timestamp_ns = 1;
    v.priority = Priority::LOW;
    auto data = HeaderCodec::marshal(v);

    REQUIRE_FALSE(HeaderCodec::unmarshal(std::vector<uint8_t>(data.begin(), data.begin() + 4))
                      .has_value());
    REQUIRE_FALSE(HeaderCodec::unmarshal(std::vector<uint8_t>(data.begin(), data.begin() + 10))
                      .has_value());
    REQUIRE_FALSE(HeaderCodec::unmarshal(std::vector<uint8_t>(data.begin(), data.begin() + 24))
                      .has_value());
}

// ── Telemetry: nested struct, array, sequence, byte-exact vectors ───────────

TEST_CASE("idl roundtrip: Telemetry marshal is byte-exact against the go-DDS reference vector",
          "[idl][roundtrip][vectors]") {
    Telemetry v;
    v.header.topic_id = "sensors/temp";
    v.header.timestamp_ns = 9'999'999'999LL;
    v.header.priority = Priority::CRITICAL;
    v.values = {1.1, 2.2, 3.3, 4.4};
    v.temperature = 98.6F;
    v.valid = true;
    v.extras = {10.0, 20.0, 30.0};

    auto data = TelemetryCodec::marshal(v);
    REQUIRE(to_hex(data) ==
            "010000000d00000073656e736f72732f74656d7000000000ffe30b540200000003000000000000009a"
            "9999999999f13f9a999999999901406666666666660a409a999999999911403333c542010000000300"
            "000000000000000000000000244000000000000034400000000000003e40");
}

TEST_CASE("idl roundtrip: Telemetry with empty extras is byte-exact against the go-DDS "
          "reference vector",
          "[idl][roundtrip][vectors]") {
    Telemetry v;
    v.header.topic_id = "x";
    v.header.priority = Priority::LOW;
    v.valid = false;

    auto data = TelemetryCodec::marshal(v);
    REQUIRE(to_hex(data) ==
            "0100000002000000780000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000000000000000");
}

TEST_CASE("idl roundtrip: Telemetry round-trips through marshal/unmarshal", "[idl][roundtrip]") {
    Telemetry v;
    v.header.topic_id = "t";
    v.header.timestamp_ns = 42;
    v.header.priority = Priority::CRITICAL;
    v.values = {1, 2, 3, 4};
    v.temperature = 36.6F;
    v.valid = true;
    v.extras = {0.1};

    auto data = TelemetryCodec::marshal(v);
    auto got = TelemetryCodec::unmarshal(data);
    REQUIRE(got.has_value());
    REQUIRE(got->header.topic_id == v.header.topic_id);
    REQUIRE(got->header.timestamp_ns == v.header.timestamp_ns);
    REQUIRE(got->header.priority == v.header.priority);
    REQUIRE(got->values == v.values);
    REQUIRE(got->temperature == v.temperature);
    REQUIRE(got->valid == v.valid);
    REQUIRE(got->extras == v.extras);
}

TEST_CASE("idl roundtrip: Telemetry with empty extras round-trips to a zero-length sequence",
          "[idl][roundtrip]") {
    Telemetry v;
    v.header.topic_id = "x";
    v.header.priority = Priority::LOW;
    v.valid = false;

    auto data = TelemetryCodec::marshal(v);
    auto got = TelemetryCodec::unmarshal(data);
    REQUIRE(got.has_value());
    REQUIRE(got->header.topic_id == v.header.topic_id);
    REQUIRE(got->valid == v.valid);
    REQUIRE(got->extras.empty());
}

TEST_CASE("idl roundtrip: TelemetryCodec::key_fields is empty (no @key field)",
          "[idl][roundtrip]") {
    REQUIRE(TelemetryCodec::key_fields().empty());
}

TEST_CASE("idl roundtrip: TelemetryCodec::unmarshal rejects a too-short CDR buffer",
          "[idl][roundtrip][error]") {
    REQUIRE_FALSE(TelemetryCodec::unmarshal({0x00}).has_value());
}

TEST_CASE("idl roundtrip: TelemetryCodec::unmarshal rejects truncation at every field boundary",
          "[idl][roundtrip][error]") {
    Telemetry v;
    v.header.topic_id = "x";
    v.header.timestamp_ns = 1;
    v.header.priority = Priority::LOW;
    v.values = {1, 2, 3, 4};
    v.temperature = 1.0F;
    v.valid = true;
    v.extras = {5.0};
    auto data = TelemetryCodec::marshal(v);

    // Truncate progressively deeper into the wire layout; every prefix
    // short of the full buffer must be rejected, never read out of bounds.
    for (std::size_t cut : {0U, 4U, 5U, 13U, 25U, 28U, 64U, 68U, 69U, 76U}) {
        if (cut > data.size()) continue;
        std::vector<uint8_t> truncated(data.begin(), data.begin() + static_cast<long>(cut));
        REQUIRE_FALSE(TelemetryCodec::unmarshal(truncated).has_value());
    }
}
