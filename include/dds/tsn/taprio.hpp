// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// tsn/taprio.hpp — IEEE 802.1Qbv (TAPRIO) time-aware shaping gate control
// list configuration. C++ port of go-DDS's `TAPRIOEntry`/`TAPRIOConfig`
// (tsn/diagnostics.go) plus `Validate`/`CycleDuration`/`TCCommand`
// (tsn/taprio.go).
//
// `TAPRIOConfig::apply()`/`verify_applied()` are Linux-only: they program
// (respectively verify) the kernel `taprio` qdisc on a real network
// interface via an `RTM_NEWQDISC`/`RTM_GETQDISC` netlink request over
// `NETLINK_ROUTE`, exactly as go-DDS's tsn/taprio_linux.go does — this
// requires `CAP_NET_ADMIN`. On every other platform they return a
// "not supported" error (tsn/taprio_stub.go's `ErrNotSupported` in
// go-DDS), matching this repo's `CMAKE_SYSTEM_NAME STREQUAL "Linux"`
// translation-unit split already established by
// src/rtps/traffic_linux.cpp / traffic_other.cpp (see
// include/dds/rtps/traffic.hpp and ROADMAP.md's Tier-1 phase 3, and this
// item's own ROADMAP.md text: "cpp-DDS should follow the same
// platform-gated pattern ... not attempt TAPRIO on macOS or Windows").
//
// Neither `dds::rtps::Participant` nor anything else in this library calls
// `apply()`/`verify_applied()` automatically — exactly as go-DDS's
// participant.go never calls `TAPRIOConfig.Apply` either; programming the
// egress NIC's qdisc is an explicit, privileged, out-of-band operator/
// application action (see go-DDS's `examples/taprio-stream`), not
// something a DDS participant does on the caller's behalf.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dds/tsn/tsn.hpp>

namespace dds::tsn {

// fusa:req REQ-TSN-006

// TAPRIOEntry is one gate control entry in an IEEE 802.1Qbv gate control
// list. gate_mask is a bitmask of open traffic classes (bit N = TC N
// open). C++ port of go-DDS's tsn.TAPRIOEntry.
struct TAPRIOEntry {
    uint8_t gate_mask{0};
    std::chrono::nanoseconds interval{0};
};

// TAPRIOConfig holds a TAPRIO gate control list, optionally derived from a
// StreamConfig via taprio_from_streams. Call tc_command() to get a tc(8)
// command-string template (portable, every platform), or apply()/
// verify_applied() (Linux only) to program/verify the qdisc directly via
// netlink. C++ port of go-DDS's tsn.TAPRIOConfig.
struct TAPRIOConfig {
    std::chrono::nanoseconds cycle_time{0}; // total schedule cycle; 0 = derive from entries
    std::vector<TAPRIOEntry> entries;       // ordered gate control entries
    std::string interface;                  // network interface to configure (e.g. "eth0"); required by apply()
    int64_t base_time{0};                   // TAPRIO schedule base time, CLOCK_TAI nanoseconds; 0 = kernel picks next cycle boundary
    bool offload{false};                    // request full hardware offload from the NIC

    // cycle_duration returns the effective cycle time: cycle_time if set,
    // otherwise the sum of every entry's interval.
    std::chrono::nanoseconds cycle_duration() const noexcept;

    // validate checks that this config is well-formed for a call to
    // apply(): interface non-empty, entries non-empty, every entry's
    // interval > 0 and representable in the kernel's uint32 nanosecond
    // gate-entry field (~4.295s). Returns nullopt on success, an error
    // message otherwise.
    std::optional<std::string> validate() const;

    // apply programs the TAPRIO qdisc on `interface` by sending an
    // RTM_NEWQDISC netlink message to the kernel, replacing any existing
    // root qdisc. Requires CAP_NET_ADMIN and Linux. Returns nullopt on
    // success. Returns "tsn: TAPRIO qdisc requires Linux" on every other
    // platform. fusa:req REQ-TSN-007
    std::optional<std::string> apply() const;

    // verify_applied queries the kernel via RTM_GETQDISC to confirm that a
    // TAPRIO qdisc is the root qdisc on `interface`. Returns nullopt if
    // "taprio" is found. Returns "tsn: TAPRIO qdisc requires Linux" on
    // every other platform. fusa:req REQ-TSN-007
    std::optional<std::string> verify_applied() const;

    // tc_command returns a tc(8) command string that programs this TAPRIO
    // schedule on iface. base_time_ns is the TAI base time in nanoseconds
    // (0 = start at the beginning of the epoch). Portable — produces a
    // string on every platform regardless of whether apply() is
    // supported. The output is a starting-point template; operators may
    // need to adjust num_tc/map/queues to match their NIC and
    // traffic-class configuration.
    std::string tc_command(const std::string& iface, int64_t base_time_ns) const;
};

// taprio_from_streams derives a simple TAPRIO gate schedule from cfg. Each
// stream's PCP value determines its traffic class (TC = PCP); the gate
// opens each TC exclusively for its transmit interval, other TCs closed
// during that slot. Returns nullopt (with a diagnostic in `error`) when
// cfg has no streams or every stream has a zero interval. C++ port of
// go-DDS's tsn.TAPRIOFromStreams.
struct TAPRIOFromStreamsResult {
    std::optional<TAPRIOConfig> config;
    std::optional<std::string>  error;

    bool ok() const noexcept { return config.has_value(); }
};
TAPRIOFromStreamsResult taprio_from_streams(const StreamConfig& cfg);

} // namespace dds::tsn
