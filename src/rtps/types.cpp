// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/types.hpp>

// C++ port of github.com/SoundMatt/go-DDS rtps/guid.go, rtps/locator.go,
// and the Header/DATA-submessage portions of rtps/message.go. See
// include/dds/rtps/types.hpp for the phase scope and the module doc
// comment there for the byte-identity contract this file must uphold.

#include <random>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace dds::rtps {

namespace {

// ── little-endian primitives ────────────────────────────────────────────────
// Mirrors Go's encoding/binary.LittleEndian used throughout message.go and
// locator.go. All RTPS discovery/framing multi-byte fields are LE-only.

inline void put_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

inline void put_i32_le(std::vector<uint8_t>& out, int32_t v) {
    // Bit-reinterpret, matching Go's uint32(int32Value) conversion.
    put_u32_le(out, static_cast<uint32_t>(v));
}

inline uint16_t get_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                  (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t get_i32_le(const uint8_t* p) {
    return static_cast<int32_t>(get_u32_le(p));
}

} // namespace

// ── GUID ─────────────────────────────────────────────────────────────────────

void GUID::encode(std::vector<uint8_t>& out) const {
    out.insert(out.end(), prefix.bytes.begin(), prefix.bytes.end());
    out.insert(out.end(), entity.bytes.begin(), entity.bytes.end());
}

std::optional<GUID> GUID::decode(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kSize) return std::nullopt;
    GUID g;
    std::memcpy(g.prefix.bytes.data(), data, GuidPrefix::kSize);
    std::memcpy(g.entity.bytes.data(), data + GuidPrefix::kSize, EntityId::kSize);
    return g;
}

// ── Locator ───────────────────────────────────────────────────────────────────

void Locator::encode(std::vector<uint8_t>& out) const {
    put_i32_le(out, kind);
    put_u32_le(out, port);
    out.insert(out.end(), address.begin(), address.end());
}

std::optional<Locator> Locator::decode(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kSize) return std::nullopt;
    Locator l;
    l.kind = get_i32_le(data);
    l.port = get_u32_le(data + 4);
    std::memcpy(l.address.data(), data + 8, l.address.size());
    return l;
}

// ── Header ────────────────────────────────────────────────────────────────────

void Header::encode(std::vector<uint8_t>& out) const {
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(protocol_version.major);
    out.push_back(protocol_version.minor);
    out.insert(out.end(), vendor_id.bytes.begin(), vendor_id.bytes.end());
    out.insert(out.end(), guid_prefix.bytes.begin(), guid_prefix.bytes.end());
}

std::optional<Header> Header::decode(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kSize) return std::nullopt;
    if (data[0] != kMagic[0] || data[1] != kMagic[1] ||
        data[2] != kMagic[2] || data[3] != kMagic[3]) {
        return std::nullopt;
    }
    Header h;
    h.protocol_version.major = data[4];
    h.protocol_version.minor = data[5];
    std::memcpy(h.vendor_id.bytes.data(), data + 6, h.vendor_id.bytes.size());
    std::memcpy(h.guid_prefix.bytes.data(), data + 8, GuidPrefix::kSize);
    return h;
}

// ── SequenceNumber ────────────────────────────────────────────────────────────

void SequenceNumber::encode(std::vector<uint8_t>& out) const {
    put_i32_le(out, high);
    put_u32_le(out, low);
}

std::optional<SequenceNumber> SequenceNumber::decode(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kSize) return std::nullopt;
    SequenceNumber sn;
    sn.high = get_i32_le(data);
    sn.low  = get_u32_le(data + 4);
    return sn;
}

// ── SubmessageHeader ──────────────────────────────────────────────────────────

void SubmessageHeader::encode(std::vector<uint8_t>& out) const {
    out.push_back(submessage_id);
    out.push_back(flags);
    put_u16_le(out, octets_to_next_header);
}

