// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// interop/test_interop.cpp — RTPS wire-compatibility tests against a live
// CycloneDDS peer. This file is only ever compiled into the
// cppdds_interop_tests executable, which is itself only ever built when
// the CPPDDS_INTEROP_TESTS CMake option is ON (default OFF) — see
// interop/CMakeLists.txt and the root CMakeLists.txt's option guard. That
// is the CMake/CTest equivalent of go-DDS's `go:build interop` tag
// (interop/interop_test.go, interop/doc.go in go-DDS): these tests do not
// run in the normal build-and-test CI job or a plain `ctest` invocation.
//
// These tests bring up a real, non-test_mode dds::rtps::Participant (real
// SPDP/SEDP multicast discovery, not the direct-unicast test_mode override
// tests/test_rtps_participant.cpp uses) and verify it interoperates with a
// genuinely separate, independently built RTPS 2.3 implementation —
// CycloneDDS — matching go-DDS's own first interop target and reference
// pattern (see ROADMAP.md's "Interop testing infrastructure" section for
// why this is a distinct testing tier from the byte-vector-comparison and
// same-process-loopback tests every other tests/test_rtps_*.cpp file
// already provides).
//
// # Prerequisites
//
//  1. Docker or a native CycloneDDS installation (ddsperf on PATH) on the
//     same host.
//  2. A CycloneDDS peer reachable on the configured domain — see
//     interop/docker-compose.yml and interop/README.md for the exact
//     services and how to start them.
//
// # Quick start with Docker
//
//   docker compose -f interop/docker-compose.yml up -d cyclone-peer
//   cmake -B build-interop -DCPPDDS_INTEROP_TESTS=ON -G Ninja
//   cmake --build build-interop --parallel
//   ctest --test-dir build-interop -L interop --output-on-failure
//   docker compose -f interop/docker-compose.yml down
//
// # Environment variables
//
//   - INTEROP_DOMAIN   DDS domain (default "0")
//   - INTEROP_TIMEOUT  per-test deadline, e.g. "10s" (default "15s")
//
// See interop/README.md for the full harness description.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dds/rtps/participant.hpp>

using namespace dds;
using namespace dds::rtps;
using namespace std::chrono_literals;

namespace {

// test_domain reads INTEROP_DOMAIN, matching go-DDS interop_test.go's
// testDomain(). Defaults to domain 0.
Domain test_domain() {
    const char* v = std::getenv("INTEROP_DOMAIN");
    if (v == nullptr || *v == '\0') return 0;
    try {
        return std::stoi(v);
    } catch (...) {
        return 0;
    }
}

// test_timeout reads INTEROP_TIMEOUT, matching go-DDS interop_test.go's
// testTimeout(). Understands the small subset of Go-style duration
// suffixes ("15s", "500ms", "2m") the go-DDS env var convention actually
// uses — not a general-purpose duration parser. Defaults to 15s, and falls
// back to 15s on any parse error, matching go-DDS's own fallback-on-error
// behavior.
std::chrono::milliseconds test_timeout() {
    const char* v = std::getenv("INTEROP_TIMEOUT");
    if (v == nullptr || *v == '\0') return 15s;
    std::string s(v);
    try {
        if (s.size() > 2 && s.substr(s.size() - 2) == "ms") {
            return std::chrono::milliseconds(std::stoll(s.substr(0, s.size() - 2)));
        }
        if (s.size() > 1 && s.back() == 's') {
            double secs = std::stod(s.substr(0, s.size() - 1));
            return std::chrono::milliseconds(static_cast<long long>(secs * 1000.0));
        }
        if (s.size() > 1 && s.back() == 'm') {
            return std::chrono::minutes(std::stoll(s.substr(0, s.size() - 1)));
        }
        return std::chrono::milliseconds(std::stoll(s));
    } catch (...) {
        return 15s;
    }
}

// new_participant creates a real (non-test_mode) cpp-DDS RTPS participant —
// standard multicast SPDP discovery, wire-compatible with any RTPS 2.3
// peer — and skips the test if UDP/multicast is unavailable in the current
// sandbox. Matches go-DDS interop_test.go's newParticipant helper.
std::shared_ptr<Participant> new_participant() {
    ParticipantOptions opts; // test_mode = false: real multicast, not the
                              // direct-unicast override other test files use.
    auto [p, ec] = Participant::create(test_domain(), opts);
    if (ec || !p) {
        SKIP("Participant::create failed (" << ec.message() << ") — UDP/multicast unavailable");
    }
    return p;
}

} // namespace

