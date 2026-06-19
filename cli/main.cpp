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
//   conform    Basic RELAY conformance self-check.
//   convert    Parse a dds-sample JSON vector and echo it back.

// fusa:req REQ-CLI-001 REQ-CLI-002 REQ-CLI-003

#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

static constexpr const char* kTool        = "cpp-dds";
static constexpr const char* kVersion     = "0.1.0";
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
              << "    \"transports\":          [\"mock\"],\n"
              << "    \"features\":            [\"loaning\"],\n"
              << "    \"interfaces\":          [\"IParticipant\", \"IPublisher\", \"ISubscriber\"],\n"
              << "    \"optional_interfaces\": [\"IMetricsProvider\", \"IHealthProvider\", \"IDrainer\", \"ILoaningPublisher\"],\n"
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

// ── convert (optional) ────────────────────────────────────────────────────────

// fusa:req REQ-CLI-003
static int cmd_convert(const std::string& topic, const std::string& hex_payload) {
    dds::Sample s;
    s.topic     = topic;
    s.timestamp = std::chrono::system_clock::now();

    if (hex_payload.size() % 2 != 0) {
        std::cerr << "convert: payload must be an even-length hex string\n";
        return 1;
    }
    for (std::size_t i = 0; i < hex_payload.size(); i += 2) {
        auto byte_str = hex_payload.substr(i, 2);
        try {
            s.payload.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
        } catch (...) {
            std::cerr << "convert: invalid hex byte: " << byte_str << "\n";
            return 1;
        }
    }

    auto m = s.to_message();
    std::cout << "protocol: " << relay::to_string(m.protocol) << "\n"
              << "id: "       << m.id << "\n"
              << "seq: "      << m.seq << "\n"
              << "payload_len: " << m.payload.size() << "\n";
    return 0;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: cpp-dds <version|capabilities|status|conform|convert> [args...]\n";
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
        std::string topic   = argc >= 3 ? argv[2] : "test/topic";
        std::string payload = argc >= 4 ? argv[3] : "DEADBEEF";
        return cmd_convert(topic, payload);
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    return 1;
}
