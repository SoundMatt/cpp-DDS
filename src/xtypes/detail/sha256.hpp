// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal SHA-256 implementation (FIPS 180-4), used only to compute
// dds::xtypes::TypeIdentifier's content-addressed hash (Identify()/
// canonical()). Not a public dds:: header — included only by
// src/xtypes/xtypes.cpp.
//
// No external crypto dependency is fetched for this project (see
// cmake/FetchDeps.cmake — Catch2 only), so — mirroring
// src/security/crypto/sha256.hpp's own precedent and stated rationale for
// the dds::security module — dds::xtypes gets its own small, from-scratch,
// standards-conformant SHA-256 rather than reaching across module
// boundaries into security's internal (explicitly non-shared) detail
// header. Verified byte-exact against FIPS 180-4 known-answer test vectors
// and, end-to-end, against go-DDS's actual crypto/sha256-derived
// TypeIdentifier hashes — see tests/test_xtypes.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dds::xtypes::detail {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const uint8_t* data, std::size_t len);
    std::array<uint8_t, 32> finish();

    static std::array<uint8_t, 32> hash(const uint8_t* data, std::size_t len);

private:
    void process_block(const uint8_t block[64]);

    uint32_t    h_[8];
    uint8_t     buf_[64];
    std::size_t buf_len_;
    uint64_t    total_len_;
};

} // namespace dds::xtypes::detail
