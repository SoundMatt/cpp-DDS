// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/security/security.hpp>

#include "crypto/gcm.hpp"
#include "crypto/sha256.hpp"

#include <cstring>
#include <random>

// fusa:req REQ-SECURITY-001 REQ-SECURITY-002 REQ-SECURITY-003
// fusa:req REQ-SECURITY-004 REQ-SECURITY-005 REQ-SECURITY-006 REQ-SECURITY-007

namespace dds::security {

// ── Error category ───────────────────────────────────────────────────────────

namespace {

class SecurityErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "dds.security"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::hmac_too_short:           return "security: HMAC payload too short";
        case Errc::hmac_verification_failed: return "security: HMAC verification failed";
        case Errc::aes_key_invalid_length:   return "security: AES-GCM key must be 32 bytes (AES-256)";
        case Errc::aes_payload_too_short:    return "security: AES-GCM payload too short";
        case Errc::aes_decrypt_failed:       return "security: AES-GCM decryption/authentication failed";
        case Errc::replay_detected:          return "security: replayed sequence number detected";
        default:                             return "security: unknown error";
        }
    }
};

} // namespace

const std::error_category& error_category() noexcept {
    static SecurityErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

// ── HMACPlugin ────────────────────────────────────────────────────────────────

// fusa:req REQ-SECURITY-003
inline constexpr std::size_t kHmacSize = 32;

HMACPlugin::HMACPlugin(const std::vector<uint8_t>& key) : key_(key) {}

std::pair<std::vector<uint8_t>, std::error_code>
HMACPlugin::seal(const std::vector<uint8_t>& plaintext) {
    auto tag = detail::hmac_sha256(key_.data(), key_.size(), plaintext.data(), plaintext.size());

    std::vector<uint8_t> out(plaintext.size() + kHmacSize);
    std::memcpy(out.data(), plaintext.data(), plaintext.size());
    std::memcpy(out.data() + plaintext.size(), tag.data(), tag.size());
    return {std::move(out), {}};
}

std::pair<std::vector<uint8_t>, std::error_code>
HMACPlugin::open(const std::vector<uint8_t>& data) {
    if (data.size() < kHmacSize) {
        return {{}, ErrHMACTooShort()};
    }
    std::size_t plain_len = data.size() - kHmacSize;
    auto expected = detail::hmac_sha256(key_.data(), key_.size(), data.data(), plain_len);

    // Constant-time comparison of the received tag against the expected one
    // — this is an authentication check on attacker-controlled data.
    uint8_t diff = 0;
    for (std::size_t i = 0; i < kHmacSize; i++) {
        diff = uint8_t(diff | (expected[i] ^ data[plain_len + i]));
    }
    if (diff != 0) {
        return {{}, ErrHMACVerificationFailed()};
    }
    return {std::vector<uint8_t>(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(plain_len)), {}};
}

std::shared_ptr<HMACPlugin> new_hmac_plugin(const std::vector<uint8_t>& key) {
    return std::make_shared<HMACPlugin>(key);
}

// ── AESGCMPlugin ──────────────────────────────────────────────────────────────

// fusa:req REQ-SECURITY-005
inline constexpr std::size_t kAesGcmKeySize = 32;
inline constexpr std::size_t kAesGcmNonceSize = 12;
inline constexpr std::size_t kAesGcmTagSize = 16;

std::pair<std::vector<uint8_t>, std::error_code>
AESGCMPlugin::seal(const std::vector<uint8_t>& plaintext) {
    uint8_t nonce[kAesGcmNonceSize];
    {
        // fusa:req REQ-SECURITY-007 — fresh CSPRNG-drawn nonce per call, so
        // two Seal() calls on identical plaintext never produce identical
        // output (no nonce reuse).
        std::random_device rd;
        for (auto& b : nonce) b = static_cast<uint8_t>(rd());
    }

    std::vector<uint8_t> ct;
    uint8_t               tag[kAesGcmTagSize];
    detail::aes256_gcm_seal(key_.data(), nonce, plaintext.data(), plaintext.size(), ct, tag);

    std::vector<uint8_t> out;
    out.reserve(kAesGcmNonceSize + ct.size() + kAesGcmTagSize);
    out.insert(out.end(), nonce, nonce + kAesGcmNonceSize);
    out.insert(out.end(), ct.begin(), ct.end());
    out.insert(out.end(), tag, tag + kAesGcmTagSize);
    return {std::move(out), {}};
}

std::pair<std::vector<uint8_t>, std::error_code>
AESGCMPlugin::open(const std::vector<uint8_t>& data) {
    if (data.size() < kAesGcmNonceSize + kAesGcmTagSize) {
        return {{}, ErrAESPayloadTooShort()};
    }
    const uint8_t* nonce    = data.data();
    std::size_t    ct_len   = data.size() - kAesGcmNonceSize - kAesGcmTagSize;
    const uint8_t* ct       = data.data() + kAesGcmNonceSize;
    const uint8_t* tag      = data.data() + kAesGcmNonceSize + ct_len;

    std::vector<uint8_t> plaintext;
    if (!detail::aes256_gcm_open(key_.data(), nonce, ct, ct_len, tag, plaintext)) {
        return {{}, ErrAESDecryptFailed()};
    }
    return {std::move(plaintext), {}};
}

std::pair<std::shared_ptr<AESGCMPlugin>, std::error_code>
new_aes_gcm_plugin(const std::vector<uint8_t>& key) {
    if (key.size() != kAesGcmKeySize) {
        return {nullptr, ErrAESKeyInvalidLength()};
    }
    // std::shared_ptr<AESGCMPlugin>(new AESGCMPlugin(...)) rather than
    // make_shared, since the constructor is private (friend-only factory).
    return {std::shared_ptr<AESGCMPlugin>(new AESGCMPlugin(key)), {}};
}

// ── Key utilities ─────────────────────────────────────────────────────────────

std::vector<uint8_t> new_random_key(std::size_t n) {
    // std::random_device draws from the OS entropy source on all supported
    // platforms (libstdc++/libc++/MSVC back it with a real CSPRNG) —
    // matching the entropy-source rationale already established in
    // src/rtps/types.cpp's new_guid_prefix().
    std::vector<uint8_t> key(n);
    std::random_device   rd;
    for (auto& b : key) b = static_cast<uint8_t>(rd());
    return key;
}

} // namespace dds::security
