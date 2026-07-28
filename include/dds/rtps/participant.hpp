// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/participant.hpp — participant/writer/reader entity lifecycle: the
// point where phases 1-5 (wire types, discovery CDR, UDP transport, SPDP,
// SEDP) are wired together into a working dds::IParticipant over real
// RTPS/UDP, plus (as of Tier-1 phase 7) reliable HEARTBEAT/ACKNACK
// retransmission and TransientLocal disk persistence.
//
// This is Tier-1 sub-phases 6 ("Entities & history cache") and 7 ("Reliable
// delivery") of the cpp-DDS RTPS roadmap (see ROADMAP.md, "Tier 1 — RTPS
// wire protocol"). It is internal, additive scaffolding: `dds::rtps::
// Participant` is a *new*, separate implementation of dds::IParticipant
// living alongside `dds::mock`'s — it is NOT wired into `dds::adapt()`'s
// default selection or any automatic-transport-selection surface (that is
// the still-unchecked `dds/auto/` roadmap item). Callers who want RTPS
// today construct `dds::rtps::Participant::create(...)` explicitly, exactly
// as they would construct `dds::mock::create(...)` explicitly.
//
// Scope: RELAY spec QoS::reliability == Reliable now gets real
// HEARTBEAT-after-write-and-periodic / ACKNACK-on-gap / retransmit-from-
// history delivery (RTPS 2.3 §8.4.9-§8.4.12) — see reliable.hpp and
// persist.hpp for that phase's own scope notes. Writes whose CDR-wrapped
// payload exceeds fragment.hpp's kMaxFragmentPayload are now split into
// DATA_FRAG submessages on send and reassembled on receive (Tier-1 phase 8,
// "Fragmentation" — see fragment.hpp's own file-level scope note for the
// full detail, including the write/retransmit/receive wiring).
// dds::rtps::new_loaning_publisher (rtps/loan.hpp) wraps a Writer with a
// dds::pool::BytePool for zero-copy loaned-sample publishing (Tier-1
// phase 9, "Loan integration") — its LoaningWriter implementation lives in
// this file's .cpp (participant.cpp), the one place the .cpp-local Writer
// type is visible; see loan.hpp's own file-level scope note. Still out of
// scope: no IPv6 (phase 10), no
// security/TSN (Tier 2/3), no INFO_TS-carried publish
// timestamps (Sample::timestamp is always the local wall-clock time of
// publish/receipt — every wire primitive this phase composes — DataSubmessage,
// cdr_wrap_payload, wrap_in_rtps_message, Heartbeat/AckNack/Gap::encode —
// was already byte-verified against go-DDS in phases 1-2/4-5 and this
// phase's own types.hpp additions; this phase introduces no wire encoding
// that isn't pinned by a golden vector somewhere).
//
// C++ port of the entity-lifecycle, best-effort-dispatch, and
// reliable-delivery portions of github.com/SoundMatt/go-DDS's
// rtps/participant.go (`participant`, `rtpsWriter`, `rtpsReader` and their
// methods) plus rtps/reliable.go and rtps/persist.go — the TSN-socket,
// security-plugin, anti-replay, and IPv6 portions of participant.go remain
// explicitly out of scope here; see the roadmap phase list for where each
// lands. Also out of scope: go-DDS's `waitDrain`/`CloseWithDrain`
// (blocking until all writes are ACKed) — not required by this phase's
// roadmap text and not exposed anywhere yet; can be added if a later phase
// needs it.
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
//     write and, as of this phase, consumed for reliable retransmission —
//     see reliable.hpp's file-level scope note on why there is no separate
//     `sendHistory` port (HistoryCache already serves that role).
//   - A reliable Writer's periodic HEARTBEAT is sent from a dedicated
//     background thread per writer (mirroring go-DDS's per-writer
//     `heartbeatLoop` goroutine), started when the writer is created and
//     joined in `Writer::close()`/`~Writer()`. `Participant::close()` closes
//     every still-registered writer (via a new `writers_` weak_ptr registry,
//     mirroring the existing `readers_` registry) so no heartbeat thread
//     outlives the participant, matching go-DDS's `participant.Close()`
//     snapshotting and closing every `rtpsWriter` before tearing down its
//     sockets.
//   - ACKNACK retransmission is broadcast to every SEDP-matched reader
//     locator for the writer's topic (not just the ACKNACK sender), and a
//     GAP is additionally sent (to both the sender and every matched
//     locator) for any requested range already evicted from history —
//     matching go-DDS's `handleAckNack` exactly, including its asymmetry
//     that GAP is sent but never parsed on receipt (go-DDS itself has no
//     `parseGAP`; this port doesn't add one either — see types.hpp's `Gap`
//     doc comment).
//   - `ParticipantOptions::persist_dir` is this port's equivalent of
//     go-DDS's `WithPersistentHistory` option: when non-empty, every
//     writer's every publish is flushed to `<persist_dir>/topic-<topic>.bin`
//     (see persist.hpp), and a `TransientLocal` subscriber falls back to
//     that on-disk copy when no in-memory `last_sample` exists yet (e.g.
//     right after process restart) — matching go-DDS's `NewSubscriber`
//     fallback order exactly.
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
#include <dds/rtps/fragment.hpp>
#include <dds/rtps/history_cache.hpp>
#include <dds/rtps/persist.hpp>
#include <dds/rtps/reliable.hpp>
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

    // How often a reliable writer's background loop re-sends a HEARTBEAT.
    // 0 => kHeartbeatPeriod. Matches go-DDS's WithHeartbeatPeriod option /
    // effectiveHeartbeatPeriod.
    std::chrono::milliseconds heartbeat_period{0};

    // Directory backing TransientLocal durability persistence (see
    // persist.hpp). Empty (the default) disables persistence entirely —
    // matches go-DDS's WithPersistentHistory(dir) option, dir == "" no-op.
    std::string persist_dir;
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

    void register_writer(const EntityId& eid, const std::shared_ptr<Writer>& w);
    void unregister_writer(const EntityId& eid);

    void update_last_sample(const std::string& topic, const Sample& s);
    std::optional<Sample> last_sample(const std::string& topic) const;

    const std::string& persist_dir() const noexcept { return persist_dir_; }
    std::chrono::milliseconds heartbeat_period() const noexcept { return heartbeat_period_; }

    // ── reliable delivery (Tier-1 phase 7) ──────────────────────────────
    // See reliable.hpp / types.hpp (Heartbeat/AckNack/Gap) and
    // participant.hpp's file-level scope notes for what these implement.

    // Updates the RecvTracker of every local reliable reader that accepts
    // writer_guid as a source, and sends ACKNACK if a gap is detected.
    // Matches go-DDS's notifyReliableReaders.
    void notify_reliable_readers(const GUID& writer_guid, const SequenceNumber& seq_num,
                                  const std::string& from_address, int from_port);

    // Responds with ACKNACK for every local reliable reader with a gap
    // against writer_guid's advertised [FirstSN, LastSN] window. Matches
    // go-DDS's handleHeartbeat.
    void handle_heartbeat(const GUID& writer_guid, const Heartbeat& hb, const std::string& from_address,
                           int from_port);

    // Looks up the local writer named by an.writer_entity_id and, if
    // reliable, delegates to Writer::handle_ack_nack for retransmission.
    // Matches go-DDS's handleAckNack.
    void handle_ack_nack(const AckNack& an, const std::string& from_address, int from_port);

    // ── background loops ────────────────────────────────────────────────

    void start_data_loop();
    void stop_data_loop();
    void data_loop();
    void handle_data_packet(const std::vector<uint8_t>& data, const std::string& from_address, int from_port);

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
    std::chrono::milliseconds heartbeat_period_{kHeartbeatPeriod};
    std::string persist_dir_;

    UdpSocket data_sock_;

    // Reassembles incoming DATA_FRAG submessages (Tier-1 phase 8) — see
    // fragment.hpp's file-level scope note for why this is wired into the
    // receive path even though go-DDS's own participant.go never does.
    FragmentAssembler frag_assembler_;

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

    mutable std::mutex                                              writers_mu_;
    std::unordered_map<EntityId, std::weak_ptr<Writer>, EntityIdHash> writers_;

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
