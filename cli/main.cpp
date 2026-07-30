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

// fusa:req REQ-CLI-001 REQ-CLI-002 REQ-CLI-003

#include "base64.hpp"
#include "json_lite.hpp"
#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

static constexpr const char* kTool        = "cpp-dds";
static constexpr const char* kVersion     = "0.2.0";
static constexpr const char* kSpecVersion = dds::kSpecVersion;
static constexpr const char* kLanguage    = "cpp";
static constexpr const char* kRuntime     = "c++17";
static constexpr int         kProtocolInt = 2;  // DDS
static constexpr const char* kProtocol    = "DDS";

// ── helper ────────────────────────────────────────────────────────────────────

static std::string format_flag(int argc, char* argv[], int from) {
    for (int i = from; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--format")
            return argv[i + 1];
    }
    return "text";
}

// ── §12.1 version ─────────────────────────────────────────────────────────────

// fusa:req REQ-CLI-001
static int cmd_version(const std::string& fmt) {
    if (fmt == "json") {
        std::cout << "{\n"
                  << "    \"tool\":         \"" << kTool        << "\",\n"
                  << "    \"protocol\":     \"" << kProtocol    << "\",\n"
                  << "    \"protocol_int\": "   << kProtocolInt << ",\n"
                  << "    \"version\":      \"" << kVersion     << "\",\n"
                  << "    \"spec_version\": \"" << kSpecVersion << "\",\n"
                  << "    \"language\":     \"" << kLanguage    << "\",\n"
                  << "    \"runtime\":      \"" << kRuntime     << "\"\n"
                  << "}\n";
    } else {
        std::cout << kTool << " " << kVersion << "\n"
                  << "relay-spec: " << kSpecVersion << "\n"
                  << "protocol: " << kProtocol << "\n";
    }
    return 0;
}

// ── §12.2 capabilities ────────────────────────────────────────────────────────

static int cmd_capabilities() {
    std::cout << "{\n"
              << "    \"kind\":                \"capabilities\",\n"
              << "    \"tool\":                \"" << kTool        << "\",\n"
              << "    \"protocol\":            \"" << kProtocol    << "\",\n"
              << "    \"protocol_int\":        "   << kProtocolInt << ",\n"
              << "    \"version\":             \"" << kVersion     << "\",\n"
              << "    \"spec_version\":        \"" << kSpecVersion << "\",\n"
              << "    \"commands\":            [\"version\", \"capabilities\", \"status\", \"conform\", \"convert\"],\n"
              << "    \"transports\":          [\"mock\", \"rtps\"],\n"
              << "    \"features\":            [\"loaning\", \"tsn\"],\n"
              << "    \"interfaces\":          [\"IParticipant\", \"IPublisher\", \"ISubscriber\"],\n"
              << "    \"optional_interfaces\": [\"IMetricsProvider\", \"IHealthProvider\", \"IDrainer\"],\n"
              << "    \"adapt\":               true\n"
              << "}\n";
    return 0;
}

// ── §12.3 status ──────────────────────────────────────────────────────────────

static int cmd_status(const std::string& fmt) {
    if (fmt == "json") {
        std::cout << "{\n"
                  << "    \"protocol\":  \"" << kProtocol << "\",\n"
                  << "    \"tool\":      \"" << kTool     << "\",\n"
                  << "    \"version\":   \"" << kVersion  << "\",\n"
                  << "    \"healthy\":   true,\n"
                  << "    \"connected\": false,\n"
                  << "    \"endpoint\":  \"\",\n"
                  << "    \"details\":   {}\n"
                  << "}\n";
    } else {
        std::cout << "protocol:  " << kProtocol << "\n"
                  << "version:   " << kVersion  << "\n"
                  << "healthy:   true\n"
                  << "connected: false\n";
    }
    return 0;  // 0 = healthy
}

// ── conform (optional) ────────────────────────────────────────────────────────

