// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/spdp.hpp>

// C++ port of github.com/SoundMatt/go-DDS rtps/spdp.go. See
// include/dds/rtps/spdp.hpp for the phase scope and the deliberate
// deviations from a literal line-for-line port.

#include <algorithm>
#include <random>

namespace dds::rtps {

namespace {

// Random jitter source for announce_loop, below. A thread-local
// std::mt19937 (seeded once per thread from std::random_device) rather
// than the C library's rand()/srand(): std::rand() is not reentrant/
// thread-safe (its hidden global state is a data race under concurrent
// use) and is flagged by cpfusa's cybersecurity analysis (CWE-330) even
// for a non-cryptographic use like send-timing jitter.
uint32_t jitter_random_u32(uint32_t bound) {
    if (bound == 0) return 0;
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, bound - 1);
    return dist(rng);
}

inline void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

inline uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Parses a dotted-quad IPv4 literal (e.g. "192.168.1.50") into 4 bytes.
// Returns false on any malformed input. UdpPacket::from_address (the only
// caller-supplied source in this codebase) is always produced by
// inet_ntop(AF_INET, ...), so this only needs to handle that shape — no
// leading zeros, whitespace, or IPv6 forms to worry about.
bool parse_ipv4(const std::string& s, std::array<uint8_t, 4>& out) {
    int          octets[4];
    std::size_t  pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        int value = 0;
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

} // namespace

// ── build_participant_data / parse_participant_data ─────────────────────────

std::vector<uint8_t> build_participant_data(const SpdpLocalInfo& info) {
    PLCDREncoder enc;

    // Protocol version {2, 3}.
    enc.add_param(kPidProtocolVersion, std::vector<uint8_t>{2, 3, 0, 0});

    // Vendor ID.
    enc.add_param(kPidVendorId,
                   std::vector<uint8_t>{info.vendor_id.bytes[0], info.vendor_id.bytes[1], 0, 0});

    // Participant GUID.
    GUID guid;
    guid.prefix = info.guid_prefix;
    guid.entity = kEntityIdParticipant;
    enc.add_guid(kPidParticipantGUID, guid);

    // Builtin endpoints supported.
    enc.add_uint32(kPidBuiltinEndpointSet, kEndpointSPDPAnnouncer | kEndpointSPDPDetector |
                                                kEndpointSEDPPubAnnouncer | kEndpointSEDPPubDetector |
                                                kEndpointSEDPSubAnnouncer | kEndpointSEDPSubDetector);

    // Metatraffic unicast locator (where SEDP traffic should be sent).
    Locator meta_locator;
    meta_locator.kind = Locator::kKindUDPv4;
    meta_locator.port = info.meta_unicast_port;
    enc.add_locator(kPidMetatrafficUnicastLocator, meta_locator);

    // Default unicast locator (where user DATA should be sent).
    Locator user_locator;
    user_locator.kind = Locator::kKindUDPv4;
    user_locator.port = info.default_unicast_port;
    enc.add_locator(kPidDefaultUnicastLocator, user_locator);

    // Participant lease duration: uint32 sec + uint32 frac = 8 bytes.
    std::vector<uint8_t> duration;
    duration.reserve(8);
    put_u32_le(duration, static_cast<uint32_t>(info.advertised_lease_duration.count()));
    put_u32_le(duration, 0); // fractional seconds
    enc.add_param(kPidParticipantLeaseDuration, duration);

    // Discovery-plugin authentication token: out of scope for this phase
    // (Tier 2 security) — see the file-level scope note in spdp.hpp.

    return enc.finish();
}

std::optional<ParticipantProxy> parse_participant_data(const GuidPrefix& remote_prefix,
                                                         const uint8_t*      payload,
                                                         std::size_t         payload_len,
                                                         const std::string&  from_address) {
    auto dec = PLCDRDecoder::create(payload, payload_len);
    if (!dec) return std::nullopt;

    ParticipantProxy proxy;
    proxy.guid.prefix = remote_prefix;
    proxy.guid.entity = kEntityIdParticipant;

    std::array<uint8_t, 4> from_bytes{};
    const bool have_from = parse_ipv4(from_address, from_bytes);

    while (true) {
        auto p = dec->next();
        if (!p) break;

        switch (p->pid) {
            case kPidMetatrafficUnicastLocator: {
                auto l = Locator::decode(p->value, p->value_len);
                if (l) {
                    proxy.metatraffic_unicast = *l;
                    if (is_zero_address(proxy.metatraffic_unicast) && have_from) {
                        std::copy(from_bytes.begin(), from_bytes.end(),
                                  proxy.metatraffic_unicast.address.begin() + 12);
                    }
                }
                break;
            }
            case kPidDefaultUnicastLocator: {
                auto l = Locator::decode(p->value, p->value_len);
                if (l) {
                    proxy.default_unicast = *l;
                    if (is_zero_address(proxy.default_unicast) && have_from) {
                        std::copy(from_bytes.begin(), from_bytes.end(),
                                  proxy.default_unicast.address.begin() + 12);
                    }
                }
                break;
            }
            case kPidBuiltinEndpointSet:
                if (p->value_len >= 4) proxy.builtin_endpoints = get_u32_le(p->value);
                break;
            case kPidParticipantLeaseDuration:
                if (p->value_len >= 4) {
                    uint32_t secs = get_u32_le(p->value);
                    if (secs > 0) proxy.lease_duration = std::chrono::seconds(secs);
                }
                break;
            case kPidParticipantGUID: {
                auto g = GUID::decode(p->value, p->value_len);
                if (g) proxy.guid = *g;
                break;
            }
            default:
                break;
        }
    }
    return proxy;
}

// ── message framing ──────────────────────────────────────────────────────────

std::vector<uint8_t> wrap_in_rtps_message(const GuidPrefix& prefix, const VendorId& vendor_id,
                                           const std::vector<uint8_t>& submessage_bytes) {
    Header h;
    h.vendor_id   = vendor_id;
    h.guid_prefix = prefix;
    // h.protocol_version defaults to {2, 3}, matching go-DDS's literal
    // ProtocolVersion: [2]byte{2, 3}.

    std::vector<uint8_t> out;
    out.reserve(Header::kSize + submessage_bytes.size());
    h.encode(out);
    out.insert(out.end(), submessage_bytes.begin(), submessage_bytes.end());
    return out;
}

std::vector<uint8_t> build_spdp_announcement(const SpdpLocalInfo& info, SequenceNumber seq_num) {
    DataSubmessage ds;
    ds.reader_entity_id = kEntityIdSPDPReader;
    ds.writer_entity_id = kEntityIdSPDPWriter;
    ds.seq_num           = seq_num;
    ds.payload            = build_participant_data(info);

    std::vector<uint8_t> submsg;
    ds.encode(submsg);

    return wrap_in_rtps_message(info.guid_prefix, info.vendor_id, submsg);
}

// ── SpdpService ───────────────────────────────────────────────────────────────

std::size_t SpdpService::GuidPrefixHash::operator()(const GuidPrefix& g) const noexcept {
    // FNV-1a over the 12 prefix bytes.
    std::size_t h = 1469598103934665603ull;
    for (uint8_t b : g.bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

SpdpService::SpdpService(SpdpConfig config, UdpSocket send_socket, UdpSocket recv_socket)
    : config_(std::move(config)), send_socket_(std::move(send_socket)),
      recv_socket_(std::move(recv_socket)) {}

SpdpService::~SpdpService() { stop(); }

void SpdpService::start() {
    if (running_.exchange(true)) return; // already started
    announce_thread_ = std::thread(&SpdpService::announce_loop, this);
    receive_thread_  = std::thread(&SpdpService::receive_loop, this);
    evict_thread_    = std::thread(&SpdpService::evict_loop, this);
}

void SpdpService::stop() {
    if (!running_.exchange(false)) return; // already stopped
    if (announce_thread_.joinable()) announce_thread_.join();
    if (receive_thread_.joinable()) receive_thread_.join();
    if (evict_thread_.joinable()) evict_thread_.join();
}

std::vector<ParticipantProxy> SpdpService::peers() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ParticipantProxy> out;
    out.reserve(peers_.size());
    for (const auto& [prefix, proxy] : peers_) {
        (void)prefix;
        out.push_back(proxy);
    }
    return out;
}

void SpdpService::send_announcement() {
    announces_sent_.fetch_add(1, std::memory_order_relaxed);
    uint32_t seq_low = seq_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto msg = build_spdp_announcement(config_.local, SequenceNumber{0, seq_low});

    const int port = config_.dest_port >= 0 ? config_.dest_port : meta_multicast_port(config_.domain);
    send_socket_.send_to(config_.dest_address, port, msg.data(), msg.size());
}

void SpdpService::handle_packet(const std::vector<uint8_t>& data, const std::string& from_address) {
    auto hdr = Header::decode(data.data(), data.size());
    if (!hdr) return;
    // Ignore our own announcements.
    if (hdr->guid_prefix == config_.local.guid_prefix) return;

    const uint8_t*    body     = data.data() + Header::kSize;
    std::size_t        body_len = data.size() - Header::kSize;
    std::size_t        pos      = 0;

    while (pos + SubmessageHeader::kSize <= body_len) {
        auto sh = SubmessageHeader::decode(body + pos, body_len - pos);
        if (!sh) break;
        const std::size_t entry_len = SubmessageHeader::kSize + sh->octets_to_next_header;
        if (pos + entry_len > body_len) break; // malformed: declared length overruns buffer

        if (sh->submessage_id == kSubmessageIdData) {
            auto ds = DataSubmessage::decode(body + pos, entry_len);
            if (ds && !ds->payload.empty() && ds->writer_entity_id == kEntityIdSPDPWriter) {
                auto proxy = parse_participant_data(hdr->guid_prefix, ds->payload.data(),
                                                     ds->payload.size(), from_address);
                if (proxy) store_peer(std::move(*proxy));
            }
        }
        pos += entry_len;
    }
}

void SpdpService::store_peer(ParticipantProxy proxy) {
    announces_received_.fetch_add(1, std::memory_order_relaxed);
    proxy.last_seen = std::chrono::steady_clock::now();
    if (proxy.lease_duration.count() == 0) {
        proxy.lease_duration = config_.peer_lease_duration.count() > 0
                                    ? config_.peer_lease_duration
                                    : kSpdpDefaultLeaseDuration;
    }
    std::lock_guard<std::mutex> lock(mu_);
    peers_[proxy.guid.prefix] = proxy;
}

void SpdpService::evict_expired() {
    const auto now = std::chrono::steady_clock::now();
    std::size_t evicted = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto lease = it->second.lease_duration.count() > 0 ? it->second.lease_duration
                                                                : kSpdpDefaultLeaseDuration;
            if (now - it->second.last_seen > lease) {
                it = peers_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
    }
    if (evicted > 0) peer_evictions_.fetch_add(evicted, std::memory_order_relaxed);
}

// ── background loops ─────────────────────────────────────────────────────────

namespace {
constexpr auto kShutdownPollSlice = std::chrono::milliseconds(50);
}

void SpdpService::announce_loop() {
    send_announcement();

    auto period = config_.announce_period.count() > 0 ? config_.announce_period : kSpdpAnnouncePeriod;

    while (running_.load(std::memory_order_relaxed)) {
        // Random jitter before the next announcement, matching go-DDS's
        // per-tick jitter sleep — added to the wait budget so total
        // shutdown responsiveness stays bounded by kShutdownPollSlice.
        auto wait = period;
        if (config_.jitter.count() > 0) {
            auto jitter_ms = static_cast<uint32_t>(config_.jitter.count());
            wait += std::chrono::milliseconds(jitter_random_u32(jitter_ms));
        }

        std::chrono::milliseconds elapsed{0};
        while (running_.load(std::memory_order_relaxed) && elapsed < wait) {
            auto slice = std::min(kShutdownPollSlice, wait - elapsed);
            std::this_thread::sleep_for(slice);
            elapsed += slice;
        }
        if (!running_.load(std::memory_order_relaxed)) return;
        send_announcement();
    }
}

void SpdpService::receive_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        // UdpSocket::recv() has its own ~250ms internal poll timeout, so
        // this loop's own running_ check is the shutdown-responsiveness
        // mechanism (mirrors go-DDS's readLoop select on p.done).
        auto pkt = recv_socket_.recv();
        if (!running_.load(std::memory_order_relaxed)) return;
        if (!pkt) continue;
        handle_packet(pkt->data, pkt->from_address);
    }
}

void SpdpService::evict_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::chrono::milliseconds elapsed{0};
        constexpr std::chrono::milliseconds kEvictPeriod{1000}; // go-DDS: once per second
        while (running_.load(std::memory_order_relaxed) && elapsed < kEvictPeriod) {
            auto slice = std::min(kShutdownPollSlice, kEvictPeriod - elapsed);
            std::this_thread::sleep_for(slice);
            elapsed += slice;
        }
        if (!running_.load(std::memory_order_relaxed)) return;
        evict_expired();
    }
}

} // namespace dds::rtps
