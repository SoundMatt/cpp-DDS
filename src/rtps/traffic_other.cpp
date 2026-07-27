// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Non-Linux stub implementations of TSN socket-marking helpers. C++ port
// of github.com/SoundMatt/go-DDS's rtps/traffic_other.go. This file is
// added to the build on every platform other than Linux — see
// CMakeLists.txt — mirroring go-DDS's `//go:build !linux` split.
//
// SO_PRIORITY, IP_TOS, SO_TXTIME, and CLOCK_TAI are Linux-specific
// features; on macOS, Windows, and other platforms these functions are
// no-ops that allow the rest of the code to compile and run without TSN
// scheduling, exactly as go-DDS's traffic_other.go documents.

#include <dds/rtps/traffic.hpp>

#include <chrono>

namespace dds::rtps {

bool set_sock_priority(const UdpSocket& /*sock*/, int /*priority*/) { return false; }
bool set_sock_tos(const UdpSocket& /*sock*/, uint8_t /*dscp*/) { return false; }
bool enable_tx_time(const UdpSocket& /*sock*/) { return false; }

bool clock_tai_now(uint64_t& out_ns) {
    // No CLOCK_TAI on non-Linux platforms in this port; fall back to the
    // ordinary wall clock (matches go-DDS's clockTAINow() -> time.Now()
    // fallback) and report unavailability via the return value.
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    out_ns = static_cast<uint64_t>(now_ns.count());
    return false;
}

bool scheduled_send(const UdpSocket& sock, const std::string& dst_address, int dst_port,
                     const uint8_t* data, std::size_t len, uint64_t /*tx_time_ns*/) {
    return sock.send_to(dst_address, dst_port, data, len);
}

} // namespace dds::rtps
