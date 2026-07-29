// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// cli/cpp_dds_cli.hpp — testable entry points for the cpp-dds command-line
// tool, factored out of main.cpp so tests/test_cpp_dds_cli.cpp can
// exercise the real argument-parsing/exit-code behavior directly
// (in-process, no subprocess spawn), mirroring ddstool/cli.hpp's own
// precedent (see that file's doc comment for the general rationale).
//
// Named cpp_dds_cli.hpp rather than cli.hpp (unlike ddstool's own
// cli.hpp/cli.cpp) to avoid an include-path ambiguity: this directory is
// itself named cli/, and ddstool/cli.hpp already exists — a bare
// #include "cli.hpp" from tests/ (which has both directories on its
// include path) would silently resolve to whichever directory's include
// path was registered first, not necessarily this one.

#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace cli {

// print_usage writes cpp-dds's top-level usage text to out.
void print_usage(std::ostream& out);

// cmd_version implements the mandatory `version` subcommand (spec §12.1):
// prints tool/protocol/spec-version information in fmt ("text" or "json")
// to out. Always returns 0.
//
// fusa:req REQ-CLI-001
int cmd_version(const std::string& fmt, std::ostream& out);

// cmd_capabilities implements the mandatory `capabilities` subcommand
// (spec §12.2): emits the capabilities document as JSON to out. Always
// returns 0.
int cmd_capabilities(std::ostream& out);

// cmd_status implements the mandatory `status` subcommand (spec §12.3):
// prints self-assessed health status in fmt to out. Always returns 0
// (healthy).
int cmd_status(const std::string& fmt, std::ostream& out);

// cmd_conform implements the optional `conform` subcommand (spec §11.2):
// a basic RELAY conformance self-check against the mock transport.
// Writes "PASS conform" to out on success; diagnostics on any failure go
// to err. Returns 0 on success, 1 on the first failed check.
//
// fusa:req REQ-CLI-002
int cmd_conform(std::ostream& out, std::ostream& err);

// cmd_convert implements the optional `convert` subcommand (spec §11.2):
// parses args (everything after "cpp-dds convert"), reads one dds.Sample
// JSON value (spec/schemas/dds-sample.json) from in, converts it to a
// relay.Message via Sample::to_message(), and writes the result as JSON
// (spec/schemas/relay-message.json) to out. Diagnostics go to err.
// Returns 0 (converted) / 1 (invalid input) / 2 (invalid args).
//
// fusa:req REQ-CLI-003
int cmd_convert(const std::vector<std::string>& args, std::istream& in,
                 std::ostream& out, std::ostream& err);

// dispatch implements cpp-dds's full subcommand dispatch (version/
// capabilities/status/conform/convert/unknown), matching main()'s
// behavior exactly; main() is a thin wrapper around this function. `in`
// is only read by the `convert` subcommand.
int dispatch(const std::vector<std::string>& args, std::istream& in,
             std::ostream& out, std::ostream& err);

} // namespace cli
