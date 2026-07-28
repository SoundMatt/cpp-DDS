// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for ddstool's subcommand dispatch and `idl` subcommand
// (ddstool/cli.hpp / ddstool/cli.cpp) — exercised in-process via
// ddstool::dispatch()/ddstool::run_idl() rather than by spawning the
// `ddstool` executable as a subprocess, matching this repo's existing
// convention for CLI-adjacent unit testing (see cli.hpp's doc comment for
// why the logic is factored out of main.cpp to make that possible).
//
// fusa:test REQ-IDL-009

#include "cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ddstool;

namespace {

const char* kBatchIDL = "struct Batch { long x; double y; };";

// write_temp_idl writes src to a fresh temp file (under the platform temp
// directory, clock- and monotonic-counter-qualified name to avoid
// collisions across parallel test runs) and returns its path.
std::string write_temp_idl(const std::string& src, const char* suffix) {
    static std::atomic<long> counter{0};
    auto now_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path();
    std::string name = "cppdds_ddstool_cli_test_" + std::to_string(now_ns) + "_" +
                        std::to_string(counter.fetch_add(1)) + suffix;
    std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << src;
    return path.string();
}

} // namespace

// ── Top-level dispatch ───────────────────────────────────────────────────────

TEST_CASE("ddstool cli: no arguments prints usage and exits 1", "[ddstool][cli]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({}, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("USAGE") != std::string::npos);
}

TEST_CASE("ddstool cli: help prints usage on stdout and exits 0", "[ddstool][cli]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"help"}, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("SUBCOMMANDS") != std::string::npos);
}

TEST_CASE("ddstool cli: -h and --help alias help", "[ddstool][cli]") {
    std::ostringstream out1, err1, out2, err2;
    REQUIRE(dispatch({"-h"}, out1, err1) == 0);
    REQUIRE(dispatch({"--help"}, out2, err2) == 0);
    REQUIRE(out1.str().find("USAGE") != std::string::npos);
    REQUIRE(out2.str().find("USAGE") != std::string::npos);
}

TEST_CASE("ddstool cli: unknown subcommand exits 1 with a descriptive message",
          "[ddstool][cli]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"frobnicate"}, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("unknown subcommand") != std::string::npos);
    REQUIRE(err.str().find("frobnicate") != std::string::npos);
}

// ── idl subcommand ────────────────────────────────────────────────────────────

TEST_CASE("ddstool cli: idl with no input file exits 1", "[ddstool][cli][idl]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl"}, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("usage") != std::string::npos);
}

TEST_CASE("ddstool cli: idl -h prints idl usage and exits 0", "[ddstool][cli][idl]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "-h"}, out, err);
    REQUIRE(rc == 0);
    REQUIRE(err.str().find("ddstool idl") != std::string::npos);
}

TEST_CASE("ddstool cli: idl with an unknown flag exits 1", "[ddstool][cli][idl]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "-bogus"}, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("unknown flag") != std::string::npos);
}

TEST_CASE("ddstool cli: idl on a missing file exits 1 with a parse error", "[ddstool][cli][idl]") {
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "/nonexistent/path/does-not-exist.idl"}, out, err);
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("idl: parse") != std::string::npos);
}

TEST_CASE("ddstool cli: idl on malformed IDL exits 1 with a descriptive parse error",
          "[ddstool][cli][idl]") {
    std::string path = write_temp_idl("struct Foo { long x", "_malformed.idl");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", path}, out, err);
    std::remove(path.c_str());
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("idl: parse") != std::string::npos);
}

TEST_CASE("ddstool cli: idl with no -out writes generated C++ to stdout", "[ddstool][cli][idl]") {
    std::string path = write_temp_idl(kBatchIDL, "_batch.idl");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", path}, out, err);
    std::remove(path.c_str());
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("struct Batch {") != std::string::npos);
    REQUIRE(out.str().find("struct BatchCodec {") != std::string::npos);
}

TEST_CASE("ddstool cli: idl -namespace overrides the emitted namespace", "[ddstool][cli][idl]") {
    std::string path = write_temp_idl(kBatchIDL, "_batch_ns.idl");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "-namespace", "mytelemetry", path}, out, err);
    std::remove(path.c_str());
    REQUIRE(rc == 0);
    REQUIRE(out.str().find("namespace mytelemetry {") != std::string::npos);
}

TEST_CASE("ddstool cli: idl -out writes generated C++ to a file, not stdout",
          "[ddstool][cli][idl]") {
    std::string in_path = write_temp_idl(kBatchIDL, "_batch_out.idl");
    std::string out_path = in_path + ".hpp";
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "-out", out_path, in_path}, out, err);
    REQUIRE(rc == 0);
    REQUIRE(out.str().empty()); // generated source went to the file, not stdout
    REQUIRE(err.str().find("wrote") != std::string::npos);

    std::ifstream generated(out_path);
    REQUIRE(generated.good());
    std::ostringstream contents;
    contents << generated.rdbuf();
    REQUIRE(contents.str().find("struct Batch {") != std::string::npos);

    std::remove(in_path.c_str());
    std::remove(out_path.c_str());
}

TEST_CASE("ddstool cli: idl -out to an unwritable path exits 1", "[ddstool][cli][idl]") {
    std::string in_path = write_temp_idl(kBatchIDL, "_batch_bad_out.idl");
    std::ostringstream out;
    std::ostringstream err;
    int rc = dispatch({"idl", "-out", "/nonexistent-dir/does/not/exist.hpp", in_path}, out, err);
    std::remove(in_path.c_str());
    REQUIRE(rc == 1);
    REQUIRE(err.str().find("idl: write") != std::string::npos);
}
