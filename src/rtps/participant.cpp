// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/participant.hpp>

// C++ port of the entity-lifecycle and best-effort-dispatch portions of
// github.com/SoundMatt/go-DDS rtps/participant.go. See
// include/dds/rtps/participant.hpp for the phase scope and the deliberate
// deviations from a literal line-for-line port.
//
// Also implements Tier-1 phase 9 ("Loan integration", rtps/loan.hpp) —
// LoaningWriter and new_loaning_publisher live here, not in a separate
// loan.cpp, because they need the concrete (.cpp-local) Writer type; see
// loan.hpp's file-level scope note.

#include <algorithm>
#include <cstdio>

#include <dds/pool/pool.hpp>
#include <dds/rtps/loan.hpp>

namespace dds::rtps {

namespace {

// Converts a UDPv4 Locator to a dotted-quad address + port pair for
// UdpSocket::send_to. Mirrors go-DDS's Locator.udpAddr(); duplicated here
// rather than shared, matching sedp.cpp's own copy of the same helper
// (spdp.cpp/sedp.cpp already established this duplication pattern for
// small address-formatting helpers in this codebase).
// Also handles UDPv6-kind locators (Tier-1 phase 10) — formatted as
// uncompressed colon-hex, valid UdpSocket::send_to input (see
// transport.cpp's format_ipv6, which this mirrors). No wire-format change:
// Locator itself has always decoded UDPv6-kind values correctly (byte-
// verified since phase 1/2 — see tests/test_rtps_types.cpp); this is
// purely address-string formatting for the send path. In practice this
// branch is not currently reachable from a real go-DDS peer, which never
// advertises a UDPv6-kind Locator over SEDP (see participant.hpp's
// file-level scope note) — it exists for any wire-conformant peer that
// does, and for symmetry with Locator's own generality.
bool locator_to_dest(const Locator& l, std::string& addr_out, int& port_out) {
    if (l.kind == Locator::kKindUDPv4) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", l.address[12], l.address[13], l.address[14],
                      l.address[15]);
        addr_out = buf;
        port_out = static_cast<int>(l.port);
        return true;
    }
    if (l.kind == Locator::kKindUDPv6) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%x:%x:%x:%x:%x:%x:%x:%x",
                      (l.address[0] << 8) | l.address[1], (l.address[2] << 8) | l.address[3],
                      (l.address[4] << 8) | l.address[5], (l.address[6] << 8) | l.address[7],
                      (l.address[8] << 8) | l.address[9], (l.address[10] << 8) | l.address[11],
                      (l.address[12] << 8) | l.address[13], (l.address[14] << 8) | l.address[15]);
        addr_out = buf;
        port_out = static_cast<int>(l.port);
        return true;
    }
    return false;
}

// Packs an rtps::GUID (prefix || entity) into the dds::Guid wire shape used
// by dds::Sample::writer_guid (plain 16-byte concatenation, no endianness
// concern — matches GUID::encode's own wire format).
Guid pack_guid(const GUID& g) {
    Guid out{};
    std::copy(g.prefix.bytes.begin(), g.prefix.bytes.end(), out.begin());
    std::copy(g.entity.bytes.begin(), g.entity.bytes.end(), out.begin() + GuidPrefix::kSize);
    return out;
}

// sn_to_u64/u64_to_sn now live in reliable.hpp (phase 7) — see that header's
// file-level scope note; this phase-6 comment's original rationale for a
// local duplicate ("phase 7 ... does not exist yet") no longer applies.

// Builds the wire message(s) for one write of `wrapped` (already
// CDR-wrapped) payload bytes at sequence number seq_num: a single DATA
// submessage when small enough to fit under fragment.hpp's
// kMaxFragmentPayload, or one message per DATA_FRAG fragment otherwise
// (Tier-1 phase 8, "Fragmentation" — see fragment.hpp's file-level scope
// note). Shared by Writer::write and Writer::handle_ack_nack so
// retransmission fragments exactly like a live write of the same payload
// would — a correctness improvement over go-DDS's own known limitation
// (its sendHistory retains only the first fragment's wire bytes for a
// fragmented write, so its ACKNACK-triggered retransmit of a fragmented
// sample only ever resends fragment #1; see fragment.hpp for the full
// rationale).
std::vector<std::vector<uint8_t>> build_data_messages(const GuidPrefix& prefix, const EntityId& writer_eid,
                                                        const SequenceNumber& seq_num,
                                                        const std::vector<uint8_t>& wrapped) {
    std::vector<std::vector<uint8_t>> msgs;
    if (wrapped.size() > kMaxFragmentPayload) {
        auto frags = split_into_fragments(writer_eid, seq_num, wrapped);
        msgs.reserve(frags.size());
        for (const auto& frag : frags) {
            std::vector<uint8_t> submsg;
            frag.encode(submsg);
            msgs.push_back(wrap_in_rtps_message(prefix, kVendorIdCppDDS, submsg));
        }
    } else {
        DataSubmessage ds;
        ds.reader_entity_id = kEntityIdUnknown;
        ds.writer_entity_id = writer_eid;
        ds.seq_num            = seq_num;
        ds.payload             = wrapped;
        std::vector<uint8_t> submsg;
        ds.encode(submsg);
        msgs.push_back(wrap_in_rtps_message(prefix, kVendorIdCppDDS, submsg));
    }
    return msgs;
}

constexpr auto kShutdownPollSlice = std::chrono::milliseconds(50);

