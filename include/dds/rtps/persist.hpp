// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/persist.hpp — TransientLocal-style durability persistence: a
// file-per-topic disk cache of the last published sample, so a late-joining
// TransientLocal subscriber can recover it even across a process restart
// (in-memory Participant::last_sample only survives within one process
// lifetime).
//
// This is part of Tier-1 sub-phase 7 of the cpp-DDS RTPS roadmap (see
// ROADMAP.md, "Tier 1 — RTPS wire protocol", phase 7: "Reliable delivery" —
// persist.go is bundled with reliable.go as this phase's reference pair).
// It is internal, additive scaffolding: NOT yet wired into the public
// dds::IParticipant / relay::INode surface beyond dds::rtps::Participant
// itself.
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/persist.go
// (persistLoad/persistFlush/persistPath and the WithPersistentHistory
// option). Participant::create's ParticipantOptions::persist_dir is this
// port's equivalent of WithPersistentHistory(dir) — see participant.hpp.
//
// File format (matching go-DDS exactly): a 4-byte little-endian length
// prefix followed by the raw sample payload bytes, at
// <dir>/topic-<sanitised(topic)>.bin, where sanitised(topic) replaces '/',
// '\\', and ':' with '_' so the name is a single flat file regardless of
// topic hierarchy. This is a plain length-prefixed byte dump, not itself an
// RTPS wire structure, so there is no go-DDS golden-vector byte-exactness
// requirement beyond the format described here (pinned by ordinary
// behavioral round-trip tests — see tests/test_rtps_reliable.cpp).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dds::rtps {

// Returns the file path for topic inside dir, matching go-DDS's persistPath.
// Exposed (not file-local) for direct testing, matching this codebase's
// existing precedent of exposing small internal helpers for white-box tests
// (e.g. rtps/spdp.hpp's build_spdp_announcement).
std::string persist_path(const std::string& dir, const std::string& topic);

// Reads the last-written payload for topic from dir, matching go-DDS's
// persistLoad. Returns std::nullopt when: dir is empty (persistence
// disabled — matches go-DDS's (nil, nil) no-op path); the file does not
// exist (normal on first run); the file is too short for the 4-byte length
// header; the declared length exceeds the 64 MiB cap; or the file has fewer
// payload bytes than declared. All of these collapse to "no persisted
// sample" for the caller (dds::rtps::Participant::new_subscriber), matching
// go-DDS's own callers, which only ever check `err == nil && payload !=
// nil`.
std::optional<std::vector<uint8_t>> persist_load(const std::string& dir, const std::string& topic);

// Writes payload to topic's file in dir, replacing any previous content,
// matching go-DDS's persistFlush. A no-op when dir is empty. Failures (e.g.
// a read-only or missing directory) are silently ignored, matching go-DDS,
// so that a write to a bad persist_dir never blocks or fails the caller's
// Writer::write.
void persist_flush(const std::string& dir, const std::string& topic, const std::vector<uint8_t>& payload);

} // namespace dds::rtps
