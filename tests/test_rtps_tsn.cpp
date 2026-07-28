// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// End-to-end tests for dds::rtps::Participant's TSN wiring (Tier 3, "tsn"
// — ParticipantOptions::tsn_config, see include/dds/rtps/participant.hpp's
// doc comment on that field and include/dds/rtps/tsn.hpp /
// include/dds/tsn/tsn.hpp). These are black-box tests through the public
// dds::rtps API, mirroring test_rtps_participant.cpp's own
// "Two Participants exchange a best-effort sample over real UDP once
// SEDP-matched" pattern — Participant::resolve_tsn/tsn_socket_for_pcp are
// private, so the only way to prove a writer is actually sending on its
// dedicated priority-marked socket (not just that the socket gets created)
// is to prove samples still arrive correctly end-to-end when that path is
// exercised. Socket-option-level behavior (SO_PRIORITY/IP_TOS/SO_TXTIME
// actually changing kernel behavior) is already covered by
// test_rtps_traffic.cpp for the underlying dds::rtps::traffic.hpp hooks
// this wiring calls into; this file's job is the wiring itself: TSN stream
// config resolution, the QoS::transport_priority fallback, and TSN
// stream MaxFragPayload driving fragmentation.
//
// tx_offset (SO_TXTIME scheduling) is deliberately not exercised with a
// nonzero value here: SO_TXTIME requires a real ETF/taprio qdisc on the
// egress interface to actually honor a launch time, which sandboxed
// CI/loopback environments do not have — traffic_linux.cpp's
// scheduled_send already covers the mechanics (see test_rtps_traffic.cpp);
// this file only needs to prove the (tx_offset == 0) common path — a
// dedicated TSN socket used for a plain, unscheduled send — behaves
// identically to the shared data_sock_ path from the caller's perspective.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <thread>

#include <dds/rtps/participant.hpp>
#include <dds/tsn/tsn.hpp>

using namespace dds;
using namespace dds::rtps;
using namespace std::chrono_literals;

namespace {

Locator make_udp4_locator(const std::array<uint8_t, 4>& quad, uint16_t port) {
    Locator l;
    l.kind = Locator::kKindUDPv4;
    l.port  = port;
    std::copy(quad.begin(), quad.end(), l.address.begin() + 12);
    return l;
}

Locator loopback_locator(uint16_t port) { return make_udp4_locator({127, 0, 0, 1}, port); }

// Feeds each participant's SedpService a proxy for the other, standing in
// for SPDP having already converged — matches
// test_rtps_participant.cpp's own cross-participant test setup.
void cross_wire(Participant& pa, Participant& pb) {
    ParticipantProxy proxy_b;
    proxy_b.guid                = GUID{pb.guid_prefix(), kEntityIdParticipant};
    proxy_b.metatraffic_unicast = loopback_locator(static_cast<uint16_t>(pb.meta_unicast_port()));
    proxy_b.default_unicast     = loopback_locator(static_cast<uint16_t>(pb.data_unicast_port()));
    pa.sedp().on_new_peer(proxy_b);

    ParticipantProxy proxy_a;
    proxy_a.guid                = GUID{pa.guid_prefix(), kEntityIdParticipant};
    proxy_a.metatraffic_unicast = loopback_locator(static_cast<uint16_t>(pa.meta_unicast_port()));
    proxy_a.default_unicast     = loopback_locator(static_cast<uint16_t>(pa.data_unicast_port()));
    pb.sedp().on_new_peer(proxy_a);
}

} // namespace

// fusa:test REQ-TSN-002 REQ-TSN-003 REQ-TSN-004 REQ-TSN-005

TEST_CASE("A TSN stream config match delivers a sample over the dedicated priority-marked socket",
          "[rtps][tsn]") {
    dds::tsn::StreamConfig cfg;
    dds::tsn::Stream        s;
    s.topic          = "TsnTopic";
    s.pcp             = 5;
    s.dscp            = 46;
    s.max_frame_size  = 1500;
    s.interval_us     = 1000;
    s.tx_offset_us    = 0; // see file-level scope note
    cfg.streams.push_back(s);

    ParticipantOptions opts_b;
    opts_b.test_mode   = true;
    opts_b.tsn_config = dds::tsn::with_stream_config(std::make_shared<dds::tsn::StreamConfig>(cfg));

    ParticipantOptions opts_a;
    opts_a.test_mode = true;

    auto [pa, eca] = Participant::create(0, opts_a);
    auto [pb, ecb] = Participant::create(0, opts_b);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);

    auto [sub, sub_ec] = pa->new_subscriber("TsnTopic", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = pb->new_publisher("TsnTopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    cross_wire(*pa, *pb);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("TsnTopic").empty();
    }
    REQUIRE(matched);

    std::vector<uint8_t> payload{0x10, 0x20, 0x30};
    CHECK_FALSE(pub->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "TsnTopic");
    CHECK(sample->payload == payload);

    pa->close();
    pb->close();
}

