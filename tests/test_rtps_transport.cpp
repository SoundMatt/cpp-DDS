// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::rtps::UdpSocket and the RTPS 2.3 §9.6.1 port-assignment
// formula — the correctness oracle named in cpp-DDS's ROADMAP.md ("Tier 1
// — RTPS wire protocol", phase 3: "UDP transport").
//
// ── How every reference value below was derived ─────────────────────────────
//
// The port formula is exact integer arithmetic (no encoding/serialization
// involved), so "byte-exact" here means "identical port numbers for
// identical (domain, participant_idx) inputs." Some of these values are
// already asserted directly against go-DDS's real functions in go-DDS's
// own rtps/wire_test.go (TestPortFormula: metaMulticastPort(0)==7400,
// metaUnicastPort(0,0)==7410, userUnicastPort(0,0)==7411,
// metaMulticastPort(99)==32150, metaUnicastPort(99,0)==32160,
// userUnicastPort(99,0)==32161) — that test already exists in the go-DDS
// repository as committed source, so those six values are cited directly
// from it rather than re-derived.
//
// The remaining values (domain=1 with a non-zero participant index, and
// domain=7/idx=3, plus userMulticastPort and the multicast group address
// strings) were produced the same way as phase 1/2's reference vectors:
// by calling go-DDS's actual functions white-box, from a scratch _test.go
// file placed temporarily inside a fresh clone of go-DDS's rtps package
// (never committed to go-DDS, never pushed anywhere):
//
//   git clone https://github.com/SoundMatt/go-DDS.git
//   cd go-DDS
//   cat > rtps/zzz_scratch_transport_vectors_test.go <<'EOF'
//   package rtps
//
//   import (
//       "fmt"
//       "testing"
//   )
//
//   func TestPrintCppDDSTransportReferenceVectors(t *testing.T) {
//       fmt.Println("META_MULTICAST_0  =", metaMulticastPort(0))
//       fmt.Println("META_UNICAST_0_0  =", metaUnicastPort(0, 0))
//       fmt.Println("USER_UNICAST_0_0  =", userUnicastPort(0, 0))
//       fmt.Println("USER_MCAST_0      =", userMulticastPort(0))
//       fmt.Println("META_MULTICAST_1  =", metaMulticastPort(1))
//       fmt.Println("META_UNICAST_1_1  =", metaUnicastPort(1, 1))
//       fmt.Println("USER_UNICAST_1_1  =", userUnicastPort(1, 1))
//       fmt.Println("USER_MCAST_1      =", userMulticastPort(1))
//       fmt.Println("META_MULTICAST_99 =", metaMulticastPort(99))
//       fmt.Println("META_UNICAST_99_0 =", metaUnicastPort(99, 0))
//       fmt.Println("USER_UNICAST_99_0 =", userUnicastPort(99, 0))
//       fmt.Println("META_UNICAST_7_3  =", metaUnicastPort(7, 3))
//       fmt.Println("USER_UNICAST_7_3  =", userUnicastPort(7, 3))
//       fmt.Println("SPDP_MCAST_ADDR   =", spdpMulticastAddr.String())
//       fmt.Println("USER_MCAST_ADDR   =", userDataMulticastAddr.String())
//   }
//   EOF
//   go test ./rtps -run TestPrintCppDDSTransportReferenceVectors -v
//
// That produced (go-DDS main @ time of writing, module github.com/SoundMatt/go-DDS):
//
//   META_MULTICAST_0  = 7400
//   META_UNICAST_0_0  = 7410
//   USER_UNICAST_0_0  = 7411
//   USER_MCAST_0      = 7401
//   META_MULTICAST_1  = 7650
//   META_UNICAST_1_1  = 7662
//   USER_UNICAST_1_1  = 7663
//   USER_MCAST_1      = 7651
//   META_MULTICAST_99 = 32150
//   META_UNICAST_99_0 = 32160
//   USER_UNICAST_99_0 = 32161
//   META_UNICAST_7_3  = 9166
//   USER_UNICAST_7_3  = 9167
//   SPDP_MCAST_ADDR   = 239.255.0.1
//   USER_MCAST_ADDR   = 239.255.0.2
//
// The scratch file was deleted immediately after and never committed —
// `git status` in the go-DDS clone was confirmed clean afterward.
//
// The UdpSocket send/recv/multicast-fallback tests below exercise the
// transport primitive itself against real OS sockets (loopback); go-DDS
// has no dedicated transport_test.go of its own to port byte-exact vectors
// from (newUnicastSocket/newMulticastReceiveSocket are only exercised
// indirectly, through full participant integration tests, which is a
// later Tier-1 phase here) — so these are ordinary behavioral tests, not
// wire-format conformance vectors.

#include <atomic>
#include <cstring>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <dds/rtps/transport.hpp>

using namespace dds::rtps;

// ── Port-assignment formula (RTPS 2.3 §9.6.1) ───────────────────────────────

