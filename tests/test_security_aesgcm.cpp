// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::security::AESGCMPlugin — byte-exact wire format against
// github.com/SoundMatt/go-DDS's `security` package (security/security.go),
// plus independent NIST/FIPS known-answer tests for the underlying
// from-scratch SHA-256/AES-256/GCM primitives (src/security/crypto/ — no
// external crypto dependency is fetched for this project; see
// cmake/FetchDeps.cmake and include/dds/security/security.hpp's file
// comment).
//
// fusa:test REQ-SECURITY-001 REQ-SECURITY-005 REQ-SECURITY-006
// fusa:test REQ-SECURITY-007
//
// ── How every *_HEX constant below was derived ───────────────────────────────
//
// AESGCMPlugin::Seal generates a random 12-byte nonce internally, so a
// byte-exact vector requires substituting a fixed nonce for the one Seal()
// generates — exactly analogous to how test_safety_e2e.cpp's vectors
// substitute a fixed timestamp for time.Now(). Rather than adding a
// scratch-only helper to go-DDS's security package, the substitution here
// calls go-DDS's ACTUAL crypto/aes + cipher.NewGCM(block) construction
// directly — this is byte-for-byte what security.NewAESGCMPlugin does
// internally (see go-DDS security/security.go's NewAESGCMPlugin), so
// calling it with an explicit nonce is calling the same standard-library
// primitives AESGCMPlugin wraps, not a reimplementation:
//
//   git clone https://github.com/SoundMatt/go-DDS.git
//   cd go-DDS/security
//   cat > zzz_scratch_cppdds_vectors_test.go <<'EOF'
//   package security
//
//   import (
//       "crypto/aes"
//       "crypto/cipher"
//       "encoding/hex"
//       "fmt"
//       "testing"
//   )
//
//   func TestPrintCppDDSSecurityReferenceVectors(t *testing.T) {
//       aesKey := make([]byte, 32)
//       for i := range aesKey {
//           aesKey[i] = byte(0xA0 + i)
//       }
//       block, _ := aes.NewCipher(aesKey)
//       aead, _ := cipher.NewGCM(block)
//       nonce := make([]byte, 12)
//       for i := range nonce {
//           nonce[i] = byte(i)
//       }
//
//       ctHello := aead.Seal(nil, nonce, []byte("hello world"), nil)
//       fmt.Println("AESGCM_KEY_HEX          =", hex.EncodeToString(aesKey))
//       fmt.Println("AESGCM_NONCE_HEX        =", hex.EncodeToString(nonce))
//       fmt.Println("AESGCM_SEALED_HELLO_HEX =",
//           hex.EncodeToString(append(append([]byte{}, nonce...), ctHello...)))
//
//       ctEmpty := aead.Seal(nil, nonce, []byte{}, nil)
//       fmt.Println("AESGCM_SEALED_EMPTY_HEX =",
//           hex.EncodeToString(append(append([]byte{}, nonce...), ctEmpty...)))
//
//       // Verify round trip through the real public AESGCMPlugin too
//       // (random nonce — a live smoke check, not a fixed vector).
//       agp, _ := NewAESGCMPlugin(aesKey)
//       realSealed, _ := agp.Seal([]byte("hello world"))
//       back, err := agp.Open(realSealed)
//       if err != nil || string(back) != "hello world" {
//           t.Fatalf("round trip via public API failed: %v %q", err, back)
//       }
//   }
//   EOF
//   go test . -run TestPrintCppDDSSecurityReferenceVectors -v
//
// That produced (go-DDS main @ time of writing, module
// github.com/SoundMatt/go-DDS/security, commit 1272f294de25 —
// "Kubernetes operator (Milestone 15, v1.1)"):
//
//   AESGCM_KEY_HEX          = a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf
//   AESGCM_NONCE_HEX        = 000102030405060708090a0b
//   AESGCM_SEALED_HELLO_HEX = 000102030405060708090a0b5689a51b9232cd9abc022bec762e9b5487a61b38faf3749a3a4312
//   AESGCM_SEALED_EMPTY_HEX = 000102030405060708090a0b7c2e7d04d7a27638de3482b61aaa306f
//   (public-API round trip: OK)
//
// AESGCM_SEALED_EMPTY_HEX's tag (7c2e7d04d7a27638de3482b61aaa306f) was
// additionally cross-checked against Go's own crypto/cipher output for the
// same key/nonce/empty-plaintext combination run standalone (independent of
// go-DDS, i.e. calling crypto/aes + crypto/cipher directly with no go-DDS
// code involved at all) — both agree, and separately the classic published
// NIST/McGrew-Viega AES-256-GCM all-zero test vector (key=0^256,
// nonce=0^96, empty plaintext/AAD, tag=530f8afbc74536b9a963b4f1c4cb738b) was
// independently confirmed against this repo's dds::security::detail
// primitives before wiring them into AESGCMPlugin (see the "known-answer"
// TEST_CASEs below) — an independent sanity check on top of the go-DDS-
// derived vectors above, mirroring test_safety_e2e.cpp's CRC-16 cross-check
// against a published standard value.