// FNV-1a hash for GUID, used to key the per-remote-writer RecvTracker map
// on the reader side (Tier-1 phase 7). Matches the FNV-1a construction
// Participant::EntityIdHash/GuidPrefixHash already use for the same reason.
struct GuidHash {
    std::size_t operator()(const GUID& g) const noexcept {
        std::size_t h = 1469598103934665603ull; // FNV-1a
        for (uint8_t b : g.prefix.bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
        for (uint8_t b : g.entity.bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};

} // namespace

std::size_t Participant::EntityIdHash::operator()(const EntityId& e) const noexcept {
    std::size_t h = 1469598103934665603ull; // FNV-1a
    for (uint8_t b : e.bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::size_t Participant::GuidPrefixHash::operator()(const GuidPrefix& g) const noexcept {
    std::size_t h = 1469598103934665603ull; // FNV-1a
    for (uint8_t b : g.bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

// ── Reader ───────────────────────────────────────────────────────────────────

// Reader implements dds::ISubscriber over the participant's dispatch
// mechanism. Not exposed in participant.hpp — matching dds::mock's
// MockSubscriber, callers only ever see it through the ISubscriber
// interface returned by Participant::new_subscriber.
class Reader : public ISubscriber, public std::enable_shared_from_this<Reader> {
public:
    Reader(std::shared_ptr<Participant> p, std::string topic, EntityId eid, int depth,
           relay::BackPressurePolicy back_pressure, std::function<bool(const Sample&)> filter, bool reliable)
        : p_(std::move(p))
        , topic_(std::move(topic))
        , eid_(eid)
        , ch_(std::make_shared<dds::Chan<Sample>>(static_cast<std::size_t>(depth)))
        , back_pressure_(back_pressure)
        , filter_(std::move(filter))
        , reliable_(reliable)
    {}

    ~Reader() override { unsubscribe(); }

    std::shared_ptr<dds::Chan<Sample>> channel() override { return ch_; }

    std::optional<Sample> try_read() override { return ch_->try_recv(); }

    void unsubscribe() override {
        bool was_open = !unsubscribed_.exchange(true);
        if (was_open) {
            p_->unregister_reader(eid_);
            ch_->close();
        }
    }

    std::error_code close() override {
        unsubscribe();
        return {};
    }

    // ── internal (used by Participant::dispatch) ────────────────────────────

    const std::string& topic() const noexcept { return topic_; }
    const EntityId&     entity_id() const noexcept { return eid_; }
    bool                reliable() const noexcept { return reliable_; }

    // Returns (creating if necessary) the RecvTracker for writer_guid.
    // Matches go-DDS's rtpsReader.trackerFor.
    std::shared_ptr<RecvTracker> tracker_for(const GUID& writer_guid) {
        std::lock_guard<std::mutex> lock(trackers_mu_);
        auto it = trackers_.find(writer_guid);
        if (it != trackers_.end()) return it->second;
        auto t = std::make_shared<RecvTracker>();
        trackers_.emplace(writer_guid, t);
        return t;
    }

    // DeliverResult reports what deliver() actually did, so callers (i.e.
    // Participant::dispatch) can attribute metrics correctly: a Filtered
    // sample was never a delivery attempt at all — matching go-DDS's
    // dispatchToReaders, whose own reader-filter check happens *before*
    // calling deliverToReader (the metrics-counting function), so a
    // filtered-out sample never touches mDelivers/mDrops either.
    enum class DeliverResult { Filtered, Delivered, Dropped };

    // Applies the sample filter (if any) and enqueues per back_pressure_.
    // Matches go-DDS's rtpsReader delivery path inlined into
    // participant.deliverToReader.
    DeliverResult deliver(const Sample& s) {
        if (filter_ && !filter_(s)) return DeliverResult::Filtered;
        bool delivered = false;
        switch (back_pressure_) {
            case relay::BackPressurePolicy::DropOldest:
                delivered = ch_->send_drop_oldest(s);
                break;
            case relay::BackPressurePolicy::Block:
                delivered = ch_->send(s);
                break;
            default: // DropNewest
                delivered = (ch_->try_send(s) == dds::Chan<Sample>::SendResult::Ok);
                break;
        }
        return delivered ? DeliverResult::Delivered : DeliverResult::Dropped;
    }

    // Closes the channel directly (used by Participant::close() to
    // unblock any blocked recv() without going through the participant's
    // own readers_ map removal, matching dds::mock's Close() behavior of
    // closing subscriber channels without a full unsubscribe pass).
    void close_channel() { ch_->close(); }

private:
    std::shared_ptr<Participant>        p_;
    std::string                          topic_;
    EntityId                              eid_;
    std::shared_ptr<dds::Chan<Sample>>   ch_;
    relay::BackPressurePolicy             back_pressure_;
    std::function<bool(const Sample&)>   filter_;
    std::atomic<bool>                     unsubscribed_{false};
    bool                                   reliable_{false};

    std::mutex                                              trackers_mu_;
    std::unordered_map<GUID, std::shared_ptr<RecvTracker>, GuidHash> trackers_;
};

// ── Writer ───────────────────────────────────────────────────────────────────

// Writer implements dds::IPublisher: best-effort DATA (or DATA_FRAG, for
// large payloads — Tier-1 phase 8) send to every SEDP-matched remote reader
// locator plus unconditional local (same-participant) delivery. C++ port of
// the best-effort subset of go-DDS's rtpsWriter.Write (rtps/participant.go)
// — see participant.hpp's file-level scope note for what is intentionally
// not ported (TSN, security).
class Writer : public IPublisher {
public:
    Writer(std::shared_ptr<Participant> p, std::string topic, EntityId eid, QoS qos, std::size_t history_depth,
           std::chrono::milliseconds hb_period)
        : p_(std::move(p))
        , topic_(std::move(topic))
        , eid_(eid)
        , qos_(qos)
        , history_(history_depth)
        , reliable_(qos.reliability == ReliabilityKind::Reliable)
        , hb_period_(hb_period)
    {
        // A reliable writer runs its own periodic-HEARTBEAT thread for as
        // long as the writer is open, matching go-DDS's per-writer
        // `heartbeatLoop` goroutine (rtps/participant.go). Capturing `this`
        // (not shared_from_this) is safe: the thread is always stopped and
        // joined in close()/~Writer() before the object can be destroyed.
        if (reliable_) {
            hb_running_.store(true);
            hb_thread_ = std::thread(&Writer::heartbeat_loop, this);
        }
    }

    ~Writer() override { close(); }

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    std::error_code write(const std::vector<uint8_t>& payload) override {
        return write(relay::Context::background(), payload);
    }

    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override {
        if (closed_.load() || p_->is_closed()) return dds::ErrClosed();
        if (ctx.done()) return dds::ErrTimeout();
        if (qos_.max_sample_size > 0 && static_cast<int>(payload.size()) > qos_.max_sample_size) {
            return dds::ErrPayloadTooLarge();
        }

        const uint64_t seq = seq_.fetch_add(1) + 1;
        const auto     now = std::chrono::system_clock::now();
        const GUID     source{p_->guid_prefix(), eid_};
        const SequenceNumber seq_num = u64_to_sn(seq);

        // Metrics: write_count_/bytes_written_ (participant-level) and this
        // topic's TopicCounter — matches go-DDS's w.p.mWrites.Add(1) /
        // topicTC.writes.Add(1) at the top of rtpsWriter.Write.
        // fusa:req REQ-METRICS-001 REQ-METRICS-005
        p_->record_write(topic_, payload.size());

        // Build wire bytes from existing, already byte-verified primitives
        // only (cdr_wrap_payload from phase 2, DataSubmessage::encode from
        // phase 1, wrap_in_rtps_message from phase 4, DataFrag::encode from
        // phase 8) — this phase introduces no new wire encoding of its own
        // beyond what build_data_messages already composes from pinned
        // primitives (see participant.hpp's file-level scope note).
        // build_data_messages fragments into DATA_FRAG submessages when the
        // CDR-wrapped payload exceeds fragment.hpp's kMaxFragmentPayload
        // (Tier-1 phase 8, "Fragmentation").
        auto msgs = build_data_messages(p_->guid_prefix(), eid_, seq_num, cdr_wrap_payload(payload));

        // HistoryCache: this writer's sequence-number-indexed retained
        // window. Populated for every writer regardless of QoS (matching
        // go-DDS's unconditional history store); consumed for reliable
        // retransmission by handle_ack_nack below (Tier-1 phase 7).
        CacheChange change;
        change.sequence_number = seq;
        change.writer_guid      = source;
        change.payload           = payload;
        change.timestamp         = now;
        history_.store(std::move(change));

        // TransientLocal durability: cache the last published sample for
        // this topic so a late-joining subscriber can receive it.
        if (qos_.durability == DurabilityKind::TransientLocal) {
            Sample s;
            s.topic           = topic_;
            s.payload          = payload;
            s.timestamp        = now;
            s.sequence_number  = seq;
            s.writer_guid      = pack_guid(source);
            p_->update_last_sample(topic_, s);
        }

        // Disk-backed durability persistence: flushed on every write
        // regardless of this writer's own QoS (matches go-DDS's
        // unconditional persistFlush call in Write()); a no-op when
        // ParticipantOptions::persist_dir is empty.
        persist_flush(p_->persist_dir(), topic_, payload);

        // Local (same-process, same-participant) delivery: unconditional,
        // topic-name-matched only — matches go-DDS's
        // dispatchToReaders acceptsSource short-circuit for
        // `source.Prefix == r.p.guidPrefix`.
        p_->dispatch(source, topic_, payload, now, seq);

        // Remote delivery: unicast every message (a single DATA, or every
        // DATA_FRAG fragment) to every SEDP-matched reader locator for this
        // topic. Matches go-DDS's Write() loop order (locator outer,
        // message inner).
        for (const auto& loc : p_->sedp().matched_reader_locators_for_topic(topic_)) {
            std::string addr;
            int          port = 0;
            if (locator_to_dest(loc, addr, port)) {
                for (const auto& m : msgs) p_->send_data(addr, port, m);
            }
        }

        // Send HEARTBEAT immediately after each reliable write so remote
        // readers can detect gaps without waiting for the periodic
        // heartbeat thread, matching go-DDS's Write().
        if (reliable_) send_heartbeat();

        return {};
    }

    std::error_code close() override {
        if (closed_.exchange(true)) return {};
        if (hb_running_.exchange(false)) {
            if (hb_thread_.joinable()) hb_thread_.join();
        }
        p_->unregister_writer(eid_);
        return {};
    }

    // is_closed reports whether close() has already run. Used by
    // LoaningWriter::loan_buffer (Tier-1 phase 9) to reject new loans
    // against a closed writer, matching go-DDS's loaningWriter.Loan()
    // checking rtpsWriter.closed directly (see loan.hpp's file-level scope
    // note on why that direct-field-access pattern becomes a public
    // accessor here instead).
    bool is_closed() const noexcept { return closed_.load(); }

    // ── reliable delivery (Tier-1 phase 7) ──────────────────────────────

    bool reliable() const noexcept { return reliable_; }

    // Builds and sends a HEARTBEAT to every SEDP-matched reader locator for
    // this topic, advertising the HistoryCache's retained [first, last]
    // span. No-op while the history is empty (nothing to advertise yet).
    // Matches go-DDS's sendHeartbeatLocked.
    void send_heartbeat() {
        if (history_.empty()) return;
        const auto [first, last] = history_.span();

        Heartbeat hb;
        hb.reader_entity_id = kEntityIdUnknown;
        hb.writer_entity_id = eid_;
        hb.first_sn           = u64_to_sn(first);
        hb.last_sn             = u64_to_sn(last);
        hb.count                = hb_count_.fetch_add(1) + 1;

        std::vector<uint8_t> submsg;
        hb.encode(submsg);
        auto msg = wrap_in_rtps_message(p_->guid_prefix(), kVendorIdCppDDS, submsg);

        for (const auto& loc : p_->sedp().matched_reader_locators_for_topic(topic_)) {
            std::string addr;
            int          port = 0;
            if (locator_to_dest(loc, addr, port)) p_->send_data(addr, port, msg);
        }
    }

    // Retransmits every requested-and-still-retained sequence number from
    // history to every SEDP-matched reader locator for this topic (not just
    // the ACKNACK sender), and sends a GAP (to the sender and every matched
    // locator) for the leading portion of the requested range already
    // evicted from history. Matches go-DDS's handleAckNack.
    void handle_ack_nack(const AckNack& an, const std::string& from_address, int from_port) {
        if (!reliable_) return;
        const uint64_t ack_base = sn_to_u64(an.base);

        for (uint64_t bit = 0; bit < 32; ++bit) {
            if ((an.bitmap & (1u << static_cast<unsigned>(bit))) == 0) continue;
            const uint64_t seq = ack_base + bit;
            auto            change = history_.get(seq);
            if (!change) continue;

            // Re-fragments when the payload is still over threshold,
            // matching a live write of the same payload — see
            // build_data_messages's own doc comment for why this is a
            // deliberate correctness improvement over go-DDS's own
            // first-fragment-only retransmit (Tier-1 phase 8).
            auto msgs = build_data_messages(p_->guid_prefix(), eid_, u64_to_sn(seq),
                                             cdr_wrap_payload(change->payload));

            for (const auto& loc : p_->sedp().matched_reader_locators_for_topic(topic_)) {
                std::string addr;
                int          port = 0;
                if (locator_to_dest(loc, addr, port)) {
                    for (const auto& m : msgs) p_->send_data(addr, port, m);
                }
            }
        }

        if (history_.empty()) return;
        const auto [hist_first, hist_last] = history_.span();
        (void)hist_last;
        if (ack_base >= hist_first) return;

        uint64_t gap_end = hist_first - 1;
        if (const uint64_t max_bit = ack_base + 31; gap_end > max_bit) gap_end = max_bit;

        Gap g;
        g.reader_entity_id = an.reader_entity_id;
        g.writer_entity_id = eid_;
        g.gap_start           = u64_to_sn(ack_base);
        g.gap_end              = u64_to_sn(gap_end);
        std::vector<uint8_t> gap_submsg;
        g.encode(gap_submsg);
        auto gap_msg = wrap_in_rtps_message(p_->guid_prefix(), kVendorIdCppDDS, gap_submsg);

        if (from_port != 0) p_->send_data(from_address, from_port, gap_msg);
        for (const auto& loc : p_->sedp().matched_reader_locators_for_topic(topic_)) {
            std::string addr;
            int          port = 0;
            if (locator_to_dest(loc, addr, port)) p_->send_data(addr, port, gap_msg);
        }
    }

private:
    // Background loop for a reliable writer's periodic HEARTBEAT, matching
    // go-DDS's heartbeatLoop. Shutdown-responsive in kShutdownPollSlice
    // increments, mirroring Participant::bridge_loop's identical pattern.
    void heartbeat_loop() {
        while (hb_running_.load(std::memory_order_relaxed)) {
            std::chrono::milliseconds elapsed{0};
            while (hb_running_.load(std::memory_order_relaxed) && elapsed < hb_period_) {
                auto slice = std::min(kShutdownPollSlice, hb_period_ - elapsed);
                std::this_thread::sleep_for(slice);
                elapsed += slice;
            }
            if (!hb_running_.load(std::memory_order_relaxed)) return;
            if (!closed_.load(std::memory_order_relaxed)) send_heartbeat();
        }
    }

    std::shared_ptr<Participant> p_;
    std::string                    topic_;
    EntityId                        eid_;
    QoS                              qos_;
    std::atomic<uint64_t>           seq_{0};
    std::atomic<bool>               closed_{false};
    HistoryCache                    history_;

    // ── reliable delivery (Tier-1 phase 7) ──────────────────────────────
    bool                             reliable_{false};
    std::chrono::milliseconds       hb_period_{kHeartbeatPeriod};
    std::atomic<int32_t>            hb_count_{0};
    std::atomic<bool>               hb_running_{false};
    std::thread                      hb_thread_;
};

// ── Participant ────────────────────────────────────────────────────────────

Participant::~Participant() { close(); }

std::pair<std::shared_ptr<Participant>, std::error_code>
Participant::create(Domain domain, ParticipantOptions opts) {
    if (auto ec = validate_domain(domain); ec) return {nullptr, ec};

    UdpSocket sedp_sock;
    UdpSocket data_sock;
    UdpSocket spdp_send_sock;
    UdpSocket spdp_recv_sock;
    int        meta_port = 0;
    int        data_port = 0;
    int        participant_idx = 0; // used to re-derive matching IPv6 port numbers below

    if (opts.test_mode) {
        auto s1 = UdpSocket::bind_unicast(0);
        auto s2 = UdpSocket::bind_unicast(0);
        auto s3 = UdpSocket::bind_unicast(0);
        auto s4 = UdpSocket::bind_unicast(0);
        if (!s1 || !s2 || !s3 || !s4) return {nullptr, relay::ErrNotConnected()};
        sedp_sock       = std::move(*s1);
        data_sock       = std::move(*s2);
        spdp_send_sock  = std::move(*s3);
        spdp_recv_sock  = std::move(*s4);
        meta_port        = sedp_sock.port();
        data_port         = data_sock.port();
    } else {
        // Matches go-DDS's newParticipant: try participant index 0..15 for
        // a free (meta_unicast_port, data_unicast_port) pair.
        bool bound = false;
        for (int i = 0; i < 16 && !bound; ++i) {
            // Fully qualified: dds::rtps::Participant's own
            // meta_unicast_port()/data_unicast_port() *member* accessors
            // (declared in participant.hpp) would otherwise hide the
            // free functions of the same name from transport.hpp inside
            // this member function's scope.
            auto s1 = UdpSocket::bind_unicast(dds::rtps::meta_unicast_port(domain, i));
            if (!s1) continue;
            auto s2 = UdpSocket::bind_unicast(dds::rtps::data_unicast_port(domain, i));
            if (!s2) continue; // s1 closes via its own destructor
            sedp_sock = std::move(*s1);
            data_sock = std::move(*s2);
            meta_port  = sedp_sock.port();
            data_port   = data_sock.port();
            participant_idx = i;
            bound       = true;
        }
        if (!bound) return {nullptr, relay::ErrNotConnected()};

        auto s3 = UdpSocket::bind_unicast(0);
        if (!s3) return {nullptr, relay::ErrNotConnected()};
        spdp_send_sock = std::move(*s3);

        auto s4 = UdpSocket::bind_multicast_receive(kSpdpMulticastAddr, meta_multicast_port(domain));
        if (!s4) return {nullptr, relay::ErrNotConnected()};
        spdp_recv_sock = std::move(*s4);
    }

    // Optional IPv6 sockets (Tier-1 phase 10). Every failure here is soft
    // (participant creation still succeeds IPv4-only), matching go-DDS's
    // "Optional IPv6 sockets. Failures are soft" comment in newParticipant
    // verbatim — see participant.hpp's file-level scope note for exactly
    // what mcast_sock_v6/meta_sock_v6 (bound-but-unconsumed) and
    // data_sock_v6 (wired into data_loop_v6) do and don't enable.
    UdpSocket mcast_sock_v6;
    UdpSocket meta_sock_v6;
    UdpSocket data_sock_v6;
    if (opts.ipv6) {
        if (opts.test_mode) {
            if (auto s = UdpSocket::bind_unicast_v6(0)) mcast_sock_v6 = std::move(*s);
            if (auto s = UdpSocket::bind_unicast_v6(0)) meta_sock_v6 = std::move(*s);
            if (auto s = UdpSocket::bind_unicast_v6(0)) data_sock_v6 = std::move(*s);
        } else {
            if (auto s = UdpSocket::bind_multicast_receive_v6(kSpdpMulticastAddrV6,
                                                                dds::rtps::meta_multicast_port(domain))) {
                mcast_sock_v6 = std::move(*s);
            }
            if (auto s = UdpSocket::bind_unicast_v6(dds::rtps::meta_unicast_port(domain, participant_idx))) {
                meta_sock_v6 = std::move(*s);
            }
            if (auto s = UdpSocket::bind_unicast_v6(dds::rtps::data_unicast_port(domain, participant_idx))) {
                data_sock_v6 = std::move(*s);
            }
        }
    }

    auto p = std::shared_ptr<Participant>(new Participant());
    p->domain_             = domain;
    p->guid_prefix_        = new_guid_prefix();
    p->meta_unicast_port_  = meta_port;
    p->data_unicast_port_  = data_port;
    p->history_depth_       = opts.history_depth > 0 ? opts.history_depth : kDefaultHistoryDepth;
    p->bridge_poll_period_  = opts.bridge_poll_period.count() > 0 ? opts.bridge_poll_period
                                                                    : std::chrono::milliseconds(200);
    p->heartbeat_period_    = opts.heartbeat_period.count() > 0 ? opts.heartbeat_period : kHeartbeatPeriod;
    p->persist_dir_          = opts.persist_dir;
    p->data_sock_           = std::move(data_sock);
    p->mcast_sock_v6_       = std::move(mcast_sock_v6);
    p->meta_sock_v6_        = std::move(meta_sock_v6);
    p->data_sock_v6_        = std::move(data_sock_v6);

    SpdpConfig spdp_cfg;
    spdp_cfg.domain                      = domain;
    spdp_cfg.local.guid_prefix           = p->guid_prefix_;
    spdp_cfg.local.meta_unicast_port     = static_cast<uint16_t>(meta_port);
    spdp_cfg.local.default_unicast_port  = static_cast<uint16_t>(data_port);
    spdp_cfg.local.vendor_id              = kVendorIdCppDDS;
    if (opts.spdp_announce_period.count() > 0) spdp_cfg.announce_period = opts.spdp_announce_period;
    if (opts.spdp_jitter.count() > 0) spdp_cfg.jitter = opts.spdp_jitter;
    if (opts.test_mode) {
        spdp_cfg.dest_address = opts.spdp_dest_address.empty() ? std::string("127.0.0.1") : opts.spdp_dest_address;
        spdp_cfg.dest_port     = opts.spdp_dest_port;
    }
    p->spdp_ = std::make_unique<SpdpService>(spdp_cfg, std::move(spdp_send_sock), std::move(spdp_recv_sock));

    SedpConfig sedp_cfg;
    sedp_cfg.local_guid_prefix = p->guid_prefix_;
    sedp_cfg.data_unicast_port  = static_cast<uint16_t>(data_port);
    sedp_cfg.vendor_id           = kVendorIdCppDDS;
    p->sedp_ = std::make_unique<SedpService>(sedp_cfg, std::move(sedp_sock));

    p->spdp_->start();
    p->sedp_->start();
    p->start_data_loop();
    p->start_data_loop_v6(); // no-op if data_sock_v6_ didn't bind (see above)
    p->start_bridge_loop();

    return {p, {}};
}

std::pair<std::shared_ptr<IPublisher>, std::error_code>
Participant::new_publisher(const std::string& topic, QoS qos) {
    if (closed_.load()) return {nullptr, dds::ErrClosed()};
    if (topic.empty()) return {nullptr, dds::ErrTopicEmpty()};

    const uint32_t n   = next_entity_ordinal();
    const EntityId eid = entity_id_for_writer(n);

    auto w = std::make_shared<Writer>(shared_from_this(), topic, eid, qos, history_depth_, heartbeat_period_);
    sedp_->register_writer(eid, topic);
    register_writer(eid, w);
    return {w, {}};
}

std::pair<std::shared_ptr<ISubscriber>, std::error_code>
Participant::new_subscriber(const std::string& topic, QoS qos, std::vector<relay::SubscriberOption> opts) {
    if (closed_.load()) return {nullptr, dds::ErrClosed()};
    if (topic.empty()) return {nullptr, dds::ErrTopicEmpty()};

    relay::SubscriberConfig cfg   = relay::apply_options(opts);
    const int                 depth = cfg.effective_depth(static_cast<int>(kDefaultChanDepth));

    const uint32_t n   = next_entity_ordinal();
    const EntityId eid = entity_id_for_reader(n);

    std::function<bool(const Sample&)> sample_filter;
    if (cfg.filter) {
        sample_filter = [f = cfg.filter](const Sample& s) { return f(s.to_message()); };
    }

    const bool reliable = qos.reliability == ReliabilityKind::Reliable;
    auto reader = std::make_shared<Reader>(shared_from_this(), topic, eid, depth, cfg.back_pressure,
                                            std::move(sample_filter), reliable);
    register_reader(eid, reader);
    sedp_->register_reader(eid, topic);

    // TransientLocal: deliver the last published sample to the new
    // subscriber, if one exists. Falls back to the on-disk persisted
    // sample (ParticipantOptions::persist_dir) when no in-memory
    // last_sample exists yet (e.g. right after a process restart), matching
    // go-DDS's NewSubscriber fallback order exactly.
    if (qos.durability == DurabilityKind::TransientLocal) {
        if (auto last = last_sample(topic)) {
            reader->deliver(*last);
        } else if (!persist_dir_.empty()) {
            if (auto payload = persist_load(persist_dir_, topic)) {
                Sample s;
                s.topic   = topic;
                s.payload = *payload;
                update_last_sample(topic, s);
                reader->deliver(s);
            }
        }
    }

    return {reader, {}};
}

std::error_code Participant::close() {
    if (closed_.exchange(true)) return {};

    // Snapshot and close every still-registered writer first (stops each
    // reliable writer's heartbeat thread), matching go-DDS's
    // participant.Close() snapshotting and closing every rtpsWriter before
    // tearing down sockets.
    std::vector<std::shared_ptr<Writer>> writers;
    {
        std::lock_guard<std::mutex> lock(writers_mu_);
        writers.reserve(writers_.size());
        for (auto& [eid, weak] : writers_) {
            (void)eid;
            if (auto w = weak.lock()) writers.push_back(std::move(w));
        }
    }
    for (auto& w : writers) w->close();

    stop_bridge_loop();
    stop_data_loop();
    stop_data_loop_v6();
    if (spdp_) spdp_->stop();
    if (sedp_) sedp_->stop();
    data_sock_.close();
    mcast_sock_v6_.close();
    meta_sock_v6_.close();
    data_sock_v6_.close();

    std::vector<std::shared_ptr<Reader>> readers;
    {
        std::lock_guard<std::mutex> lock(readers_mu_);
        readers.reserve(readers_.size());
        for (auto& [eid, weak] : readers_) {
            (void)eid;
            if (auto r = weak.lock()) readers.push_back(std::move(r));
        }
        readers_.clear();
    }
    for (auto& r : readers) r->close_channel();

    return {};
}

void Participant::dispatch(const GUID& source, const std::string& topic_filter,
                            const std::vector<uint8_t>& payload, std::chrono::system_clock::time_point ts,
                            uint64_t seq_num) {
    std::vector<std::shared_ptr<Reader>> snapshot;
    {
        std::lock_guard<std::mutex> lock(readers_mu_);
        snapshot.reserve(readers_.size());
        for (auto& [eid, weak] : readers_) {
            (void)eid;
            if (auto r = weak.lock()) snapshot.push_back(std::move(r));
        }
    }

    const Guid writer_dds = pack_guid(source);

    for (auto& r : snapshot) {
        if (!topic_filter.empty() && r->topic() != topic_filter) continue;

        bool accepted = (source.prefix == guid_prefix_);
        if (!accepted) {
            auto matched = sedp_->matched_writer_guids_for_reader(r->entity_id());
            accepted       = std::find(matched.begin(), matched.end(), source) != matched.end();
        }
        if (!accepted) continue;

        Sample s;
        s.topic           = r->topic();
        s.payload          = payload;
        s.timestamp        = ts;
        s.sequence_number  = seq_num;
        s.writer_guid      = writer_dds;

        // Metrics: deliver_count_/drop_count_ (participant-level) and
        // r->topic()'s TopicCounter — matches go-DDS's
        // deliverToReader/topicCounterFor. A Filtered result means the
        // reader's own filter rejected the sample *before* an enqueue was
        // even attempted, so (matching go-DDS's dispatchToReaders, whose
        // filter check happens before calling deliverToReader at all) it is
        // not counted as either a delivery or a drop.
        // fusa:req REQ-METRICS-002 REQ-METRICS-005
        const auto byte_len = static_cast<uint64_t>(payload.size());
        switch (r->deliver(s)) {
        case Reader::DeliverResult::Delivered: {
            deliver_count_.fetch_add(1);
            bytes_delivered_.fetch_add(byte_len);
            auto tc = topic_counter_for(r->topic());
            tc->deliver_count.fetch_add(1);
            tc->bytes_delivered.fetch_add(byte_len);
            break;
        }
        case Reader::DeliverResult::Dropped: {
            drop_count_.fetch_add(1);
            auto tc = topic_counter_for(r->topic());
            tc->drop_count.fetch_add(1);
            break;
        }
        case Reader::DeliverResult::Filtered:
            break;
        }
    }
}

bool Participant::send_data(const std::string& dst_address, int dst_port, const std::vector<uint8_t>& msg) {
    return data_sock_.send_to(dst_address, dst_port, msg.data(), msg.size());
}

void Participant::register_reader(const EntityId& eid, const std::shared_ptr<Reader>& r) {
    std::lock_guard<std::mutex> lock(readers_mu_);
    readers_[eid] = r;
}

void Participant::unregister_reader(const EntityId& eid) {
    std::lock_guard<std::mutex> lock(readers_mu_);
    readers_.erase(eid);
}

void Participant::register_writer(const EntityId& eid, const std::shared_ptr<Writer>& w) {
    std::lock_guard<std::mutex> lock(writers_mu_);
    writers_[eid] = w;
}

void Participant::unregister_writer(const EntityId& eid) {
    std::lock_guard<std::mutex> lock(writers_mu_);
    writers_.erase(eid);
}

void Participant::update_last_sample(const std::string& topic, const Sample& s) {
    std::lock_guard<std::mutex> lock(last_mu_);
    last_samples_[topic] = s;
}

std::optional<Sample> Participant::last_sample(const std::string& topic) const {
    std::lock_guard<std::mutex> lock(last_mu_);
    auto                          it = last_samples_.find(topic);
    if (it == last_samples_.end()) return std::nullopt;
    return it->second;
}

// ── metrics (relay::IMetricsProvider / IDiscoveryMetricsProvider /
//    ITopicMetricsProvider) ───────────────────────────────────────────────────
// C++ port of go-DDS's rtps.participant Metrics/DiscoveryMetrics/TopicMetrics
// (rtps/participant.go) — see participant.hpp's file-level scope note.

// Returns (creating if necessary) the per-topic counter for topic. Matches
// go-DDS's participant.topicCounterFor.
// fusa:req REQ-METRICS-005
std::shared_ptr<TopicCounter> Participant::topic_counter_for(const std::string& topic) {
    std::lock_guard<std::mutex> lock(topic_metrics_mu_);
    auto it = topic_metrics_.find(topic);
    if (it != topic_metrics_.end()) return it->second;
    auto tc = std::make_shared<TopicCounter>();
    topic_metrics_.emplace(topic, tc);
    return tc;
}

// Records one write of byte_len bytes on topic. Matches go-DDS's
// w.p.mWrites.Add(1) / w.p.mBytesWritten.Add(...) / topicTC.writes.Add(1) /
// topicTC.bytesW.Add(...) at the top of rtpsWriter.Write.
// fusa:req REQ-METRICS-001 REQ-METRICS-005
void Participant::record_write(const std::string& topic, std::size_t byte_len) {
    write_count_.fetch_add(1);
    bytes_written_.fetch_add(byte_len);
    auto tc = topic_counter_for(topic);
    tc->write_count.fetch_add(1);
    tc->bytes_written.fetch_add(byte_len);
}

// fusa:req REQ-METRICS-001
relay::Metrics Participant::metrics() const {
    relay::Metrics m;
    m.write_count     = write_count_.load();
    m.deliver_count    = deliver_count_.load();
    m.drop_count       = drop_count_.load();
    m.bytes_written    = bytes_written_.load();
    m.bytes_delivered  = bytes_delivered_.load();
    return m;
}

// discovery_metrics is sourced from live SpdpService/SedpService state — the
// one place this port genuinely improves on dds::mock's necessarily-zeroed
// equivalent (the mock has no real network discovery to source counters
// from at all). Matches go-DDS's rtps.participant.DiscoveryMetrics exactly:
// spdp_'s announce/eviction counters and current peer count, sedp_'s
// cumulative endpoint-match counter.
// fusa:req REQ-METRICS-004
relay::DiscoveryMetrics Participant::discovery_metrics() const {
    relay::DiscoveryMetrics m;
    m.announces_sent     = spdp_->announces_sent();
    m.announces_received = spdp_->announces_received();
    m.peers_known         = spdp_->peers().size();
    m.peer_evictions       = spdp_->peer_evictions();
    m.endpoint_matches      = sedp_->endpoint_matches();
    return m;
}

// fusa:req REQ-METRICS-005
std::vector<relay::TopicMetrics> Participant::topic_metrics() const {
    std::vector<relay::TopicMetrics> result;
    std::lock_guard<std::mutex> lock(topic_metrics_mu_);
    result.reserve(topic_metrics_.size());
    for (const auto& [topic, tc] : topic_metrics_) {
        relay::TopicMetrics m;
        m.topic           = topic;
        m.write_count     = tc->write_count.load();
        m.deliver_count   = tc->deliver_count.load();
        m.drop_count      = tc->drop_count.load();
        m.bytes_written   = tc->bytes_written.load();
        m.bytes_delivered = tc->bytes_delivered.load();
        result.push_back(std::move(m));
    }
    return result;
}

// ── data receive loop ────────────────────────────────────────────────────────

void Participant::start_data_loop() {
    if (data_running_.exchange(true)) return;
    data_thread_ = std::thread(&Participant::data_loop, this);
}

void Participant::stop_data_loop() {
    if (!data_running_.exchange(false)) return;
    if (data_thread_.joinable()) data_thread_.join();
}

void Participant::data_loop() {
    while (data_running_.load(std::memory_order_relaxed)) {
        // UdpSocket::recv() has its own ~250ms internal poll timeout, so
        // this loop's own running check is the shutdown-responsiveness
        // mechanism (mirrors SpdpService::receive_loop / SedpService::
        // receive_loop's identical pattern).
        auto pkt = data_sock_.recv();
        if (!data_running_.load(std::memory_order_relaxed)) return;
        if (!pkt) continue;
        handle_data_packet(pkt->data, pkt->from_address, pkt->from_port);
    }
}

// ── IPv6 data receive loop (Tier-1 phase 10) ────────────────────────────────
// Mirrors data_loop() exactly, reading from data_sock_v6_ instead of
// data_sock_ but feeding the same handle_data_packet — matching go-DDS's
// dataReceiveLoop fan-in of dataSock and dataSockV6 into one
// handleDataPacket. See participant.hpp's file-level scope note: this is
// receive-only — any reply handle_data_packet triggers still goes out via
// the IPv4 data_sock_ (send_data), matching go-DDS's p.dataSock.send(...)
// call sites verbatim.

void Participant::start_data_loop_v6() {
    if (!data_sock_v6_.valid()) return; // IPv6 not requested, or bind failed (soft failure)
    if (data_running_v6_.exchange(true)) return;
    data_thread_v6_ = std::thread(&Participant::data_loop_v6, this);
}

void Participant::stop_data_loop_v6() {
    if (!data_running_v6_.exchange(false)) return;
    if (data_thread_v6_.joinable()) data_thread_v6_.join();
}

void Participant::data_loop_v6() {
    while (data_running_v6_.load(std::memory_order_relaxed)) {
        auto pkt = data_sock_v6_.recv();
        if (!data_running_v6_.load(std::memory_order_relaxed)) return;
        if (!pkt) continue;
        handle_data_packet(pkt->data, pkt->from_address, pkt->from_port);
    }
}

void Participant::handle_data_packet(const std::vector<uint8_t>& data, const std::string& from_address,
                                       int from_port) {
    auto hdr = Header::decode(data.data(), data.size());
    if (!hdr) return;
    if (hdr->guid_prefix == guid_prefix_) return; // ignore our own (shouldn't normally arrive unicast)

    const uint8_t* body     = data.data() + Header::kSize;
    std::size_t     body_len = data.size() - Header::kSize;
    std::size_t     pos      = 0;

    while (pos + SubmessageHeader::kSize <= body_len) {
        auto sh = SubmessageHeader::decode(body + pos, body_len - pos);
        if (!sh) break;
        const std::size_t entry_len = SubmessageHeader::kSize + sh->octets_to_next_header;
        if (pos + entry_len > body_len) break; // malformed: declared length overruns buffer

        if (sh->submessage_id == kSubmessageIdData) {
            auto ds = DataSubmessage::decode(body + pos, entry_len);
            if (ds && !ds->payload.empty()) {
                auto raw = cdr_unwrap_payload(ds->payload);
                if (raw) {
                    const GUID source{hdr->guid_prefix, ds->writer_entity_id};
                    const auto now = std::chrono::system_clock::now();
                    // Reliable-delivery bookkeeping (record + ACKNACK-on-gap)
                    // happens before dispatch, matching go-DDS's ordering of
                    // notifyReliableReaders then dispatchToReaders.
                    notify_reliable_readers(source, ds->seq_num, from_address, from_port);
                    dispatch(source, "", *raw, now, sn_to_u64(ds->seq_num));
                }
            }
        } else if (sh->submessage_id == kSubmessageIdHeartbeat) {
            auto hb = Heartbeat::decode(body + pos, entry_len);
            if (hb) {
                const GUID writer_guid{hdr->guid_prefix, hb->writer_entity_id};
                handle_heartbeat(writer_guid, *hb, from_address, from_port);
            }
        } else if (sh->submessage_id == kSubmessageIdAckNack) {
            auto an = AckNack::decode(body + pos, entry_len);
            if (an) handle_ack_nack(*an, from_address, from_port);
        } else if (sh->submessage_id == kSubmessageIdDataFrag) {
            // Tier-1 phase 8 ("Fragmentation") — see fragment.hpp's
            // file-level scope note for why this reassembly is wired into
            // the receive path even though go-DDS's own participant.go
            // never wires its own (otherwise fully working)
            // fragmentAssembler into an equivalent switch case.
            auto df = DataFrag::decode(body + pos, entry_len);
            if (df) {
                const GUID source{hdr->guid_prefix, df->writer_entity_id};
                if (auto reassembled = frag_assembler_.receive(source, *df)) {
                    auto raw = cdr_unwrap_payload(*reassembled);
                    if (raw) {
                        const auto now = std::chrono::system_clock::now();
                        // Matches the plain-DATA path's ordering above:
                        // reliable-delivery bookkeeping before dispatch.
                        notify_reliable_readers(source, df->writer_seq_num, from_address, from_port);
                        dispatch(source, "", *raw, now, sn_to_u64(df->writer_seq_num));
                    }
                }
            }
        }
        pos += entry_len;
    }
}

// ── reliable delivery (Tier-1 phase 7) ───────────────────────────────────────

void Participant::notify_reliable_readers(const GUID& writer_guid, const SequenceNumber& seq_num,
                                            const std::string& from_address, int from_port) {
    std::vector<std::shared_ptr<Reader>> snapshot;
    {
        std::lock_guard<std::mutex> lock(readers_mu_);
        snapshot.reserve(readers_.size());
        for (auto& [eid, weak] : readers_) {
            (void)eid;
            if (auto r = weak.lock()) snapshot.push_back(std::move(r));
        }
    }

    for (auto& r : snapshot) {
        if (!r->reliable()) continue;
        bool accepted = (writer_guid.prefix == guid_prefix_);
        if (!accepted) {
            auto matched = sedp_->matched_writer_guids_for_reader(r->entity_id());
            accepted       = std::find(matched.begin(), matched.end(), writer_guid) != matched.end();
        }
        if (!accepted) continue;

        auto tracker = r->tracker_for(writer_guid);
        const uint64_t sn = sn_to_u64(seq_num);
        tracker->record(sn);
        // The writer's history reaches at least this SN, so NACK any gap below it.
        auto [base, bitmap, need_ack] = tracker->missing(sn);
        if (!need_ack || from_port == 0) continue;

        AckNack an;
        an.reader_entity_id = r->entity_id();
        an.writer_entity_id = writer_guid.entity;
        an.base                = u64_to_sn(base);
        an.bitmap               = bitmap;
        an.count                = tracker->next_ack_count();
        std::vector<uint8_t> submsg;
        an.encode(submsg);
        auto msg = wrap_in_rtps_message(guid_prefix_, kVendorIdCppDDS, submsg);
        send_data(from_address, from_port, msg);
    }
}

void Participant::handle_heartbeat(const GUID& writer_guid, const Heartbeat& hb, const std::string& from_address,
                                     int from_port) {
    std::vector<std::shared_ptr<Reader>> snapshot;
    {
        std::lock_guard<std::mutex> lock(readers_mu_);
        snapshot.reserve(readers_.size());
        for (auto& [eid, weak] : readers_) {
            (void)eid;
            if (auto r = weak.lock()) snapshot.push_back(std::move(r));
        }
    }

    for (auto& r : snapshot) {
        if (!r->reliable()) continue;
        bool accepted = (writer_guid.prefix == guid_prefix_);
        if (!accepted) {
            auto matched = sedp_->matched_writer_guids_for_reader(r->entity_id());
            accepted       = std::find(matched.begin(), matched.end(), writer_guid) != matched.end();
        }
        if (!accepted) continue;

        auto tracker = r->tracker_for(writer_guid);
        // On first contact, anchor the cumulative-ACK base at the writer's
        // FirstSN so the reader can request the writer's whole live history.
        tracker->init_expected(sn_to_u64(hb.first_sn));
        // Re-NACK every SN still missing up to the writer's LastSN. Because
        // the watermark never skips a gap, a lost retransmit is requested
        // again on each periodic HEARTBEAT until it arrives.
        auto [base, bitmap, need_ack] = tracker->missing(sn_to_u64(hb.last_sn));
        if (!need_ack || from_port == 0) continue;

        AckNack an;
        an.reader_entity_id = r->entity_id();
        an.writer_entity_id = writer_guid.entity;
        an.base                = u64_to_sn(base);
        an.bitmap               = bitmap;
        an.count                = tracker->next_ack_count();
        std::vector<uint8_t> submsg;
        an.encode(submsg);
        auto msg = wrap_in_rtps_message(guid_prefix_, kVendorIdCppDDS, submsg);
        send_data(from_address, from_port, msg);
    }
}

void Participant::handle_ack_nack(const AckNack& an, const std::string& from_address, int from_port) {
    std::shared_ptr<Writer> w;
    {
        std::lock_guard<std::mutex> lock(writers_mu_);
        auto it = writers_.find(an.writer_entity_id);
        if (it != writers_.end()) w = it->second.lock();
    }
    if (!w) return;
    w->handle_ack_nack(an, from_address, from_port);
}

// ── SPDP -> SEDP peer-bridge loop ────────────────────────────────────────────

void Participant::start_bridge_loop() {
    if (bridge_running_.exchange(true)) return;
    bridge_thread_ = std::thread(&Participant::bridge_loop, this);
}

void Participant::stop_bridge_loop() {
    if (!bridge_running_.exchange(false)) return;
    if (bridge_thread_.joinable()) bridge_thread_.join();
}

void Participant::bridge_loop() {
    sync_peers();
    while (bridge_running_.load(std::memory_order_relaxed)) {
        std::chrono::milliseconds elapsed{0};
        while (bridge_running_.load(std::memory_order_relaxed) && elapsed < bridge_poll_period_) {
            auto slice = std::min(kShutdownPollSlice, bridge_poll_period_ - elapsed);
            std::this_thread::sleep_for(slice);
            elapsed += slice;
        }
        if (!bridge_running_.load(std::memory_order_relaxed)) return;
        sync_peers();
    }
}

void Participant::sync_peers() {
    auto peers = spdp_->peers();

    std::vector<ParticipantProxy> new_peers;
    std::vector<GuidPrefix>        evicted_prefixes;
    {
        std::lock_guard<std::mutex> lock(known_peers_mu_);
        std::unordered_map<GuidPrefix, bool, GuidPrefixHash> seen;
        for (const auto& proxy : peers) {
            seen[proxy.guid.prefix] = true;
            if (known_peers_.find(proxy.guid.prefix) == known_peers_.end()) {
                known_peers_[proxy.guid.prefix] = true;
                new_peers.push_back(proxy);
            }
        }
        for (auto it = known_peers_.begin(); it != known_peers_.end();) {
            if (seen.find(it->first) == seen.end()) {
                evicted_prefixes.push_back(it->first);
                it = known_peers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& proxy : new_peers) sedp_->on_new_peer(proxy);
    for (const auto& prefix : evicted_prefixes) sedp_->on_peer_evicted(prefix);
}

// ── Loan integration (Tier-1 phase 9) ────────────────────────────────────────
//
// See rtps/loan.hpp for the full file-level scope note. C++ port of
// go-DDS's rtps/loan.go loaningWriter/NewLoaningPublisher.

namespace {

// LoaningWriter wraps a Writer with a dds::pool::BytePool for
// allocation-free loaned-sample publishing, matching go-DDS's
// loaningWriter (which embeds *rtpsWriter and adds a *pool.BytePool).
// write()/close() delegate to the wrapped Writer unchanged — this class
// adds only loan_buffer/write_loaned/return_loan.
class LoaningWriter : public ILoaningPublisher {
public:
    LoaningWriter(std::shared_ptr<Writer> w, std::size_t buf_size)
        : w_(std::move(w)), pool_(buf_size) {}

    std::error_code write(const std::vector<uint8_t>& payload) override { return w_->write(payload); }

    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override {
        return w_->write(ctx, payload);
    }

    std::error_code close() override { return w_->close(); }

    // Matches go-DDS's loaningWriter.Loan: reject on a closed writer, then
    // hand out a pool buffer, returning ErrLoanBuffer (and putting the
    // undersized buffer straight back) if the pool's configured capacity
    // can't satisfy the request.
    std::pair<std::vector<uint8_t>*, std::error_code> loan_buffer(std::size_t size) override {
        if (w_->is_closed()) return {nullptr, dds::ErrClosed()};

        std::vector<uint8_t>* buf = pool_.get();
        if (buf->capacity() < size) {
            pool_.put(buf);
            return {nullptr, dds::ErrLoanBuffer()};
        }
        buf->resize(size);
        return {buf, {}};
    }

    // Matches go-DDS's loaningWriter.Commit: publish, then return the
    // buffer to the pool regardless of whether the write succeeded (no
    // validation that buf actually came from this pool — see loan.hpp's
    // file-level scope note on why that matches go-DDS's own behavior).
    std::error_code write_loaned(std::vector<uint8_t>* buf) override {
        std::error_code err = w_->write(*buf);
        pool_.put(buf);
        return err;
    }

    // return_loan has no go-DDS LoaningPublisher equivalent (dds.
    // ILoaningPublisher added it ahead of this phase as an explicit
    // discard-without-publish operation) — trivially returns the buffer to
    // the pool without writing.
    void return_loan(std::vector<uint8_t>* buf) override { pool_.put(buf); }

private:
    std::shared_ptr<Writer> w_;
    dds::pool::BytePool     pool_;
};

} // namespace

std::pair<std::shared_ptr<ILoaningPublisher>, std::error_code>
new_loaning_publisher(const std::shared_ptr<IParticipant>& p, const std::string& topic, QoS qos,
                       std::size_t buf_size) {
    auto [pub, err] = p->new_publisher(topic, qos);
    if (err) return {nullptr, err};

    // Matches go-DDS's `rw, ok := pub.(*rtpsWriter)` type assertion: any
    // IParticipant implementation other than dds::rtps::Participant (whose
    // new_publisher always returns a Writer) fails this cast.
    auto w = std::dynamic_pointer_cast<Writer>(pub);
    if (!w) {
        pub->close();
        return {nullptr, dds::ErrLoanBuffer()};
    }
    return {std::make_shared<LoaningWriter>(std::move(w), buf_size), std::error_code{}};
}

} // namespace dds::rtps