TEST_CASE("RTPS port formula matches go-DDS reference values", "[rtps][transport]") {
    CHECK(meta_multicast_port(0) == 7400);
    CHECK(meta_unicast_port(0, 0) == 7410);
    CHECK(data_unicast_port(0, 0) == 7411);
    CHECK(user_multicast_port(0) == 7401);

    CHECK(meta_multicast_port(1) == 7650);
    CHECK(meta_unicast_port(1, 1) == 7662);
    CHECK(data_unicast_port(1, 1) == 7663);
    CHECK(user_multicast_port(1) == 7651);

    CHECK(meta_multicast_port(99) == 32150);
    CHECK(meta_unicast_port(99, 0) == 32160);
    CHECK(data_unicast_port(99, 0) == 32161);

    CHECK(meta_unicast_port(7, 3) == 9166);
    CHECK(data_unicast_port(7, 3) == 9167);
}

TEST_CASE("RTPS multicast group constants match the standard addresses", "[rtps][transport]") {
    CHECK(std::string(kSpdpMulticastAddr) == "239.255.0.1");
    CHECK(std::string(kUserDataMulticastAddr) == "239.255.0.2");
}

// ── UdpSocket behavior ───────────────────────────────────────────────────────

TEST_CASE("UdpSocket::bind_unicast with port 0 gets an OS-assigned ephemeral port",
          "[rtps][transport]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());
    CHECK(sock->valid());
    CHECK(sock->port() != 0);
}

TEST_CASE("UdpSocket send/recv round-trip over loopback", "[rtps][transport]") {
    auto server = UdpSocket::bind_unicast(0);
    auto client = UdpSocket::bind_unicast(0);
    REQUIRE(server.has_value());
    REQUIRE(client.has_value());

    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    REQUIRE(client->send_to("127.0.0.1", server->port(), payload.data(), payload.size()));

    // Poll a few times: recv() has an internal ~250ms timeout per call,
    // matching go-DDS's readLoop poll interval, so a single call can
    // legitimately return std::nullopt while the datagram is still in
    // flight on a loaded CI runner.
    std::optional<UdpPacket> received;
    for (int attempt = 0; attempt < 8 && !received.has_value(); ++attempt) {
        received = server->recv();
    }

    REQUIRE(received.has_value());
    CHECK(received->data == payload);
    CHECK(received->from_port == client->port());
    CHECK((received->from_address == "127.0.0.1" || received->from_address == "0.0.0.0"));
}

TEST_CASE("UdpSocket::recv times out on an idle socket instead of blocking forever",
          "[rtps][transport]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());

    auto pkt = sock->recv();
    CHECK_FALSE(pkt.has_value());
}

TEST_CASE("UdpSocket move semantics transfer ownership and leave the source invalid",
          "[rtps][transport]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());
    int original_port = sock->port();

    UdpSocket moved(std::move(*sock));
    CHECK(moved.valid());
    CHECK(moved.port() == original_port);
    CHECK_FALSE(sock->valid()); // NOLINT(bugprone-use-after-move) — deliberately checking moved-from state
}

TEST_CASE("UdpSocket::bind_multicast_receive succeeds (real join or unicast fallback) "
          "and still delivers same-host unicast traffic on its bound port",
          "[rtps][transport]") {
    // go-DDS's own newMulticastReceiveSocket falls back to a plain unicast
    // bind when the sandbox/CI environment has no multicast-capable route
    // — "the socket then works for intra-process delivery; SPDP peer
    // discovery across network boundaries is simply disabled in that
    // case." This test asserts exactly that guaranteed behavior (the
    // socket is always usable for same-host delivery) rather than
    // asserting true multicast reception, which is not guaranteed in a
    // sandboxed CI runner.
    auto mcast_sock = UdpSocket::bind_multicast_receive(kSpdpMulticastAddr, 0);
    REQUIRE(mcast_sock.has_value());
    CHECK(mcast_sock->valid());
    CHECK(mcast_sock->port() != 0);

    auto sender = UdpSocket::bind_unicast(0);
    REQUIRE(sender.has_value());

    const std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    REQUIRE(sender->send_to("127.0.0.1", mcast_sock->port(), payload.data(), payload.size()));

    std::optional<UdpPacket> received;
    for (int attempt = 0; attempt < 8 && !received.has_value(); ++attempt) {
        received = mcast_sock->recv();
    }
    REQUIRE(received.has_value());
    CHECK(received->data == payload);
}

TEST_CASE("UdpSocket::send_to on an invalid/closed socket fails cleanly", "[rtps][transport]") {
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());
    sock->close();
    CHECK_FALSE(sock->valid());

    const std::vector<uint8_t> payload{0x00};
    CHECK_FALSE(sock->send_to("127.0.0.1", 7400, payload.data(), payload.size()));
    CHECK_FALSE(sock->recv().has_value());
}
