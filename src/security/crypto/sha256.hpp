// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal SHA-256 / HMAC-SHA-256 implementation (FIPS 180-4 / RFC 2104).
// Not a public dds:: header — included only by src/security/security.cpp.
// No external crypto dependency is fetched for this project (see
// cmake/FetchDeps.cmake — Catch2 only), so dds::security ports go-DDS's use
// of Go's crypto/sha256 + crypto/hmac stdlib packages with a small, from-
// scratch, standards-conformant implementation instead of vendoring a large
// third-party crypto library. Verified byte-exact against FIPS 180-4 SHA-256
// test vectors and RFC 4231 HMAC-SHA-256 test vectors — see
// tests/test_security_hmac.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dds::security::detail {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const uint8_t* data, std::size_t len);
    std::array<uint8_t, 32> finish();

    static std::array<uint8_t, 32> hash(const uint8_t* data, std::size_t len);

private:
    void process_block(const uint8_t block[64]);

    uint32_t h_[8];
    uint8_t  buf_[64];
    std::size_t buf_len_;
    uint64_t total_len_;
};

// hmac_sha256 computes the RFC 2104 HMAC of msg under key, using SHA-256 as
// the underlying hash (RFC 4231).
std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, std::size_t key_len, const uint8_t* msg,
                                     std::size_t msg_len);

} // namespace dds::security::detail
