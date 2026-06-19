// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-DDS-001 REQ-DDS-002 REQ-DDS-003 REQ-DDS-004 REQ-DDS-005
// fusa:test REQ-DDS-006 REQ-DDS-007 REQ-DDS-008 REQ-DDS-009 REQ-DDS-010
// fusa:test REQ-DDS-011 REQ-DDS-012 REQ-DDS-013
// fusa:test REQ-SEC-001 REQ-SEC-002 REQ-SEC-003 REQ-SEC-006
// fusa:test REQ-CHAN-001 REQ-CHAN-002 REQ-CHAN-003 REQ-CHAN-004
// fusa:test REQ-SAFETY-002
// fusa:test REQ-RELAY-VEC-001

#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <chrono>
#include <thread>

using namespace dds;
using namespace std::chrono_literals;

// ── Domain ────────────────────────────────────────────────────────────────────

TEST_CASE("validate_domain: valid boundaries", "[dds][REQ-DDS-001][REQ-SEC-001]") {
    CHECK_FALSE(validate_domain(0));
    CHECK_FALSE(validate_domain(232));
    CHECK_FALSE(validate_domain(100));
}

TEST_CASE("validate_domain: out-of-range returns error", "[dds][REQ-DDS-001][REQ-SEC-001]") {
    CHECK(validate_domain(-1)  == ErrDomainOutOfRange());
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

TEST_CASE("GUID known-hex encoding matches spec vector", "[dds][REQ-DDS-002][REQ-RELAY-VEC-001]") {
    // RELAY spec spec-15.7.2 golden vector: writer_guid bytes [1..16]
    // must encode to dds.writer_guid = "0102030405060708090a0b0c0d0e0f10"
    Sample s;
    s.topic = "rt/chatter";
    s.payload = {104, 101, 108, 108, 111, 32, 100, 100, 115}; // "hello dds"
    s.sequence_number = 7;
    for (int i = 0; i < 16; ++i)
        s.writer_guid[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i + 1);

    auto m = s.to_message();
    REQUIRE(m.meta.count("dds.writer_guid") == 1);
    CHECK(m.meta.at("dds.writer_guid") == "0102030405060708090a0b0c0d0e0f10");
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

// ── RELAY spec golden vector ──────────────────────────────────────────────────

TEST_CASE("RELAY spec spec-15.7.2 golden vector: to_message fields", "[dds][REQ-RELAY-VEC-001]") {
    // Spec vector: topic=rt/chatter, payload="hello dds" (base64 aGVsbG8gZGRz),
    // seq=7, writer_guid=[1..16], meta dds.writer_guid=0102030405060708090a0b0c0d0e0f10
    Sample s;
    s.topic           = "rt/chatter";
    s.payload         = {104, 101, 108, 108, 111, 32, 100, 100, 115}; // "hello dds"
    s.sequence_number = 7;
    for (int i = 0; i < 16; ++i)
        s.writer_guid[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i + 1);

    auto m = s.to_message();

    CHECK(static_cast<int>(m.protocol) == 2);   // DDS
    CHECK(m.id      == "rt/chatter");
    CHECK(m.payload == std::vector<uint8_t>{104, 101, 108, 108, 111, 32, 100, 100, 115});
    CHECK(m.seq     == 7);
    REQUIRE(m.meta.count("dds.writer_guid") == 1);
    CHECK(m.meta.at("dds.writer_guid") == "0102030405060708090a0b0c0d0e0f10");
}

TEST_CASE("RELAY spec spec-15.7.2 golden vector: from_message round-trip", "[dds][REQ-RELAY-VEC-001]") {
    relay::Message m;
    m.protocol = relay::Protocol::DDS;
    m.id       = "rt/chatter";
    m.payload  = {104, 101, 108, 108, 111, 32, 100, 100, 115};
    m.seq      = 7;
    m.meta["dds.writer_guid"] = "0102030405060708090a0b0c0d0e0f10";

    auto [s, ec] = from_message(m);
    CHECK_FALSE(ec);
    CHECK(s.topic           == "rt/chatter");
    CHECK(s.payload         == m.payload);
    CHECK(s.sequence_number == 7);
    for (int i = 0; i < 16; ++i)
        CHECK(s.writer_guid[static_cast<std::size_t>(i)] == static_cast<uint8_t>(i + 1));
}

// ── Error category ────────────────────────────────────────────────────────────

TEST_CASE("DDS error sentinel error_category name is dds", "[dds][REQ-DDS-008]") {
    CHECK(std::string(error_category().name()) == "dds");
}

TEST_CASE("DDS error codes are defined", "[dds][REQ-DDS-008][REQ-SAFETY-002]") {
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

// ── Domain boundary error matches spec error vector ───────────────────────────

TEST_CASE("Domain 233 is out-of-range per spec error vector", "[dds][REQ-DDS-001][REQ-RELAY-VEC-001]") {
    CHECK(validate_domain(233) == ErrDomainOutOfRange());
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

TEST_CASE("ILoaningPublisher is a polymorphic abstract type", "[dds][REQ-DDS-013]") {
    CHECK(std::is_abstract<ILoaningPublisher>::value);
    CHECK(std::is_polymorphic<ILoaningPublisher>::value);
    CHECK((std::is_base_of<IPublisher, ILoaningPublisher>::value));
}

// ── kSpecVersion ──────────────────────────────────────────────────────────────

TEST_CASE("kSpecVersion is 1.7", "[dds]") {
    CHECK(std::string(kSpecVersion) == "1.7");
}

// ── Constants ─────────────────────────────────────────────────────────────────

TEST_CASE("kDefaultChanDepth and kMaxChanDepth are sane", "[dds][REQ-SEC-006][REQ-CHAN-001]") {
    CHECK(kDefaultChanDepth == 64);
    CHECK(kMaxChanDepth     == 4096);
    CHECK(kDefaultChanDepth <= kMaxChanDepth);
}

// ── Chan<T> semantics ─────────────────────────────────────────────────────────

TEST_CASE("Chan try_send / try_recv are non-blocking", "[dds][REQ-CHAN-002]") {
    auto ch = std::make_shared<Chan<int>>(2);
    CHECK(ch->try_send(1) == Chan<int>::SendResult::Ok);
    CHECK(ch->try_send(2) == Chan<int>::SendResult::Ok);
    CHECK(ch->try_send(3) == Chan<int>::SendResult::Full);

    auto v = ch->try_recv();
    REQUIRE(v.has_value());
    CHECK(*v == 1);
}

TEST_CASE("Chan closed channel recv returns nullopt", "[dds][REQ-CHAN-003]") {
    auto ch = std::make_shared<Chan<int>>(4);
    ch->close();
    auto v = ch->recv();
    CHECK_FALSE(v.has_value());
}

TEST_CASE("Chan closed channel send returns false", "[dds][REQ-CHAN-003]") {
    auto ch = std::make_shared<Chan<int>>(4);
    ch->close();
    CHECK_FALSE(ch->send(42));
    CHECK(ch->try_send(42) == Chan<int>::SendResult::Closed);
}

TEST_CASE("Chan recv_until returns nullopt on timeout", "[dds][REQ-CHAN-002]") {
    auto ch = std::make_shared<Chan<int>>(4);
    auto dl = std::chrono::steady_clock::now() + 10ms;
    auto v = ch->recv_until(dl);
    CHECK_FALSE(v.has_value());
}

TEST_CASE("Chan recv_until returns value before deadline", "[dds][REQ-CHAN-002]") {
    auto ch = std::make_shared<Chan<int>>(4);
    std::thread([ch]{ std::this_thread::sleep_for(5ms); ch->send(42); }).detach();
    auto dl = std::chrono::steady_clock::now() + 500ms;
    auto v = ch->recv_until(dl);
    REQUIRE(v.has_value());
    CHECK(*v == 42);
}

TEST_CASE("Chan send_drop_oldest replaces head when full", "[dds][REQ-CHAN-004]") {
    auto ch = std::make_shared<Chan<int>>(2);
    CHECK(ch->try_send(1) == Chan<int>::SendResult::Ok);
    CHECK(ch->try_send(2) == Chan<int>::SendResult::Ok);
    // drop oldest (1), insert 3
    CHECK(ch->send_drop_oldest(3));
    auto v1 = ch->recv(); REQUIRE(v1); CHECK(*v1 == 2);
    auto v2 = ch->recv(); REQUIRE(v2); CHECK(*v2 == 3);
}

// ── with_filter subscriber option ─────────────────────────────────────────────

TEST_CASE("with_filter: only matching samples delivered", "[dds][REQ-DDS-010]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    // Only pass samples with payload[0] == 0xAA
    auto filter = with_filter([](const Sample& s) {
        return !s.payload.empty() && s.payload[0] == 0xAA;
    });

    auto [sub, es] = p->new_subscriber("filter/topic", default_qos(), {filter});
    REQUIRE_FALSE(es);
    auto [pub, ep] = p->new_publisher("filter/topic", default_qos());
    REQUIRE_FALSE(ep);

    CHECK_FALSE(pub->write({0xBB})); // rejected by filter
    CHECK_FALSE(pub->write({0xAA})); // passes filter
    CHECK_FALSE(pub->write({0xCC})); // rejected by filter
    CHECK_FALSE(pub->write({0xAA, 0x11})); // passes filter

    auto ch = sub->channel();
    auto s1 = ch->recv();
    REQUIRE(s1.has_value());
    CHECK(s1->payload[0] == 0xAA);

    auto s2 = ch->recv();
    REQUIRE(s2.has_value());
    CHECK(s2->payload[0] == 0xAA);
    CHECK(s2->payload.size() == 2);

    CHECK_FALSE(sub->try_read().has_value()); // nothing else
}

TEST_CASE("with_filter: null filter accepts all samples", "[dds][REQ-DDS-010]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [sub, _s] = p->new_subscriber("nofilter/topic", default_qos());
    auto [pub, _p] = p->new_publisher("nofilter/topic", default_qos());

    CHECK_FALSE(pub->write({1}));
    CHECK_FALSE(pub->write({2}));
    CHECK_FALSE(pub->write({3}));

    auto ch = sub->channel();
    REQUIRE(ch->recv().has_value());
    REQUIRE(ch->recv().has_value());
    REQUIRE(ch->recv().has_value());
}

// ── WaitSet ───────────────────────────────────────────────────────────────────

TEST_CASE("WaitSet wait_any returns sample from first ready channel", "[dds][REQ-DDS-012]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [sub1, _s1] = p->new_subscriber("ws/topic1", default_qos());
    auto [sub2, _s2] = p->new_subscriber("ws/topic2", default_qos());
    auto [pub1, _p1] = p->new_publisher("ws/topic1", default_qos());
    auto [pub2, _p2] = p->new_publisher("ws/topic2", default_qos());

    WaitSet ws;
    ws.add(sub1->channel(), 1);
    ws.add(sub2->channel(), 2);

    // Publish on topic2 first
    CHECK_FALSE(pub2->write({0xB2}));

    auto [sample, idx] = ws.wait_any(500ms);
    REQUIRE(sample.has_value());
    CHECK(idx == 2);
    CHECK(sample->payload == std::vector<uint8_t>{0xB2});
}

TEST_CASE("WaitSet wait_any timeout returns nullopt and -1", "[dds][REQ-DDS-012]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);
    auto [sub, _] = p->new_subscriber("ws/timeout", default_qos());

    WaitSet ws;
    ws.add(sub->channel(), 0);

    auto [sample, idx] = ws.wait_any(20ms);
    CHECK_FALSE(sample.has_value());
    CHECK(idx == -1);
}

TEST_CASE("WaitSet wait_any receives from multiple topics", "[dds][REQ-DDS-012]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [s1, _s1] = p->new_subscriber("ws2/a", default_qos());
    auto [s2, _s2] = p->new_subscriber("ws2/b", default_qos());
    auto [p1, _p1] = p->new_publisher("ws2/a", default_qos());
    auto [p2, _p2] = p->new_publisher("ws2/b", default_qos());

    WaitSet ws;
    ws.add(s1->channel(), 10);
    ws.add(s2->channel(), 20);

    CHECK_FALSE(p1->write({0x01}));
    CHECK_FALSE(p2->write({0x02}));

    int count = 0;
    for (int i = 0; i < 2; ++i) {
        auto [sample, idx] = ws.wait_any(500ms);
        REQUIRE(sample.has_value());
        CHECK((idx == 10 || idx == 20));
        ++count;
    }
    CHECK(count == 2);
}
