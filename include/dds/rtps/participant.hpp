// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/participant.hpp — participant/writer/reader entity lifecycle: the
// point where phases 1-5 (wire types, discovery CDR, UDP transport, SPDP,
// SEDP) are wired together into a working best-effort dds::IParticipant
// over real RTPS/UDP.
//
// This is Tier-1 sub-phase 6 of the cpp-DDS RTPS roadmap (see ROADMAP.md,
// "Tier 1 — RTPS wire protocol", phase 6: "Entities & history cache"). It is
// internal, additive scaffolding: `dds::rtps::Participant` is a *new*,
// separate implementation of dds::IParticipant living alongside
// `dds::mock`'s — it is NOT wired into `dds::adapt()`'s default selection
// or any automatic-transport-selection surface (that is the still-unchecked
// `dds/auto/` roadmap item). Callers who want RTPS today construct
// `dds::rtps::Participant::create(...)` explicitly, exactly as they would
// construct `dds::mock::create(...)` explicitly.
//
// Scope: **best-effort delivery only** (RELAY spec QoS::reliability ==
// BestEffort is the only path this phase implements end-to-end; passing
// Reliable QoS is accepted — no error — but a Reliable publisher/subscriber
// behaves identically to a BestEffort one until Tier-1 phase 7 ("Reliable
// delivery — HEARTBEAT/ACKNACK") lands and consumes the HistoryCache this
// phase introduces). No fragmentation (phase 8), no loan integration
// (phase 9), no IPv6 (phase 10), no security/TSN (Tier 2/3), no
// INFO_TS-carried publish timestamps (Sample::timestamp is always the local
// wall-clock time of publish/receipt — every wire primitive this phase
// composes — DataSubmessage, cdr_wrap_payload, wrap_in_rtps_message — was
// already byte-verified against go-DDS in phases 1-2/4-5; this phase
// introduces no *new* wire encoding of its own).
//
// C++ port of the entity-lifecycle and best-effort-dispatch portions of
// github.com/SoundMatt/go-DDS's rtps/participant.go (`participant`,
// `rtpsWriter`, `rtpsReader` and their methods) — the reliable-delivery
// (HEARTBEAT/ACKNACK), TSN-socket, security-plugin, anti-replay,
// persistence, and IPv6 portions of that file are explicitly out of scope
// here; see the roadmap phase list for where each lands.
//
// Scope notes (deliberate deviations from a literal line-for-line port):
//
//   - go-DDS's participant owns and shares a single `metaSock` between SPDP
//     announcement sends and SEDP send/receive. Because dds::rtps::UdpSocket
//     is move-only (one owner), this port gives SpdpService its own
//     dedicated send socket (matching spdp.hpp's own scope note) and a
//     separate socket for SedpService — three metadata sockets total
//     (spdp-send, spdp-recv-multicast, sedp) instead of go-DDS's two
//     (mcastSock, metaSock).
//   - SPDP -> SEDP peer wiring (go-DDS: `s.p.sedp.onNewPeer` /
//     `onPeerEvicted` called directly from spdpService) is done here via a
//     participant-owned poll loop that diffs SpdpService::peers() against
//     the previously seen set and calls SedpService::on_new_peer /
//     on_peer_evicted — because SpdpService (phase 4) deliberately has no
//     push-callback hook (see spdp.hpp's own scope note: that hook is
//     "omitted... depend[s] on machinery... that doesn't exist until later
//     phases" — this is that later phase, and a poll loop was chosen over
//     retrofitting a callback into the already-tested SpdpService to avoid
//     touching phase-4's frozen, byte-verified surface).
//   - Local (same-process, same-participant) writer -> reader delivery
//     happens unconditionally by topic-name match, exactly matching
//     go-DDS's dispatchToReaders acceptsSource short-circuit
//     (`g.Prefix == r.p.guidPrefix` always accepts). Remote (received-over-
//     UDP) delivery requires the reader's SEDP-matched-writer-GUID list
//     (queried via SedpService::matched_writer_guids_for_reader) to contain
//     the sending writer's GUID — matching go-DDS's fallback branch of the
//     same acceptsSource check.
//   - HistoryCache (rtps/history_cache.hpp) is populated by every writer
//     write but not yet consumed by anything (no retransmission exists
//     yet) — see history_cache.hpp's own scope note on why it is not a
//     byte-for-byte port of go-DDS's reliable-only `sendHistory`.
//   - `ParticipantOptions::test_mode` exists purely for deterministic
//     tests: it binds every socket to an OS-assigned ephemeral port and
//     sends SPDP announcements via direct unicast to a configured
//     destination instead of the standard multicast group — the same
//     "sandboxed CI has no guaranteed multicast route" accommodation
//     SpdpService/SedpService's own two-instance convergence tests already
//     rely on (see their file-level scope notes). Production participants
//     MUST leave this false.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dds/dds.hpp>
#include <dds/rtps/history_cache.hpp>
#include <dds/rtps/sedp.hpp>
#include <dds/rtps/spdp.hpp>
#include <dds/rtps/transport.hpp>
#include <dds/rtps/types.hpp>

namespace dds::rtps {

// Forward declarations of the IPublisher/ISubscriber implementations —
// defined in participant.cpp, not part of the public surface (matching
// dds::mock's MockPublisher/MockSubscriber, which are likewise .cpp-local).
class Writer;
class Reader;

// ParticipantOptions configures a Participant at creation time. See the
// file-level scope note above for `test_mode`.
struct ParticipantOptions {
    bool        test_mode{false};
    std::string spdp_dest_address;  // test_mode only; empty => "127.0.0.1"
    int         spdp_dest_port{-1}; // test_mode only; required when test_mode is set

