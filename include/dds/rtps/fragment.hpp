// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/fragment.hpp — splitting a large payload into DATA_FRAG submessages
// (RTPS 2.3 §8.3.7.3) on write, and reassembling them back into a complete
// payload on receipt.
//
// This is Tier-1 sub-phase 8 of the cpp-DDS RTPS roadmap (see ROADMAP.md,
// "Tier 1 — RTPS wire protocol", phase 8: "Fragmentation"). It is internal,
// additive scaffolding: NOT yet wired into the public dds::IParticipant /
// relay::INode surface beyond dds::rtps::Participant itself (see
// participant.hpp's own file-level scope note on what "wired in" means at
// this tier).
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/fragment.go. The DataFrag
// wire struct itself (encode/decode) lives in types.hpp/types.cpp, matching
// this codebase's existing precedent of keeping submessage wire types there
// (Heartbeat/AckNack/Gap, added in Tier-1 phase 7, are the direct
// precedent) — this header carries only the *bookkeeping* around DataFrag:
// the split-into-fragments producer (splitIntoFragments/splitIntoFragmentsN)
// and the receive-side fragmentAssembler, exactly matching reliable.hpp's
// own split (types.hpp = wire struct, reliable.hpp = bookkeeping) for
// phase 7's Heartbeat/AckNack.
//
// Scope notes (deliberate deviations from a literal line-for-line port):
//
//   - go-DDS's fragmentAssembler keys reassembly buffers by
//     {writer EntityId, seqLo uint32} only — it never includes the
//     source participant's GuidPrefix or the sequence number's high word.
//     That is a latent cross-participant collision risk go-DDS's own code
//     never exercises (see this header's next bullet: go-DDS never wires
//     fragmentAssembler into its receive path at all, so the risk is
//     theoretical there). Because this port *does* wire reassembly into
//     the receive path (Participant::handle_data_packet), FragmentAssembler
//     here keys on the full writer GUID (GuidPrefix + EntityId) plus the
//     full 64-bit sequence number (via reliable.hpp's sn_to_u64, already
//     used for exactly this purpose elsewhere in this codebase) instead.
//     This has no wire-format consequence — the key is never serialized —
//     and is a strict correctness improvement, not an independent
//     simplification.
//   - go-DDS defines both marshalDataFrag/parseDataFrag *and* a fully
//     working fragmentAssembler, but rtps/participant.go's
//     handleDataPacket switch (the receive-side submessage dispatch) has
//     no `case submsgDATAFRAG` at all — go-DDS's own writer fragments
//     large payloads on send, but no go-DDS participant ever reassembles
//     an incoming DATA_FRAG. This looks like an oversight rather than a
//     deliberate design choice (unlike, say, GAP's genuinely-never-defined
//     parseGAP — see types.hpp's Gap doc comment): the reassembly
//     machinery already exists byte-for-byte in fragment.go, it is simply
//     never called. Porting only the write side would leave cpp-DDS unable
//     to receive its own fragmented writes — not a materially working
//     "Fragmentation" phase. This port therefore completes the round trip:
//     Participant::handle_data_packet (participant.cpp) gains a
//     `case kSubmessageIdDataFrag` that calls the already byte-verified
//     DataFrag::decode and this header's FragmentAssembler::receive, then
//     dispatches the reassembled payload exactly like a normal DATA
//     submessage. This reuses only already-verified primitives, the same
//     pattern phase 6/7 already established for composing new behavior out
//     of byte-pinned building blocks — it does not introduce any new wire
//     encoding of its own.
//   - go-DDS's rtpsWriter.fragmentSize() consults an optional TSN stream
//     descriptor (Tier 3, not yet ported) for a frame-size-bound override;
//     cpp-DDS's Writer (participant.cpp) has no TSN writer yet, so it
//     always uses kMaxFragmentPayload directly — there is nothing to port
//     for the TSN branch at this tier.
//   - go-DDS's rtpsWriter.Write stores only the *first* fragment's wire
//     message in its sendHistory ring buffer for a fragmented write
//     ("For fragmented payloads this stores only the first fragment; a
//     future enhancement can store per-fragment msgs" — rtps/participant.go)
//     — so go-DDS's own ACKNACK-triggered retransmission of a fragmented
//     sample only ever resends fragment #1, never the rest. cpp-DDS's
//     HistoryCache (Tier-1 phase 6) already stores the decoded payload
//     rather than pre-framed wire bytes specifically so retransmission can
//     re-encode fresh wire bytes (see history_cache.hpp/reliable.hpp's own
//     scope notes) — Writer::handle_ack_nack (participant.cpp) reuses that
//     to re-fragment the full payload on retransmit when it is still over
//     threshold, which is a strict correctness improvement over go-DDS's
//     known limitation, not a byte-format change (every individual
//     retransmitted fragment is still byte-identical to what a live write
//     of the same payload would produce).

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <dds/rtps/reliable.hpp> // sn_to_u64
#include <dds/rtps/types.hpp>

