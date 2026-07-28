// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/security/security.hpp — pluggable transport-security for cpp-DDS.
//
// C++ port of github.com/SoundMatt/go-DDS's `security` package
// (security/security.go): every outbound payload can be passed through
// Plugin::seal before transmission, and every inbound payload through
// Plugin::open before delivery to the application. See ROADMAP.md, "Tier 2
// — safety and security", `security`.
//
// Two built-in plugins are provided (plus the identity NullPlugin):
//
//   - NullPlugin   — identity transform; no confidentiality, no integrity.
//   - HMACPlugin   — appends an HMAC-SHA-256 tag to each payload. Integrity
//                    and authentication without confidentiality; zero
//                    payload expansion beyond the 32-byte tag.
//   - AESGCMPlugin — encrypts with AES-256-GCM (AEAD). Confidentiality,
//                    integrity, and authenticity; payload expands by 12
//                    bytes (nonce) + 16 bytes (GCM tag) = 28 bytes.
//
// Wire formats (byte-for-byte identical to go-DDS's, verified against
// reference vectors independently derived from a fresh go-DDS clone — see
// tests/test_security_hmac.cpp / tests/test_security_aesgcm.cpp):
//
//   HMACPlugin:   | plaintext... | HMAC-SHA-256[32] |
//   AESGCMPlugin: | nonce[12] | AES-256-GCM ciphertext... | GCM-tag[16] |
//
// No external crypto dependency is fetched for this project (see
// cmake/FetchDeps.cmake); SHA-256/HMAC/AES-256/GCM are implemented from
// scratch in src/security/crypto/ (an internal, non-public implementation
// detail) and independently verified against FIPS/RFC/NIST known-answer
// test vectors as well as go-DDS's actual crypto/aes + crypto/cipher +
// crypto/hmac stdlib output.
//
// go-DDS's `security.cert`/`security.discovery` (PKI-based mutual
// authentication and DDS-discovery security wrapping, security/cert.go +
// security/discovery.go) are separate surfaces beyond ROADMAP.md's stated
// scope for this item ("HMAC-SHA-256 message authentication, AES-256-GCM
// encryption layer, topic ACL, anti-replay sequence number enforcement")
// and are out of scope here, matching the `ddssafety`/E2E item's precedent
// of scoping out go-DDS's safety/metrics.go.
//
// Scope: internal, additive — dds::security::Plugin is a standalone
// seal/open library, not wired into dds::adapt(), dds::mock, or the RTPS
// transport, matching dds::safety::E2EPublisher's own precedent (see
// include/dds/safety/e2e.hpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace dds::security {

// ── Error codes ───────────────────────────────────────────────────────────────

// fusa:req REQ-SECURITY-004 REQ-SECURITY-006
enum class Errc : int {
    hmac_too_short           = 1,
    hmac_verification_failed = 2,
    aes_key_invalid_length   = 3,
    aes_payload_too_short    = 4,
    aes_decrypt_failed       = 5,
    replay_detected          = 6,
};

const std::error_category& error_category() noexcept;
std::error_code             make_error_code(Errc e) noexcept;

inline std::error_code ErrHMACTooShort() noexcept {
    return make_error_code(Errc::hmac_too_short);
}
inline std::error_code ErrHMACVerificationFailed() noexcept {
    return make_error_code(Errc::hmac_verification_failed);
}
inline std::error_code ErrAESKeyInvalidLength() noexcept {
    return make_error_code(Errc::aes_key_invalid_length);
}
inline std::error_code ErrAESPayloadTooShort() noexcept {
    return make_error_code(Errc::aes_payload_too_short);
}
inline std::error_code ErrAESDecryptFailed() noexcept {
    return make_error_code(Errc::aes_decrypt_failed);
}
// ErrReplay — shared with dds::security::replay.hpp's ReplayGuard, mirrors
// go-DDS's exported `security.ErrReplay` sentinel.
inline std::error_code ErrReplay() noexcept {
    return make_error_code(Errc::replay_detected);
}

// ── Plugin ────────────────────────────────────────────────────────────────────

// Plugin is implemented by any type that can seal (encrypt/sign) and open
// (decrypt/verify) a DDS payload. seal and open must be inverses:
// plaintext == open(seal(plaintext)).second for any plaintext, when both
// calls succeed. Implementations must be safe for concurrent use from
// multiple threads (fusa:req REQ-SAFETY-001).
// fusa:req REQ-SECURITY-001
class Plugin {
public:
    virtual ~Plugin() = default;

