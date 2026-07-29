// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for cpp-dds's subcommand dispatch and version/capabilities/status/
// conform/convert subcommands (cli/cpp_dds_cli.hpp / cli/cpp_dds_cli.cpp)
// — exercised in-process via cli::dispatch()/cli::cmd_*() rather than by
// spawning the `cpp-dds` executable as a subprocess, matching this repo's
// existing convention for CLI-adjacent unit testing (see
// tests/test_ddstool_cli.cpp and cpp_dds_cli.hpp's doc comment for why the
// logic is factored out of main.cpp to make that possible).
//
// fusa:test REQ-CLI-001 REQ-CLI-002 REQ-CLI-003

#include "cpp_dds_cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

using namespace cli;

// ── Top-level dispatch ───────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: no arguments prints usage and exits 1", "[cli][cpp-dds]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("Usage:") != std::string::npos);
}

TEST_CASE("cpp-dds cli: unknown subcommand exits 1 with a descriptive message",
          "[cli][cpp-dds]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"frobnicate"}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("Unknown command: frobnicate") != std::string::npos);
}

// ── version (§12.1, REQ-CLI-001) ────────────────────────────────────────────

TEST_CASE("cpp-dds cli: version text format", "[cli][cpp-dds][REQ-CLI-001]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"version"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("cpp-dds") != std::string::npos);
    REQUIRE(out.str().find("relay-spec:") != std::string::npos);
    REQUIRE(out.str().find("protocol: DDS") != std::string::npos);
}

TEST_CASE("cpp-dds cli: version --format json", "[cli][cpp-dds][REQ-CLI-001]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"version", "--format", "json"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("\"tool\":") != std::string::npos);
    REQUIRE(out.str().find("\"protocol\":     \"DDS\"") != std::string::npos);
}

TEST_CASE("cpp-dds cli: cmd_version is directly callable (in-process seam)",
          "[cli][cpp-dds][REQ-CLI-001]") {
    std::ostringstream out;
    REQUIRE(cmd_version("text", out) == 0);
    REQUIRE(out.str().find("cpp-dds") != std::string::npos);
}

// ── capabilities (§12.2) ─────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: capabilities emits a JSON document", "[cli][cpp-dds]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"capabilities"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("\"kind\":                \"capabilities\"") != std::string::npos);
    REQUIRE(out.str().find("\"adapt\":               true") != std::string::npos);
}

// ── status (§12.3) ───────────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: status text format reports healthy", "[cli][cpp-dds]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"status"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("healthy:   true") != std::string::npos);
}

TEST_CASE("cpp-dds cli: status --format json reports healthy", "[cli][cpp-dds]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"status", "--format", "json"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("\"healthy\":   true") != std::string::npos);
}

// ── conform (§11.2, REQ-CLI-002) ────────────────────────────────────────────

TEST_CASE("cpp-dds cli: conform passes the mock-transport self-check",
          "[cli][cpp-dds][REQ-CLI-002]") {
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"conform"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str() == "PASS conform\n");
    REQUIRE(err.str().empty());
}

TEST_CASE("cpp-dds cli: cmd_conform is directly callable (in-process seam)",
          "[cli][cpp-dds][REQ-CLI-002]") {
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(cmd_conform(out, err) == 0);
    REQUIRE(out.str() == "PASS conform\n");
}

// ── convert (§11.2, REQ-CLI-003) ────────────────────────────────────────────

namespace {
// RELAY spec §15.7.2 golden vector: topic=rt/chatter, payload="hello dds"
// (base64 aGVsbG8gZGRz), seq=7, writer_guid bytes [1..16], meta
// dds.writer_guid=0102030405060708090a0b0c0d0e0f10 — same vector as
// tests/test_dds.cpp's "RELAY spec spec-15.7.2 golden vector" cases, here
// driven end-to-end through the CLI's stdin/stdout JSON surface instead of
// calling Sample::to_message() directly.
const char* kGoldenSampleJSON = R"({
  "topic": "rt/chatter",
  "payload": "aGVsbG8gZGRz",
  "timestamp": "2026-01-01T00:00:00Z",
  "seq": 7,
  "writer_guid": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]
})";
} // namespace

TEST_CASE("cpp-dds cli: convert reproduces the §15.7.2 golden vector",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "DDS"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(err.str().empty());
    REQUIRE(out.str().find("\"id\": \"rt/chatter\"") != std::string::npos);
    REQUIRE(out.str().find("\"seq\": 7") != std::string::npos);
    REQUIRE(out.str().find("\"dds.writer_guid\": \"0102030405060708090a0b0c0d0e0f10\"") !=
            std::string::npos);
}

TEST_CASE("cpp-dds cli: convert --protocol is case-insensitive", "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "dds"}, in, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("\"id\": \"rt/chatter\"") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert requires --protocol (exit 2)", "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert"}, in, out, err);
    REQUIRE(rc == 2);
    REQUIRE(err.str().find("--protocol is required") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects an unsupported protocol (exit 1)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "ROS2"}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("not supported by cpp-dds") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects an unsupported --format (exit 2)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "DDS", "--format", "yaml"}, in, out, err);
    REQUIRE(rc == 2);
    REQUIRE(err.str().find("unsupported format") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects an unrecognized argument (exit 2)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(kGoldenSampleJSON);
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--bogus"}, in, out, err);
    REQUIRE(rc == 2);
    REQUIRE(err.str().find("unrecognized argument") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects malformed JSON on stdin (exit 1)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in("not json");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "DDS"}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("invalid input") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects a dds.Sample missing a required field (exit 1)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(R"({"topic": "rt/chatter"})");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "DDS"}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("invalid input") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert rejects a writer_guid with the wrong length (exit 1)",
          "[cli][cpp-dds][REQ-CLI-003]") {
    std::istringstream in(R"({
      "topic": "rt/chatter",
      "payload": "aGVsbG8gZGRz",
      "timestamp": "2026-01-01T00:00:00Z",
      "seq": 7,
      "writer_guid": [1,2,3]
    })");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"convert", "--protocol", "DDS"}, in, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("invalid input") != std::string::npos);
}