namespace dds::rtps {

// maxFragmentPayload (go-DDS name), the maximum bytes placed in a single
// DATA_FRAG body. Chosen to keep the full RTPS packet under 1400 bytes on a
// typical Ethernet MTU of 1500 bytes (headers ~100 bytes). Matches go-DDS's
// rtps/fragment.go maxFragmentPayload constant.
inline constexpr uint16_t kMaxFragmentPayload = 1200;

// The maximum DataSize accepted from an incoming DATA_FRAG submessage.
// Frames claiming a larger size are discarded to prevent memory exhaustion
// from malformed or malicious peers. Matches go-DDS's maxReassemblyBytes.
inline constexpr uint32_t kMaxReassemblyBytes = 16u * 1024u * 1024u; // 16 MiB

// How long an incomplete fragment reassembly is held before being
// discarded, bounding memory use when fragments are permanently lost.
// Matches go-DDS's staleFragAge. steady_clock, not wall-clock, matching the
// same steady-clock-for-interval-math precedent already used for lease
// eviction in spdp.cpp.
inline constexpr std::chrono::seconds kStaleFragAge{5};

// FragmentAssembler reassembles DATA_FRAG submessages for a single (writer
// GUID, sequence number) pair. C++ port of go-DDS's fragmentAssembler
// (rtps/fragment.go) — see this header's file-level scope note for why the
// reassembly key is a full GUID + 64-bit sequence number here rather than
// go-DDS's EntityId + low-32-bits-only key. Thread-safe (matches go-DDS's
// sync.Mutex-guarded struct).
class FragmentAssembler {
public:
    // Adds a fragment (arrived from writer_guid) and returns the complete
    // reassembled payload once every fragment has arrived; returns
    // std::nullopt while reassembly is still incomplete (or the fragment is
    // rejected as implausible). Matches go-DDS's fragmentAssembler.receive,
    // including its exact sweep-for-stale-entries-on-every-call behavior
    // (there is no separate background eviction thread, matching go-DDS).
    std::optional<std::vector<uint8_t>> receive(const GUID& writer_guid, const DataFrag& f) {
        if (f.fragment_size == 0 || f.data_size == 0 || f.fragments_in_submsg == 0) return std::nullopt;
        if (f.data_size > kMaxReassemblyBytes) return std::nullopt;
        const uint32_t total = (f.data_size + static_cast<uint32_t>(f.fragment_size) - 1) /
                                static_cast<uint32_t>(f.fragment_size);

        const Key key{writer_guid, sn_to_u64(f.writer_seq_num)};
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mu_);

        // Evict incomplete reassemblies older than kStaleFragAge. This
        // prevents memory accumulation from lost or abandoned fragment
        // streams, matching go-DDS's identical sweep-on-every-receive-call.
        for (auto it = buffers_.begin(); it != buffers_.end();) {
            if (now - it->second.created > kStaleFragAge) {
                it = buffers_.erase(it);
            } else {
                ++it;
            }
        }

        auto it = buffers_.find(key);
        if (it == buffers_.end()) {
            Buffer buf;
            buf.data.assign(f.data_size, uint8_t{0});
            buf.total   = total;
            buf.created = now;
            // received_mask tracks which 0-based fragment indices have
            // actually been written, so a retransmitted/duplicate fragment
            // cannot be counted twice toward `received` (see this method's
            // doc comment: a duplicate satisfying `received >= total`
            // without every index having genuinely arrived would deliver
            // zero-filled data for whichever index was actually lost —
            // this is a strict correctness fix over go-DDS's fragment.go,
            // which has the same latent bare-counter bug but never wires
            // fragmentAssembler into its receive path, so it never
            // exercises it; see this header's file-level scope note).
            buf.received_mask.assign(total, false);
            it = buffers_.emplace(key, std::move(buf)).first;
        }
        Buffer& buf = it->second;

