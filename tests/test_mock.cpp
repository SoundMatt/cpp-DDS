// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-MOCK-001 REQ-MOCK-002 REQ-MOCK-003 REQ-MOCK-004 REQ-MOCK-005
// fusa:test REQ-DDS-005 REQ-DDS-006 REQ-DDS-007

#include <dds/mock/participant.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace dds;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::shared_ptr<IParticipant> make_p(Domain d = 0) {
    auto [p, ec] = dds::mock::create(d);
    REQUIRE_FALSE(ec);
    REQUIRE(p);
    return p;
}

// ── create ────────────────────────────────────────────────────────────────────

TEST_CASE("create: valid domain succeeds", "[mock][REQ-MOCK-001]") {
    auto [p, ec] = dds::mock::create(0);
    CHECK_FALSE(ec);
    CHECK(p);
    CHECK(p->domain() == 0);
}

TEST_CASE("create: domain 232 is valid boundary", "[mock][REQ-MOCK-001]") {
    auto [p, ec] = dds::mock::create(232);
    CHECK_FALSE(ec);
    CHECK(p);
}

TEST_CASE("create: domain out-of-range returns error", "[mock][REQ-MOCK-001]") {
    auto [p, ec] = dds::mock::create(-1);
    CHECK(ec == ErrDomainOutOfRange());
    CHECK(!p);

    auto [p2, ec2] = dds::mock::create(233);
    CHECK(ec2 == ErrDomainOutOfRange());
}

// ── publisher / subscriber basics ────────────────────────────────────────────

TEST_CASE("new_publisher: empty topic returns ErrTopicEmpty", "[mock][REQ-DDS-007]") {
    auto p = make_p();
    auto [pub, ec] = p->new_publisher("", default_qos());
    CHECK(ec == ErrTopicEmpty());
    CHECK(!pub);
}

TEST_CASE("new_subscriber: empty topic returns ErrTopicEmpty", "[mock][REQ-DDS-007]") {
    auto p = make_p();
    auto [sub, ec] = p->new_subscriber("", default_qos());
    CHECK(ec == ErrTopicEmpty());
    CHECK(!sub);
}

TEST_CASE("publish and receive a sample", "[mock][REQ-MOCK-003][REQ-MOCK-004]") {
    auto p1 = make_p();
    auto p2 = make_p();

    auto [sub, ec_sub] = p2->new_subscriber("test/topic", default_qos());
    REQUIRE_FALSE(ec_sub);
    REQUIRE(sub);

    auto [pub, ec_pub] = p1->new_publisher("test/topic", default_qos());
    REQUIRE_FALSE(ec_pub);
    REQUIRE(pub);

    auto ec = pub->write({0xDE, 0xAD});
    CHECK_FALSE(ec);

    auto ch = sub->channel();
    auto sample = ch->recv();
    REQUIRE(sample.has_value());
    CHECK(sample->topic   == "test/topic");
    CHECK(sample->payload == std::vector<uint8_t>{0xDE, 0xAD});
    CHECK(sample->sequence_number == 0);
}

TEST_CASE("sequence numbers increment per publisher", "[mock][REQ-MOCK-003]") {
    auto p = make_p();

    auto [sub, _s] = p->new_subscriber("seq/topic", default_qos());
    auto [pub, _p] = p->new_publisher("seq/topic", default_qos());

    pub->write({1});
    pub->write({2});
    pub->write({3});

    auto ch = sub->channel();
    auto s0 = ch->recv(); REQUIRE(s0); CHECK(s0->sequence_number == 0);
    auto s1 = ch->recv(); REQUIRE(s1); CHECK(s1->sequence_number == 1);
    auto s2 = ch->recv(); REQUIRE(s2); CHECK(s2->sequence_number == 2);
}

// ── try_read ──────────────────────────────────────────────────────────────────

TEST_CASE("try_read returns nullopt when empty", "[mock][REQ-DDS-006]") {
    auto p = make_p();
    auto [sub, _] = p->new_subscriber("try/topic", default_qos());
    CHECK_FALSE(sub->try_read().has_value());
}

TEST_CASE("try_read returns sample without blocking", "[mock][REQ-DDS-006]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("try/topic", default_qos());
    auto [pub, _p] = p->new_publisher("try/topic", default_qos());

    pub->write({0xAA});
    auto s = sub->try_read();
    REQUIRE(s.has_value());
    CHECK(s->payload == std::vector<uint8_t>{0xAA});
}

// ── unsubscribe ───────────────────────────────────────────────────────────────

TEST_CASE("unsubscribe: channel closed after unsubscribe", "[mock][REQ-MOCK-005]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("unsub/topic", default_qos());
    auto ch = sub->channel();

    sub->unsubscribe();
    CHECK(ch->is_closed());
}

TEST_CASE("unsubscribe: idempotent — safe to call twice", "[mock][REQ-MOCK-005]") {
    auto p = make_p();
    auto [sub, _] = p->new_subscriber("unsub2/topic", default_qos());

    sub->unsubscribe();
    REQUIRE_NOTHROW(sub->unsubscribe());
}

TEST_CASE("unsubscribe: further publishes not delivered after unsubscribe", "[mock][REQ-MOCK-005]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("after/topic", default_qos());
    auto [pub, _p] = p->new_publisher("after/topic", default_qos());

    sub->unsubscribe();
    pub->write({0xFF});

    // Channel is closed — try_read returns nullopt.
    CHECK_FALSE(sub->try_read().has_value());
}

