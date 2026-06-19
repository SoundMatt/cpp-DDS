// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds.hpp — Core DDS types and IParticipant interface.
// C++ translation of github.com/SoundMatt/go-DDS, RELAY spec §8.2.

#pragma once

#include "relay.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// fusa:req REQ-DDS-001
// fusa:req REQ-DDS-002
// fusa:req REQ-DDS-003
// fusa:req REQ-DDS-004
// fusa:req REQ-DDS-005

namespace dds {

// ── Spec version ─────────────────────────────────────────────────────────────

inline constexpr const char* kSpecVersion = "1.7";

// ── Constants ─────────────────────────────────────────────────────────────────

inline constexpr int         kDomainMin        = 0;
inline constexpr int         kDomainMax        = 232;
inline constexpr std::size_t kDefaultChanDepth = 64;

// ── Domain ────────────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-001
using Domain = int;

// ── GUID ─────────────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-002
using Guid = std::array<uint8_t, 16>;

// ── QoS enumerations ──────────────────────────────────────────────────────────

// fusa:req REQ-DDS-003
enum class ReliabilityKind : int { BestEffort = 0, Reliable = 1 };

// fusa:req REQ-DDS-003
enum class DurabilityKind  : int { Volatile = 0, TransientLocal = 1 };

// ── QoS ──────────────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-003
struct QoS {
    ReliabilityKind reliability{ReliabilityKind::BestEffort};
    DurabilityKind  durability{DurabilityKind::Volatile};
    int             history_depth{1};
    std::chrono::nanoseconds deadline{0};        // 0 = disabled
    int             max_sample_size{0};          // 0 = unlimited
    int             transport_priority{0};
    std::chrono::nanoseconds latency_budget{0};
    std::chrono::nanoseconds lifespan{0};
    std::chrono::nanoseconds publish_period{0};
};

// DefaultQoS returns BestEffort + Volatile + history_depth=1.
inline QoS default_qos() noexcept { return QoS{}; }

// ReliableQoS returns Reliable + TransientLocal + history_depth=1.
inline QoS reliable_qos() noexcept {
    QoS q;
    q.reliability = ReliabilityKind::Reliable;
    q.durability  = DurabilityKind::TransientLocal;
    return q;
}

// ── Sample ────────────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-004
struct Sample {
    std::string                           topic;
    std::vector<uint8_t>                  payload;
    std::chrono::system_clock::time_point timestamp;
    uint64_t                              sequence_number{0};
    Guid                                  writer_guid{};

    relay::Message to_message() const;
};

// from_message converts a relay::Message to a Sample (§15.7.2).
// fusa:req REQ-DDS-004
std::pair<Sample, std::error_code> from_message(const relay::Message& m);

// ── Error category ────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-008
enum class Errc : int {
    topic_empty     = 1,
    qos_mismatch    = 2,
    deadline_missed = 3,
    sample_rejected = 4,
    resource_limits = 5,
    loan_buffer     = 6,
    domain_out_of_range = 7,
};

const std::error_category& error_category() noexcept;
std::error_code make_error_code(Errc e) noexcept;

// Relay sentinel aliases in the dds namespace.
inline std::error_code ErrClosed()          noexcept { return relay::ErrClosed(); }
inline std::error_code ErrNotConnected()    noexcept { return relay::ErrNotConnected(); }
inline std::error_code ErrTimeout()         noexcept { return relay::ErrTimeout(); }
inline std::error_code ErrPayloadTooLarge() noexcept { return relay::ErrPayloadTooLarge(); }

// DDS-specific errors — wrap the closest relay sentinel per spec §5.3.
inline std::error_code ErrTopicEmpty()        noexcept { return make_error_code(Errc::topic_empty); }
inline std::error_code ErrQoSMismatch()       noexcept { return make_error_code(Errc::qos_mismatch); }
inline std::error_code ErrDeadlineMissed()    noexcept { return make_error_code(Errc::deadline_missed); }
inline std::error_code ErrSampleRejected()    noexcept { return make_error_code(Errc::sample_rejected); }
inline std::error_code ErrResourceLimits()    noexcept { return make_error_code(Errc::resource_limits); }
inline std::error_code ErrLoanBuffer()        noexcept { return make_error_code(Errc::loan_buffer); }
inline std::error_code ErrDomainOutOfRange()  noexcept { return make_error_code(Errc::domain_out_of_range); }

// ── Domain validation ─────────────────────────────────────────────────────────

// fusa:req REQ-DDS-001
std::error_code validate_domain(Domain d) noexcept;

// ── IPublisher ────────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-005
class IPublisher {
public:
    virtual ~IPublisher() = default;

    // Write publishes payload to the publisher's topic.
    // Returns ErrClosed if the publisher or participant is closed.
    // Returns ErrPayloadTooLarge if QoS.max_sample_size > 0 and payload
    // exceeds that limit.
    virtual std::error_code write(const std::vector<uint8_t>& payload) = 0;

    // write with context — returns ErrTimeout if ctx expires before the write
    // can be accepted by a reliable subscriber.
    virtual std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) = 0;

    // close releases publisher resources. Idempotent.
    virtual std::error_code close() = 0;
};

// ── ISubscriber ───────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-006
class ISubscriber {
public:
    virtual ~ISubscriber() = default;

    // channel returns the delivery channel. Samples are pushed here by
    // the participant when a matching publish arrives.
    virtual std::shared_ptr<dds::Chan<Sample>> channel() = 0;

    // try_read returns the next sample without blocking; returns nullopt if empty.
    virtual std::optional<Sample> try_read() = 0;

    // unsubscribe removes this subscriber from the participant routing table.
    // The channel is closed; further sends to it are silently dropped.
    // Safe to call multiple times (idempotent).
    virtual void unsubscribe() = 0;

    // close is an alias for unsubscribe. Idempotent.
    virtual std::error_code close() = 0;
};

// ── IParticipant ──────────────────────────────────────────────────────────────

// fusa:req REQ-DDS-007
class IParticipant {
public:
    virtual ~IParticipant() = default;

    // new_publisher creates a publisher for topic with the given QoS.
    // Returns ErrClosed if the participant is closed.
    // Returns ErrTopicEmpty if topic is empty.
    virtual std::pair<std::shared_ptr<IPublisher>, std::error_code>
        new_publisher(const std::string& topic, QoS qos) = 0;

    // new_subscriber creates a subscriber for topic with the given QoS.
    // opts configures channel depth and back-pressure.
    virtual std::pair<std::shared_ptr<ISubscriber>, std::error_code>
        new_subscriber(const std::string& topic, QoS qos,
                       std::vector<relay::SubscriberOption> opts = {}) = 0;

    // domain returns the DDS domain this participant joined.
    virtual Domain domain() const noexcept = 0;

    // close closes the participant and all publishers/subscribers it owns.
    // Idempotent.
    virtual std::error_code close() = 0;
};

// ── adapt ─────────────────────────────────────────────────────────────────────

// adapt wraps a participant as a relay::INode for cross-protocol routing.
// The Node's send() creates an ephemeral publisher on msg.id as topic.
// The Node's subscribe() creates a subscriber with with_topic() routing.
// fusa:req REQ-DDS-009
std::unique_ptr<relay::INode> adapt(std::shared_ptr<IParticipant> p);

} // namespace dds

// Register dds::Errc as a std::error_code enum.
namespace std {
template<>
struct is_error_code_enum<dds::Errc> : true_type {};
} // namespace std
