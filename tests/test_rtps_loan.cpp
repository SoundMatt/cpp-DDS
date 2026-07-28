// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Behavioral tests for dds::rtps::new_loaning_publisher / LoaningWriter
// (Tier-1 phase 9, "Loan integration" — see ROADMAP.md and
// include/dds/rtps/loan.hpp's file-level scope note).
//
// This phase introduces no new wire encoding: write_loaned ultimately calls
// the same Writer::write already byte-verified by phases 1-2/4-8's own test
// files (composing DataSubmessage::encode, cdr_wrap_payload,
// wrap_in_rtps_message, and — over threshold — DataFrag::encode), so there
// are no new golden vectors to pin here. These tests instead exercise the
// phase-9-specific glue: loan/commit round-tripping through same-process
// dispatch AND real loopback UDP delivery once SEDP-matched (proving the
// loan path is genuinely wired into the same RTPS writer path a plain
// Writer::write uses, not a parallel one), pool exhaustion, closed-writer
// rejection, discard-without-publish, and the two go-DDS-mirrored error
// paths (wrong participant type, publisher-creation failure) from
// rtps/loan.go's NewLoaningPublisher.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include <dds/mock/participant.hpp>
#include <dds/rtps/loan.hpp>
#include <dds/rtps/participant.hpp>

using namespace dds;
using namespace dds::rtps;
using namespace std::chrono_literals;

namespace {

Locator loopback_locator(uint16_t port) {
    Locator l;
    l.kind = Locator::kKindUDPv4;
    l.port  = port;
    l.address[12] = 127;
    l.address[15] = 1;
    return l;
}

} // namespace

TEST_CASE("Loan/write_loaned round-trips a sample through same-process delivery", "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/roundtrip", default_qos());
    REQUIRE_FALSE(sub_ec);

    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/roundtrip", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);
    REQUIRE(lp);

    auto [buf, loan_ec] = lp->loan_buffer(10);
    REQUIRE_FALSE(loan_ec);
    REQUIRE(buf);
    const std::vector<uint8_t> payload{'h', 'e', 'l', 'l', 'o', 'w', 'o', 'r', 'l', 'd'};
    std::copy(payload.begin(), payload.end(), buf->begin());

    CHECK_FALSE(lp->write_loaned(buf));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "loan/roundtrip");
    CHECK(sample->payload == payload);

    p->close();
}

TEST_CASE("loan_buffer rejects a size exceeding the pool's configured capacity",
          "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/toobig", default_qos(), 64);
    REQUIRE_FALSE(lp_ec);

    auto [buf, loan_ec] = lp->loan_buffer(1024);
    CHECK_FALSE(buf);
    CHECK(loan_ec == ErrLoanBuffer());

    p->close();
}

TEST_CASE("loan_buffer rejects once the loaning publisher is closed", "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/closed", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);
    CHECK_FALSE(lp->close());

    auto [buf, loan_ec] = lp->loan_buffer(10);
    CHECK_FALSE(buf);
    CHECK(loan_ec == ErrClosed());

    p->close();
}

TEST_CASE("return_loan discards a loaned buffer without publishing it", "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/discard", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/discard", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);

    auto [buf, loan_ec] = lp->loan_buffer(4);
    REQUIRE_FALSE(loan_ec);
    lp->return_loan(buf);

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 200ms);
    CHECK_FALSE(sample.has_value()); // nothing published

    // The pool must still be usable after a discard.
    auto [buf2, loan_ec2] = lp->loan_buffer(4);
    CHECK_FALSE(loan_ec2);
    REQUIRE(buf2);
    lp->return_loan(buf2);

    p->close();
}

TEST_CASE("write() still works directly on a LoaningPublisher (IPublisher passthrough)",
          "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/directwrite", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/directwrite", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);

    const std::vector<uint8_t> payload{1, 2, 3, 4};
    CHECK_FALSE(lp->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(sample.has_value());
    CHECK(sample->payload == payload);

    p->close();
}

TEST_CASE("new_loaning_publisher rejects a non-rtps participant", "[rtps][loan]") {
    auto [mp, mp_ec] = dds::mock::create(0);
    REQUIRE_FALSE(mp_ec);

    auto [lp, lp_ec] = new_loaning_publisher(mp, "loan/wrongtype", default_qos(), 256);
    CHECK_FALSE(lp);
    CHECK(lp_ec == ErrLoanBuffer());

    mp->close();
}

TEST_CASE("new_loaning_publisher propagates new_publisher's error on a closed participant",
          "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(p->close());

    auto [lp, lp_ec] = new_loaning_publisher(p, "loan/afterclose", default_qos(), 256);
    CHECK_FALSE(lp);
    CHECK(lp_ec == ErrClosed());
}

TEST_CASE("Loaned samples are delivered over real loopback UDP once SEDP-matched",
          "[rtps][loan]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [pa, eca] = Participant::create(0, opts);
    auto [pb, ecb] = Participant::create(0, opts);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);

    auto [sub, sub_ec] = pa->new_subscriber("loan/e2e", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [lp, lp_ec] = new_loaning_publisher(pb, "loan/e2e", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);

    // Stand in for SPDP convergence, exactly matching
    // test_rtps_participant.cpp's own cross-participant test.
    ParticipantProxy proxy_b;
    proxy_b.guid                = GUID{pb->guid_prefix(), kEntityIdParticipant};
    proxy_b.metatraffic_unicast = loopback_locator(static_cast<uint16_t>(pb->meta_unicast_port()));
    proxy_b.default_unicast     = loopback_locator(static_cast<uint16_t>(pb->data_unicast_port()));
    pa->sedp().on_new_peer(proxy_b);

    ParticipantProxy proxy_a;
    proxy_a.guid                = GUID{pa->guid_prefix(), kEntityIdParticipant};
    proxy_a.metatraffic_unicast = loopback_locator(static_cast<uint16_t>(pa->meta_unicast_port()));
    proxy_a.default_unicast     = loopback_locator(static_cast<uint16_t>(pa->data_unicast_port()));
    pb->sedp().on_new_peer(proxy_a);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("loan/e2e").empty();
    }
    REQUIRE(matched);

    auto [buf, loan_ec] = lp->loan_buffer(4);
    REQUIRE_FALSE(loan_ec);
    (*buf)[0] = 0xDE;
    (*buf)[1] = 0xAD;
    (*buf)[2] = 0xBE;
    (*buf)[3] = 0xEF;
    CHECK_FALSE(lp->write_loaned(buf));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "loan/e2e");
    CHECK(sample->payload == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    pa->close();
    pb->close();
}
