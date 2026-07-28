// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/pool/pool.hpp — BytePool and SampleBuffer, the allocation-efficient
// data structures backing zero-copy loaned-sample publishing
// (dds::ILoaningPublisher, RELAY spec §8.3) and bounded sample staging.
//
// C++ port of github.com/SoundMatt/go-DDS's pool/pool.go (139 LOC total).
// go-DDS's BytePool wraps sync.Pool, a goroutine-safe free list of
// GC-tracked []byte slices; this header ports the same Get/Put semantics
// (a zero-length, pre-allocated buffer handed out on Get,
// truncated-and-recycled on Put, undersized buffers discarded) over a
// mutex-guarded free list of owned std::vector<uint8_t> objects.
//
// SampleBuffer ports go-DDS's fixed-capacity concurrent ring buffer of
// dds.Sample values (for staging received samples between a subscriber
// channel and an application processing loop) over a mutex-guarded
// std::vector<dds::Sample> ring, matching Push/Pop/Len/Cap semantics
// exactly. This is the "Also within ddscore but not RTPS-specific" list's
// `ILoaningPublisher` item's other half (see ROADMAP.md) — Tier-1 phase 9
// ("Loan integration") only needed BytePool, which is why this file
// originally ported only that piece; SampleBuffer was left for this item,
// which is the "if/when something actually needs it" this file's own
// prior scope note anticipated (the mock-participant-backed
// ILoaningPublisher in dds/mock/loan.hpp does not itself need
// SampleBuffer either — it reuses BytePool exactly as the RTPS side does
// — but this header is where go-DDS's own pool.go keeps both types
// together, so the C++ port follows suit for this item's completion).
//
// Ownership note (a genuine divergence from go-DDS, forced by the
// interface shape dds::ILoaningPublisher already committed to before this
// file existed): go-DDS's Get/Put operate on []byte, a view over
// GC-tracked storage — a caller that drops a loaned slice without calling
// Put or Commit merely leaves it for the garbage collector. dds::
// ILoaningPublisher::loan_buffer instead returns a raw
// `std::vector<uint8_t>*` that BytePool itself owns; a caller that never
// calls write_loaned/return_loan (BytePool::put) leaks that buffer for the
// life of the pool, exactly as any other manually-managed C++ resource
// would. This is inherent to the pointer-based interface shape, not a
// choice made in this file. SampleBuffer has no such divergence: Push/Pop
// operate purely by value (dds::Sample, matching go-DDS's dds.Sample),
// so there is no pointer-ownership question to resolve.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <dds/dds.hpp>

namespace dds::pool {

// BytePool recycles fixed-capacity byte buffers to avoid a heap allocation
// per loaned sample on a hot publish path. Thread-safe.
class BytePool {
public:
    // Buffers obtained via get() have capacity >= size (size <= 0 defaults
    // to 4096, matching go-DDS's pool.New(0)/pool.New(-1) behavior).
    explicit BytePool(std::size_t size = 0) : size_(size > 0 ? size : kDefaultSize) {}

    ~BytePool() = default;

    BytePool(const BytePool&)            = delete;
    BytePool& operator=(const BytePool&) = delete;

    // get returns a zero-length buffer with capacity >= the pool's
    // configured size, owned by the pool until returned via put(). Reuses a
    // previously put() buffer when the free list is non-empty; otherwise
    // allocates a new one (matching go-DDS's sync.Pool.New fallback).
    std::vector<uint8_t>* get() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!free_.empty()) {
            std::vector<uint8_t>* buf = free_.back().release();
            free_.pop_back();
            return buf;
        }
        auto* buf = new std::vector<uint8_t>();
        buf->reserve(size_);
        return buf;
    }

    // put returns buf to the pool for reuse, truncating it to zero length.
    // Buffers with capacity below the pool's configured size are discarded
    // (deleted) instead of retained, matching go-DDS's BytePool.Put — this
    // keeps the pool from ever accumulating undersized buffers. buf must
    // have been returned by get() on this same pool; passing nullptr is a
    // no-op.
    void put(std::vector<uint8_t>* buf) {
        if (!buf) return;
        if (buf->capacity() < size_) {
            delete buf;
            return;
        }
        buf->clear();
        std::lock_guard<std::mutex> lock(mu_);
        free_.emplace_back(buf);
    }

    // size returns the pool's configured minimum buffer capacity.
    std::size_t size() const noexcept { return size_; }

private:
    static constexpr std::size_t kDefaultSize = 4096;

    std::size_t size_;
    std::mutex  mu_;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> free_;
};

// SampleBuffer is a fixed-capacity concurrent ring buffer of dds::Sample
// values. It provides bounded, allocation-free queuing suitable for use as
// a staging area between a subscriber channel and an application
// processing loop running at a different rate. Thread-safe. C++ port of
// go-DDS's pool.SampleBuffer.
class SampleBuffer {
public:
    // Buffers obtained via SampleBuffer(capacity) hold at most capacity
    // samples (capacity <= 0 defaults to 64, matching go-DDS's
    // NewSampleBuffer(0)/NewSampleBuffer(-1) behavior).
    explicit SampleBuffer(std::size_t capacity = 0)
        : cap_(capacity > 0 ? capacity : kDefaultCapacity), buf_(cap_) {}

    ~SampleBuffer() = default;

    SampleBuffer(const SampleBuffer&)            = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

    // push adds s to the ring buffer. Returns false if the buffer is full.
    bool push(const Sample& s) {
        std::lock_guard<std::mutex> lock(mu_);
        if (len_ == cap_) return false;
        buf_[tail_] = s;
        tail_ = (tail_ + 1) % cap_;
        ++len_;
        return true;
    }

    // pop removes and returns the oldest sample. Returns nullopt if the
    // buffer is empty.
    std::optional<Sample> pop() {
        std::lock_guard<std::mutex> lock(mu_);
        if (len_ == 0) return std::nullopt;
        Sample s = std::move(buf_[head_]);
        buf_[head_] = Sample{}; // release payload storage, matching go-DDS's GC-release comment
        head_ = (head_ + 1) % cap_;
        --len_;
        return s;
    }

    // len returns the number of samples currently held in the buffer.
    std::size_t len() const {
        std::lock_guard<std::mutex> lock(mu_);
        return len_;
    }

    // cap returns the buffer's maximum capacity.
    std::size_t cap() const noexcept { return cap_; }

private:
    static constexpr std::size_t kDefaultCapacity = 64;

    std::size_t         cap_;
    std::vector<Sample> buf_;
    std::size_t         head_{0};
    std::size_t         tail_{0};
    std::size_t         len_{0};
    mutable std::mutex  mu_;
};

} // namespace dds::pool
