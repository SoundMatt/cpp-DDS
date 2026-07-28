// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// ddstool/cli.hpp — testable entry points for the ddstool command-line
// tool, factored out of main.cpp so tests/test_idl.cpp can exercise the
// real argument-parsing/exit-code behavior directly (in-process, no
// subprocess spawn) rather than only asserting on dds::idl library calls.

#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace ddstool {

// print_usage writes ddstool's top-level usage text to out.
void print_usage(std::ostream& out);

// run_idl implements the `idl` subcommand: parses args (everything after
// "ddstool idl"), reads/parses the named .idl file, generates C++ source,
// and writes it to out_stream (no -out flag) or the file named by -out.
// Diagnostics go to err_stream. Returns the process exit code (0 on
// success, 1 on any failure).
int run_idl(const std::vector<std::string>& args, std::ostream& out_stream,
            std::ostream& err_stream);

// dispatch implements ddstool's full subcommand dispatch (idl/help/
// unknown), matching main()'s behavior exactly; main() is a thin wrapper
// around this function.
int dispatch(const std::vector<std::string>& args, std::ostream& out_stream,
             std::ostream& err_stream);

} // namespace ddstool
