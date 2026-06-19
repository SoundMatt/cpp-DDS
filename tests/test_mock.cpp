// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-MOCK-001 REQ-MOCK-002 REQ-MOCK-003 REQ-MOCK-004 REQ-MOCK-005
// fusa:test REQ-DDS-005 REQ-DDS-006 REQ-DDS-007 REQ-DDS-009
// fusa:test REQ-METRICS-001 REQ-METRICS-002 REQ-METRICS-003
// fusa:test REQ-HEALTH-001 REQ-HEALTH-002
// fusa:test REQ-SEC-002 REQ-SEC-003 REQ-SEC-004 REQ-SEC-005
// fusa:test REQ-LIFECYCLE-001 REQ-LIFECYCLE-002 REQ-LIFECYCLE-003
// fusa:test REQ-LIFECYCLE-004 REQ-LIFECYCLE-005
// fusa:test REQ-CHAN-004 REQ-SAFETY-001 REQ-SAFETY-003

#include <dds/mock/participant.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

using namespace dds;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::shared_ptr<dds::mock::IMockParticipant> make_p(Domain d = 0) {
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

TEST_CASE("create: domain out-of-range returns error", "[mock][REQ-MOCK-001][REQ-SEC-001]") {
    auto [p, ec] = dds::mock::create(-1);
    CHECK(ec == ErrDomainOutOfRange());
    CHECK(!p);

    auto [p2, ec2] = dds::mock::create(233);
    CHECK(ec2 == ErrDomainOutOfRange());
}

// ── publisher / subscriber basics ────────────────────────────────────────────

TEST_CASE("new_publisher: empty topic returns ErrTopicEmpty", "[mock][REQ-DDS-007][REQ-SEC-003]") {
    auto p = make_p();
    auto [pub, ec] = p->new_publisher("", default_qos());
    CHECK(ec == ErrTopicEmpty());
    CHECK(!pub);
}

TEST_CASE("new_subscriber: empty topic returns ErrTopicEmpty", "[mock][REQ-DDS-007][REQ-SEC-003]") {
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

    CHECK_FALSE(pub->write({0xDE, 0xAD}));

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

    CHECK_FALSE(pub->write({1}));
    CHECK_FALSE(pub->write({2}));
    CHECK_FALSE(pub->write({3}));

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
    auto [sub, _s] = p->new_subscriber("try/topic2", default_qos());
    auto [pub, _p] = p->new_publisher("try/topic2", default_qos());

    CHECK_FALSE(pub->write({0xAA}));
    auto s = sub->try_read();
    REQUIRE(s.has_value());
    CHECK(s->payload == std::vector<uint8_t>{0xAA});
}

// ── unsubscribe ───────────────────────────────────────────────────────────────

TEST_CASE("unsubscribe: channel closed after unsubscribe", "[mock][REQ-MOCK-005][REQ-LIFECYCLE-002]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("unsub/topic", default_qos());
    auto ch = sub->channel();

    sub->unsubscribe();
    CHECK(ch->is_closed());
}

TEST_CASE("unsubscribe: idempotent - safe to call twice", "[mock][REQ-MOCK-005][REQ-LIFECYCLE-002]") {
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
    CHECK_FALSE(pub->write({0xFF}));

    CHECK_FALSE(sub->try_read().has_value());
}

TEST_CASE("subscriber destructor auto-unsubscribes", "[mock][REQ-LIFECYCLE-005]") {
    auto p = make_p();
    std::shared_ptr<dds::Chan<Sample>> ch;
    {
        auto [sub, _] = p->new_subscriber("dtor/topic", default_qos());
        ch = sub->channel();
        // sub goes out of scope here, destructor called
    }
    CHECK(ch->is_closed());
}

// ── close ─────────────────────────────────────────────────────────────────────

TEST_CASE("participant close: subsequent new_publisher returns ErrClosed", "[mock][REQ-MOCK-002][REQ-LIFECYCLE-004]") {
    auto p = make_p();
    CHECK_FALSE(p->close());
    auto [pub, ec] = p->new_publisher("closed/topic", default_qos());
    CHECK(ec == ErrClosed());
    CHECK(!pub);
}

TEST_CASE("participant close: subsequent new_subscriber returns ErrClosed", "[mock][REQ-MOCK-002][REQ-LIFECYCLE-004]") {
    auto p = make_p();
    CHECK_FALSE(p->close());
    auto [sub, ec] = p->new_subscriber("closed/topic", default_qos());
    CHECK(ec == ErrClosed());
    CHECK(!sub);
}

TEST_CASE("participant close is idempotent", "[mock][REQ-MOCK-002][REQ-LIFECYCLE-003]") {
    auto p = make_p();
    CHECK_FALSE(p->close());
    CHECK_FALSE(p->close());
}

TEST_CASE("publisher close: write returns ErrClosed after close", "[mock][REQ-DDS-005][REQ-LIFECYCLE-001][REQ-SEC-004]") {
    auto p = make_p();
    auto [pub, _] = p->new_publisher("pub/close", default_qos());
    CHECK_FALSE(pub->close());
    CHECK(pub->write({1, 2, 3}) == ErrClosed());
}

TEST_CASE("publisher close is idempotent", "[mock][REQ-LIFECYCLE-001]") {
    auto p = make_p();
    auto [pub, _] = p->new_publisher("pub/idempotent", default_qos());
    CHECK_FALSE(pub->close());
    CHECK_FALSE(pub->close());
}

// ── max_sample_size QoS ───────────────────────────────────────────────────────

TEST_CASE("write rejects payload exceeding max_sample_size", "[mock][REQ-DDS-005][REQ-SEC-002]") {
    auto p = make_p();
    QoS q;
    q.max_sample_size = 4;
    auto [pub, _] = p->new_publisher("size/topic", q);
    REQUIRE(pub);

    CHECK_FALSE(pub->write({1, 2, 3, 4}));
    CHECK(pub->write({1, 2, 3, 4, 5}) == ErrPayloadTooLarge());
}

// ── context deadline ──────────────────────────────────────────────────────────

TEST_CASE("write with expired context returns ErrTimeout", "[mock][REQ-DDS-005][REQ-SAFETY-003]") {
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
    CHECK_FALSE(pub->write({0x42}));

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

    CHECK_FALSE(pub->write({0xAB}));

    auto ch1 = sub1->channel();
    auto ch2 = sub2->channel();

    auto s1 = ch1->recv();
    auto s2 = ch2->recv();

    REQUIRE(s1.has_value()); CHECK(s1->payload == std::vector<uint8_t>{0xAB});
    REQUIRE(s2.has_value()); CHECK(s2->payload == std::vector<uint8_t>{0xAB});
}

// ── Back-pressure policies ─────────────────────────────────────────────────

TEST_CASE("DropOldest: oldest item evicted when channel full", "[mock][REQ-MOCK-004][REQ-CHAN-004]") {
    auto p = make_p();
    auto depth = relay::with_channel_depth(2);
    auto bp    = relay::with_back_pressure(relay::BackPressurePolicy::DropOldest);
    auto [sub, _s] = p->new_subscriber("drop_oldest/topic", default_qos(), {depth, bp});
    auto [pub, _p] = p->new_publisher("drop_oldest/topic", default_qos());

    // Fill channel (cap=2), then overfill — oldest should be evicted
    CHECK_FALSE(pub->write({1}));
    CHECK_FALSE(pub->write({2}));
    CHECK_FALSE(pub->write({3})); // evicts {1}

    auto ch = sub->channel();
    auto r1 = ch->recv(); REQUIRE(r1); CHECK(r1->payload == std::vector<uint8_t>{2});
    auto r2 = ch->recv(); REQUIRE(r2); CHECK(r2->payload == std::vector<uint8_t>{3});
}

TEST_CASE("DropNewest: incoming item dropped when channel full", "[mock][REQ-MOCK-004][REQ-CHAN-004]") {
    auto p = make_p();
    auto depth = relay::with_channel_depth(2);
    auto bp    = relay::with_back_pressure(relay::BackPressurePolicy::DropNewest);
    auto [sub, _s] = p->new_subscriber("drop_newest/topic", default_qos(), {depth, bp});
    auto [pub, _p] = p->new_publisher("drop_newest/topic", default_qos());

    CHECK_FALSE(pub->write({1}));
    CHECK_FALSE(pub->write({2}));
    CHECK_FALSE(pub->write({3})); // dropped — channel full

    auto ch = sub->channel();
    auto r1 = ch->recv(); REQUIRE(r1); CHECK(r1->payload == std::vector<uint8_t>{1});
    auto r2 = ch->recv(); REQUIRE(r2); CHECK(r2->payload == std::vector<uint8_t>{2});
    CHECK_FALSE(sub->try_read().has_value());
}

TEST_CASE("Block: sender blocks until space available", "[mock][REQ-MOCK-004][REQ-CHAN-004]") {
    auto p = make_p();
    auto depth = relay::with_channel_depth(1);
    auto bp    = relay::with_back_pressure(relay::BackPressurePolicy::Block);
    auto [sub, _s] = p->new_subscriber("block/topic", default_qos(), {depth, bp});
    auto [pub, _p] = p->new_publisher("block/topic", default_qos());

    CHECK_FALSE(pub->write({0xAA})); // fills channel

    // Capture channel directly to avoid C++17 structured-binding capture restriction.
    auto sub_ch = sub->channel();
    std::thread consumer([sub_ch]() {
        std::this_thread::sleep_for(20ms);
        auto s = sub_ch->recv();
        (void)s;
    });

    CHECK_FALSE(pub->write({0xBB})); // blocks until consumer reads
    consumer.join();

    auto r = sub_ch->recv();
    REQUIRE(r.has_value());
    CHECK(r->payload == std::vector<uint8_t>{0xBB});
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
    CHECK(node->send(relay::Context::background(), m));
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
    REQUIRE_FALSE(node1->send(relay::Context::background(), msg));

    auto recv = ch->recv();
    REQUIRE(recv.has_value());
    CHECK(recv->id      == "relay/topic");
    CHECK(recv->payload == std::vector<uint8_t>{0xCA, 0xFE});
}

// ── Metrics ───────────────────────────────────────────────────────────────────

TEST_CASE("metrics: write_count increments per write", "[mock][REQ-METRICS-001][REQ-METRICS-003]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("met/topic", default_qos());
    auto [pub, _p] = p->new_publisher("met/topic", default_qos());

    auto m0 = p->metrics();
    CHECK(m0.write_count == 0);

    CHECK_FALSE(pub->write({1, 2, 3}));
    CHECK_FALSE(pub->write({4, 5}));

    auto m2 = p->metrics();
    CHECK(m2.write_count == 2);
    CHECK(m2.bytes_written == 5); // 3 + 2
}

TEST_CASE("metrics: deliver_count tracks successful deliveries", "[mock][REQ-METRICS-002][REQ-METRICS-003]") {
    auto p = make_p();
    auto [sub1, _s1] = p->new_subscriber("met2/topic", default_qos());
    auto [sub2, _s2] = p->new_subscriber("met2/topic", default_qos());
    auto [pub, _p]   = p->new_publisher("met2/topic", default_qos());

    CHECK_FALSE(pub->write({0xAB}));

    auto m = p->metrics();
    // 2 subscribers received the sample
    CHECK(m.write_count   == 1);
    CHECK(m.deliver_count == 2);
}

TEST_CASE("metrics: drop_count tracks dropped samples", "[mock][REQ-METRICS-002]") {
    auto p = make_p();
    // cap=1 channel with DropNewest so extras are dropped
    auto depth = relay::with_channel_depth(1);
    auto [sub, _s] = p->new_subscriber("met3/topic", default_qos(), {depth});
    auto [pub, _p] = p->new_publisher("met3/topic", default_qos());

    CHECK_FALSE(pub->write({1})); // delivered
    CHECK_FALSE(pub->write({2})); // dropped (channel full, DropNewest)
    CHECK_FALSE(pub->write({3})); // dropped

    auto m = p->metrics();
    CHECK(m.write_count   == 3);
    CHECK(m.deliver_count == 1);
    CHECK(m.drop_count    == 2);
}

TEST_CASE("metrics: error_count increments on write errors", "[mock][REQ-METRICS-001]") {
    auto p = make_p();
    QoS q;
    q.max_sample_size = 2;
    auto [pub, _p] = p->new_publisher("met4/topic", q);

    CHECK_FALSE(pub->write({1, 2})); // ok
    CHECK(pub->write({1, 2, 3}) == ErrPayloadTooLarge()); // error

    auto m = p->metrics();
    CHECK(m.error_count == 1);
}

TEST_CASE("metrics: bytes_written reflects payload sizes", "[mock][REQ-METRICS-001]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("bytes/topic", default_qos());
    auto [pub, _p] = p->new_publisher("bytes/topic", default_qos());

    CHECK_FALSE(pub->write({1, 2, 3, 4, 5})); // 5 bytes

    auto m = p->metrics();
    CHECK(m.bytes_written == 5);
}

// ── Health ────────────────────────────────────────────────────────────────────

TEST_CASE("health: OK when active", "[mock][REQ-HEALTH-001]") {
    auto p = make_p();
    auto h = p->health();
    CHECK(h.status == relay::HealthStatus::OK);
}

TEST_CASE("health: Down after close", "[mock][REQ-HEALTH-002]") {
    auto p = make_p();
    CHECK_FALSE(p->close());
    auto h = p->health();
    CHECK(h.status == relay::HealthStatus::Down);
    CHECK_FALSE(h.details.empty());
}

// ── IDrainer ──────────────────────────────────────────────────────────────────

TEST_CASE("close_with_drain: returns ok when channel empties within timeout", "[mock]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("drain/topic", default_qos());
    auto [pub, _p] = p->new_publisher("drain/topic", default_qos());

    // Capture channel directly to avoid C++17 structured-binding capture restriction.
    auto sub_ch = sub->channel();
    std::thread reader([sub_ch]() {
        auto s = sub_ch->recv();
        (void)s;
    });
    CHECK_FALSE(pub->write({0x99}));

    auto ec = p->close_with_drain(500ms);
    CHECK_FALSE(ec);
    reader.join();
}

TEST_CASE("close_with_drain: returns ErrTimeout if channel not drained in time", "[mock]") {
    auto p = make_p();
    auto [sub, _s] = p->new_subscriber("drain2/topic", default_qos());
    auto [pub, _p] = p->new_publisher("drain2/topic", default_qos());

    // Leave sample in channel, expect timeout
    CHECK_FALSE(pub->write({0xFF}));

    auto ec = p->close_with_drain(20ms);
    CHECK(ec == ErrTimeout());
    // participant is still closed after drain timeout
    CHECK(p->health().status == relay::HealthStatus::Down);
}

// ── Concurrent safety ─────────────────────────────────────────────────────────

TEST_CASE("concurrent publish from multiple threads is safe", "[mock][REQ-SEC-005][REQ-SAFETY-001]") {
    auto p = make_p();
    constexpr int kPubs    = 4;
    constexpr int kMsgs    = 25;

    auto [sub, _s] = p->new_subscriber("concurrent/topic", default_qos(),
        {relay::with_channel_depth(kPubs * kMsgs)});

    std::vector<std::thread> threads;
    threads.reserve(kPubs);
    for (int i = 0; i < kPubs; ++i) {
        threads.emplace_back([&p, i, kMsgs]() {
            auto [pub, ec] = p->new_publisher("concurrent/topic", default_qos());
            REQUIRE_FALSE(ec);
            for (int j = 0; j < kMsgs; ++j)
                CHECK_FALSE(pub->write({static_cast<uint8_t>(i), static_cast<uint8_t>(j)}));
        });
    }
    for (auto& t : threads) t.join();

    auto ch = sub->channel();
    int count = 0;
    while (auto s = ch->try_recv()) ++count;
    CHECK(count == kPubs * kMsgs);
}

TEST_CASE("concurrent subscribe and publish is safe", "[mock][REQ-SEC-005][REQ-SAFETY-001]") {
    auto p = make_p();
    auto [pub, ep] = p->new_publisher("concsub/topic", default_qos());
    REQUIRE_FALSE(ep);

    std::atomic<int> received{0};
    constexpr int kSubs = 4;
    std::vector<std::thread> threads;
    threads.reserve(kSubs);
    for (int i = 0; i < kSubs; ++i) {
        threads.emplace_back([&p, &received]() {
            auto [sub, ec] = p->new_subscriber("concsub/topic", default_qos());
            REQUIRE_FALSE(ec);
            auto s = sub->channel()->recv_until(
                std::chrono::steady_clock::now() + 500ms);
            if (s) received++;
        });
    }

    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(pub->write({0x42}));

    for (auto& t : threads) t.join();
    CHECK(received.load() == kSubs);
}

// ── IMockParticipant interface hierarchy ──────────────────────────────────────

TEST_CASE("IMockParticipant implements all optional capability interfaces", "[mock][REQ-METRICS-003][REQ-HEALTH-001]") {
    auto p = make_p();

    // Verify static interface inheritance (no dynamic_cast needed).
    // Multiple-inheritance adjusts sub-object pointers, so we check non-null
    // and that each interface returns valid results rather than comparing addresses.
    relay::IMetricsProvider* mp = p.get();
    relay::IHealthProvider*  hp = p.get();
    relay::IDrainer*         dr = p.get();

    CHECK(mp != nullptr);
    CHECK(hp != nullptr);
    CHECK(dr != nullptr);

    // Verify each interface is functional via the base pointer
    auto m = mp->metrics();
    CHECK(m.write_count == 0);

    auto h = hp->health();
    CHECK(h.status == relay::HealthStatus::OK);

    // cast back to IMockParticipant to confirm they share the same object
    CHECK(dynamic_cast<dds::mock::IMockParticipant*>(mp) != nullptr);
    CHECK(dynamic_cast<dds::mock::IMockParticipant*>(hp) != nullptr);
    CHECK(dynamic_cast<dds::mock::IMockParticipant*>(dr) != nullptr);
}