        // Copy each fragment's bytes into the correct position, matching
        // go-DDS's identical offset arithmetic (including its behavior on
        // an implausible fragment_starting_num == 0: frag_idx underflows,
        // offset becomes huge, and the fragment is silently dropped by the
        // `offset >= data_size` guard below — unsigned wraparound is
        // well-defined in both Go and C++, so this port reproduces that
        // behavior exactly rather than adding a guard go-DDS doesn't have).
        const uint32_t frag_idx = f.fragment_starting_num - 1; // convert to 0-based
        for (uint16_t i = 0; i < f.fragments_in_submsg; ++i) {
            const uint32_t idx    = frag_idx + i;
            const uint32_t offset = idx * static_cast<uint32_t>(f.fragment_size);
            if (offset >= f.data_size) break;
            uint32_t frag_start = static_cast<uint32_t>(i) * static_cast<uint32_t>(f.fragment_size);
            uint32_t frag_end   = frag_start + f.fragment_size;
            if (frag_end > f.payload.size()) frag_end = static_cast<uint32_t>(f.payload.size());
            uint32_t end = offset + (frag_end - frag_start);
            if (end > f.data_size) end = f.data_size;
            if (frag_start < frag_end && offset < end) {
                std::copy(f.payload.begin() + frag_start, f.payload.begin() + frag_end, buf.data.begin() + offset);
            }
            // Only count this index toward completion the first time it is
            // genuinely written. `idx` is always < buf.total here: offset
            // (== idx * fragment_size) already passed the `offset >=
            // f.data_size` guard above, and total == ceil(data_size /
            // fragment_size), so idx < total is guaranteed.
            if (!buf.received_mask[idx]) {
                buf.received_mask[idx] = true;
                ++buf.received;
            }
        }

        if (buf.received >= buf.total) {
            std::vector<uint8_t> result = std::move(buf.data);
            buffers_.erase(it);
            return result;
        }
        return std::nullopt;
    }

private:
    struct Key {
        GUID     writer;
        uint64_t seq{0};

        bool operator==(const Key& o) const noexcept { return writer == o.writer && seq == o.seq; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            std::size_t h = 1469598103934665603ull; // FNV-1a
            for (uint8_t b : k.writer.prefix.bytes) {
                h ^= b;
                h *= 1099511628211ull;
            }
            for (uint8_t b : k.writer.entity.bytes) {
                h ^= b;
                h *= 1099511628211ull;
            }
            for (int i = 0; i < 8; ++i) {
                h ^= static_cast<uint8_t>(k.seq >> (i * 8));
                h *= 1099511628211ull;
            }
            return h;
        }
    };
    struct Buffer {
        std::vector<uint8_t>                 data;
        std::vector<bool>                     received_mask; // per-fragment-index arrival, sized to `total`
        uint32_t                              received{0};   // count of *distinct* indices received so far
        uint32_t                              total{0};
        std::chrono::steady_clock::time_point created{};
    };

    std::mutex                                    mu_;
    std::unordered_map<Key, Buffer, KeyHash>      buffers_;
};

// Breaks payload into DataFrag values with at most max_payload_size bytes
// per fragment (kMaxFragmentPayload if max_payload_size <= 0), matching
// go-DDS's splitIntoFragmentsN: reader_entity_id is always kEntityIdUnknown,
// fragments_in_submsg is always 1 (one fragment per submessage — see
// types.hpp's DataFrag doc comment).
inline std::vector<DataFrag> split_into_fragments_n(const EntityId& writer_eid, const SequenceNumber& seq_num,
                                                     const std::vector<uint8_t>& payload, int max_payload_size) {
    if (max_payload_size <= 0) max_payload_size = static_cast<int>(kMaxFragmentPayload);
    const uint16_t    size  = static_cast<uint16_t>(max_payload_size);
    const std::size_t total = payload.size();
    const uint32_t     num_frags =
        size > 0 ? static_cast<uint32_t>((total + size - 1) / size) : 0;

    std::vector<DataFrag> frags;
    frags.reserve(num_frags);
    std::size_t offset   = 0;
    uint32_t     frag_num = 1; // 1-based
    while (offset < total) {
        std::size_t end = offset + size;
        if (end > total) end = total;

        DataFrag f;
        f.writer_entity_id       = writer_eid;
        f.reader_entity_id       = kEntityIdUnknown;
        f.writer_seq_num         = seq_num;
        f.fragment_starting_num  = frag_num;
        f.fragments_in_submsg    = 1;
        f.fragment_size          = size;
        f.data_size              = static_cast<uint32_t>(total);
        f.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                          payload.begin() + static_cast<std::ptrdiff_t>(end));
        frags.push_back(std::move(f));

        offset = end;
        ++frag_num;
    }
    return frags;
}

// Breaks payload into DataFrag values using the default kMaxFragmentPayload
// size. Matches go-DDS's splitIntoFragments.
inline std::vector<DataFrag> split_into_fragments(const EntityId& writer_eid, const SequenceNumber& seq_num,
                                                   const std::vector<uint8_t>& payload) {
    return split_into_fragments_n(writer_eid, seq_num, payload, static_cast<int>(kMaxFragmentPayload));
}

} // namespace dds::rtps
