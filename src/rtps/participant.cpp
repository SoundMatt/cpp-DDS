// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/participant.hpp>

// C++ port of the entity-lifecycle and best-effort-dispatch portions of
// github.com/SoundMatt/go-DDS rtps/participant.go. See
// include/dds/rtps/participant.hpp for the phase scope and the deliberate
// deviations from a literal line-for-line port.

#include <algorithm>
#include <cstdio>

namespace dds::rtps {

namespace {

// Converts a UDPv4 Locator to a dotted-quad address + port pair for
// UdpSocket::send_to. Mirrors go-DDS's Locator.udpAddr(); duplicated here
// rather than shared, matching sedp.cpp's own copy of the same helper
// (spdp.cpp/sedp.cpp already established this duplication pattern for
// small address-formatting helpers in this codebase).
bool locator_to_dest(const Locator& l, std::string& addr_out, int& port_out) {
    if (l.kind != Locator::kKindUDPv4) return false;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", l.address[12], l.address[13], l.address[14],
                  l.address[15]);
    addr_out = buf;
    port_out = static_cast<int>(l.port);
    return true;
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

// Packs a 64-bit sequence-number counter into the wire High:Low
// SequenceNumber shape (RTPS 2.3 §9.3.2), matching go-DDS's u64ToSN
// (rtps/reliable.go) — duplicated here as a small, self-contained helper
// rather than depending on phase 7 (reliable delivery), which does not
// exist yet.
SequenceNumber u64_to_sn(uint64_t v) {
    return SequenceNumber{static_cast<int32_t>(v >> 32), static_cast<uint32_t>(v)};
}

// Unpacks a wire SequenceNumber back into a 64-bit value, matching go-DDS's
// snToU64.
uint64_t sn_to_u64(const SequenceNumber& sn) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(sn.high)) << 32) | static_cast<uint64_t>(sn.low);
}

constexpr auto kShutdownPollSlice = std::chrono::milliseconds(50);

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
           relay::BackPressurePolicy back_pressure, std::function<bool(const Sample&)> filter)
        : p_(std::move(p))
        , topic_(std::move(topic))
        , eid_(eid)
        , ch_(std::make_shared<dds::Chan<Sample>>(static_cast<std::size_t>(depth)))
        , back_pressure_(back_pressure)
        , filter_(std::move(filter))
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

    // Applies the sample filter (if any) and enqueues per back_pressure_.
    void deliver(const Sample& s) {
        if (filter_ && !filter_(s)) return;
        switch (back_pressure_) {
            case relay::BackPressurePolicy::DropOldest:
                ch_->send_drop_oldest(s);
                break;
            case relay::BackPressurePolicy::Block:
                ch_->send(s);
                break;
            default: // DropNewest
                ch_->try_send(s);
                break;
        }
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
};

// ── Writer ───────────────────────────────────────────────────────────────────

// Writer implements dds::IPublisher: best-effort DATA send to every
// SEDP-matched remote reader locator plus unconditional local (same-
// participant) delivery. C++ port of the best-effort subset of go-DDS's
// rtpsWriter.Write (rtps/participant.go) — see participant.hpp's file-level
// scope note for what is intentionally not ported (reliability, TSN,
// security, fragmentation).
class Writer : public IPublisher {
public:
    Writer(std::shared_ptr<Participant> p, std::string topic, EntityId eid, QoS qos, std::size_t history_depth)
        : p_(std::move(p)), topic_(std::move(topic)), eid_(eid), qos_(qos), history_(history_depth) {}

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

