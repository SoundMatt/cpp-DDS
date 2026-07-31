// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// cpp-dds — RELAY-conformant DDS CLI tool.
//
// Mandatory subcommands (§11.1):
//   version [--format text|json]    Print version and spec information.
//   capabilities                    Emit capabilities document as JSON.
//   status [--format text|json]     Self-assessed health status.
//
// Optional subcommands (§11.2):
//   conform                          Basic RELAY conformance self-check.
//   convert --protocol P [--format json]
//     Reads one dds.Sample value as JSON on stdin (spec/schemas/dds-sample.json),
//     runs it through Sample::to_message(), and writes the resulting
//     relay.Message as JSON on stdout. This is the §11.2 black-box driver
//     surface used by `relay interop` / `relay convert --protocol DDS`.
//
// See cpp_dds_cli.hpp/cpp_dds_cli.cpp for the actual subcommand
// dispatch/implementations (factored out so tests/test_cli_main.cpp can
// exercise them in-process, matching ddstool/main.cpp's identical
// precedent); this file is just the process entry point.

#include "cpp_dds_cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return cppdds_cli::dispatch(args, std::cin, std::cout, std::cerr);
}
