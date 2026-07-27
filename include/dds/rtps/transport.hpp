// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/transport.hpp — UDP socket transport for RTPS 2.3: send/recv,
// the §9.6.1 port-assignment formula, and the standard discovery
// multicast group.
//
// This is Tier-1 sub-phase 3 of the cpp-DDS RTPS roadmap (see ROADMAP.md,
// "Tier 1 — RTPS wire protocol", phase 3: "UDP transport"). It is internal,
// additive scaffolding: NOT yet wired into the public dds::IParticipant /
// relay::INode surface. That happens in later phases (SPDP/SEDP discovery,
// then entities) once this transport primitive and phase 1/2's wire types
// and discovery CDR encoding are available to build on.
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/transport.go (unicast and
// multicast socket setup, send/recv) plus the port-assignment formula and
// multicast group constant from rtps/locator.go and the rtps package's own
// doc comment (rtps.go):
//
//   metaMulticast(domain)      = 7400 + 250*domain
//   metaUnicast(domain, i)     = 7400 + 250*domain + 10 + 2*i
//   dataUnicast(domain, i)     = 7400 + 250*domain + 11 + 2*i
//
// Discovery multicast group: 239.255.0.1 (RTPS 2.3 §9.6.1).
//
// Scope notes (deliberate deviations from a literal line-for-line port):
//
//   - IPv4 only. go-DDS's transport.go additionally has IPv6 socket
//     constructors (newUnicastSocketV6, newMulticastReceiveSocketV6); the
//     roadmap explicitly defers "IPv6 / wildcard locators" to a later,
//     best-effort, non-gating phase (Tier 1 phase 10), so this port omits
//     the V6 path rather than half-implementing it here.
//   - This primitive is a *synchronous* send/recv socket wrapper, not an
//     async goroutine-plus-channel read loop. go-DDS's udpSocket runs a
//     background goroutine (readLoop) that pushes into a buffered Go
//     channel; the C++ equivalent of "a background thread delivering into
//     a queue" belongs with the entities/participant integration (phase 6)
//     that will actually consume it, per this phase's own scope ("socket
//     send/recv"). recv() here blocks for a short internal timeout
//     (matching go-DDS's 250ms SetReadDeadline poll interval) and returns
//     std::nullopt on timeout so callers can loop and check their own
//     shutdown condition, exactly mirroring go-DDS's readLoop poll/select
//     pattern without requiring a thread to exist yet.
//   - Multicast group membership joins on the default route interface
//     (IP_ADD_MEMBERSHIP with INADDR_ANY as the interface address) rather
//     than go-DDS's explicit firstMulticastInterface() enumeration step.
//     This is what Go's own net.ListenMulticastUDP does internally when no
//     interface is specified, so the effective behavior is the same; the
//     explicit enumeration in go-DDS exists because Go's API takes an
//     *interface, not because the selection logic itself differs.
//   - Platform-specific socket tuning (SO_PRIORITY / IP_TOS / SO_TXTIME /
//     CLOCK_TAI) is declared in rtps/traffic.hpp, mirroring go-DDS's own
//     split of that concern into a separate file (traffic_linux.go /
//     traffic_other.go) from transport.go.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dds::rtps {

// ── Port assignment (RTPS 2.3 §9.6.1) ───────────────────────────────────────

inline constexpr int kPortBase        = 7400;
inline constexpr int kDomainGain      = 250;
inline constexpr int kParticipantGain = 2;
inline constexpr int kMetaOffset      = 10;
inline constexpr int kDataOffset      = 11;

// metaMulticast(domain) = 7400 + 250*domain — the SPDP discovery multicast
// port for a domain.
constexpr int meta_multicast_port(int domain) noexcept {
    return kPortBase + kDomainGain * domain;
}

// metaUnicast(domain, i) = 7400 + 250*domain + 10 + 2*i — the metadata
// (discovery) unicast port for the i'th participant in a domain.
constexpr int meta_unicast_port(int domain, int participant_idx) noexcept {
    return kPortBase + kDomainGain * domain + kMetaOffset + kParticipantGain * participant_idx;
}

// dataUnicast(domain, i) = 7400 + 250*domain + 11 + 2*i — the user-data
// unicast port for the i'th participant in a domain.
constexpr int data_unicast_port(int domain, int participant_idx) noexcept {
    return kPortBase + kDomainGain * domain + kDataOffset + kParticipantGain * participant_idx;
}

// User-data multicast port for a domain (go-DDS locator.go: userMulticastPort,
// portBase + domainGain*domain + 1). Not part of the OMG §9.6.1 formula
// itself (that formula only defines the four ports above) but part of the
// same port family go-DDS derives from it for its multicast fast-path.
constexpr int user_multicast_port(int domain) noexcept {
    return kPortBase + kDomainGain * domain + 1;
}

// ── Multicast groups ─────────────────────────────────────────────────────────

// Standard RTPS 2.3 discovery (SPDP) multicast group.
inline constexpr const char* kSpdpMulticastAddr = "239.255.0.1";

// go-DDS's user-data multicast group (domain-scoped fast path; not part of
// the OMG spec's mandated addresses but shared here for parity since
// user_multicast_port exists to serve it).
inline constexpr const char* kUserDataMulticastAddr = "239.255.0.2";

// ── UDP socket ───────────────────────────────────────────────────────────────

#if defined(_WIN32)
using NativeSocketHandle = std::uintptr_t; // matches Windows SOCKET (UINT_PTR)
#else
using NativeSocketHandle = int; // POSIX file descriptor
#endif

// UdpPacket is a received UDP datagram with sender address, mirroring
// go-DDS's udpPacket{data, from}.
struct UdpPacket {
    std::vector<uint8_t> data;
    std::string          from_address; // dotted-quad IPv4
    int                  from_port{0};
};

// UdpSocket wraps a bound, IPv4 UDP socket. Move-only (owns a native OS
// handle). Corresponds to go-DDS's udpSocket, minus the background
// readLoop goroutine — see the file-level scope note above.
class UdpSocket {
public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    ~UdpSocket();

    // Binds 0.0.0.0:<port> (IPv4). If that port is in use it tries
    // port+1 … port+15 before giving up (matches go-DDS's
    // newUnicastSocket). If port == 0, binds to an OS-assigned ephemeral
    // port instead (no retry needed; useful for tests and loaned sockets).
    static std::optional<UdpSocket> bind_unicast(int port);

    // Binds a socket that receives on the given multicast group and port.
    // If the OS has no multicast-capable route (common in containers and
    // sandboxed CI), falls back to a plain unicast bind on the same port
    // (matches go-DDS's newMulticastReceiveSocket fallback) — the socket
    // still works for same-host delivery; only true cross-host multicast
    // discovery is disabled in that case.
    static std::optional<UdpSocket> bind_multicast_receive(const std::string& group, int port);

    bool valid() const noexcept { return handle_ != kInvalidHandle; }
    int  port() const noexcept { return port_; }
    NativeSocketHandle native_handle() const noexcept { return handle_; }

    // Sends data to dst_address:dst_port (dst_address is a dotted-quad
    // IPv4 literal). Returns false on error.
    bool send_to(const std::string& dst_address, int dst_port,
                 const uint8_t* data, std::size_t len) const;

    // Blocking receive with an internal ~250ms timeout, matching go-DDS's
    // readLoop poll interval (SetReadDeadline(250ms) then continue). The
    // caller is expected to loop and check its own shutdown condition
    // between calls, exactly as go-DDS's readLoop does around s.done.
    // Returns std::nullopt on timeout, error, or an invalid socket.
    std::optional<UdpPacket> recv() const;

    // Closes the underlying OS socket. Safe to call more than once.
    void close();

private:
    static constexpr NativeSocketHandle kInvalidHandle = static_cast<NativeSocketHandle>(-1);

    NativeSocketHandle handle_{kInvalidHandle};
    int                 port_{0};
};

} // namespace dds::rtps