TEST_CASE("QoS::transport_priority is a fallback PCP selector when no stream config matches",
          "[rtps][tsn]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [pa, eca] = Participant::create(0, opts);
    auto [pb, ecb] = Participant::create(0, opts);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);

    auto [sub, sub_ec] = pa->new_subscriber("PriorityFallbackTopic", default_qos());
    REQUIRE_FALSE(sub_ec);

    QoS qos          = default_qos();
    qos.transport_priority = 3;
    auto [pub, pub_ec] = pb->new_publisher("PriorityFallbackTopic", qos);
    REQUIRE_FALSE(pub_ec);

    cross_wire(*pa, *pb);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("PriorityFallbackTopic").empty();
    }
    REQUIRE(matched);

    std::vector<uint8_t> payload{0xAA, 0xBB};
    CHECK_FALSE(pub->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample.has_value());
    CHECK(sample->payload == payload);

    pa->close();
    pb->close();
}

TEST_CASE("A TSN stream's small MaxFragPayload forces DATA_FRAG fragmentation and still "
          "round-trips correctly",
          "[rtps][tsn]") {
    dds::tsn::StreamConfig cfg;
    dds::tsn::Stream        s;
    s.topic         = "TsnFragTopic";
    s.pcp            = 2;
    // max_frag_payload() = max_frame_size - 48 = 52 bytes — far below both
    // the payload below and fragment.hpp's default kMaxFragmentPayload
    // (1200), so this write only fragments because fragment_size() honors
    // the TSN stream's bound.
    s.max_frame_size = 100;
    cfg.streams.push_back(s);

    ParticipantOptions opts_b;
    opts_b.test_mode   = true;
    opts_b.tsn_config = dds::tsn::with_stream_config(std::make_shared<dds::tsn::StreamConfig>(cfg));

    ParticipantOptions opts_a;
    opts_a.test_mode = true;

    auto [pa, eca] = Participant::create(0, opts_a);
    auto [pb, ecb] = Participant::create(0, opts_b);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);

    auto [sub, sub_ec] = pa->new_subscriber("TsnFragTopic", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = pb->new_publisher("TsnFragTopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    cross_wire(*pa, *pb);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("TsnFragTopic").empty();
    }
    REQUIRE(matched);

    std::vector<uint8_t> payload(200, 0x5A);
    CHECK_FALSE(pub->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "TsnFragTopic");
    CHECK(sample->payload == payload);

    pa->close();
    pb->close();
}

TEST_CASE("Multiple writers resolving to the same PCP share one dedicated TSN socket",
          "[rtps][tsn]") {
    // Not directly observable through the public API (tsn_socket_for_pcp
    // is private — see this file's own header comment), but this proves
    // the sharing doesn't corrupt delivery when two different topics both
    // resolve to PCP 4 on the same participant.
    dds::tsn::StreamConfig cfg;
    dds::tsn::Stream        s1;
    s1.topic = "TsnShareA";
    s1.pcp    = 4;
    cfg.streams.push_back(s1);
    dds::tsn::Stream s2;
    s2.topic = "TsnShareB";
    s2.pcp    = 4;
    cfg.streams.push_back(s2);

    ParticipantOptions opts_b;
    opts_b.test_mode   = true;
    opts_b.tsn_config = dds::tsn::with_stream_config(std::make_shared<dds::tsn::StreamConfig>(cfg));

    ParticipantOptions opts_a;
    opts_a.test_mode = true;

    auto [pa, eca] = Participant::create(0, opts_a);
    auto [pb, ecb] = Participant::create(0, opts_b);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);

    auto [sub_a, sub_a_ec] = pa->new_subscriber("TsnShareA", default_qos());
    REQUIRE_FALSE(sub_a_ec);
    auto [sub_b, sub_b_ec] = pa->new_subscriber("TsnShareB", default_qos());
    REQUIRE_FALSE(sub_b_ec);
    auto [pub_a, pub_a_ec] = pb->new_publisher("TsnShareA", default_qos());
    REQUIRE_FALSE(pub_a_ec);
    auto [pub_b, pub_b_ec] = pb->new_publisher("TsnShareB", default_qos());
    REQUIRE_FALSE(pub_b_ec);

    cross_wire(*pa, *pb);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("TsnShareA").empty() &&
                  !pb->sedp().matched_reader_locators_for_topic("TsnShareB").empty();
    }
    REQUIRE(matched);

    std::vector<uint8_t> payload_a{0x01};
    std::vector<uint8_t> payload_b{0x02};
    CHECK_FALSE(pub_a->write(payload_a));
    CHECK_FALSE(pub_b->write(payload_b));

    auto sample_a = sub_a->channel()->recv_until(std::chrono::steady_clock::now() + 3s);
    auto sample_b = sub_b->channel()->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample_a.has_value());
    REQUIRE(sample_b.has_value());
    CHECK(sample_a->payload == payload_a);
    CHECK(sample_b->payload == payload_b);

    pa->close();
    pb->close();
}