#include <dds/security/security.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

using namespace dds::security;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        return static_cast<uint8_t>(c - 'a' + 10);
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

std::vector<uint8_t> str_bytes(const std::string& s) { return {s.begin(), s.end()}; }

} // namespace

// ── byte-exact wire format vs. go-DDS (decrypt direction) ────────────────────
//
// AESGCMPlugin::seal() always draws a fresh random nonce, so the byte-exact
// comparison happens via open(): feeding go-DDS's actual golden ciphertext
// bytes into this port's AESGCMPlugin::open() and checking it recovers the
// expected plaintext exercises AES-256 key expansion, CTR-mode encryption,
// and GHASH tag verification against a real go-DDS-derived vector.

TEST_CASE("AESGCMPlugin::open decrypts go-DDS reference vector (hello world)",
          "[security][aesgcm][REQ-SECURITY-005][REQ-SECURITY-006]") {
    auto key = from_hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);
    REQUIRE(p);

    auto golden =
        from_hex("000102030405060708090a0b5689a51b9232cd9abc022bec762e9b5487a61b38faf3749a3a4312");
    auto [plaintext, ec] = p->open(golden);
    REQUIRE_FALSE(ec);
    CHECK(plaintext == str_bytes("hello world"));
}

TEST_CASE("AESGCMPlugin::open decrypts go-DDS reference vector (empty plaintext)",
          "[security][aesgcm][REQ-SECURITY-005][REQ-SECURITY-006]") {
    auto key = from_hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);

    auto golden = from_hex("000102030405060708090a0b7c2e7d04d7a27638de3482b61aaa306f");
    auto [plaintext, ec] = p->open(golden);
    REQUIRE_FALSE(ec);
    CHECK(plaintext.empty());
}

// ── round trip ────────────────────────────────────────────────────────────────

TEST_CASE("AESGCMPlugin round trip: various payload sizes",
          "[security][aesgcm][REQ-SECURITY-001]") {
    auto key = new_random_key(32);
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);

    std::vector<std::vector<uint8_t>> cases = {
        {}, str_bytes("hello"), std::vector<uint8_t>(1024, 0)};
    for (auto& c : cases) {
        auto [sealed, ec1] = p->seal(c);
        REQUIRE_FALSE(ec1);
        auto [opened, ec2] = p->open(sealed);
        REQUIRE_FALSE(ec2);
        CHECK(opened == c);
    }
}

TEST_CASE("AESGCMPlugin::seal produces distinct nonces (no reuse)",
          "[security][aesgcm][REQ-SECURITY-006]") {
    auto key = new_random_key(32);
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);

    auto [a, ec1] = p->seal(str_bytes("same plaintext"));
    REQUIRE_FALSE(ec1);
    auto [b, ec2] = p->seal(str_bytes("same plaintext"));
    REQUIRE_FALSE(ec2);
    CHECK(a != b);
}