    std::chrono::milliseconds spdp_announce_period{0}; // 0 => kSpdpAnnouncePeriod
    std::chrono::milliseconds spdp_jitter{0};          // 0 => no jitter
    // How often the SPDP -> SEDP peer-bridge poll loop runs (see the
    // file-level scope note on why this is a poll rather than a push
    // callback).
    std::chrono::milliseconds bridge_poll_period{200};

    // Per-writer HistoryCache capacity. 0 => kDefaultHistoryDepth.
    std::size_t history_depth{kDefaultHistoryDepth};
};

// Participant is a working dds::IParticipant backed by real RTPS/UDP:
// SPDP participant discovery, SEDP endpoint discovery, and best-effort
// DATA delivery (unicast to every SEDP-matched remote reader, plus
// unconditional same-process delivery to local readers on the same topic).
// C++ port of the entity-lifecycle and best-effort-dispatch portions of
// go-DDS's rtps.participant — see the file-level scope note for exactly
// what is and is not ported at this phase.
class Participant : public IParticipant, public std::enable_shared_from_this<Participant> {
public:
    // Binds sockets, starts SPDP/SEDP and the participant's own receive/
    // bridge threads, and returns a ready-to-use participant. Returns
    // ErrDomainOutOfRange if domain is outside 0-232; returns
    // relay::ErrNotConnected if no socket could be bound (e.g. every
    // participant-index port pair in 0..15 is already taken).
    static std::pair<std::shared_ptr<Participant>, std::error_code>
    create(Domain domain, ParticipantOptions opts = {});

    ~Participant() override;

    Participant(const Participant&)            = delete;
    Participant& operator=(const Participant&) = delete;

    // ── IParticipant ──────────────────────────────────────────────────────

    std::pair<std::shared_ptr<IPublisher>, std::error_code>
    new_publisher(const std::string& topic, QoS qos) override;

    std::pair<std::shared_ptr<ISubscriber>, std::error_code>
    new_subscriber(const std::string& topic, QoS qos,
                   std::vector<relay::SubscriberOption> opts = {}) override;

    Domain domain() const noexcept override { return domain_; }

    std::error_code close() override;

    // ── Inspection (tests, and future wiring into the public API) ──────────

    const GuidPrefix& guid_prefix() const noexcept { return guid_prefix_; }
    int  meta_unicast_port() const noexcept { return meta_unicast_port_; }
    int  data_unicast_port() const noexcept { return data_unicast_port_; }
    SpdpService& spdp() noexcept { return *spdp_; }
    SedpService& sedp() noexcept { return *sedp_; }
    bool is_closed() const noexcept { return closed_.load(); }

private:
    friend class Writer;
    friend class Reader;

    Participant() = default;

    // ── internal API used by Writer/Reader (see the friend declarations
    // above) — not part of the public dds::IParticipant surface, kept
    // public only so Writer/Reader (defined in participant.cpp) don't need
    // full class access via friendship boilerplate for every member. ──────

    uint32_t next_entity_ordinal() noexcept { return entity_counter_.fetch_add(1) + 1; }

    // Delivers payload to every local reader accepting `source`.
    // topic_filter == "" disables topic filtering (used for the UDP receive
    // path, where acceptance is entirely SEDP-match-driven); non-empty
    // topic_filter additionally requires an exact topic-name match (used
    // for the local-write path). Matches go-DDS's dispatchToReaders.
    void dispatch(const GUID& source, const std::string& topic_filter, const std::vector<uint8_t>& payload,
                   std::chrono::system_clock::time_point ts, uint64_t seq_num);

    bool send_data(const std::string& dst_address, int dst_port, const std::vector<uint8_t>& msg);

    void register_reader(const EntityId& eid, const std::shared_ptr<Reader>& r);
    void unregister_reader(const EntityId& eid);

    void update_last_sample(const std::string& topic, const Sample& s);
    std::optional<Sample> last_sample(const std::string& topic) const;

    // ── background loops ────────────────────────────────────────────────

    void start_data_loop();
    void stop_data_loop();
    void data_loop();
    void handle_data_packet(const std::vector<uint8_t>& data, const std::string& from_address);

    void start_bridge_loop();
    void stop_bridge_loop();
    void bridge_loop();
    void sync_peers();

    // ── state ────────────────────────────────────────────────────────────

    Domain     domain_{0};
    GuidPrefix guid_prefix_{};
    int        meta_unicast_port_{0};
    int        data_unicast_port_{0};
    std::size_t history_depth_{kDefaultHistoryDepth};

    UdpSocket data_sock_;

    std::unique_ptr<SpdpService> spdp_;
    std::unique_ptr<SedpService> sedp_;

    std::atomic<bool>     closed_{false};
    std::atomic<uint32_t> entity_counter_{0};

    struct EntityIdHash {
        std::size_t operator()(const EntityId& e) const noexcept;
    };
    struct GuidPrefixHash {
        std::size_t operator()(const GuidPrefix& g) const noexcept;
    };

    mutable std::mutex                                              readers_mu_;
    std::unordered_map<EntityId, std::weak_ptr<Reader>, EntityIdHash> readers_;

    mutable std::mutex                       last_mu_;
    std::unordered_map<std::string, Sample>  last_samples_;

    std::chrono::milliseconds bridge_poll_period_{200};
    std::atomic<bool>         bridge_running_{false};
    std::thread                bridge_thread_;
    mutable std::mutex                                     known_peers_mu_;
    std::unordered_map<GuidPrefix, bool, GuidPrefixHash>   known_peers_;

    std::atomic<bool> data_running_{false};
    std::thread        data_thread_;
};

} // namespace dds::rtps
