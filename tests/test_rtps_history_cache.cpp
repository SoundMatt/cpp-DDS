// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Unit tests for dds::rtps::HistoryCache (Tier-1 phase 6, "Entities &
// history cache"). This is internal bookkeeping with no wire-format output
// of its own (see history_cache.hpp's file-level scope note), so — unlike
// the wire-format tests in test_rtps_types.cpp / test_rtps_cdr.cpp /
// test_rtps_spdp.cpp / test_rtps_sedp.cpp — there is no go-DDS golden
// vector to pin here; these are ordinary behavioral unit tests.

#include <catch2/catch_test_macros.hpp>

#include <dds/rtps/history_cache.hpp>

using namespace dds::rtps;

namespace {

GUID sample_writer_guid() {
    return GUID{GuidPrefix{{0x01, 0x0f, 0x99, 0x6e, 0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x01}},
                EntityId{{0x00, 0x00, 0x01, 0x03}}};
}

CacheChange sample_change(uint64_t seq, uint8_t byte) {
    CacheChange c;
    c.sequence_number = seq;
    c.writer_guid       = sample_writer_guid();
    c.payload            = {byte};
    c.timestamp           = std::chrono::system_clock::now();
    return c;
}

} // namespace

TEST_CASE("HistoryCache starts empty", "[rtps][history_cache]") {
    HistoryCache h;
    CHECK(h.empty());
    CHECK(h.size() == 0);
    CHECK_FALSE(h.latest().has_value());
    CHECK_FALSE(h.get(1).has_value());
    CHECK(h.span() == std::pair<uint64_t, uint64_t>{0, 0});
}

TEST_CASE("HistoryCache::store then get round-trips a change", "[rtps][history_cache]") {
    HistoryCache h;
    h.store(sample_change(1, 0xAA));

    CHECK_FALSE(h.empty());
    CHECK(h.size() == 1);
    auto c = h.get(1);
    REQUIRE(c.has_value());
    CHECK(c->sequence_number == 1);
    CHECK(c->writer_guid == sample_writer_guid());
    CHECK(c->payload == std::vector<uint8_t>{0xAA});
}

TEST_CASE("HistoryCache::latest returns the most recently stored change", "[rtps][history_cache]") {
    HistoryCache h;
    h.store(sample_change(1, 0x01));
    h.store(sample_change(2, 0x02));
    h.store(sample_change(3, 0x03));

    auto latest = h.latest();
    REQUIRE(latest.has_value());
    CHECK(latest->sequence_number == 3);
    CHECK(latest->payload == std::vector<uint8_t>{0x03});
}

TEST_CASE("HistoryCache::span reports the retained sequence-number range", "[rtps][history_cache]") {
    HistoryCache h;
    h.store(sample_change(5, 0x00));
    h.store(sample_change(6, 0x00));
    h.store(sample_change(7, 0x00));

    auto [first, last] = h.span();
    CHECK(first == 5);
    CHECK(last == 7);
}

TEST_CASE("HistoryCache evicts the oldest change once depth is exceeded", "[rtps][history_cache]") {
    HistoryCache h(3);
    h.store(sample_change(1, 0x01));
    h.store(sample_change(2, 0x02));
    h.store(sample_change(3, 0x03));
    CHECK(h.size() == 3);

    h.store(sample_change(4, 0x04));
    CHECK(h.size() == 3); // still bounded to depth
    CHECK_FALSE(h.get(1).has_value()); // oldest evicted
    REQUIRE(h.get(4).has_value());
    CHECK(h.get(4)->payload == std::vector<uint8_t>{0x04});

    auto [first, last] = h.span();
    CHECK(first == 2);
    CHECK(last == 4);
}

TEST_CASE("HistoryCache never exceeds its configured depth across many stores",
          "[rtps][history_cache]") {
    HistoryCache h(4);
    for (uint64_t i = 1; i <= 100; ++i) {
        h.store(sample_change(i, static_cast<uint8_t>(i)));
        CHECK(h.size() <= 4);
    }
    CHECK(h.size() == 4);
    auto [first, last] = h.span();
    CHECK(first == 97);
    CHECK(last == 100);
}

TEST_CASE("HistoryCache constructed with depth 0 clamps to 1", "[rtps][history_cache]") {
    HistoryCache h(0);
    CHECK(h.depth() == 1);
    h.store(sample_change(1, 0x01));
    h.store(sample_change(2, 0x02));
    CHECK(h.size() == 1);
    REQUIRE(h.latest().has_value());
    CHECK(h.latest()->sequence_number == 2);
}

TEST_CASE("HistoryCache::clear empties the cache", "[rtps][history_cache]") {
    HistoryCache h;
    h.store(sample_change(1, 0x01));
    h.store(sample_change(2, 0x02));
    REQUIRE(h.size() == 2);

    h.clear();
    CHECK(h.empty());
    CHECK(h.size() == 0);
}

TEST_CASE("HistoryCache default depth matches go-DDS's maxHistoryDepth constant",
          "[rtps][history_cache]") {
    HistoryCache h;
    CHECK(h.depth() == 256);
    CHECK(kDefaultHistoryDepth == 256);
}