        // Build wire bytes: existing, already byte-verified primitives only
        // (cdr_wrap_payload from phase 2, DataSubmessage::encode from
        // phase 1, wrap_in_rtps_message from phase 4) — this phase
        // introduces no new wire encoding of its own (see participant.hpp's
        // file-level scope note).
        DataSubmessage ds;
        ds.reader_entity_id = kEntityIdUnknown;
        ds.writer_entity_id = eid_;
        ds.seq_num            = u64_to_sn(seq);
        ds.payload             = cdr_wrap_payload(payload);
        std::vector<uint8_t> submsg;
        ds.encode(submsg);
        auto msg = wrap_in_rtps_message(p_->guid_prefix(), kVendorIdCppDDS, submsg);

        // HistoryCache: scaffolding for phase 7 (reliable delivery), not
        // yet consumed — see history_cache.hpp's own scope note.
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

        // Local (same-process, same-participant) delivery: unconditional,
        // topic-name-matched only — matches go-DDS's
        // dispatchToReaders acceptsSource short-circuit for
        // `source.Prefix == r.p.guidPrefix`.
        p_->dispatch(source, topic_, payload, now, seq);

        // Remote delivery: unicast to every SEDP-matched reader locator for
        // this topic.
        for (const auto& loc : p_->sedp().matched_reader_locators_for_topic(topic_)) {
            std::string addr;
            int          port = 0;
            if (locator_to_dest(loc, addr, port)) {
                p_->send_data(addr, port, msg);
            }
        }

        return {};
    }

    std::error_code close() override {
        closed_.store(true);
        return {};
    }

private:
    std::shared_ptr<Participant> p_;
    std::string                    topic_;
    EntityId                        eid_;
    QoS                              qos_;
    std::atomic<uint64_t>           seq_{0};
    std::atomic<bool>               closed_{false};
    HistoryCache                    history_;
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

    auto p = std::shared_ptr<Participant>(new Participant());
    p->domain_             = domain;
    p->guid_prefix_        = new_guid_prefix();
    p->meta_unicast_port_  = meta_port;
    p->data_unicast_port_  = data_port;
    p->history_depth_       = opts.history_depth > 0 ? opts.history_depth : kDefaultHistoryDepth;
    p->bridge_poll_period_  = opts.bridge_poll_period.count() > 0 ? opts.bridge_poll_period
                                                                    : std::chrono::milliseconds(200);
    p->data_sock_           = std::move(data_sock);

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
    p->start_bridge_loop();

    return {p, {}};
}

std::pair<std::shared_ptr<IPublisher>, std::error_code>
Participant::new_publisher(const std::string& topic, QoS qos) {
    if (closed_.load()) return {nullptr, dds::ErrClosed()};
    if (topic.empty()) return {nullptr, dds::ErrTopicEmpty()};

    const uint32_t n   = next_entity_ordinal();
    const EntityId eid = entity_id_for_writer(n);

    auto w = std::make_shared<Writer>(shared_from_this(), topic, eid, qos, history_depth_);
    sedp_->register_writer(eid, topic);
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

    auto reader = std::make_shared<Reader>(shared_from_this(), topic, eid, depth, cfg.back_pressure,
                                            std::move(sample_filter));
    register_reader(eid, reader);
    sedp_->register_reader(eid, topic);

    // TransientLocal: deliver the last published sample to the new
    // subscriber, if one exists.
    if (qos.durability == DurabilityKind::TransientLocal) {
        if (auto last = last_sample(topic)) {
            reader->deliver(*last);
        }
    }

    return {reader, {}};
}

std::error_code Participant::close() {
    if (closed_.exchange(true)) return {};

    stop_bridge_loop();
    stop_data_loop();
    if (spdp_) spdp_->stop();
    if (sedp_) sedp_->stop();
    data_sock_.close();

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
        r->deliver(s);
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
        handle_data_packet(pkt->data, pkt->from_address);
    }
}

void Participant::handle_data_packet(const std::vector<uint8_t>& data, const std::string& from_address) {
    (void)from_address;
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
                    dispatch(source, "", *raw, now, sn_to_u64(ds->seq_num));
                }
            }
        }
        pos += entry_len;
    }
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

} // namespace dds::rtps