std::optional<SubmessageHeader> SubmessageHeader::decode(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kSize) return std::nullopt;
    SubmessageHeader h;
    h.submessage_id          = data[0];
    h.flags                  = data[1];
    h.octets_to_next_header  = get_u16_le(data + 2);
    return h;
}

// ── DATA submessage ───────────────────────────────────────────────────────────

void DataSubmessage::encode(std::vector<uint8_t>& out) const {
    // Body layout (§9.4.5.3): extraFlags(2) + octetsToInlineQos(2) +
    // readerId(4) + writerId(4) + seqNum.high(4) + seqNum.low(4) + payload.
    // octetsToInlineQos = 16: fixed distance from the end of that field to
    // the start of the payload, matching go-DDS's marshalDataSubmessage.
    std::vector<uint8_t> body;
    body.reserve(20 + payload.size());
    put_u16_le(body, 0);  // extraFlags
    put_u16_le(body, 16); // octetsToInlineQos
    body.insert(body.end(), reader_entity_id.bytes.begin(), reader_entity_id.bytes.end());
    body.insert(body.end(), writer_entity_id.bytes.begin(), writer_entity_id.bytes.end());
    put_i32_le(body, seq_num.high);
    put_u32_le(body, seq_num.low);
    body.insert(body.end(), payload.begin(), payload.end());

    SubmessageHeader hdr;
    hdr.submessage_id         = kSubmessageIdData;
    hdr.flags                 = kFlagEndianness | kFlagData;
    hdr.octets_to_next_header = static_cast<uint16_t>(body.size());

    out.reserve(out.size() + SubmessageHeader::kSize + body.size());
    hdr.encode(out);
    out.insert(out.end(), body.begin(), body.end());
}

std::optional<DataSubmessage> DataSubmessage::decode(const uint8_t* data, std::size_t len) {
    auto hdr = SubmessageHeader::decode(data, len);
    if (!hdr || hdr->submessage_id != kSubmessageIdData) return std::nullopt;

    const std::size_t body_len = hdr->octets_to_next_header;
    if (SubmessageHeader::kSize + body_len > len) return std::nullopt;
    if (body_len < 20) return std::nullopt;

    const uint8_t* body = data + SubmessageHeader::kSize;

    DataSubmessage ds;
    std::memcpy(ds.reader_entity_id.bytes.data(), body + 4, EntityId::kSize);
    std::memcpy(ds.writer_entity_id.bytes.data(), body + 8, EntityId::kSize);
    ds.seq_num.high = get_i32_le(body + 12);
    ds.seq_num.low  = get_u32_le(body + 16);

    if ((hdr->flags & kFlagData) != 0 && body_len > 20) {
        ds.payload.assign(body + 20, body + body_len);
    }
    return ds;
}

// ── Participant/entity identity allocation ──────────────────────────────────

GuidPrefix new_guid_prefix() {
    GuidPrefix p;
    // 8 bytes of entropy. std::random_device rather than the C standard
    // library's legacy pseudo-random generator, matching spdp.cpp's own
    // jitter-source rationale (CWE-330 / hidden global state under
    // concurrent use).
    std::random_device rd;
    for (std::size_t i = 0; i < 8; ++i) {
        p.bytes[i] = static_cast<uint8_t>(rd());
    }
    // Low 4 bytes of the process ID, matching go-DDS's newGuidPrefix — keeps
    // participants on the same host distinguishable even if rd() degrades
    // to a fixed value (some sandboxed environments do this).
#if defined(_WIN32)
    uint32_t pid = static_cast<uint32_t>(::GetCurrentProcessId());
#else
    uint32_t pid = static_cast<uint32_t>(::getpid());
#endif
    p.bytes[8]  = static_cast<uint8_t>(pid);
    p.bytes[9]  = static_cast<uint8_t>(pid >> 8);
    p.bytes[10] = static_cast<uint8_t>(pid >> 16);
    p.bytes[11] = static_cast<uint8_t>(pid >> 24);
    return p;
}

} // namespace dds::rtps