// fusa:req REQ-CLI-002
static int cmd_conform() {
    auto [p, ec] = dds::mock::create(0);
    if (ec) {
        std::cerr << "conform: create participant failed: " << ec.message() << "\n";
        return 1;
    }

    auto [sub, ec_sub] = p->new_subscriber("conform/topic", dds::default_qos());
    if (ec_sub) {
        std::cerr << "conform: new_subscriber failed: " << ec_sub.message() << "\n";
        return 1;
    }

    auto [pub, ec_pub] = p->new_publisher("conform/topic", dds::default_qos());
    if (ec_pub) {
        std::cerr << "conform: new_publisher failed: " << ec_pub.message() << "\n";
        return 1;
    }

    std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    if (auto wr = pub->write(payload); wr) {
        std::cerr << "conform: write failed: " << wr.message() << "\n";
        return 1;
    }

    auto sample = sub->channel()->recv();
    if (!sample || sample->payload != payload) {
        std::cerr << "conform: unexpected sample\n";
        return 1;
    }

    if (auto ec_d = dds::validate_domain(232); ec_d) {
        std::cerr << "conform: validate_domain(232) failed: " << ec_d.message() << "\n";
        return 1;
    }
    if (auto ec_d = dds::validate_domain(233); !ec_d) {
        std::cerr << "conform: validate_domain(233) should have failed\n";
        return 1;
    }

    auto node = dds::adapt(p);
    if (node->protocol() != relay::Protocol::DDS) {
        std::cerr << "conform: adapt node protocol mismatch\n";
        return 1;
    }

    std::cout << "PASS conform\n";
    return 0;
}

// ── convert (optional, §11.2) ─────────────────────────────────────────────────

// looks_like_rfc3339 is a lenient structural check ("YYYY-MM-DDTHH:MM:SS...")
// — not a full calendar/leap-second validator. cpp-dds does not need the
// input timestamp's exact value: per §11.2, "timestamp MAY be zeroed in the
// output to keep results comparable", and the RELAY reference `convert`
// (cmd/relay/convert.go) always zeroes it. cmd_convert mirrors that.
static bool looks_like_rfc3339(const std::string& s) {
    if (s.size() < 20) return false;
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    // "YYYY-MM-DDTHH:MM:SS" fixed-width prefix, then optional fraction, then
    // 'Z' or a "+HH:MM" / "-HH:MM" offset.
    static constexpr int kDigitPos[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int p : kDigitPos) if (!is_digit(s[static_cast<std::size_t>(p)])) return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;
    std::size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        ++i;
        std::size_t start = i;
        while (i < s.size() && is_digit(s[i])) ++i;
        if (i == start) return false;
    }
    if (i >= s.size()) return false;
    if (s[i] == 'Z') return i + 1 == s.size();
    if (s[i] == '+' || s[i] == '-') {
        return s.size() == i + 6 && is_digit(s[i + 1]) && is_digit(s[i + 2]) &&
               s[i + 3] == ':' && is_digit(s[i + 4]) && is_digit(s[i + 5]);
    }
    return false;
}

// parse_dds_sample decodes a dds.Sample JSON value (spec/schemas/dds-sample.json)
// into dds::Sample. Throws cli::json::ParseError on any schema violation.
static dds::Sample parse_dds_sample(const cli::json::Value& v) {
    if (!v.is_object()) throw cli::json::ParseError("dds.Sample: expected a JSON object");

    dds::Sample s;

    const auto* topic = v.find("topic");
    if (!topic) throw cli::json::ParseError("dds.Sample: missing required field \"topic\"");
    s.topic = topic->as_string();

    const auto* payload = v.find("payload");
    if (!payload) throw cli::json::ParseError("dds.Sample: missing required field \"payload\"");
    auto decoded = cli::base64::decode(payload->as_string());
    if (!decoded) throw cli::json::ParseError("dds.Sample: \"payload\" is not valid base64");
    s.payload = std::move(*decoded);

    const auto* timestamp = v.find("timestamp");
    if (!timestamp) throw cli::json::ParseError("dds.Sample: missing required field \"timestamp\"");
    if (!looks_like_rfc3339(timestamp->as_string()))
        throw cli::json::ParseError("dds.Sample: \"timestamp\" is not a valid RFC 3339 date-time");
    s.timestamp = std::chrono::system_clock::time_point{}; // zeroed — see cmd_convert.

    const auto* seq = v.find("seq");
    if (!seq) throw cli::json::ParseError("dds.Sample: missing required field \"seq\"");
    double seq_n = seq->as_number();
    if (seq_n < 0) throw cli::json::ParseError("dds.Sample: \"seq\" must not be negative");
    s.sequence_number = static_cast<uint64_t>(seq_n);

    const auto* guid = v.find("writer_guid");
    if (!guid) throw cli::json::ParseError("dds.Sample: missing required field \"writer_guid\"");
    const auto& items = guid->as_array();
    if (items.size() != 16)
        throw cli::json::ParseError("dds.Sample: \"writer_guid\" must have exactly 16 elements");
    for (std::size_t i = 0; i < 16; ++i) {
        double b = items[i].as_number();
        if (b < 0 || b > 255)
            throw cli::json::ParseError("dds.Sample: \"writer_guid\" elements must be 0-255");
        s.writer_guid[i] = static_cast<uint8_t>(b);
    }

    return s;
}

