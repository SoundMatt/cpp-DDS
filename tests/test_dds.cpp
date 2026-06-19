// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-DDS-001 REQ-DDS-002 REQ-DDS-003 REQ-DDS-004 REQ-DDS-005
// fusa:test REQ-DDS-006 REQ-DDS-007 REQ-DDS-008 REQ-DDS-009

#include <dds/dds.hpp>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <chrono>

using namespace dds;

// ── Domain ────────────────────────────────────────────────────────────────────

TEST_CASE("validate_domain: valid boundaries", "[dds][REQ-DDS-001]") {
    CHECK_FALSE(validate_domain(0));
    CHECK_FALSE(validate_domain(232));
    CHECK_FALSE(validate_domain(100));
}

TEST_CASE("validate_domain: out-of-range returns error", "[dds][REQ-DDS-001]") {
    CHECK(validate_domain(-1) == ErrDomainOutOfRange());
    CHECK(validate_domain(233) == ErrDomainOutOfRange());
}

TEST_CASE("kDomainMin/kDomainMax constants", "[dds][REQ-DDS-001]") {
    CHECK(kDomainMin == 0);
    CHECK(kDomainMax == 232);
}

// ── GUID ──────────────────────────────────────────────────────────────────────

TEST_CASE("Guid is 16-byte array", "[dds][REQ-DDS-002]") {
    Guid g{};
    CHECK(g.size() == 16);
    for (auto b : g) CHECK(b == 0);
}

// ── QoS ───────────────────────────────────────────────────────────────────────

TEST_CASE("default_qos returns BestEffort Volatile history_depth=1", "[dds][REQ-DDS-003]") {
    auto q = default_qos();
    CHECK(q.reliability  == ReliabilityKind::BestEffort);
    CHECK(q.durability   == DurabilityKind::Volatile);
    CHECK(q.history_depth == 1);
    CHECK(q.deadline.count() == 0);
    CHECK(q.max_sample_size == 0);
}

TEST_CASE("reliable_qos returns Reliable TransientLocal history_depth=1", "[dds][REQ-DDS-003]") {
    auto q = reliable_qos();
    CHECK(q.reliability  == ReliabilityKind::Reliable);
    CHECK(q.durability   == DurabilityKind::TransientLocal);
    CHECK(q.history_depth == 1);
}

TEST_CASE("ReliabilityKind enum values", "[dds][REQ-DDS-003]") {
    CHECK(static_cast<int>(ReliabilityKind::BestEffort) == 0);
    CHECK(static_cast<int>(ReliabilityKind::Reliable)   == 1);
}

TEST_CASE("DurabilityKind enum values", "[dds][REQ-DDS-003]") {
    CHECK(static_cast<int>(DurabilityKind::Volatile)       == 0);
    CHECK(static_cast<int>(DurabilityKind::TransientLocal) == 1);
}

// ── Sample ────────────────────────────────────────────────────────────────────

TEST_CASE("Sample to_message round-trip", "[dds][REQ-DDS-004]") {
    Sample s;
    s.topic           = "vehicle/speed";
    s.payload         = {0xDE, 0xAD, 0xBE, 0xEF};
    s.timestamp       = std::chrono::system_clock::now();
    s.sequence_number = 42;
    s.writer_guid[0]  = 0xAB;

    auto m = s.to_message();
    CHECK(m.protocol == relay::Protocol::DDS);
    CHECK(m.id       == "vehicle/speed");
    CHECK(m.payload  == s.payload);
    CHECK(m.seq      == 42);
    CHECK(!m.meta["dds.writer_guid"].empty());
}

TEST_CASE("from_message round-trip", "[dds][REQ-DDS-004]") {
    Sample orig;
    orig.topic           = "sensors/temp";
    orig.payload         = {0x01, 0x02, 0x03};
    orig.timestamp       = std::chrono::system_clock::now();
    orig.sequence_number = 7;
    orig.writer_guid[0]  = 0xFF;
    orig.writer_guid[15] = 0x0A;

    auto m = orig.to_message();
    auto [got, ec] = from_message(m);
    CHECK_FALSE(ec);
    CHECK(got.topic           == orig.topic);
    CHECK(got.payload         == orig.payload);
    CHECK(got.sequence_number == orig.sequence_number);
    CHECK(got.writer_guid     == orig.writer_guid);
}

TEST_CASE("from_message with missing guid is ok", "[dds][REQ-DDS-004]") {
    relay::Message m;
    m.protocol = relay::Protocol::DDS;
    m.id       = "some/topic";
    m.payload  = {1, 2, 3};

    auto [s, ec] = from_message(m);
    CHECK_FALSE(ec);
    CHECK(s.topic == "some/topic");
    Guid zero{};
    CHECK(s.writer_guid == zero);
}

// ── Error sentinels ───────────────────────────────────────────────────────────

TEST_CASE("DDS error sentinel error_category name is dds", "[dds][REQ-DDS-008]") {
    CHECK(std::string(error_category().name()) == "dds");
}

TEST_CASE("DDS error codes are defined", "[dds][REQ-DDS-008]") {
    CHECK(ErrTopicEmpty());
    CHECK(ErrQoSMismatch());
    CHECK(ErrDeadlineMissed());
    CHECK(ErrSampleRejected());
    CHECK(ErrResourceLimits());
    CHECK(ErrLoanBuffer());
    CHECK(ErrDomainOutOfRange());
}

TEST_CASE("DDS errors have non-empty messages", "[dds][REQ-DDS-008]") {
    CHECK_FALSE(ErrTopicEmpty().message().empty());
    CHECK_FALSE(ErrQoSMismatch().message().empty());
    CHECK_FALSE(ErrDeadlineMissed().message().empty());
    CHECK_FALSE(ErrSampleRejected().message().empty());
    CHECK_FALSE(ErrResourceLimits().message().empty());
    CHECK_FALSE(ErrLoanBuffer().message().empty());
    CHECK_FALSE(ErrDomainOutOfRange().message().empty());
}

TEST_CASE("DDS relay sentinel aliases match relay errors", "[dds][REQ-DDS-008]") {
    CHECK(ErrClosed()          == relay::ErrClosed());
    CHECK(ErrNotConnected()    == relay::ErrNotConnected());
    CHECK(ErrTimeout()         == relay::ErrTimeout());
    CHECK(ErrPayloadTooLarge() == relay::ErrPayloadTooLarge());
}

// ── Interface types ───────────────────────────────────────────────────────────

TEST_CASE("IPublisher is a polymorphic abstract type", "[dds][REQ-DDS-005]") {
    CHECK(std::is_abstract<IPublisher>::value);
    CHECK(std::is_polymorphic<IPublisher>::value);
}

TEST_CASE("ISubscriber is a polymorphic abstract type", "[dds][REQ-DDS-006]") {
    CHECK(std::is_abstract<ISubscriber>::value);
    CHECK(std::is_polymorphic<ISubscriber>::value);
}

TEST_CASE("IParticipant is a polymorphic abstract type", "[dds][REQ-DDS-007]") {
    CHECK(std::is_abstract<IParticipant>::value);
    CHECK(std::is_polymorphic<IParticipant>::value);
}

// ── kSpecVersion ──────────────────────────────────────────────────────────────

TEST_CASE("kSpecVersion is 1.7", "[dds]") {
    CHECK(std::string(kSpecVersion) == "1.7");
}
