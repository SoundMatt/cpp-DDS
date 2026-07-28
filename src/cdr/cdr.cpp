// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/cdr/cdr.hpp>

#include <cstring>

// fusa:req REQ-CDR-001 REQ-CDR-002 REQ-CDR-003 REQ-CDR-004 REQ-CDR-005

namespace dds::cdr {

namespace {

// encapHeader is CDR_LE: scheme 0x0001 written little-endian = bytes
// {0x01,0x00,0x00,0x00} (go-DDS: cdr.encapHeader, RTPS/XCDR1 §10.2 Table 10.1).
constexpr uint8_t kEncapHeader[4] = {0x01, 0x00, 0x00, 0x00};
constexpr std::size_t kEncapHeaderLen = 4;

template <typename T>
void append_le(std::vector<uint8_t>& buf, T v) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &v, sizeof(T));
    // This codebase targets little-endian platforms only (matching every
    // other byte-exact wire-format port in this repo, e.g. dds::rtps::cdr);
    // a straight memcpy is therefore already little-endian.
    buf.insert(buf.end(), raw, raw + sizeof(T));
}

template <typename T>
T read_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

} // namespace

// ── Encoder ───────────────────────────────────────────────────────────────────

Encoder::Encoder() {
    buf_.insert(buf_.end(), kEncapHeader, kEncapHeader + kEncapHeaderLen);
}

void Encoder::align(std::size_t n) {
    std::size_t pad = (n - (buf_.size() % n)) % n;
    buf_.insert(buf_.end(), pad, 0);
}

void Encoder::write_bool(bool v) { buf_.push_back(v ? 1 : 0); }

void Encoder::write_uint8(uint8_t v) { buf_.push_back(v); }

void Encoder::write_int8(int8_t v) { buf_.push_back(static_cast<uint8_t>(v)); }

void Encoder::write_int16(int16_t v) {
    align(2);
    append_le<uint16_t>(buf_, static_cast<uint16_t>(v));
}

void Encoder::write_uint16(uint16_t v) {
    align(2);
    append_le<uint16_t>(buf_, v);
}

void Encoder::write_int32(int32_t v) {
    align(4);
    append_le<uint32_t>(buf_, static_cast<uint32_t>(v));
}

void Encoder::write_uint32(uint32_t v) {
    align(4);
    append_le<uint32_t>(buf_, v);
}

void Encoder::write_int64(int64_t v) {
    align(8);
    append_le<uint64_t>(buf_, static_cast<uint64_t>(v));
}

void Encoder::write_uint64(uint64_t v) {
    align(8);
    append_le<uint64_t>(buf_, v);
}

void Encoder::write_float32(float v) {
    align(4);
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    append_le<uint32_t>(buf_, bits);
}

void Encoder::write_float64(double v) {
    align(8);
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    append_le<uint64_t>(buf_, bits);
}

void Encoder::write_string(const std::string& s) {
    align(4);
    append_le<uint32_t>(buf_, static_cast<uint32_t>(s.size() + 1));
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0); // null terminator
}

void Encoder::write_bytes(const std::vector<uint8_t>& b) {
    align(4);
    append_le<uint32_t>(buf_, static_cast<uint32_t>(b.size()));
    buf_.insert(buf_.end(), b.begin(), b.end());
}

// ── Decoder ───────────────────────────────────────────────────────────────────

std::optional<Decoder> Decoder::create(const uint8_t* data, std::size_t len) {
    if (len < kEncapHeaderLen) {
        return std::nullopt;
    }
    // Accept CDR_LE (0x0001) and CDR_BE (0x0000); decode LE only (go-DDS:
    // cdr.NewDecoder() — the same accept-both-emit-LE-only quirk).
    uint16_t scheme = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    if (scheme != 0x0001 && scheme != 0x0000) {
        return std::nullopt;
    }
    return Decoder(data, len, kEncapHeaderLen);
}

std::optional<Decoder> Decoder::create(const std::vector<uint8_t>& data) {
    return create(data.data(), data.size());
}

void Decoder::align(std::size_t n) noexcept {
    std::size_t pad = (n - (pos_ % n)) % n;
    pos_ += pad;
}

bool Decoder::need(std::size_t n) const noexcept { return pos_ + n <= len_; }

std::optional<bool> Decoder::read_bool() {
    if (!need(1)) return std::nullopt;
    bool v = data_[pos_] != 0;
    ++pos_;
    return v;
}

std::optional<uint8_t> Decoder::read_uint8() {
    if (!need(1)) return std::nullopt;
    uint8_t v = data_[pos_];
    ++pos_;
    return v;
}

std::optional<int8_t> Decoder::read_int8() {
    auto v = read_uint8();
    if (!v) return std::nullopt;
    return static_cast<int8_t>(*v);
}

std::optional<int16_t> Decoder::read_int16() {
    align(2);
    if (!need(2)) return std::nullopt;
    int16_t v = static_cast<int16_t>(read_le<uint16_t>(data_ + pos_));
    pos_ += 2;
    return v;
}

std::optional<uint16_t> Decoder::read_uint16() {
    align(2);
    if (!need(2)) return std::nullopt;
    uint16_t v = read_le<uint16_t>(data_ + pos_);
    pos_ += 2;
    return v;
}

std::optional<int32_t> Decoder::read_int32() {
    align(4);
    if (!need(4)) return std::nullopt;
    int32_t v = static_cast<int32_t>(read_le<uint32_t>(data_ + pos_));
    pos_ += 4;
    return v;
}

std::optional<uint32_t> Decoder::read_uint32() {
    align(4);
    if (!need(4)) return std::nullopt;
    uint32_t v = read_le<uint32_t>(data_ + pos_);
    pos_ += 4;
    return v;
}

std::optional<int64_t> Decoder::read_int64() {
    align(8);
    if (!need(8)) return std::nullopt;
    int64_t v = static_cast<int64_t>(read_le<uint64_t>(data_ + pos_));
    pos_ += 8;
    return v;
}

std::optional<uint64_t> Decoder::read_uint64() {
    align(8);
    if (!need(8)) return std::nullopt;
    uint64_t v = read_le<uint64_t>(data_ + pos_);
    pos_ += 8;
    return v;
}

std::optional<float> Decoder::read_float32() {
    auto bits = read_uint32();
    if (!bits) return std::nullopt;
    float v;
    std::memcpy(&v, &*bits, sizeof(v));
    return v;
}

std::optional<double> Decoder::read_float64() {
    auto bits = read_uint64();
    if (!bits) return std::nullopt;
    double v;
    std::memcpy(&v, &*bits, sizeof(v));
    return v;
}

std::optional<std::string> Decoder::read_string() {
    auto n = read_uint32();
    if (!n) return std::nullopt;
    if (*n == 0) return std::string();
    if (!need(*n)) return std::nullopt;
    const uint8_t* raw = data_ + pos_;
    std::size_t rawLen = *n;
    pos_ += rawLen;
    // Strip null terminator.
    if (rawLen > 0 && raw[rawLen - 1] == 0) {
        --rawLen;
    }
    return std::string(reinterpret_cast<const char*>(raw), rawLen);
}

std::optional<std::vector<uint8_t>> Decoder::read_bytes() {
    auto n = read_uint32();
    if (!n) return std::nullopt;
    if (!need(*n)) return std::nullopt;
    std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + *n);
    pos_ += *n;
    return out;
}

} // namespace dds::cdr
