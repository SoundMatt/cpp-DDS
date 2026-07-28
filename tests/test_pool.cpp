// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Behavioral tests for dds::pool::BytePool and dds::pool::SampleBuffer.
// BytePool coverage dates to Tier-1 phase 9, "Loan integration"; SampleBuffer
// coverage is new for the "Also within ddscore but not RTPS-specific"
// ROADMAP.md `ILoaningPublisher` item (see include/dds/pool/pool.hpp's
// file-level scope note). Both mirror go-DDS's pool/pool_test.go coverage
// (TestBytePool_*/TestSampleBuffer_*) exactly: BytePool's capacity/length
// invariants on a fresh Get, reuse after Put, undersized-buffer discard,
// zero/negative-size defaulting, no-reallocation-within-capacity, and
// concurrent use; SampleBuffer's push/pop round-trip, pop-on-empty,
// push-on-full, wraparound ordering, len tracking, cap reporting,
// zero-size defaulting, and concurrent push/pop.

#include <dds/pool/pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using dds::pool::BytePool;
using dds::pool::SampleBuffer;

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

// ── SampleBuffer ─────────────────────────────────────────────────────────────

TEST_CASE("SampleBuffer::push then pop round-trips a sample", "[pool]") {
    SampleBuffer sb(4);
    dds::Sample  s;
    s.topic   = "t";
    s.payload = {'p'};
    CHECK(sb.push(s));

    auto got = sb.pop();
    REQUIRE(got.has_value());
    CHECK(got->topic == "t");
    CHECK(got->payload == std::vector<uint8_t>{'p'});
}

TEST_CASE("SampleBuffer::pop on an empty buffer returns nullopt", "[pool]") {
    SampleBuffer sb(4);
    CHECK_FALSE(sb.pop().has_value());
}

TEST_CASE("SampleBuffer::push on a full buffer returns false", "[pool]") {
    SampleBuffer sb(2);
    CHECK(sb.push(dds::Sample{}));
    CHECK(sb.push(dds::Sample{}));
    CHECK_FALSE(sb.push(dds::Sample{}));
}

TEST_CASE("SampleBuffer preserves FIFO order across wraparound", "[pool]") {
    SampleBuffer sb(3);
    for (int i = 0; i < 3; ++i) {
        dds::Sample s;
        s.topic = "t" + std::to_string(i);
        REQUIRE(sb.push(s));
    }
    sb.pop();                                      // head moves to 1
    dds::Sample s3;
    s3.topic = "t3";
    REQUIRE(sb.push(s3));                          // tail wraps to 0

    const std::vector<std::string> expected{"t1", "t2", "t3"};
    for (const auto& want : expected) {
        auto got = sb.pop();
        REQUIRE(got.has_value());
        CHECK(got->topic == want);
    }
}

TEST_CASE("SampleBuffer::len tracks the number of held samples", "[pool]") {
    SampleBuffer sb(8);
    CHECK(sb.len() == 0);
    sb.push(dds::Sample{});
    CHECK(sb.len() == 1);
    sb.pop();
    CHECK(sb.len() == 0);
}

TEST_CASE("SampleBuffer::cap reports the configured capacity", "[pool]") {
    SampleBuffer sb(16);
    CHECK(sb.cap() == 16);
}

TEST_CASE("SampleBuffer defaults a zero or negative-equivalent capacity to 64", "[pool]") {
    SampleBuffer sb(0);
    CHECK(sb.cap() == 64);
}

TEST_CASE("SampleBuffer: full then drain returns samples in push order", "[pool]") {
    constexpr std::size_t kCap = 4;
    SampleBuffer          sb(kCap);
    for (std::size_t i = 0; i < kCap; ++i) {
        dds::Sample s;
        s.topic = "s" + std::to_string(i);
        REQUIRE(sb.push(s));
    }
    for (std::size_t i = 0; i < kCap; ++i) {
        auto got = sb.pop();
        REQUIRE(got.has_value());
        CHECK(got->topic == "s" + std::to_string(i));
    }
    CHECK_FALSE(sb.pop().has_value());
}

TEST_CASE("SampleBuffer is safe under concurrent push/pop", "[pool]") {
    SampleBuffer sb(128);

    std::vector<std::thread> producers;
    producers.reserve(10);
    for (int i = 0; i < 10; ++i) {
        producers.emplace_back([&sb] {
            for (int j = 0; j < 10; ++j) {
                dds::Sample s;
                s.topic = "concurrent";
                sb.push(s);
            }
        });
    }
    std::atomic<bool> stop{false};
    std::thread       consumer([&sb, &stop] {
        while (!stop.load()) {
            sb.pop();
        }
    });
    for (auto& t : producers) t.join();
    stop.store(true);
    consumer.join();
}
