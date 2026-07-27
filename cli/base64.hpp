// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// base64.hpp — standard (RFC 4648 §4) base64 codec for the cpp-dds CLI.
//
// `relay.Message.Payload` and `dds.Sample.Payload` are `contentEncoding:
// base64` in the RELAY JSON schemas (spec/schemas/*.json) — this is what
// `convert` (spec §11.2) reads/writes on the payload field.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cli::base64 {

inline std::string encode(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    std::size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// decode returns nullopt if the input is not valid base64.
inline std::optional<std::vector<uint8_t>> decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    // Strip trailing padding and whitespace for length validation.
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        s.push_back(c);
    }
    if (s.empty()) return std::vector<uint8_t>{};
    if (s.size() % 4 != 0) return std::nullopt;

    std::size_t pad = 0;
    if (s.size() >= 1 && s[s.size() - 1] == '=') pad++;
    if (s.size() >= 2 && s[s.size() - 2] == '=') pad++;

    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);

    for (std::size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k) {
            char c = s[i + static_cast<std::size_t>(k)];
            if (c == '=') { v[k] = 0; continue; }
            v[k] = val(c);
            if (v[k] < 0) return std::nullopt;
        }
        uint32_t n = (static_cast<uint32_t>(v[0]) << 18) |
                     (static_cast<uint32_t>(v[1]) << 12) |
                     (static_cast<uint32_t>(v[2]) << 6) |
                     static_cast<uint32_t>(v[3]);
        bool last_block = (i + 4 == s.size());
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        if (!(last_block && pad >= 2))
            out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        if (!(last_block && pad >= 1))
            out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    return out;
}

} // namespace cli::base64
