// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/cdr.hpp>

#include <cstring>

// C++ port of github.com/SoundMatt/go-DDS rtps/cdr.go (plCDREncoder /
// plCDRDecoder) plus the cdrWrapPayload/cdrUnwrapPayload helpers from
// rtps/message.go. See include/dds/rtps/cdr.hpp for the phase scope and
// the byte-identity contract this file must uphold.

namespace dds::rtps {

namespace {

// ── little-endian primitives ────────────────────────────────────────────────
// Mirrors Go's encoding/binary.LittleEndian, same as rtps/types.cpp.

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

} // namespace

// ── PLCDREncoder ─────────────────────────────────────────────────────────────

PLCDREncoder::PLCDREncoder() {
    // Encapsulation header: scheme (2 bytes LE) + options (2 bytes, zero).
    put_u16_le(buf_, kSchemePLCDRLE);
    put_u16_le(buf_, 0);
}

void PLCDREncoder::add_param(uint16_t pid, const uint8_t* value, std::size_t len) {
    const std::size_t padded = (len + 3) & ~static_cast<std::size_t>(3);
    put_u16_le(buf_, pid);
    put_u16_le(buf_, static_cast<uint16_t>(padded));
    buf_.insert(buf_.end(), value, value + len);
    for (std::size_t i = len; i < padded; ++i) {
        buf_.push_back(0x00);
    }
}

void PLCDREncoder::add_param(uint16_t pid, const std::vector<uint8_t>& value) {
    add_param(pid, value.data(), value.size());
}

void PLCDREncoder::add_uint32(uint16_t pid, uint32_t v) {
    std::vector<uint8_t> b;
    put_u32_le(b, v);
    add_param(pid, b);
}

void PLCDREncoder::add_locator(uint16_t pid, const Locator& l) {
    std::vector<uint8_t> b;
    b.reserve(Locator::kSize);
    l.encode(b);
    add_param(pid, b);
}

void PLCDREncoder::add_guid(uint16_t pid, const GUID& g) {
    std::vector<uint8_t> b;
    b.reserve(GUID::kSize);
    g.encode(b);
    add_param(pid, b);
}

void PLCDREncoder::add_string(uint16_t pid, const std::string& s) {
    // uint32 length (chars + null terminator) + chars + null terminator.
    const uint32_t str_len = static_cast<uint32_t>(s.size() + 1);
    std::vector<uint8_t> raw;
    raw.reserve(4 + s.size() + 1);
    put_u32_le(raw, str_len);
    raw.insert(raw.end(), s.begin(), s.end());
    raw.push_back(0x00); // null terminator
    add_param(pid, raw);
}

void PLCDREncoder::add_bytes(uint16_t pid, const std::vector<uint8_t>& v) {
    add_param(pid, v);
}

std::vector<uint8_t> PLCDREncoder::finish() const {
    std::vector<uint8_t> out = buf_;
    put_u16_le(out, kPidSentinel);
    put_u16_le(out, 0);
    return out;
}

// ── PLCDRDecoder ─────────────────────────────────────────────────────────────

std::optional<PLCDRDecoder> PLCDRDecoder::create(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 4) return std::nullopt;
    const uint16_t scheme = get_u16_le(data);
    if (scheme != kSchemePLCDRLE) return std::nullopt;
    return PLCDRDecoder(data, len, 4);
}

std::optional<Param> PLCDRDecoder::next() {
    for (;;) {
        if (pos_ + 4 > len_) return std::nullopt;
        const uint16_t pid    = get_u16_le(data_ + pos_);
        const std::size_t length = get_u16_le(data_ + pos_ + 2);
        pos_ += 4;
        if (pid == kPidSentinel) return std::nullopt;
        if (pid == kPidPad) continue; // skip, matching go-DDS's recursive next()
        if (pos_ + length > len_) return std::nullopt;
        Param p;
        p.pid       = pid;
        p.value     = data_ + pos_;
        p.value_len = length;
        pos_ += length;
        return p;
    }
}

// ── standalone decode helpers ────────────────────────────────────────────────

std::optional<std::string> decode_string(const uint8_t* value, std::size_t len) {
    if (value == nullptr || len < 4) return std::nullopt;
    const std::size_t n = get_u32_le(value);
    if (len < 4 + n) return std::nullopt;
    const uint8_t* s = value + 4;
    std::size_t s_len = n;
    if (s_len > 0 && s[s_len - 1] == 0) {
        --s_len;
    }
    return std::string(reinterpret_cast<const char*>(s), s_len);
}

// ── CDR_LE payload encapsulation ─────────────────────────────────────────────

std::vector<uint8_t> cdr_wrap_payload(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(4 + payload.size());
    put_u16_le(out, kSchemeCDRLE);
    put_u16_le(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<std::vector<uint8_t>> cdr_unwrap_payload(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 4) return std::nullopt;
    const uint16_t scheme = get_u16_le(data);
    if (scheme != kSchemeCDRLE && scheme != kSchemePLCDRLE) return std::nullopt;
    return std::vector<uint8_t>(data + 4, data + len);
}

} // namespace dds::rtps
