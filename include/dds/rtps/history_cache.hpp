// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/history_cache.hpp — the per-endpoint HistoryCache (RTPS 2.3 §8.2.4):
// a bounded, sequence-number-indexed store of CacheChanges a writer keeps so
// later machinery (retransmission, late-joiner durability) can plug into it.
//
// This is part of Tier-1 sub-phase 6 of the cpp-DDS RTPS roadmap (see
// ROADMAP.md, "Tier 1 — RTPS wire protocol", phase 6: "Entities & history
// cache"). Unlike phases 1-5, there is no single go-DDS file this is a
// line-for-line port of: go-DDS's closest equivalent is rtps/reliable.go's
// `sendHistory` (a fixed-256-depth ring buffer of raw wire-message bytes,
// used only for HEARTBEAT/ACKNACK retransmission — Tier-1 phase 7, not yet
// built). This phase's HistoryCache is deliberately more general than that:
// it stores decoded CacheChange values (sequence number, writer GUID,
// payload, timestamp) rather than pre-framed wire bytes, so it can serve
// this phase's own best-effort writer bookkeeping *and* be the storage
// phase 7 wraps for retransmission, without phase 6 having to guess at
// phase 7's exact wire-replay needs. It is intentionally NOT a byte-exact
// port of `sendHistory`'s ring-array implementation — there is no
// go-DDS-observable wire behavior here to pin a golden vector against; a
// HistoryCache's contents are never themselves serialized to the wire (only
// individual CacheChange payloads are, via the existing, already
// byte-verified DataSubmessage/cdr_wrap_payload machinery in
// participant.cpp).
//
// Capacity default: 256, matching go-DDS's `maxHistoryDepth` constant (a
// reasonable reliable-retransmission-sized window) — NOT dds::QoS's
// `history_depth` field (DDS KEEP_LAST sample count), which go-DDS's own
// `rtps` package never consumes either (see ParticipantOptions::history_depth
// in participant.hpp for where this constructor parameter is threaded from).

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <dds/rtps/types.hpp>

namespace dds::rtps {

// Matches go-DDS's rtps/reliable.go maxHistoryDepth constant.
inline constexpr std::size_t kDefaultHistoryDepth = 256;

// CacheChange is one entry in a HistoryCache: a single decoded sample
// attributed to its writer, with the RTPS sequence number and local receipt/
// publish timestamp.
struct CacheChange {
    uint64_t                              sequence_number{0};
    GUID                                  writer_guid{};
    std::vector<uint8_t>                  payload;
    std::chrono::system_clock::time_point timestamp{};
};

// HistoryCache is a thread-safe, bounded FIFO store of CacheChanges for one
// endpoint, indexed by sequence number. When more than `depth` changes have
// been stored, the oldest is evicted — the same "retain the last N" window
// semantics as go-DDS's sendHistory, generalized to not assume a
// reliability-specific wire-message payload.
class HistoryCache {
public:
    explicit HistoryCache(std::size_t depth = kDefaultHistoryDepth) : depth_(depth > 0 ? depth : 1) {}

    // Appends change, evicting the oldest entry if the cache is at capacity.
    // Does not enforce or check strictly-increasing sequence numbers (the
    // caller — a writer's monotonic seq counter — already guarantees that);
    // storing an out-of-order or duplicate sequence number simply appends
    // it, matching a plain bounded FIFO rather than sendHistory's
    // seq%depth ring-slot overwrite semantics.
    void store(CacheChange change) {
        std::lock_guard<std::mutex> lock(mu_);
        changes_.push_back(std::move(change));
        if (changes_.size() > depth_) changes_.pop_front();
    }

    // Returns the CacheChange for sequence_number, or std::nullopt if it was
    // never stored or has since been evicted.
    std::optional<CacheChange> get(uint64_t sequence_number) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& c : changes_) {
            if (c.sequence_number == sequence_number) return c;
        }
        return std::nullopt;
    }

    // Returns the most recently stored change, or std::nullopt if empty.
    std::optional<CacheChange> latest() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (changes_.empty()) return std::nullopt;
        return changes_.back();
    }

    // Returns {lowest, highest} retained sequence numbers. The bool is
    // false (and the pair {0, 0}) when the cache is empty.
    std::pair<uint64_t, uint64_t> span() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (changes_.empty()) return {0, 0};
        return {changes_.front().sequence_number, changes_.back().sequence_number};
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return changes_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return changes_.size();
    }

    std::size_t depth() const noexcept { return depth_; }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        changes_.clear();
    }

private:
    mutable std::mutex      mu_;
    std::size_t              depth_;
    std::deque<CacheChange>  changes_;
};

} // namespace dds::rtps