// write_relay_message_json serialises a relay::Message per
// spec/schemas/relay-message.json. The timestamp field is always emitted as
// the Go zero-value string ("0001-01-01T00:00:00Z") to match the RELAY
// reference `convert`'s cross-implementation normalisation (§11.2).
static void write_relay_message_json(std::ostream& out, const relay::Message& m) {
    out << "{\n"
        << "    \"protocol\": " << static_cast<int>(m.protocol) << ",\n"
        << "    \"version\": {\n"
        << "        \"major\": " << m.version.major << ",\n"
        << "        \"minor\": " << m.version.minor << ",\n"
        << "        \"patch\": " << m.version.patch << "\n"
        << "    },\n"
        << "    \"id\": " << cli::json::escape(m.id) << ",\n"
        << "    \"payload\": " << cli::json::escape(cli::base64::encode(m.payload)) << ",\n"
        << "    \"timestamp\": \"0001-01-01T00:00:00Z\",\n"
        << "    \"seq\": " << m.seq << ",\n"
        << "    \"meta\": {";
    bool first = true;
    for (const auto& [k, val] : m.meta) {
        out << (first ? "\n" : ",\n")
            << "        " << cli::json::escape(k) << ": " << cli::json::escape(val);
        first = false;
    }
    out << (first ? "}\n" : "\n    }\n");
    out << "}\n";
}

// fusa:req REQ-CLI-003
static int cmd_convert(int argc, char* argv[]) {
    std::optional<std::string> protocol;
    std::string                fmt = "json";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--protocol" && i + 1 < argc) {
            protocol = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            fmt = argv[++i];
        } else {
            std::cerr << "convert: unrecognized argument: " << arg << "\n";
            return 2;
        }
    }

    if (!protocol) {
        std::cerr << "convert: --protocol is required\n";
        return 2;
    }
    if (fmt != "json") {
        std::cerr << "convert: unsupported format \"" << fmt << "\"\n";
        return 2;
    }

    std::string upper_protocol;
    for (char c : *protocol) upper_protocol.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (upper_protocol != kProtocol) {
        std::cerr << "convert: protocol \"" << *protocol << "\" not supported by cpp-dds (want DDS)\n";
        return 1;
    }

    std::ostringstream stdin_buf;
    stdin_buf << std::cin.rdbuf();
    std::string input = stdin_buf.str();

    try {
        auto value  = cli::json::parse(input);
        auto sample = parse_dds_sample(value);
        auto msg    = sample.to_message();
        write_relay_message_json(std::cout, msg);
        return 0;
    } catch (const cli::json::ParseError& e) {
        std::cerr << "convert: invalid input: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "convert: invalid input: " << e.what() << "\n";
        return 1;
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: cpp-dds <version|capabilities|status|conform|convert> [args...]\n"
                     "  convert --protocol P [--format json]   (reads dds.Sample JSON on stdin)\n";
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "version") {
        return cmd_version(format_flag(argc, argv, 2));
    }
    if (cmd == "capabilities") {
        return cmd_capabilities();
    }
    if (cmd == "status") {
        return cmd_status(format_flag(argc, argv, 2));
    }
    if (cmd == "conform") {
        return cmd_conform();
    }
    if (cmd == "convert") {
        return cmd_convert(argc, argv);
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    return 1;
}