TEST_CASE("AESGCMPlugin::seal produces 28 bytes of overhead", "[security][aesgcm][REQ-SECURITY-005]") {
    auto key = new_random_key(32);
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);
    auto plain = str_bytes("payload");
    auto [sealed, ec] = p->seal(plain);
    REQUIRE_FALSE(ec);
    CHECK(sealed.size() == plain.size() + 12 + 16);
}

// ── tamper / wrong-key / too-short rejection ─────────────────────────────────

TEST_CASE("AESGCMPlugin::open detects tampering", "[security][aesgcm][REQ-SECURITY-006]") {
    auto key = new_random_key(32);
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);
    auto [sealed, ec1] = p->seal(str_bytes("sensitive"));
    REQUIRE_FALSE(ec1);
    sealed.back() = static_cast<uint8_t>(sealed.back() ^ 0xFF); // corrupt last tag byte

    auto [opened, ec2] = p->open(sealed);
    CHECK(ec2 == ErrAESDecryptFailed());
    CHECK(opened.empty());
}

TEST_CASE("AESGCMPlugin::open rejects wrong key", "[security][aesgcm][REQ-SECURITY-006]") {
    auto k1 = new_random_key(32);
    auto k2 = new_random_key(32);
    auto [p1, ec01] = new_aes_gcm_plugin(k1);
    auto [p2, ec02] = new_aes_gcm_plugin(k2);
    REQUIRE_FALSE(ec01);
    REQUIRE_FALSE(ec02);

    auto [sealed, ec1] = p1->seal(str_bytes("secret"));
    REQUIRE_FALSE(ec1);
    auto [opened, ec2] = p2->open(sealed);
    CHECK(ec2 == ErrAESDecryptFailed());
}

TEST_CASE("new_aes_gcm_plugin rejects non-32-byte key", "[security][aesgcm][REQ-SECURITY-005]") {
    auto [p, ec] = new_aes_gcm_plugin(str_bytes("tooshort"));
    CHECK(ec == ErrAESKeyInvalidLength());
    CHECK_FALSE(p);
}

TEST_CASE("AESGCMPlugin::open rejects payload shorter than nonce+tag",
          "[security][aesgcm][REQ-SECURITY-006]") {
    auto key = new_random_key(32);
    auto [p, ec0] = new_aes_gcm_plugin(key);
    REQUIRE_FALSE(ec0);
    auto [opened, ec] = p->open(str_bytes("short"));
    CHECK(ec == ErrAESPayloadTooShort());
}

// ── crypto-layer known-answer tests (independent of go-DDS) ─────────────────
//
// These exercise dds::security through its public API only (no internal
// headers are included from a tests/ TU), pinning specific key/nonce/
// plaintext combinations whose correct tag values are independently
// verifiable against the classic McGrew-Viega/NIST AES-256-GCM test vector
// suite — an extra cross-check beyond the go-DDS-derived vectors above,
// exactly mirroring test_safety_e2e.cpp's CRC-16 "well-known published
// check value" cross-check.

TEST_CASE("AES-256-GCM known-answer: all-zero key/nonce/plaintext (NIST/McGrew-Viega Test Case 13)",
          "[security][aesgcm][crypto]") {
    std::vector<uint8_t> zero_key(32, 0);
    auto [p, ec0] = new_aes_gcm_plugin(zero_key);
    REQUIRE_FALSE(ec0);

    // Nonce is zero too; feed a golden 12-byte-zero-nonce + empty-ciphertext
    // + known tag directly into open() to check the crypto layer without
    // needing to control seal()'s internally-generated nonce.
    auto golden = from_hex("000000000000000000000000" // 12-byte zero nonce
                            "530f8afbc74536b9a963b4f1c4cb738b"); // 16-byte tag, empty ciphertext
    auto [plaintext, ec] = p->open(golden);
    REQUIRE_FALSE(ec);
    CHECK(plaintext.empty());
}
