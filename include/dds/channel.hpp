// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <cstddef>

namespace dds {

// Chan<T> is a bounded, thread-safe FIFO channel.
//
// Mirrors Go's buffered channel semantics:
//   recv() blocks until an item is available or the channel is closed+empty.
//   close() wakes all blocked receivers.
//   send() blocks until capacity is available (Block back-pressure).
//   try_send() is non-blocking — returns Full when at capacity.
//   send_drop_oldest() replaces the head item when at capacity (DropOldest).
template<typename T>
class Chan {
public:
    enum class SendResult { Ok, Full, Closed };

    explicit Chan(std::size_t capacity = 64) : capacity_(capacity) {}

    Chan(const Chan&)            = delete;
    Chan& operator=(const Chan&) = delete;

    bool send(T value) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_full_.wait(lk, [this]{ return closed_ || buf_.size() < capacity_; });
        if (closed_) return false;
        buf_.push_back(std::move(value));
        cv_not_empty_.notify_one();
        return true;
    }

    SendResult try_send(T value) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_)               return SendResult::Closed;
        if (buf_.size() >= capacity_) return SendResult::Full;
        buf_.push_back(std::move(value));
        cv_not_empty_.notify_one();
        return SendResult::Ok;
    }

    bool send_drop_oldest(T value) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return false;
        if (buf_.size() >= capacity_) buf_.pop_front();
        buf_.push_back(std::move(value));
        cv_not_empty_.notify_one();
        return true;
    }

    std::optional<T> recv() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_empty_.wait(lk, [this]{ return !buf_.empty() || closed_; });
        if (buf_.empty()) return std::nullopt;
        T val = std::move(buf_.front());
        buf_.pop_front();
        cv_not_full_.notify_one();
        return val;
    }

    std::optional<T> try_recv() {
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.empty()) return std::nullopt;
        T val = std::move(buf_.front());
        buf_.pop_front();
        cv_not_full_.notify_one();
        return val;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lk(mu_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return buf_.size();
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<T>           buf_;
    std::size_t             capacity_;
    bool                    closed_{false};
};

} // namespace dds
