// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/sedp.hpp>

// C++ port of github.com/SoundMatt/go-DDS rtps/sedp.go. See
// include/dds/rtps/sedp.hpp for the phase scope and the deliberate
// deviations from a literal line-for-line port.

#include <algorithm>
#include <cstdio>

namespace dds::rtps {

namespace {

inline void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

// Parses a dotted-quad IPv4 literal (e.g. "192.168.1.50") into 4 bytes.
// Returns false on any malformed input. Mirrors spdp.cpp's own copy
// (UdpPacket::from_address is always produced by inet_ntop(AF_INET, ...),
// so this only ever needs to handle that shape).
bool parse_ipv4(const std::string& s, std::array<uint8_t, 4>& out) {
    int         octets[4];
    std::size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        int value  = 0;
        int digits = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            value = value * 10 + (s[pos] - '0');
            ++pos;
            ++digits;
            if (digits > 3 || value > 255) return false;
        }
        octets[i] = value;
        if (i < 3) {
            if (pos >= s.size() || s[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != s.size()) return false;
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(octets[i]);
    return true;
}

bool is_zero_address(const Locator& l) {
    return std::all_of(l.address.begin(), l.address.end(), [](uint8_t b) { return b == 0; });
}

// Converts a UDPv4 Locator to a dotted-quad address + port pair for
// UdpSocket::send_to. Mirrors go-DDS's Locator.udpAddr().
bool locator_to_dest(const Locator& l, std::string& addr_out, int& port_out) {
    if (l.kind != Locator::kKindUDPv4) return false;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", l.address[12], l.address[13], l.address[14],
                  l.address[15]);
    addr_out = buf;
    port_out = static_cast<int>(l.port);
    return true;
}

} // namespace

// ── build_endpoint_data / parse_endpoint_data ───────────────────────────────

std::vector<uint8_t> build_endpoint_data(const EndpointInfo& info, uint16_t data_unicast_port) {
    PLCDREncoder enc;
    enc.add_guid(kPidEndpointGUID, info.guid);
    enc.add_string(kPidTopicName, info.topic_name);
    enc.add_string(kPidTypeName, "CDR_BLOB"); // opaque type for raw byte payloads

    Locator user_locator;
    user_locator.kind = Locator::kKindUDPv4;
    user_locator.port = data_unicast_port;
    enc.add_locator(kPidDefaultUnicastLocator, user_locator);

    // EndpointPlugin authentication token: out of scope for this phase
    // (Tier 2 security) — see the file-level scope note in sedp.hpp.

    return enc.finish();
}

std::optional<ParsedEndpointInfo> parse_endpoint_data(const GuidPrefix&   remote_prefix,
                                                        const uint8_t*      payload,
                                                        std::size_t         payload_len,
                                                        const std::string&  from_address) {
    auto dec = PLCDRDecoder::create(payload, payload_len);
    if (!dec) return std::nullopt;

    ParsedEndpointInfo info;
    info.guid.prefix = remote_prefix;

    std::array<uint8_t, 4> from_bytes{};
    const bool              have_from = parse_ipv4(from_address, from_bytes);

    while (true) {
        auto p = dec->next();
        if (!p) break;

        switch (p->pid) {
            case kPidEndpointGUID: {
                auto g = GUID::decode(p->value, p->value_len);
                if (g) info.guid = *g;
                break;
            }
            case kPidTopicName: {
                auto t = decode_string(p->value, p->value_len);
                if (t) info.topic_name = *t;
                break;
            }
            case kPidDefaultUnicastLocator: {
                auto l = Locator::decode(p->value, p->value_len);
                if (l) {
                    info.data_locator = *l;
                    if (is_zero_address(info.data_locator) && have_from) {
                        std::copy(from_bytes.begin(), from_bytes.end(),
                                  info.data_locator.address.begin() + 12);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    if (info.topic_name.empty()) return std::nullopt;
    return info;
}

// ── message framing ──────────────────────────────────────────────────────────

std::vector<uint8_t> build_sedp_announcement(const GuidPrefix& local_prefix, const VendorId& vendor_id,
                                              const EntityId& writer_eid, const EntityId& reader_eid,
                                              SequenceNumber seq_num, const std::vector<uint8_t>& payload) {
    DataSubmessage ds;
    ds.reader_entity_id = reader_eid;
    ds.writer_entity_id = writer_eid;
    ds.seq_num           = seq_num;
    ds.payload            = payload;

    std::vector<uint8_t> submsg;
    ds.encode(submsg);

    return wrap_in_rtps_message(local_prefix, vendor_id, submsg);
}

// ── SedpService ───────────────────────────────────────────────────────────────

std::size_t SedpService::GuidPrefixHash::operator()(const GuidPrefix& g) const noexcept {
    std::size_t h = 1469598103934665603ull; // FNV-1a
    for (uint8_t b : g.bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::size_t SedpService::EntityIdHash::operator()(const EntityId& e) const noexcept {
    std::size_t h = 1469598103934665603ull; // FNV-1a
    for (uint8_t b : e.bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::size_t SedpService::GUIDHash::operator()(const GUID& g) const noexcept {
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

SedpService::SedpService(SedpConfig config, UdpSocket socket)
    : config_(std::move(config)), socket_(std::move(socket)) {}

SedpService::~SedpService() { stop(); }

void SedpService::start() {
    if (running_.exchange(true)) return; // already started
    receive_thread_ = std::thread(&SedpService::receive_loop, this);
}

void SedpService::stop() {
    if (!running_.exchange(false)) return; // already stopped
    if (receive_thread_.joinable()) receive_thread_.join();
}

void SedpService::register_writer(const EntityId& eid, const std::string& topic_name) {
    EndpointInfo info;
    info.guid       = GUID{config_.local_guid_prefix, eid};
    info.topic_name = topic_name;
    info.is_writer  = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        local_writers_[eid] = info;
    }
    announce_writer(info, nullptr);
}

void SedpService::register_reader(const EntityId& eid, const std::string& topic_name) {
    EndpointInfo info;
    info.guid       = GUID{config_.local_guid_prefix, eid};
    info.topic_name = topic_name;
    info.is_writer  = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        local_readers_[eid] = info;
        // Match against already-discovered remote writers.
        for (const auto& [guid, rw] : remote_writers_) {
            if (rw.topic_name == topic_name) {
                matched_writers_by_reader_[eid].push_back(guid);
            }
        }
    }
    announce_reader(info, nullptr);
}

void SedpService::on_new_peer(const ParticipantProxy& peer) {
    std::vector<EndpointInfo> writers;
    std::vector<EndpointInfo> readers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        known_peers_[peer.guid.prefix] = peer;
        writers.reserve(local_writers_.size());
        for (const auto& [eid, w] : local_writers_) {
            (void)eid;
            writers.push_back(w);
        }
        readers.reserve(local_readers_.size());
        for (const auto& [eid, r] : local_readers_) {
            (void)eid;
            readers.push_back(r);
        }
    }
    for (const auto& w : writers) announce_writer(w, &peer);
    for (const auto& r : readers) announce_reader(r, &peer);
}

void SedpService::on_peer_evicted(const GuidPrefix& prefix) {
    std::lock_guard<std::mutex> lock(mu_);
    known_peers_.erase(prefix);
    for (auto it = remote_writers_.begin(); it != remote_writers_.end();) {
        if (it->first.prefix == prefix) {
            remote_writer_locs_.erase(it->first);
            it = remote_writers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = remote_readers_.begin(); it != remote_readers_.end();) {
        if (it->first.prefix == prefix) {
            remote_reader_locs_.erase(it->first);
            it = remote_readers_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<GUID> SedpService::matched_writer_guids_for_reader(const EntityId& reader_eid) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto                         it = matched_writers_by_reader_.find(reader_eid);
    if (it == matched_writers_by_reader_.end()) return {};
    return it->second;
}

std::optional<Locator> SedpService::writer_locator(const GUID& writer_guid) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto                         it = remote_writer_locs_.find(writer_guid);
    if (it == remote_writer_locs_.end()) return std::nullopt;
    return it->second;
}

std::optional<Locator> SedpService::reader_locator(const GUID& reader_guid) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto                         it = remote_reader_locs_.find(reader_guid);
    if (it == remote_reader_locs_.end()) return std::nullopt;
    return it->second;
}

void SedpService::announce_writer(const EndpointInfo& info, const ParticipantProxy* only_to) {
    auto payload = build_endpoint_data(info, config_.data_unicast_port);
    uint32_t seq_low = seq_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto msg = build_sedp_announcement(config_.local_guid_prefix, config_.vendor_id,
                                        kEntityIdSEDPPubWriter, kEntityIdSEDPPubReader,
                                        SequenceNumber{0, seq_low}, payload);
    announces_sent_.fetch_add(1, std::memory_order_relaxed);
    broadcast(msg, only_to);
}

void SedpService::announce_reader(const EndpointInfo& info, const ParticipantProxy* only_to) {
    auto payload = build_endpoint_data(info, config_.data_unicast_port);
    uint32_t seq_low = seq_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto msg = build_sedp_announcement(config_.local_guid_prefix, config_.vendor_id,
                                        kEntityIdSEDPSubWriter, kEntityIdSEDPSubReader,
                                        SequenceNumber{0, seq_low}, payload);
    announces_sent_.fetch_add(1, std::memory_order_relaxed);
    broadcast(msg, only_to);
}

void SedpService::broadcast(const std::vector<uint8_t>& msg, const ParticipantProxy* only_to) {
    if (only_to != nullptr) {
        send_to(msg, *only_to);
        return;
    }
    std::vector<ParticipantProxy> peers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        peers.reserve(known_peers_.size());
        for (const auto& [prefix, proxy] : known_peers_) {
            (void)prefix;
            peers.push_back(proxy);
        }
    }
    for (const auto& peer : peers) send_to(msg, peer);
}

void SedpService::send_to(const std::vector<uint8_t>& msg, const ParticipantProxy& peer) {
    std::string addr;
    int         port = 0;
    if (!locator_to_dest(peer.metatraffic_unicast, addr, port)) return;
    socket_.send_to(addr, port, msg.data(), msg.size());
}

void SedpService::handle_packet(const std::vector<uint8_t>& data, const std::string& from_address) {
    auto hdr = Header::decode(data.data(), data.size());
    if (!hdr) return;
    if (hdr->guid_prefix == config_.local_guid_prefix) return; // own packet

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
                if (ds->writer_entity_id == kEntityIdSEDPPubWriter) {
                    handle_endpoint_announce(hdr->guid_prefix, ds->payload, true, from_address);
                } else if (ds->writer_entity_id == kEntityIdSEDPSubWriter) {
                    handle_endpoint_announce(hdr->guid_prefix, ds->payload, false, from_address);
                }
            }
        }
        pos += entry_len;
    }
}

void SedpService::handle_endpoint_announce(const GuidPrefix& remote_prefix, const std::vector<uint8_t>& payload,
                                            bool is_writer, const std::string& from_address) {
    auto parsed = parse_endpoint_data(remote_prefix, payload.data(), payload.size(), from_address);
    if (!parsed) return;

    announces_received_.fetch_add(1, std::memory_order_relaxed);

    EndpointInfo info;
    info.guid       = parsed->guid;
    info.topic_name = parsed->topic_name;
    info.is_writer  = is_writer;

    if (is_writer) {
        on_remote_writer(info, parsed->data_locator);
    } else {
        on_remote_reader(info, parsed->data_locator);
    }
}

void SedpService::on_remote_writer(const EndpointInfo& info, const Locator& data_locator) {
    std::lock_guard<std::mutex> lock(mu_);
    remote_writers_[info.guid]     = info;
    remote_writer_locs_[info.guid] = data_locator;
    // Match against local readers for this topic.
    for (const auto& [eid, lr] : local_readers_) {
        if (lr.topic_name == info.topic_name) {
            auto& matched = matched_writers_by_reader_[eid];
            if (std::find(matched.begin(), matched.end(), info.guid) == matched.end()) {
                matched.push_back(info.guid);
                endpoint_matches_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void SedpService::on_remote_reader(const EndpointInfo& info, const Locator& data_locator) {
    std::lock_guard<std::mutex> lock(mu_);
    remote_readers_[info.guid]     = info;
    remote_reader_locs_[info.guid] = data_locator;
}

void SedpService::receive_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        // UdpSocket::recv() has its own ~250ms internal poll timeout, so
        // this loop's own running_ check is the shutdown-responsiveness
        // mechanism (mirrors go-DDS's readLoop select on p.done, and
        // SpdpService::receive_loop's identical pattern).
        auto pkt = socket_.recv();
        if (!running_.load(std::memory_order_relaxed)) return;
        if (!pkt) continue;
        handle_packet(pkt->data, pkt->from_address);
    }
}

} // namespace dds::rtps
