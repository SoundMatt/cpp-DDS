// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal AES-256-GCM AEAD (NIST SP 800-38D), 96-bit nonce, 128-bit tag, no
// additional authenticated data — the exact mode Go's
// cipher.NewGCM(aes.NewCipher(key)) implements for a 12-byte nonce and nil
// AAD, which is what go-DDS's security.AESGCMPlugin uses internally. Not a
// public dds:: header — included only by src/security/security.cpp.
// Verified byte-exact against Go's crypto/cipher GCM output (including the
// classic all-zero-key/nonce/plaintext NIST GCM test vector, tag
// 530f8afbc74536b9a963b4f1c4cb738b) and against go-DDS's real
// security.AESGCMPlugin/NewAESGCMPlugin output — see
// tests/test_security_aesgcm.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dds::security::detail {

// aes256_gcm_seal encrypts plaintext (len bytes) under key/nonce, appending
// no padding: out_ciphertext receives exactly `len` bytes and out_tag[16]
// receives the GCM authentication tag.
void aes256_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* plaintext,
                      std::size_t len, std::vector<uint8_t>& out_ciphertext, uint8_t out_tag[16]);

// aes256_gcm_open verifies tag against key/nonce/ciphertext and, on success,
// fills out_plaintext with the decrypted bytes and returns true. On tag
// mismatch it returns false and leaves out_plaintext unspecified.
bool aes256_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ciphertext,
                      std::size_t len, const uint8_t tag[16], std::vector<uint8_t>& out_plaintext);

} // namespace dds::security::detail
