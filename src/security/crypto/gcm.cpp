// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gcm.hpp"
#include "aes.hpp"

#include <cstring>

namespace dds::security::detail {

namespace {

// GF(2^128) multiplication as defined by NIST SP 800-38D §6.3. X and Y are
// 16-byte blocks in the standard big-endian bit ordering (bit 0 = MSB of
// byte 0). Bit-serial algorithm — not performance-optimized, but a direct,
// easily-audited transcription of the spec, matching common GHASH reference
// implementations.
void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    std::memcpy(v, y, 16);

    for (int i = 0; i < 128; i++) {
        int byte_i = i / 8;
        int bit_i  = 7 - (i % 8);
        if (x[byte_i] & (1 << bit_i)) {
            for (int k = 0; k < 16; k++) z[k] = uint8_t(z[k] ^ v[k]);
        }
        bool lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) {
            v[k] = uint8_t((v[k] >> 1) | ((v[k - 1] & 1) << 7));
        }
        v[0] = uint8_t(v[0] >> 1);
        if (lsb) {
            v[0] = uint8_t(v[0] ^ 0xE1); // R = 11100001 || 0^120
        }
    }
    std::memcpy(out, z, 16);
}

// ghash_update folds `blocks` 16-byte blocks of `data` into the running
// GHASH value y (in/out), using key h.
void ghash_update(const uint8_t h[16], const uint8_t* data, std::size_t blocks, uint8_t y[16]) {
    for (std::size_t b = 0; b < blocks; b++) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) y[i] = uint8_t(y[i] ^ data[b * 16 + static_cast<std::size_t>(i)]);
        gf128_mul(y, h, block);
        std::memcpy(y, block, 16);
    }
}

// inc32 increments the last 32 bits (big-endian) of a 16-byte counter block.
inline void inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

// gctr XORs `len` bytes of `in` with the AES-CTR keystream starting at
// counter block `icb` (not modified), writing the result to `out`.
void gctr(const Aes256& aes, const uint8_t icb[16], const uint8_t* in, std::size_t len, uint8_t* out) {
    uint8_t counter[16];
    std::memcpy(counter, icb, 16);
    std::size_t off = 0;
    while (off < len) {
        uint8_t ks[16];
        aes.encrypt_block(counter, ks);
        std::size_t n = (len - off) < 16 ? (len - off) : 16;
        for (std::size_t i = 0; i < n; i++) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
        off += n;
        inc32(counter);
    }
}

// compute_tag computes the GCM authentication tag over ciphertext with
// empty additional authenticated data (go-DDS's AESGCMPlugin never passes
// AAD to aead.Seal/Open).
void compute_tag(const Aes256& aes, const uint8_t h[16], const uint8_t j0[16],
                  const uint8_t* ciphertext, std::size_t ct_len, uint8_t tag_out[16]) {
    uint8_t y[16] = {0};

    std::size_t full_blocks = ct_len / 16;
    if (full_blocks > 0) {
        ghash_update(h, ciphertext, full_blocks, y);
    }
    std::size_t rem = ct_len % 16;
    if (rem > 0) {
        uint8_t last[16] = {0};
        std::memcpy(last, ciphertext + full_blocks * 16, rem);
        ghash_update(h, last, 1, y);
    }

    // Length block: 64-bit AAD bit-length (always 0 here) || 64-bit
    // ciphertext bit-length, big-endian.
    uint8_t  len_block[16] = {0};
    uint64_t ct_bits       = uint64_t(ct_len) * 8;
    for (int i = 0; i < 8; i++) {
        len_block[8 + i] = uint8_t(ct_bits >> (56 - 8 * i));
    }
    ghash_update(h, len_block, 1, y);

    // T = GCTR_K(J0, S); S is a single block, so this is AES_K(J0) XOR S.
    uint8_t ks[16];
    aes.encrypt_block(j0, ks);
    for (int i = 0; i < 16; i++) tag_out[i] = uint8_t(y[i] ^ ks[i]);
}

} // namespace

void aes256_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* plaintext,
                      std::size_t len, std::vector<uint8_t>& out_ciphertext, uint8_t out_tag[16]) {
    Aes256 aes(key);

    // J0 = IV || 0^31 || 1 (96-bit IV followed by a 32-bit big-endian 1).
    uint8_t j0[16] = {0};
    std::memcpy(j0, nonce, 12);
    j0[15] = 1;

    // Ciphertext uses CTR starting at inc32(J0) — counter value 2.
    uint8_t icb[16];
    std::memcpy(icb, j0, 16);
    inc32(icb);

    out_ciphertext.assign(len, 0);
    if (len > 0) gctr(aes, icb, plaintext, len, out_ciphertext.data());

    uint8_t zero[16] = {0};
    uint8_t h[16];
    aes.encrypt_block(zero, h); // H = AES_K(0^128)

    compute_tag(aes, h, j0, out_ciphertext.data(), len, out_tag);
}

bool aes256_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ciphertext,
                      std::size_t len, const uint8_t tag[16], std::vector<uint8_t>& out_plaintext) {
    Aes256 aes(key);

    uint8_t j0[16] = {0};
    std::memcpy(j0, nonce, 12);
    j0[15] = 1;

    uint8_t zero[16] = {0};
    uint8_t h[16];
    aes.encrypt_block(zero, h);

    uint8_t expected_tag[16];
    compute_tag(aes, h, j0, ciphertext, len, expected_tag);

    // Constant-time tag comparison — do not leak timing information about
    // where a mismatch occurs (this is an authentication check on
    // attacker-controlled data).
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff = uint8_t(diff | (expected_tag[i] ^ tag[i]));
    if (diff != 0) return false;

    uint8_t icb[16];
    std::memcpy(icb, j0, 16);
    inc32(icb);

    out_plaintext.assign(len, 0);
    if (len > 0) gctr(aes, icb, ciphertext, len, out_plaintext.data());
    return true;
}

} // namespace dds::security::detail
