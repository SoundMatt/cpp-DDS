// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// cpp-dds — RELAY-conformant DDS CLI tool. See cpp_dds_cli.hpp/
// cpp_dds_cli.cpp for the actual subcommand dispatch/implementation
// (factored out so tests/test_cpp_dds_cli.cpp can exercise it in-process,
// matching ddstool/main.cpp's own precedent); this file is just the
// process entry point.

#include "cpp_dds_cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return cli::dispatch(args, std::cin, std::cout, std::cerr);
}
