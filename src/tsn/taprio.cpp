// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Portable (every-platform) portion of dds::tsn::TAPRIOConfig: validation,
// cycle-time derivation, tc(8) command-string generation, and deriving a
// schedule from a StreamConfig. C++ port of go-DDS's tsn/taprio.go
// (Validate/CycleDuration) and the TAPRIOConfig/TCCommand/
// TAPRIOFromStreams portions of tsn/diagnostics.go. The Linux-only
// apply()/verify_applied() (netlink) implementations live in
// taprio_linux.cpp; the non-Linux stub lives in taprio_other.cpp — see
// include/dds/tsn/taprio.hpp's file-level scope note.

#include <dds/tsn/taprio.hpp>

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dds::tsn {

// fusa:req REQ-TSN-006

namespace {
// Maximum TAPRIO gate entry interval expressible in the kernel's uint32
// nanosecond field (~4.295s). Linux TAPRIO rejects larger values anyway;
// validate() checks early to produce a clear diagnostic.
constexpr int64_t kMaxEntryIntervalNs = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
} // namespace

std::chrono::nanoseconds TAPRIOConfig::cycle_duration() const noexcept {
    if (cycle_time.count() > 0) return cycle_time;
    std::chrono::nanoseconds total{0};
    for (const auto& e : entries) total += e.interval;
    return total;
}

std::optional<std::string> TAPRIOConfig::validate() const {
    if (interface.empty()) return "tsn: TAPRIOConfig: Interface must not be empty";
    if (entries.empty()) return "tsn: TAPRIOConfig: Entries must not be empty";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.interval.count() <= 0)
            return "tsn: TAPRIOConfig: entry[" + std::to_string(i) + "].Interval must be > 0";
        if (e.interval.count() > kMaxEntryIntervalNs)
            return "tsn: TAPRIOConfig: entry[" + std::to_string(i) +
                   "].Interval exceeds uint32 ns limit (~4.295s)";
    }
    return std::nullopt;
}

std::string TAPRIOConfig::tc_command(const std::string& iface, int64_t base_time_ns) const {
    std::ostringstream b;
    b << "tc qdisc replace dev " << iface << " parent root handle 100 taprio"
      << " num_tc 8"
      << " map 0 1 2 3 4 5 6 7"
      << " queues 1@0 1@1 1@2 1@3 1@4 1@5 1@6 1@7"
      << " base-time " << base_time_ns;
    for (const auto& e : entries) {
        // %02x-equivalent: zero-padded to 2 hex digits, matching go-DDS's
        // TCCommand exactly (fmt.Fprintf(&b, " sched-entry S %02x %d", ...)).
        b << " sched-entry S " << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<unsigned>(e.gate_mask) << std::dec << std::setfill(' ') << std::setw(0)
          << " " << e.interval.count();
    }
    b << " flags 0x1";
    return b.str();
}

// fusa:req REQ-TSN-006
TAPRIOFromStreamsResult taprio_from_streams(const StreamConfig& cfg) {
    TAPRIOFromStreamsResult res;
    if (cfg.streams.empty()) {
        res.error = "tsn: TAPRIOFromStreams: no streams configured";
        return res;
    }

    int64_t cycle_ns = 0;
    for (const auto& s : cfg.streams) {
        if (s.interval_us > 0) cycle_ns += s.interval_us * 1000;
    }
    if (cycle_ns == 0) {
        res.error = "tsn: TAPRIOFromStreams: all streams have zero interval";
        return res;
    }

    TAPRIOConfig tc;
    tc.cycle_time = std::chrono::nanoseconds(cycle_ns);
    for (const auto& s : cfg.streams) {
        if (s.interval_us <= 0) continue;
        TAPRIOEntry e;
        e.gate_mask = static_cast<uint8_t>(1u << s.pcp);
        e.interval  = std::chrono::microseconds(s.interval_us);
        tc.entries.push_back(e);
    }
    res.config = std::move(tc);
    return res;
}

} // namespace dds::tsn
