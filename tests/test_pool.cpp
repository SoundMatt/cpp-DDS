// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Behavioral tests for dds::pool::BytePool (Tier-1 phase 9, "Loan
// integration" — see ROADMAP.md and include/dds/pool/pool.hpp's file-level
// scope note). Test cases mirror go-DDS's pool/pool_test.go BytePool
// coverage (TestBytePool_*): capacity/length invariants on a fresh Get,
// reuse after Put, undersized-buffer discard, zero/negative-size
// defaulting, no-reallocation-within-capacity, and concurrent use.

#include <dds/pool/pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

using dds::pool::BytePool;

TEST_CASE("BytePool::get returns a zero-length buffer with sufficient capacity", "[pool]") {
    BytePool bp(512);
    auto*    buf = bp.get();
    CHECK(buf->capacity() >= 512);
    CHECK(buf->empty());
    bp.put(buf);
}

TEST_CASE("BytePool::put then get reuses the same buffer's storage", "[pool]") {
    BytePool bp(256);
    auto*    buf = bp.get();
    buf->push_back(1);
    buf->push_back(2);
    buf->push_back(3);
    bp.put(buf);

    auto* reused = bp.get();
    CHECK(reused->capacity() >= 256);
    CHECK(reused->empty());
    bp.put(reused);
}

TEST_CASE("BytePool::put discards an undersized buffer instead of pooling it", "[pool]") {
    BytePool bp(1024);
    auto*    small = new std::vector<uint8_t>();
    small->reserve(16);
    bp.put(small); // should not crash; buffer is simply discarded (deleted)

    // A subsequent get() must still return a properly-sized buffer, not the
    // discarded undersized one.
    auto* buf = bp.get();
    CHECK(buf->capacity() >= 1024);
    bp.put(buf);
}

TEST_CASE("BytePool defaults a zero or negative-equivalent size to 4096", "[pool]") {
    BytePool bp(0);
    CHECK(bp.size() == 4096);
    auto* buf = bp.get();
    CHECK(buf->capacity() >= 4096);
    bp.put(buf);
}

TEST_CASE("BytePool::put(nullptr) is a no-op", "[pool]") {
    BytePool bp(64);
    bp.put(nullptr); // must not crash
    auto* buf = bp.get();
    CHECK(buf->capacity() >= 64);
    bp.put(buf);
}

TEST_CASE("BytePool buffers do not reallocate when filled within the configured capacity",
          "[pool]") {
    constexpr std::size_t kSize = 64;
    BytePool               bp(kSize);
    auto*                  buf = bp.get();
    const auto*             original_data = buf->data();
    for (std::size_t i = 0; i < kSize; ++i) buf->push_back(static_cast<uint8_t>(i));
    CHECK(buf->capacity() == kSize);
    CHECK(buf->data() == original_data); // no reallocation occurred
    bp.put(buf);
}

TEST_CASE("BytePool is safe under concurrent get/put", "[pool]") {
    BytePool bp(128);

    std::vector<std::thread> threads;
    threads.reserve(50);
    for (int i = 0; i < 50; ++i) {
        threads.emplace_back([&bp] {
            auto* buf = bp.get();
            buf->push_back(static_cast<uint8_t>(buf->size()));
            bp.put(buf);
        });
    }
    for (auto& t : threads) t.join();
}
