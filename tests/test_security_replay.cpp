// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::security::ReplayGuard — behavioral port of
// github.com/SoundMatt/go-DDS's security.ReplayGuard (security/replay.go).
// No wire format is involved (this is a purely in-memory sliding-window
// sequence tracker), so correctness here is behavioral parity, mirroring
// go-DDS's replay_test.go case matrix.
//
// fusa:test REQ-SECURITY-009 REQ-SAFETY-001

#include <dds/security/replay.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace dds::security;
using namespace std::chrono_literals;

TEST_CASE("ReplayGuard: first sighting of a sequence number is allowed",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(30s);
    auto        now = std::chrono::steady_clock::now();
    CHECK_FALSE(g.check(1, now));
}

TEST_CASE("ReplayGuard: repeated sequence number within the window is a replay",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(30s);
    auto        now = std::chrono::steady_clock::now();
    CHECK_FALSE(g.check(42, now));
    CHECK(g.check(42, now) == ErrReplay());
}

TEST_CASE("ReplayGuard: distinct sequence numbers are independently allowed",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(30s);
    auto        now = std::chrono::steady_clock::now();
    CHECK_FALSE(g.check(1, now));
    CHECK_FALSE(g.check(2, now));
}

TEST_CASE("ReplayGuard::purge removes entries older than the window",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(50ms);
    auto        past = std::chrono::steady_clock::now() - 100ms; // outside window
    CHECK_FALSE(g.check(99, past));

    g.purge();
    CHECK(g.len() == 0);
}

TEST_CASE("ReplayGuard: an expired sequence number is allowed again after the window",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(50ms);
    auto        past = std::chrono::steady_clock::now() - 100ms;
    CHECK_FALSE(g.check(7, past));

    auto future = std::chrono::steady_clock::now();
    CHECK_FALSE(g.check(7, future));
}

TEST_CASE("ReplayGuard: non-positive window defaults to 30 seconds",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(std::chrono::steady_clock::duration::zero());
    CHECK_FALSE(g.check(1, std::chrono::steady_clock::now()));
}

TEST_CASE("ReplayGuard::len tracks the number of currently-tracked sequence numbers",
          "[security][replay][REQ-SECURITY-009]") {
    ReplayGuard g(1min);
    auto        now = std::chrono::steady_clock::now();
    CHECK(g.len() == 0);
    CHECK_FALSE(g.check(1, now));
    CHECK_FALSE(g.check(2, now));
    CHECK(g.len() == 2);
}

TEST_CASE("ReplayGuard: concurrent check() calls do not race",
          "[security][replay][REQ-SECURITY-009][REQ-SAFETY-001]") {
    ReplayGuard g(1s);
    auto        now = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(20);
    for (uint64_t i = 0; i < 20; i++) {
        threads.emplace_back([&g, now, i] { (void)g.check(i, now); });
    }
    for (auto& t : threads) t.join();

    CHECK(g.len() == 20);
}

TEST_CASE("new_replay_guard factory constructs a usable guard",
          "[security][replay][REQ-SECURITY-009]") {
    auto g = new_replay_guard(30s);
    REQUIRE(g);
    CHECK_FALSE(g->check(1, std::chrono::steady_clock::now()));
}
