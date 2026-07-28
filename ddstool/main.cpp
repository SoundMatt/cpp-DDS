// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// ddstool — IDL-compiler command-line tool for cpp-DDS. See cli.hpp/cli.cpp
// for the actual subcommand dispatch/`idl` implementation (factored out so
// tests/test_idl.cpp can exercise it in-process); this file is just the
// process entry point.

#include "cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return ddstool::dispatch(args, std::cout, std::cerr);
}
