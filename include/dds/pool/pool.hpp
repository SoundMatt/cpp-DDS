// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/pool/pool.hpp — BytePool, a fixed-capacity byte-buffer allocator used
// to back zero-copy loaned-sample publishing (dds::ILoaningPublisher,
// RELAY spec §8.3).
//
// C++ port of the BytePool portion of github.com/SoundMatt/go-DDS's
// pool/pool.go (139 LOC total). go-DDS's BytePool wraps sync.Pool, a
// goroutine-safe free list of GC-tracked []byte slices; this header ports
// the same Get/Put semantics (a zero-length, pre-allocated buffer handed
// out on Get, truncated-and-recycled on Put, undersized buffers discarded)
// over a mutex-guarded free list of owned std::vector<uint8_t> objects.
//
// Scope: this file ports only BytePool, the piece Tier-1 phase 9 ("Loan
// integration", see ROADMAP.md and rtps/loan.hpp) needs to back
// dds::ILoaningPublisher::loan_buffer/write_loaned/return_loan. go-DDS's
// pool.go also defines SampleBuffer (a fixed-capacity ring buffer of
// dds.Sample values, for staging received samples) — that has no bearing
// on loaned *writes* and is left for the separate, not-yet-scheduled
// ddscore roadmap item ("`ILoaningPublisher` ... backed by a pool
// allocator", the "Also within ddscore but not RTPS-specific" list in
// ROADMAP.md) if/when something actually needs it.
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
// choice made in this file.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

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

} // namespace dds::pool
