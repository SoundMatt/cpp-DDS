// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Behavioral tests for dds::mock::new_loaning_publisher / MockLoaningPublisher
// (the "Also within ddscore but not RTPS-specific" ROADMAP.md item —
// see include/dds/mock/loan.hpp's file-level scope note).
//
// This mirrors tests/test_rtps_loan.cpp's own coverage exactly (same
// scope note applies: no new wire encoding here either — write_loaned
// ultimately calls the same MockPublisher::write already exercised by
// test_mock.cpp), minus the cross-participant real-transport end-to-end
// case, which has no mock-side equivalent (the mock transport is
// same-process by construction — there is no second transport leg to
// prove the loan path is wired into). Covers: loan/commit
// round-tripping through same-process broker dispatch, pool exhaustion,
// closed-publisher rejection, discard-without-publish, direct write()
// passthrough, and the two go-DDS-mirrored error paths (wrong
// participant type, publisher-creation failure) from mock/loan.go's
// NewLoaningPublisher.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <dds/mock/loan.hpp>
#include <dds/mock/participant.hpp>
#include <dds/rtps/participant.hpp>

using namespace dds;
using namespace std::chrono_literals;

TEST_CASE("mock: Loan/write_loaned round-trips a sample through same-process delivery",
          "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/roundtrip", default_qos());
    REQUIRE_FALSE(sub_ec);

    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/roundtrip", default_qos(), 256);
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

TEST_CASE("mock: loan_buffer rejects a size exceeding the pool's configured capacity",
          "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/toobig", default_qos(), 64);
    REQUIRE_FALSE(lp_ec);

    auto [buf, loan_ec] = lp->loan_buffer(1024);
    CHECK_FALSE(buf);
    CHECK(loan_ec == ErrLoanBuffer());

    p->close();
}

TEST_CASE("mock: loan_buffer rejects once the loaning publisher is closed", "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/closed", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);
    CHECK_FALSE(lp->close());

    auto [buf, loan_ec] = lp->loan_buffer(10);
    CHECK_FALSE(buf);
    CHECK(loan_ec == ErrClosed());

    p->close();
}

TEST_CASE("mock: return_loan discards a loaned buffer without publishing it", "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/discard", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/discard", default_qos(), 256);
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

TEST_CASE("mock: write() still works directly on a LoaningPublisher (IPublisher passthrough)",
          "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("loan/directwrite", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/directwrite", default_qos(), 256);
    REQUIRE_FALSE(lp_ec);

    const std::vector<uint8_t> payload{1, 2, 3, 4};
    CHECK_FALSE(lp->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(sample.has_value());
    CHECK(sample->payload == payload);

    p->close();
}

TEST_CASE("mock: new_loaning_publisher rejects a non-mock participant", "[mock][loan]") {
    dds::rtps::ParticipantOptions opts;
    opts.test_mode = true;
    auto [rp, rp_ec] = dds::rtps::Participant::create(0, opts);
    REQUIRE_FALSE(rp_ec);

    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(rp, "loan/wrongtype", default_qos(), 256);
    CHECK_FALSE(lp);
    CHECK(lp_ec == ErrLoanBuffer());

    rp->close();
}

TEST_CASE("mock: new_loaning_publisher propagates new_publisher's error on a closed participant",
          "[mock][loan]") {
    auto [p, ec] = dds::mock::create(0);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(p->close());

    auto [lp, lp_ec] = dds::mock::new_loaning_publisher(p, "loan/afterclose", default_qos(), 256);
    CHECK_FALSE(lp);
    CHECK(lp_ec == ErrClosed());
}
