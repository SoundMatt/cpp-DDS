// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/reliable.hpp — reader-side reliable-delivery bookkeeping (RTPS 2.3
// §8.4.9-§8.4.12): the sliding-window gap tracker a reliable reader uses to
// decide when to send ACKNACK and what to put in it.
//
// This is Tier-1 sub-phase 7 of the cpp-DDS RTPS roadmap (see ROADMAP.md,
// "Tier 1 — RTPS wire protocol", phase 7: "Reliable delivery"). It is
// internal, additive scaffolding: NOT yet wired into the public
// dds::IParticipant / relay::INode surface beyond dds::rtps::Participant
// itself (see participant.hpp's own file-level scope note on what "wired
// in" means at this tier).
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/reliable.go — specifically
// its *receiver*-side half (recvTracker, snToU64/u64ToSN, maxReorderAhead).
// The *sender*-side half of reliable.go (sendHistory, a fixed-256-depth ring
// buffer of raw wire-message bytes keyed by sequence number) is NOT ported
// here: cpp-DDS already has a more general per-writer sequence-number store
// from Tier-1 phase 6 (dds::rtps::HistoryCache, see history_cache.hpp) that
// this phase's Writer reuses for retransmission instead — see
// history_cache.hpp's own file-level scope note, which anticipated exactly
// this reuse ("be the storage phase 7 wraps for retransmission"). Because
// HistoryCache stores decoded CacheChange values rather than pre-framed wire
// bytes, retransmission re-encodes a fresh DATA submessage from the stored
// (sequence_number, payload) pair rather than replaying byte-identical
// original bytes — functionally equivalent, and every primitive it
// recomposes (DataSubmessage::encode, cdr_wrap_payload,
// wrap_in_rtps_message) is already byte-verified against go-DDS in earlier
// phases, so there is no new wire encoding introduced by that choice either.
//
// RecvTracker's own arithmetic (expected/ahead-set bookkeeping, the 32-bit
// NACK bitmap window) has no independent wire-format output of its own — it
// only ever feeds the already byte-verified AckNack::encode (types.hpp) — so
// unlike types.hpp's Heartbeat/AckNack/Gap, this header carries no golden
// vector requirement; it is verified with ordinary behavioral unit tests
// mirroring go-DDS's reliable_test.go (see tests/test_rtps_reliable.cpp).
//
// Header-only, matching history_cache.hpp's own precedent for small,
// mutex-guarded bookkeeping types in this codebase (not registered as a
// library source file in CMakeLists.txt).

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <unordered_set>

#include <dds/rtps/types.hpp>

namespace dds::rtps {

// Matches go-DDS's rtps/reliable.go heartbeatPeriod constant: how often a
// reliable writer's background loop re-sends a HEARTBEAT even without a new
// write.
inline constexpr std::chrono::milliseconds kHeartbeatPeriod{200};

// Matches go-DDS's rtps/reliable.go maxReorderAhead constant: how far ahead
// of the cumulative-ACK watermark a received sequence number is buffered in
// RecvTracker::ahead_ before it is simply dropped (bounding memory against a
// misbehaving or wildly-out-of-order writer).
inline constexpr uint64_t kMaxReorderAhead = 8192;

// Packs an RTPS SequenceNumber (High:Low) into a single uint64 so
// reliability bookkeeping never aliases after the low 32 bits wrap (RTPS 2.3
// §8.3.5), matching go-DDS's snToU64.
constexpr uint64_t sn_to_u64(const SequenceNumber& sn) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(sn.high)) << 32) | static_cast<uint64_t>(sn.low);
}

// Splits a uint64 sequence number back into the wire High:Low form, matching
// go-DDS's u64ToSN.
constexpr SequenceNumber u64_to_sn(uint64_t v) noexcept {
    return SequenceNumber{static_cast<int32_t>(v >> 32), static_cast<uint32_t>(v)};
}

// RecvTracker tracks reliable-delivery state for a single remote writer, on
// the reader side. C++ port of go-DDS's recvTracker (rtps/reliable.go).
//
// It maintains a sliding window: expected_ is the lowest sequence number not
// yet received (the cumulative-ACK base — everything below it has arrived),
// and ahead_ holds out-of-order SNs at or above expected_ that have already
// been received. expected_ only ever advances over a contiguous run, so a
// missing SN is re-NACKed on every HEARTBEAT until it actually arrives, and
// gaps larger than one 32-bit ACKNACK window are recovered window-by-window
// as the watermark advances. Thread-safe (matches go-DDS's sync.Mutex-guarded
// struct).
class RecvTracker {
public:
    // Sets the cumulative-ACK base on first contact with a writer (typically
    // from a HEARTBEAT's FirstSN) so the reader can request the writer's
    // whole live history. No-op once the tracker has seen any sample,
    // matching go-DDS's initExpected.
    void init_expected(uint64_t first_sn) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!init_done_) {
            expected_  = first_sn;
            init_done_ = true;
        }
    }

    // Marks seq as received and advances the contiguous watermark over any
    // buffered successors. Returns false when seq was already delivered
    // (below the watermark) or already buffered, so callers can suppress
    // duplicate delivery. Matches go-DDS's record.
    bool record(uint64_t seq) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!init_done_) {
            expected_  = seq;
            init_done_ = true;
        }
        if (seq < expected_) return false; // already delivered
        if (seq == expected_) {
            ++expected_;
            while (ahead_.count(expected_) != 0) {
                ahead_.erase(expected_);
                ++expected_;
            }
            return true;
        }
        // seq > expected_: buffer it (bounded) unless already seen.
        if (ahead_.count(seq) != 0) return false;
        if (seq - expected_ <= kMaxReorderAhead) ahead_.insert(seq);
        return true;
    }

    // Returns the ACKNACK base and bitmap describing the sequence numbers
    // still missing in [expected_, last_sn], capped at one 32-bit window.
    // base is the cumulative-ACK watermark; bit N set means base+N is
    // missing. need_ack is true when at least one SN in the window is
    // missing. Matches go-DDS's missing.
    std::tuple<uint64_t, uint32_t, bool> missing(uint64_t last_sn) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (!init_done_) return {0, 0, false};
        const uint64_t base = expected_;
        if (last_sn < base) return {base, 0, false}; // fully caught up with the writer
        uint64_t end = last_sn;
        if (end > base + 31) end = base + 31;
        uint32_t bitmap  = 0;
        bool     need_ack = false;
        for (uint64_t sn = base; sn <= end; ++sn) {
            // expected_ is never present in ahead_, so this also flags base itself.
            if (ahead_.count(sn) == 0) {
                bitmap |= 1u << static_cast<unsigned>(sn - base);
                need_ack = true;
            }
        }
        return {base, bitmap, need_ack};
    }

    // Returns a monotonically increasing count for ACKNACK, matching go-DDS's
    // nextAckCount.
    int32_t next_ack_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return ++ack_count_;
    }

private:
    mutable std::mutex       mu_;
    uint64_t                  expected_{0};
    std::unordered_set<uint64_t> ahead_;
    bool                       init_done_{false};
    int32_t                    ack_count_{0};
};

} // namespace dds::rtps
