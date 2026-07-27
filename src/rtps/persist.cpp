// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/persist.hpp>

// C++ port of github.com/SoundMatt/go-DDS rtps/persist.go. See
// include/dds/rtps/persist.hpp for the phase scope and file-format contract
// this file must uphold.

#include <fstream>

namespace dds::rtps {

namespace {

constexpr uint32_t kMaxPayloadBytes = 64u * 1024 * 1024; // 64 MiB cap, matching go-DDS.

} // namespace

std::string persist_path(const std::string& dir, const std::string& topic) {
    std::string safe;
    safe.reserve(topic.size());
    for (char c : topic) {
        safe.push_back((c == '/' || c == '\\' || c == ':') ? '_' : c);
    }
    const bool has_trailing_sep = !dir.empty() && (dir.back() == '/' || dir.back() == '\\');
    return dir + (has_trailing_sep ? "" : "/") + "topic-" + safe + ".bin";
}

std::optional<std::vector<uint8_t>> persist_load(const std::string& dir, const std::string& topic) {
    if (dir.empty()) return std::nullopt;

    std::ifstream f(persist_path(dir, topic), std::ios::binary);
    if (!f) return std::nullopt; // file not found on first run — normal

    uint8_t hdr[4];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f || f.gcount() != static_cast<std::streamsize>(sizeof(hdr))) return std::nullopt;

    const uint32_t length = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                             (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    if (length > kMaxPayloadBytes) return std::nullopt;

    std::vector<uint8_t> buf(length);
    if (length > 0) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length));
        if (!f || static_cast<uint32_t>(f.gcount()) != length) return std::nullopt;
    }
    return buf;
}

void persist_flush(const std::string& dir, const std::string& topic, const std::vector<uint8_t>& payload) {
    if (dir.empty()) return;

    std::ofstream f(persist_path(dir, topic), std::ios::binary | std::ios::trunc);
    if (!f) return; // e.g. read-only or missing directory — silently ignored

    const uint32_t length = static_cast<uint32_t>(payload.size());
    const uint8_t  hdr[4] = {
        static_cast<uint8_t>(length),
        static_cast<uint8_t>(length >> 8),
        static_cast<uint8_t>(length >> 16),
        static_cast<uint8_t>(length >> 24),
    };
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    if (!payload.empty()) {
        f.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    // Write failures are silently ignored (matches go-DDS's persistFlush).
}

} // namespace dds::rtps
