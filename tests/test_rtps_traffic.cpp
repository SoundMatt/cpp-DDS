// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::rtps's platform socket-tuning hooks (rtps/traffic.hpp).
// Not a wire-format/byte-exact suite (these are socket options and clock
// reads, not encoded data on the wire) — these tests assert the API
// behaves sanely on whichever platform CI happens to run on: real effect
// on Linux where the underlying kernel feature is available, and a
// well-defined no-op/fallback everywhere else. Mirrors go-DDS's own
// traffic_linux.go / traffic_other.go split (see
// include/dds/rtps/traffic.hpp for the file-split rationale).

#include <catch2/catch_test_macros.hpp>

#include <dds/rtps/traffic.hpp>
#include <dds/rtps/transport.hpp>

using namespace dds::rtps;

TEST_CASE("clock_tai_now always produces a plausible timestamp", "[rtps][traffic]") {
    uint64_t ns = 0;
    // Return value is platform-dependent (true on Linux with CLOCK_TAI
    // available, false as a documented fallback elsewhere) — only the
    // output value is asserted here.
    (void)clock_tai_now(ns);
    // Sanity bound: some time after 2020-01-01 in nanoseconds-since-epoch.
    CHECK(ns > 1'577'836'800ull * 1'000'000'000ull);
}

TEST_CASE("set_sock_priority / set_sock_tos / enable_tx_time do not crash on any platform",
          "[rtps][traffic]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());

    // No behavioral assertion on the boolean result: true (applied) on
    // Linux where the kernel feature exists, false (no-op) elsewhere —
    // both are correct per-platform outcomes documented in traffic.hpp.
    (void)set_sock_priority(*sock, 3);
    (void)set_sock_tos(*sock, 46); // EF (Expedited Forwarding) DSCP
    (void)enable_tx_time(*sock);
}

TEST_CASE("scheduled_send with tx_time_ns == 0 behaves like a plain send", "[rtps][traffic]") {
    auto server = UdpSocket::bind_unicast(0);
    auto client = UdpSocket::bind_unicast(0);
    REQUIRE(server.has_value());
    REQUIRE(client.has_value());

    const std::vector<uint8_t> payload{0xAA, 0xBB};
    CHECK(scheduled_send(*client, "127.0.0.1", server->port(), payload.data(), payload.size(),
                          /*tx_time_ns=*/0));

    std::optional<UdpPacket> received;
    for (int attempt = 0; attempt < 8 && !received.has_value(); ++attempt) {
        received = server->recv();
    }
    REQUIRE(received.has_value());
    CHECK(received->data == payload);
}

TEST_CASE("scheduled_send on an invalid socket fails cleanly", "[rtps][traffic]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());
    sock->close();

    const std::vector<uint8_t> payload{0x01};
    CHECK_FALSE(scheduled_send(*sock, "127.0.0.1", 7400, payload.data(), payload.size(), 0));
}
