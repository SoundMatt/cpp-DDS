// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// relay.hpp — RELAY spec types shared across all protocol implementations.
// Mirrors the RELAY Go package spec v1.10.

#pragma once

#include "channel.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace relay {

// ── Spec version ─────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-020
inline constexpr const char* kRelaySpecVersion = "1.10";
// Legacy alias kept so dds::kSpecVersion (which reads from relay) still works.
inline constexpr const char* kSpecVersion      = kRelaySpecVersion;

// ── Protocol ─────────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-001 REQ-RELAY-002
enum class Protocol : int {
    CAN    = 1,
    DDS    = 2,
    LIN    = 3,
    MQTT   = 4,
    RCP    = 5,
    SOMEIP = 6,
};

// fusa:req REQ-RELAY-003
std::string to_string(Protocol p);

// fusa:req REQ-RELAY-059
Protocol    parse_protocol(std::string_view s);
bool        try_parse_protocol(std::string_view s, Protocol& out) noexcept;

// ── Version ───────────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-004 REQ-RELAY-005
struct Version {
    int major{};
    int minor{};
    int patch{};

    std::string to_string() const;
    bool operator==(const Version& o) const noexcept {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
};

// ── Message ───────────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-006 REQ-RELAY-007
struct Message {
    Protocol  protocol{};
    Version   version{};
    std::string id;
    std::vector<uint8_t> payload;
    std::chrono::system_clock::time_point timestamp;
    uint64_t  seq{};
    std::unordered_map<std::string, std::string> meta;
};

// ── Context ───────────────────────────────────────────────────────────────────

// relay::Context mirrors Go's context.Context for C++ protocol implementations.
// fusa:req REQ-RELAY-060
class Context {
public:
    static Context background() noexcept { return Context{}; }

    static Context with_deadline(std::chrono::steady_clock::time_point tp) noexcept {
        Context c;
        c.deadline_ = tp;
        return c;
    }

    static Context with_timeout(std::chrono::steady_clock::duration d) noexcept {
        return with_deadline(std::chrono::steady_clock::now() + d);
    }

    bool done() const noexcept {
        if (!deadline_) return false;
        return std::chrono::steady_clock::now() >= *deadline_;
    }

    std::optional<std::chrono::steady_clock::time_point> deadline() const noexcept {
        return deadline_;
    }

private:
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

// ── Error codes ───────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-008 REQ-RELAY-009 REQ-RELAY-010 REQ-RELAY-011 REQ-RELAY-012
enum class Errc : int {
    closed            = 1,
    not_connected     = 2,
    timeout           = 3,
    payload_too_large = 4,
};

// fusa:req REQ-RELAY-021
const std::error_category& error_category() noexcept;
std::error_code            make_error_code(Errc e) noexcept;

// Convenience error constants — compare with == like Go's errors.Is.
inline std::error_code ErrClosed()          noexcept { return make_error_code(Errc::closed); }
inline std::error_code ErrNotConnected()    noexcept { return make_error_code(Errc::not_connected); }
inline std::error_code ErrTimeout()         noexcept { return make_error_code(Errc::timeout); }
inline std::error_code ErrPayloadTooLarge() noexcept { return make_error_code(Errc::payload_too_large); }

// ── Back-pressure ─────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-015
enum class BackPressurePolicy {
    DropNewest = 0,
    DropOldest = 1,
    Block      = 2,
};

// ── Subscriber options ────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-016
struct SubscriberConfig {
    int                chan_depth{};
    BackPressurePolicy back_pressure{BackPressurePolicy::DropNewest};
    uint32_t           event_id{};
    std::string        topic_name;
    // Optional message filter — null means accept all (fusa:req REQ-DDS-010)
    std::function<bool(const Message&)> filter;
    // Optional deadline-missed callback (fusa:req REQ-DDS-011)
    std::function<void()>               deadline_missed;

    // fusa:req REQ-RELAY-019
    int effective_depth(int default_depth) const noexcept {
        return chan_depth > 0 ? chan_depth : default_depth;
    }
};

// fusa:req REQ-RELAY-017
using SubscriberOption = std::function<void(SubscriberConfig&)>;

// fusa:req REQ-RELAY-051 REQ-RELAY-056
inline SubscriberOption with_channel_depth(int n) {
    return [n](SubscriberConfig& c){ c.chan_depth = n; };
}
inline SubscriberOption with_back_pressure(BackPressurePolicy p) {
    return [p](SubscriberConfig& c){ c.back_pressure = p; };
}
inline SubscriberOption with_event_id(uint32_t id) {
    return [id](SubscriberConfig& c){ c.event_id = id; };
}
inline SubscriberOption with_topic(std::string name) {
    return [n = std::move(name)](SubscriberConfig& c){ c.topic_name = n; };
}

// fusa:req REQ-RELAY-018
inline SubscriberConfig apply_options(const std::vector<SubscriberOption>& opts) {
    SubscriberConfig c;
    for (auto& o : opts) o(c);
    return c;
}

// ── INode ─────────────────────────────────────────────────────────────────────

// fusa:req REQ-RELAY-013
class INode {
public:
    virtual ~INode() = default;

    virtual Protocol protocol() const noexcept = 0;

    virtual std::error_code send(Context ctx, const Message& msg) = 0;

    virtual std::pair<std::shared_ptr<dds::Chan<Message>>, std::error_code>
        subscribe(std::vector<SubscriberOption> opts = {}) = 0;

    virtual std::error_code close() noexcept = 0;
};

// fusa:req REQ-RELAY-014
class ICaller : public INode {
public:
    virtual std::pair<Message, std::error_code>
        call(Context ctx, const Message& req) = 0;
};

// ── Optional capability interfaces ───────────────────────────────────────────

// fusa:req REQ-RELAY-023 REQ-RELAY-024 REQ-RELAY-025
enum class HealthStatus : int { OK = 0, Degraded = 1, Down = 2 };

struct Health {
    HealthStatus status{HealthStatus::OK};
    std::string  details;
};

class IHealthProvider {
public:
    virtual ~IHealthProvider() = default;
    virtual Health health() const = 0;
};

// fusa:req REQ-RELAY-026 REQ-RELAY-029
struct Metrics {
    uint64_t write_count{};
    uint64_t deliver_count{};
    uint64_t drop_count{};
    uint64_t bytes_written{};
    uint64_t bytes_delivered{};
    uint64_t error_count{};
};

// fusa:req REQ-RELAY-027
class IMetricsProvider {
public:
    virtual ~IMetricsProvider() = default;
    virtual Metrics metrics() const = 0;
};

// fusa:req REQ-RELAY-028
class IDrainer {
public:
    virtual ~IDrainer() = default;
    virtual std::error_code close_with_drain(std::chrono::milliseconds timeout) = 0;
};

} // namespace relay

// fusa:req REQ-RELAY-022
namespace std {
template<>
struct is_error_code_enum<relay::Errc> : true_type {};
} // namespace std
