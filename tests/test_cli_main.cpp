// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for the cpp-dds CLI's version/capabilities/status/conform/convert
// subcommands (cli/cpp_dds_cli.hpp / cli/cpp_dds_cli.cpp), exercised
// in-process via cppdds_cli::dispatch() rather than by spawning the
// `cpp-dds` executable as a subprocess, matching this repo's existing
// convention for CLI-adjacent unit testing (see
// tests/test_ddstool_cli.cpp's identical precedent for ddstool, and
// cpp_dds_cli.hpp's own doc comment for why the logic is factored out of
// main.cpp to make this possible, and for why the file isn't named the
// more obvious cli.hpp).
//
// fusa:test REQ-CLI-001 REQ-CLI-002 REQ-CLI-003

#include "cpp_dds_cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dds/dds.hpp>

#include <sstream>
#include <string>

using namespace cppdds_cli;

namespace {

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err) {
    return dispatch(args, in, out, err);
}

} // namespace

// ── version (REQ-CLI-001) ────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: version with no --format prints text and exits 0", "[cli][version]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"version"}, in, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("cpp-dds") != std::string::npos);
    CHECK(out.str().find(std::string("relay-spec: ") + dds::kSpecVersion) != std::string::npos);
    CHECK(out.str().find("protocol: DDS") != std::string::npos);
    CHECK(err.str().empty());
}

TEST_CASE("cpp-dds cli: version --format json prints the spec version and protocol as JSON",
          "[cli][version]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"version", "--format", "json"}, in, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("\"tool\":         \"cpp-dds\"") != std::string::npos);
    CHECK(out.str().find(std::string("\"spec_version\": \"") + dds::kSpecVersion + "\"") != std::string::npos);
    CHECK(out.str().find("\"protocol_int\": 2") != std::string::npos);
}

// ── capabilities ──────────────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: capabilities lists every subcommand and exits 0", "[cli][capabilities]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"capabilities"}, in, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("\"kind\":                \"capabilities\"") != std::string::npos);
    CHECK(out.str().find("\"version\", \"capabilities\", \"status\", \"conform\", \"convert\"") !=
          std::string::npos);
}

// ── status ────────────────────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: status reports healthy and exits 0", "[cli][status]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"status"}, in, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("healthy:   true") != std::string::npos);
}

// ── conform (REQ-CLI-002) ────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: conform runs the self-check end to end and prints PASS", "[cli][conform]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"conform"}, in, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("PASS conform") != std::string::npos);
    CHECK(err.str().empty());
}

// ── convert (REQ-CLI-003) ────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: convert --protocol DDS reads a dds.Sample and writes a relay.Message",
          "[cli][convert]") {
    // writer_guid: 16 bytes, all zero except a marker in the last byte, so
    // the emitted hex is easy to assert on.
    std::istringstream in(
        R"({"topic":"MyTopic","payload":"AQIDBA==","timestamp":"2026-01-01T00:00:00Z","seq":7,)"
        R"("writer_guid":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9]})");
    std::ostringstream out, err;
    int rc = run({"convert", "--protocol", "DDS"}, in, out, err);
    CHECK(rc == 0);
    CHECK(err.str().empty());
    CHECK(out.str().find("\"protocol\": 2") != std::string::npos); // relay::Protocol::DDS
    CHECK(out.str().find("\"id\": \"MyTopic\"") != std::string::npos);
    CHECK(out.str().find("\"seq\": 7") != std::string::npos);
    // Timestamp is always normalized to the Go zero-value string (§11.2).
    CHECK(out.str().find("\"timestamp\": \"0001-01-01T00:00:00Z\"") != std::string::npos);
    CHECK(out.str().find("\"dds.writer_guid\": \"00000000000000000000000000000009\"") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert with an unsupported protocol exits 1", "[cli][convert]") {
    std::istringstream in("{}");
    std::ostringstream out, err;
    int rc = run({"convert", "--protocol", "MQTT"}, in, out, err);
    CHECK(rc == 1);
    CHECK(err.str().find("not supported") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert without --protocol exits 2", "[cli][convert]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"convert"}, in, out, err);
    CHECK(rc == 2);
    CHECK(err.str().find("--protocol is required") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert with malformed JSON on stdin exits 1", "[cli][convert]") {
    std::istringstream in("not json");
    std::ostringstream out, err;
    int rc = run({"convert", "--protocol", "DDS"}, in, out, err);
    CHECK(rc == 1);
    CHECK(err.str().find("invalid input") != std::string::npos);
}

TEST_CASE("cpp-dds cli: convert with a missing required field exits 1", "[cli][convert]") {
    std::istringstream in(R"({"topic":"T"})");
    std::ostringstream out, err;
    int rc = run({"convert", "--protocol", "DDS"}, in, out, err);
    CHECK(rc == 1);
    CHECK(err.str().find("invalid input") != std::string::npos);
}

// ── dispatch ──────────────────────────────────────────────────────────────────

TEST_CASE("cpp-dds cli: no arguments prints usage on stderr and exits 1", "[cli][dispatch]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({}, in, out, err);
    CHECK(rc == 1);
    CHECK(err.str().find("Usage: cpp-dds") != std::string::npos);
}

TEST_CASE("cpp-dds cli: unknown subcommand exits 1 with a descriptive message", "[cli][dispatch]") {
    std::istringstream in;
    std::ostringstream out, err;
    int rc = run({"frobnicate"}, in, out, err);
    CHECK(rc == 1);
    CHECK(err.str().find("Unknown command: frobnicate") != std::string::npos);
}
