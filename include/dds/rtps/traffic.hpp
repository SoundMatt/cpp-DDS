// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/traffic.hpp — Platform-specific socket tuning hooks: egress
// priority (SO_PRIORITY), DSCP marking (IP_TOS), and TSN scheduled
// transmission (SO_TXTIME / CLOCK_TAI).
//
// Part of Tier-1 sub-phase 3 ("UDP transport") of the cpp-DDS RTPS
// roadmap: the roadmap item explicitly calls out go-DDS's traffic_linux.go
// / traffic_other.go split as the pattern to mirror alongside transport.go
// itself. These hooks are unused by transport.hpp's plain send_to()/recv();
// they exist for later TSN integration (Tier 3, go-DDS's tsn.go) to call
// into, exactly as go-DDS's own transport.go does not call them directly
// either — only rtps/tsn.go does.
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/traffic_linux.go
// (Linux: real SO_PRIORITY / IP_TOS / SO_TXTIME / CLOCK_TAI via raw
// syscalls) and rtps/traffic_other.go (all other platforms: no-op stubs
// so the rest of the code compiles and runs without TSN scheduling).
//
// Build split: src/rtps/traffic_linux.cpp is compiled only when
// CMAKE_SYSTEM_NAME is "Linux"; src/rtps/traffic_other.cpp is compiled on
// every other platform (macOS, Windows) — see CMakeLists.txt. This mirrors
// go-DDS's //go:build linux / //go:build !linux file split rather than an
// in-file #ifdef, matching the roadmap's "expect a similar ... split"
// framing (the roadmap itself allows either an #ifdef or "equivalent
// CMake/TU split").

#pragma once

#include <cstdint>
#include <string>

#include <dds/rtps/transport.hpp>

namespace dds::rtps {

// Sets SO_PRIORITY on sock. The Linux kernel maps priority values to
// traffic classes via tc / qdisc rules; values 0-7 directly correspond to
// VLAN PCP when the egress interface is configured for it. No-op (returns
// false) on non-Linux platforms.
bool set_sock_priority(const UdpSocket& sock, int priority);

// Sets the IP ToS byte to encode a DSCP value. dscp is the 6-bit DSCP code
// point (0-63); it is shifted left by 2 to produce the full 8-bit ToS byte
// (ECN bits remain 0). No-op (returns false) on non-Linux platforms.
bool set_sock_tos(const UdpSocket& sock, uint8_t dscp);

// Enables SO_TXTIME on sock using CLOCK_TAI. Requires Linux >= 4.19 and an
// ETF or taprio qdisc on the egress NIC. Returns false (silently ignorable
// by the caller) on older kernels or non-Linux platforms.
bool enable_tx_time(const UdpSocket& sock);

// Returns the current CLOCK_TAI time as nanoseconds since the Unix epoch.
// CLOCK_TAI runs at the same rate as UTC but is not adjusted for leap
// seconds, making it suitable as the reference clock for TSN scheduled
// transmit. Falls back to the ordinary wall clock (and returns false) if
// the kernel clock is unavailable or on non-Linux platforms.
bool clock_tai_now(uint64_t& out_ns);

// Transmits data to dst_address:dst_port at the scheduled TAI time
// tx_time_ns via a SO_TXTIME-scheduled send. The ETF or taprio qdisc on
// the egress NIC holds the packet in a time-sorted queue until the
// scheduled time.
//
// If tx_time_ns is 0, or SO_TXTIME was never enabled on sock (e.g. a
// non-TSN interface, or a non-Linux platform), this falls back to a plain
// UdpSocket::send_to.
bool scheduled_send(const UdpSocket& sock, const std::string& dst_address, int dst_port,
                     const uint8_t* data, std::size_t len, uint64_t tx_time_ns);

} // namespace dds::rtps