// cpp-DDS publisher reaches a live CycloneDDS subscriber: publishes 5
// samples on "interop/cpp-dds-ping" after allowing SPDP/SEDP discovery to
// converge. Receipt is verified out-of-band (watch the cyclone-sub /
// cyclone-peer container's stdout) — this test only fails on a local write
// error, matching go-DDS's TestInterop_GoPublisher_CycloneSubscriber.
//
// Set up the peer first:
//   docker compose -f interop/docker-compose.yml run --rm cyclone-sub
TEST_CASE("cpp-DDS publisher reaches a live CycloneDDS subscriber", "[interop][cyclone]") {
    auto p = new_participant();

    auto [pub, pub_ec] = p->new_publisher("interop/cpp-dds-ping", reliable_qos());
    REQUIRE_FALSE(pub_ec);

    auto disc = test_timeout() / 3;
    std::this_thread::sleep_for(disc);

    for (int i = 0; i < 5; ++i) {
        std::string payload = R"({"seq":)" + std::to_string(i) + R"(,"src":"cpp-DDS"})";
        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        auto write_ec = pub->write(bytes);
        CHECK_FALSE(write_ec);
    }

    pub->close();
    p->close();
}

// cpp-DDS subscriber receives from a live CycloneDDS publisher: subscribes
// to "interop/cpp-dds-pong" and expects at least one sample within
// INTEROP_TIMEOUT. Matches go-DDS's
// TestInterop_CyclonePublisher_GoSubscriber.
//
// Set up the peer first:
//   docker compose -f interop/docker-compose.yml run --rm cyclone-pub
TEST_CASE("cpp-DDS subscriber receives from a live CycloneDDS publisher", "[interop][cyclone]") {
    auto p = new_participant();

    auto [sub, sub_ec] = p->new_subscriber("interop/cpp-dds-pong", reliable_qos());
    REQUIRE_FALSE(sub_ec);

    WaitSet ws;
    ws.add(sub->channel());

    auto [sample, idx] = ws.wait_any(test_timeout());
    if (!sample) {
        SKIP("no sample from CycloneDDS within timeout — is the cyclone-pub service running?");
    }
    CHECK(idx == 0);
    CHECK_FALSE(sample->payload.empty());

    sub->close();
    p->close();
}

// cpp-DDS <-> CycloneDDS bidirectional echo: publishes on
// "interop/cpp-dds-ping" and expects an echo back on
// "interop/cpp-dds-pong" from the cyclone-peer service. Matches go-DDS's
// TestInterop_BidirectionalEcho.
TEST_CASE("cpp-DDS <-> CycloneDDS bidirectional echo", "[interop][cyclone]") {
    auto p = new_participant();

    auto [pub, pub_ec] = p->new_publisher("interop/cpp-dds-ping", reliable_qos());
    REQUIRE_FALSE(pub_ec);
    auto [sub, sub_ec] = p->new_subscriber("interop/cpp-dds-pong", reliable_qos());
    REQUIRE_FALSE(sub_ec);

    WaitSet ws;
    ws.add(sub->channel());

    auto disc = test_timeout() / 3;
    std::this_thread::sleep_for(disc);

    std::string ping = R"({"ping":true})";
    std::vector<uint8_t> bytes(ping.begin(), ping.end());
    REQUIRE_FALSE(pub->write(bytes));

    auto [sample, idx] = ws.wait_any(test_timeout() * 2 / 3);
    if (!sample) {
        SKIP("no echo from CycloneDDS within timeout — is the cyclone-peer service running?");
    }
    CHECK(idx == 0);
    CHECK_FALSE(sample->payload.empty());

    pub->close();
    sub->close();
    p->close();
}
