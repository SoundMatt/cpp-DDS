// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// ddstool/cli.cpp — implementation of ddstool's subcommand dispatch and
// `idl` subcommand. See cli.hpp for why this is a separate translation
// unit from main.cpp.
//
// C++ port of go-DDS's `cmd/ddstool` binary's `idl` subcommand (the only
// subcommand within this roadmap item's scope -- go-DDS's `pub`/`sub`/
// `discover` subcommands are out of scope here; cpp-DDS already has its
// own `cpp-dds` RELAY-conformance CLI under cli/ for protocol-level
// concerns. This is a deliberate, documented scope difference from the
// go-DDS reference `ddstool`, matching every prior Tier-3 internal-library
// port's own precedent of landing a scoped subset first.
//
// Usage:
//   ddstool idl [-out <file>] [-namespace <name>] <input.idl>
//   ddstool help

// fusa:req REQ-IDL-009

#include "cli.hpp"

#include "dds/idl/idl.hpp"

#include <fstream>
#include <sstream>

namespace ddstool {

void print_usage(std::ostream& out) {
    out <<
        "ddstool - cpp-DDS IDL-compiler command-line tool\n"
        "\n"
        "USAGE\n"
        "  ddstool <subcommand> [flags]\n"
        "\n"
        "SUBCOMMANDS\n"
        "  idl       Compile an IDL file to a C++ header\n"
        "  help      Show this message\n"
        "\n"
        "Run 'ddstool idl -h' for idl-subcommand flags.\n";
}

namespace {

void print_idl_usage(std::ostream& err) {
    err << "idl: usage: ddstool idl [-out <file>] [-namespace <name>] <input.idl>\n";
}

} // namespace

int run_idl(const std::vector<std::string>& args, std::ostream& out_stream,
            std::ostream& err_stream) {
    std::string out_path;
    std::string ns_override;
    std::string input;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help") {
            print_idl_usage(err_stream);
            return 0;
        }
        if (a == "-out") {
            if (i + 1 >= args.size()) {
                print_idl_usage(err_stream);
                return 1;
            }
            out_path = args[++i];
        } else if (a == "-namespace") {
            if (i + 1 >= args.size()) {
                print_idl_usage(err_stream);
                return 1;
            }
            ns_override = args[++i];
        } else if (!a.empty() && a[0] == '-') {
            err_stream << "idl: unknown flag " << a << "\n";
            print_idl_usage(err_stream);
            return 1;
        } else {
            input = a;
        }
    }
    if (input.empty()) {
        print_idl_usage(err_stream);
        return 1;
    }

    dds::idl::ParseResult pr = dds::idl::parse_file(input);
    if (!pr.ok()) {
        err_stream << "idl: parse " << input << ": " << pr.error->message << "\n";
        return 1;
    }
    if (!ns_override.empty()) {
        pr.module->name = ns_override;
    }

    dds::idl::GenerateResult gr = dds::idl::generate(*pr.module);
    if (!gr.ok()) {
        err_stream << "idl: generate: " << *gr.error << "\n";
        return 1;
    }

    if (out_path.empty()) {
        out_stream << *gr.source;
        return 0;
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err_stream << "idl: write " << out_path << ": could not open file for writing\n";
        return 1;
    }
    out << *gr.source;
    if (!out) {
        err_stream << "idl: write " << out_path << ": write failed\n";
        return 1;
    }
    err_stream << "idl: wrote " << out_path << "\n";
    return 0;
}

int dispatch(const std::vector<std::string>& args, std::ostream& out_stream,
             std::ostream& err_stream) {
    if (args.empty()) {
        print_usage(err_stream);
        return 1;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "idl") {
        return run_idl(rest, out_stream, err_stream);
    }
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        print_usage(out_stream);
        return 0;
    }
    err_stream << "ddstool: unknown subcommand \"" << cmd << "\"\n";
    print_usage(err_stream);
    return 1;
}

} // namespace ddstool
