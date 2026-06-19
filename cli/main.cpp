// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// cpp-dds — RELAY-conformant DDS CLI tool.
//
// Subcommands:
//   version    Print version and spec information.
//   conform    Basic RELAY conformance self-check.
//   convert    Parse a dds-sample JSON vector and echo it back.

#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

static constexpr const char* kVersion    = "0.1.0";
static constexpr const char* kSpecVersion = dds::kSpecVersion;

// ── subcommands ───────────────────────────────────────────────────────────────

static int cmd_version() {
    std::cout << "cpp-dds " << kVersion << "\n"
              << "relay-spec: " << kSpecVersion << "\n"
              << "protocol: DDS\n";
    return 0;
}

static int cmd_conform() {
    // Self-check: create a mock participant, publish, and receive a sample.
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

    // Validate domain boundary.
    if (auto ec_d = dds::validate_domain(232); ec_d) {
        std::cerr << "conform: validate_domain(232) failed: " << ec_d.message() << "\n";
        return 1;
    }
    if (auto ec_d = dds::validate_domain(233); !ec_d) {
        std::cerr << "conform: validate_domain(233) should have failed\n";
        return 1;
    }

    // Test adapt().
    auto node = dds::adapt(p);
    if (node->protocol() != relay::Protocol::DDS) {
        std::cerr << "conform: adapt node protocol mismatch\n";
        return 1;
    }

    std::cout << "PASS conform\n";
    return 0;
}

static int cmd_convert(const std::string& topic, const std::string& hex_payload) {
    dds::Sample s;
    s.topic     = topic;
    s.timestamp = std::chrono::system_clock::now();

    // Parse hex payload (pairs of hex digits).
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
        std::cerr << "Usage: cpp-dds <version|conform|convert> [args...]\n";
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "version") {
        return cmd_version();
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
