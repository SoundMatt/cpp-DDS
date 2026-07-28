// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/security/replay.hpp>

// fusa:req REQ-SECURITY-009 REQ-SAFETY-001

namespace dds::security {

namespace {
const std::chrono::steady_clock::duration kDefaultWindow =
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(30));
}

ReplayGuard::ReplayGuard(std::chrono::steady_clock::duration window)
    : window_(window.count() > 0 ? window : kDefaultWindow) {}

std::error_code ReplayGuard::check(uint64_t seq, std::chrono::steady_clock::time_point ts) {
    std::lock_guard<std::mutex> lk(mu_);
    purge_locked(ts);
    if (seen_.find(seq) != seen_.end()) {
        return ErrReplay();
    }
    seen_[seq] = ts;
    return {};
}

void ReplayGuard::purge() {
    std::lock_guard<std::mutex> lk(mu_);
    purge_locked(std::chrono::steady_clock::now());
}

std::size_t ReplayGuard::len() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_.size();
}

void ReplayGuard::purge_locked(std::chrono::steady_clock::time_point now) {
    auto cutoff = now - window_;
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (it->second < cutoff) {
            it = seen_.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<ReplayGuard> new_replay_guard(std::chrono::steady_clock::duration window) {
    return std::make_shared<ReplayGuard>(window);
}

} // namespace dds::security