    // seal transforms plaintext into a protected form ready for
    // transmission. Returns {sealed, {}} on success or {{}, ec} on failure.
    virtual std::pair<std::vector<uint8_t>, std::error_code>
    seal(const std::vector<uint8_t>& plaintext) = 0;

    // open reverses seal, returning the original plaintext. Returns an
    // error if the payload is invalid, tampered, or cannot be decrypted.
    virtual std::pair<std::vector<uint8_t>, std::error_code>
    open(const std::vector<uint8_t>& ciphertext) = 0;
};

// ── NullPlugin ────────────────────────────────────────────────────────────────

// NullPlugin is the identity transform: seal and open return the input
// unchanged. Use when no security is required.
// fusa:req REQ-SECURITY-002
class NullPlugin : public Plugin {
public:
    std::pair<std::vector<uint8_t>, std::error_code>
    seal(const std::vector<uint8_t>& plaintext) override {
        return {plaintext, {}};
    }
    std::pair<std::vector<uint8_t>, std::error_code>
    open(const std::vector<uint8_t>& ciphertext) override {
        return {ciphertext, {}};
    }
};

// ── HMACPlugin ────────────────────────────────────────────────────────────────

// HMACPlugin appends an HMAC-SHA-256 authentication tag to each payload. It
// provides integrity and peer authentication but NOT confidentiality — the
// payload travels in plaintext. Wire format: | plaintext... | HMAC[32] |.
// fusa:req REQ-SECURITY-003 REQ-SECURITY-004 REQ-SECURITY-007
class HMACPlugin : public Plugin {
public:
    // Constructs an HMACPlugin keyed with key. The key is copied — later
    // mutation of the caller's buffer does not affect this plugin
    // (REQ-SECURITY-007). The key should be at least 32 bytes of random
    // data; see new_random_key.
    explicit HMACPlugin(const std::vector<uint8_t>& key);

    std::pair<std::vector<uint8_t>, std::error_code>
    seal(const std::vector<uint8_t>& plaintext) override;
    std::pair<std::vector<uint8_t>, std::error_code>
    open(const std::vector<uint8_t>& data) override;

private:
    std::vector<uint8_t> key_;
};

// new_hmac_plugin constructs an HMACPlugin (go-DDS: security.NewHMACPlugin).
std::shared_ptr<HMACPlugin> new_hmac_plugin(const std::vector<uint8_t>& key);

// ── AESGCMPlugin ──────────────────────────────────────────────────────────────

// AESGCMPlugin encrypts payloads with AES-256-GCM (authenticated
// encryption): confidentiality, integrity, and authenticity. Each seal call
// generates a fresh 12-byte random nonce prepended to the ciphertext. Wire
// format: | nonce[12] | ciphertext... | GCM-tag[16] | — 28 bytes overhead.
// fusa:req REQ-SECURITY-005 REQ-SECURITY-006 REQ-SECURITY-007
class AESGCMPlugin : public Plugin {
public:
    std::pair<std::vector<uint8_t>, std::error_code>
    seal(const std::vector<uint8_t>& plaintext) override;
    std::pair<std::vector<uint8_t>, std::error_code>
    open(const std::vector<uint8_t>& data) override;

private:
    friend std::pair<std::shared_ptr<AESGCMPlugin>, std::error_code>
    new_aes_gcm_plugin(const std::vector<uint8_t>& key);

    explicit AESGCMPlugin(std::vector<uint8_t> key) : key_(std::move(key)) {}

    std::vector<uint8_t> key_;
};

// new_aes_gcm_plugin constructs an AESGCMPlugin. key must be exactly 32
// bytes (AES-256); returns ErrAESKeyInvalidLength otherwise (go-DDS:
// security.NewAESGCMPlugin).
std::pair<std::shared_ptr<AESGCMPlugin>, std::error_code>
new_aes_gcm_plugin(const std::vector<uint8_t>& key);

// ── Key utilities ─────────────────────────────────────────────────────────────

// new_random_key returns n cryptographically random bytes suitable for use
// as a plugin key, drawn from the OS entropy source (go-DDS:
// security.NewRandomKey).
std::vector<uint8_t> new_random_key(std::size_t n);

} // namespace dds::security

namespace std {
template <>
struct is_error_code_enum<dds::security::Errc> : true_type {};
} // namespace std