// ── close ─────────────────────────────────────────────────────────────────────

TEST_CASE("participant close: subsequent new_publisher returns ErrClosed", "[mock][REQ-MOCK-002]") {
    auto p = make_p();
    p->close();
    auto [pub, ec] = p->new_publisher("closed/topic", default_qos());
    CHECK(ec == ErrClosed());
    CHECK(!pub);
}

TEST_CASE("participant close: subsequent new_subscriber returns ErrClosed", "[mock][REQ-MOCK-002]") {
    auto p = make_p();
    p->close();
    auto [sub, ec] = p->new_subscriber("closed/topic", default_qos());
    CHECK(ec == ErrClosed());
    CHECK(!sub);
}

TEST_CASE("participant close is idempotent", "[mock][REQ-MOCK-002]") {
    auto p = make_p();
    CHECK_FALSE(p->close());
    CHECK_FALSE(p->close());
}

TEST_CASE("publisher close: write returns ErrClosed after close", "[mock][REQ-DDS-005]") {
    auto p = make_p();
    auto [pub, _] = p->new_publisher("pub/close", default_qos());
    pub->close();
    CHECK(pub->write({1, 2, 3}) == ErrClosed());
}

// ── max_sample_size QoS ───────────────────────────────────────────────────────

TEST_CASE("write rejects payload exceeding max_sample_size", "[mock][REQ-DDS-005]") {
    auto p = make_p();
    QoS q;
    q.max_sample_size = 4;
    auto [pub, _] = p->new_publisher("size/topic", q);
    REQUIRE(pub);

    CHECK_FALSE(pub->write({1, 2, 3, 4}));
    CHECK(pub->write({1, 2, 3, 4, 5}) == ErrPayloadTooLarge());
}

// ── context deadline ──────────────────────────────────────────────────────────

TEST_CASE("write with expired context returns ErrTimeout", "[mock][REQ-DDS-005]") {
    auto p = make_p();
    auto [pub, _] = p->new_publisher("ctx/topic", default_qos());

    auto ctx = relay::Context::with_timeout(std::chrono::milliseconds{0});
    std::this_thread::sleep_for(5ms);
    CHECK(pub->write(ctx, {1}) == ErrTimeout());
}

// ── TransientLocal durability ─────────────────────────────────────────────────

TEST_CASE("TransientLocal: late subscriber receives last sample", "[mock][REQ-DDS-003]") {
    auto p = make_p();
    QoS q = reliable_qos();

    auto [pub, _p] = p->new_publisher("transient/topic", q);
    pub->write({0x42});

    // Subscribe after the publish.
    auto [sub, _s] = p->new_subscriber("transient/topic", q);
    auto ch = sub->channel();

    auto sample = ch->recv();
    REQUIRE(sample.has_value());
    CHECK(sample->payload == std::vector<uint8_t>{0x42});
}

// ── Multiple independent subscribers ─────────────────────────────────────────

TEST_CASE("multiple subscribers each receive independent copies", "[mock][REQ-MOCK-004]") {
    auto p = make_p();
    auto [sub1, _s1] = p->new_subscriber("multi/topic", default_qos());
    auto [sub2, _s2] = p->new_subscriber("multi/topic", default_qos());
    auto [pub, _p]   = p->new_publisher("multi/topic", default_qos());

    pub->write({0xAB});

    auto ch1 = sub1->channel();
    auto ch2 = sub2->channel();

    auto s1 = ch1->recv();
    auto s2 = ch2->recv();

    REQUIRE(s1.has_value()); CHECK(s1->payload == std::vector<uint8_t>{0xAB});
    REQUIRE(s2.has_value()); CHECK(s2->payload == std::vector<uint8_t>{0xAB});
}

// ── adapt(): relay::INode wrapper ─────────────────────────────────────────────

TEST_CASE("adapt: protocol() returns DDS", "[mock][REQ-DDS-009]") {
    auto p = make_p();
    auto node = adapt(p);
    CHECK(node->protocol() == relay::Protocol::DDS);
}

TEST_CASE("adapt: send with empty id returns error", "[mock][REQ-DDS-009]") {
    auto p = make_p();
    auto node = adapt(p);
    relay::Message m;
    m.payload = {1, 2, 3};
    CHECK(node->send(m));
}

TEST_CASE("adapt: subscribe without topic returns error", "[mock][REQ-DDS-009]") {
    auto p = make_p();
    auto node = adapt(p);
    auto [ch, ec] = node->subscribe();
    CHECK(ec);
}

TEST_CASE("adapt: send and subscribe round-trip via relay::Message", "[mock][REQ-DDS-009]") {
    auto p1 = make_p();
    auto p2 = make_p();

    auto node1 = adapt(p1);
    auto node2 = adapt(p2);

    auto [ch, ec] = node2->subscribe({relay::with_topic("relay/topic")});
    REQUIRE_FALSE(ec);
    REQUIRE(ch);

    relay::Message msg;
    msg.id      = "relay/topic";
    msg.payload = {0xCA, 0xFE};
    REQUIRE_FALSE(node1->send(msg));

    auto recv = ch->recv();
    REQUIRE(recv.has_value());
    CHECK(recv->id      == "relay/topic");
    CHECK(recv->payload == std::vector<uint8_t>{0xCA, 0xFE});
}
