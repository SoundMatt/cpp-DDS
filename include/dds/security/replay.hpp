// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/security/replay.hpp — anti-replay sequence-number enforcement.
//
// C++ port of github.com/SoundMatt/go-DDS's `security` package
// (security/replay.go): ReplayGuard tracks recently-seen sequence numbers
// within a sliding time window and rejects duplicates. See ROADMAP.md,
// "Tier 2 — safety and security", `security`.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <dds/security/security.hpp> // for Errc::replay_detected / ErrReplay()

namespace dds::security {

// ReplayGuard protects against replay attacks by tracking recently-seen
// sequence numbers within a sliding time window. Each sequence number is
// associated with the timestamp of the message that carried it; a sequence
// number is considered a replay if it has been seen within `window` of the
// current call. Safe for concurrent use from multiple threads
// (fusa:req REQ-SAFETY-001). Non-copyable, non-movable (holds a mutex);
// construct via new_replay_guard, matching dds::safety::E2EPublisher's own
// shared_ptr-factory precedent.
// fusa:req REQ-SECURITY-009
class ReplayGuard {
public:
    // Constructs a ReplayGuard with the given sliding window. A window of
    // zero or negative duration is replaced with 30 seconds.
    explicit ReplayGuard(std::chrono::steady_clock::duration window);

    ReplayGuard(const ReplayGuard&)            = delete;
    ReplayGuard& operator=(const ReplayGuard&) = delete;

    // check reports whether seq is a replay. If seq has not been seen
    // within `window` of ts, it is recorded and {} (no error) is returned.
    // If seq has already been seen within the window, ErrReplay() is
    // returned. ts is the claimed send timestamp of the message; entries
    // whose recorded timestamp is more than `window` before ts are pruned
    // on each call.
    std::error_code check(uint64_t seq, std::chrono::steady_clock::time_point ts);

    // purge removes all entries older than `window` before now(). check
    // purges automatically; call this explicitly only when driving the
    // clock externally (e.g. in tests).
    void purge();

    // len returns the number of sequence numbers currently tracked.
    std::size_t len() const;

private:
    void purge_locked(std::chrono::steady_clock::time_point now);

    mutable std::mutex                                                  mu_;
    std::chrono::steady_clock::duration                                 window_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> seen_;
};

// new_replay_guard constructs a ReplayGuard (go-DDS: security.NewReplayGuard).
std::shared_ptr<ReplayGuard> new_replay_guard(std::chrono::steady_clock::duration window);

} // namespace dds::security
