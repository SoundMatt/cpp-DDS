// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// cli/cpp_dds_cli.hpp — testable entry points for the cpp-dds command-line
// tool, factored out of main.cpp so tests/test_cli_main.cpp can exercise
// the real argument-parsing/exit-code/output behavior directly (in-process,
// no subprocess spawn), matching the exact precedent already established
// for ddstool (see ddstool/cli.hpp's identical doc comment) and closing the
// REQ-CLI-001/REQ-CLI-002/REQ-CLI-003 test-coverage gap tracked by
// REQ-SAFETY-004 (requirements traceability — every fusa:req'd requirement
// must also carry a fusa:test).
//
// Named `cppdds_cli` rather than reusing the bare `cli` namespace: this
// directory's json_lite.hpp/base64.hpp already occupy `cli::json`/
// `cli::base64`. File itself is named cpp_dds_cli.hpp rather than the more
// obvious cli.hpp because ddstool/cli.hpp is a distinct header of the same
// bare name, and both cli/ and ddstool/ end up on the same include path
// for tests/test_cli_main.cpp (which also needs ddstool's), so `"cli.hpp"`
// would resolve ambiguously (silently picking whichever -I entry comes
// first) rather than failing loudly.

#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace cppdds_cli {

int cmd_version(const std::string& fmt, std::ostream& out);

int cmd_capabilities(std::ostream& out);

int cmd_status(const std::string& fmt, std::ostream& out);

int cmd_conform(std::ostream& out, std::ostream& err);

// args holds every argument after "convert" (i.e. no "cpp-dds"/"convert"
// prefix). Reads the dds.Sample JSON input from in_stream rather than
// std::cin directly so tests can supply arbitrary input without touching
// real process stdin.
int cmd_convert(const std::vector<std::string>& args, std::istream& in_stream, std::ostream& out,
                 std::ostream& err);

// dispatch implements cpp-dds's full subcommand dispatch (version/
// capabilities/status/conform/convert/unknown), matching main()'s behavior
// exactly; main() is a thin wrapper around this function. args holds every
// argument after the program name (args[0] is the subcommand, if any).
int dispatch(const std::vector<std::string>& args, std::istream& in_stream, std::ostream& out, std::ostream& err);

} // namespace cppdds_cli
