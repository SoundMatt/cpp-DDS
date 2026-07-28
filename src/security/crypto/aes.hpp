// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal AES-256 (FIPS 197 / Rijndael, Nk=8/Nr=14) block cipher, forward
// (encryption) direction only — sufficient for CTR-based modes such as GCM,
// which never invoke the inverse cipher. Not a public dds:: header — used
// only by gcm.cpp. Verified byte-exact against the NIST SP 800-38A
// AES-256-ECB known-answer test vector — see tests/test_security_aesgcm.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dds::security::detail {

class Aes256 {
public:
    explicit Aes256(const uint8_t key[32]);

    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t rk_[60]; // 4 * (Nr + 1) = 4 * 15
};

} // namespace dds::security::detail
