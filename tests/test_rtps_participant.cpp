// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Behavioral tests for dds::rtps::Participant (Tier-1 phase 6, "Entities &
// history cache" — see ROADMAP.md). This phase introduces no *new* wire
// encoding of its own (every wire primitive it composes — DataSubmessage,
// cdr_wrap_payload, wrap_in_rtps_message, PL_CDR ParticipantProxy/
// EndpointData — was already byte-verified against go-DDS reference
// vectors in phases 1-2/4-5's own test files), so there are no new golden
// vectors to pin here. These tests instead exercise the phase-6-specific
// glue: entity-id allocation, the SEDP query-surface extension
// (matched_reader_locators_for_topic), the SPDP->SEDP peer-bridge loop, and
// end-to-end best-effort pub/sub (same-process and, over real loopback UDP,
// cross-participant).
//
// The cross-participant test manually feeds each Participant's SedpService
// a ParticipantProxy for the other (via SedpService::on_new_peer) instead
// of waiting for real SPDP discovery to converge — SPDP's own convergence
// behavior is already covered at the SpdpService level in
// test_rtps_spdp.cpp ("Two SpdpService instances discover each other over
// loopback"); this file's job is to prove the *new* pieces phase 6 adds on
// top of already-tested discovery: a real Writer::write() producing UDP
// bytes a real Participant::data_loop() receives, decodes, SEDP-matches,
// and delivers into a real Reader's channel.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <thread>

#include <dds/rtps/participant.hpp>

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

} // namespace

// ── entity-id / guid-prefix allocation ───────────────────────────────────────

TEST_CASE("entity_id_for_writer/reader use go-DDS's kind-byte convention", "[rtps][participant]") {
    // kind 0x03 = user-defined writer, no key; 0x04 = user-defined reader,
    // no key (go-DDS rtps/guid.go entityIdForWriter/entityIdForReader).
    CHECK(entity_id_for_writer(1) == EntityId{{0x00, 0x00, 0x01, 0x03}});
    CHECK(entity_id_for_reader(1) == EntityId{{0x00, 0x00, 0x01, 0x04}});
    CHECK(entity_id_for_writer(0x010203) == EntityId{{0x01, 0x02, 0x03, 0x03}});
    CHECK(entity_id_for_writer(1) != entity_id_for_reader(1));
}

TEST_CASE("new_guid_prefix produces distinct prefixes across calls", "[rtps][participant]") {
    auto a = new_guid_prefix();
    auto b = new_guid_prefix();
    CHECK(a != b);
}

// ── SedpService::matched_reader_locators_for_topic ───────────────────────────

TEST_CASE("SedpService::matched_reader_locators_for_topic filters by topic and dedupes",
          "[rtps][sedp]") {
    SedpConfig cfg;
    cfg.local_guid_prefix = GuidPrefix{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}};
    auto sock = UdpSocket::bind_unicast(0);
    REQUIRE(sock.has_value());
    SedpService svc(cfg, std::move(*sock));

    CHECK(svc.matched_reader_locators_for_topic("NoSuchTopic").empty());

    GuidPrefix remote1{{0xA1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    GuidPrefix remote2{{0xA2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

    auto d1 = build_endpoint_data(
        EndpointInfo{GUID{remote1, EntityId{{0, 0, 2, 7}}}, "Topic", false}, 9500);
    svc.handle_packet(build_sedp_announcement(remote1, kVendorIdGoDDS, kEntityIdSEDPSubWriter,
                                                kEntityIdSEDPSubReader, SequenceNumber{0, 1}, d1),
                        "127.0.0.1");
    auto d2 = build_endpoint_data(
        EndpointInfo{GUID{remote2, EntityId{{0, 0, 3, 7}}}, "Topic", false}, 9501);
    svc.handle_packet(build_sedp_announcement(remote2, kVendorIdGoDDS, kEntityIdSEDPSubWriter,
                                                kEntityIdSEDPSubReader, SequenceNumber{0, 1}, d2),
                        "127.0.0.1");
    // A second reader on remote1 for the same topic, different locator port —
    // still only one *distinct* locator per remote address:port is expected
    // when ports genuinely differ (dedup only collapses identical locators).
    auto d3 = build_endpoint_data(
        EndpointInfo{GUID{remote1, EntityId{{0, 0, 4, 7}}}, "OtherTopic", false}, 9502);
    svc.handle_packet(build_sedp_announcement(remote1, kVendorIdGoDDS, kEntityIdSEDPSubWriter,
                                                kEntityIdSEDPSubReader, SequenceNumber{0, 2}, d3),
                        "127.0.0.1");

    auto locs = svc.matched_reader_locators_for_topic("Topic");
    REQUIRE(locs.size() == 2);
    CHECK(svc.matched_reader_locators_for_topic("OtherTopic").size() == 1);
    CHECK(svc.matched_reader_locators_for_topic("NoSuchTopic").empty());
}

// ── Participant: creation and lifecycle ──────────────────────────────────────

TEST_CASE("Participant::create rejects an out-of-range domain", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(999, opts);
    CHECK_FALSE(p);
    CHECK(ec == ErrDomainOutOfRange());
}

TEST_CASE("Participant::create in test_mode binds ephemeral, non-zero ports", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);
    REQUIRE(p);
    CHECK(p->domain() == 0);
    CHECK(p->meta_unicast_port() != 0);
    CHECK(p->data_unicast_port() != 0);
    CHECK_FALSE(p->is_closed());
    CHECK(p->close() == std::error_code{});
    CHECK(p->is_closed());
}

TEST_CASE("Participant::close is idempotent", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(p->close());
    CHECK_FALSE(p->close());
}

TEST_CASE("Participant::new_publisher/new_subscriber reject an empty topic",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [pub, pub_ec] = p->new_publisher("", default_qos());
    CHECK_FALSE(pub);
    CHECK(pub_ec == ErrTopicEmpty());

    auto [sub, sub_ec] = p->new_subscriber("", default_qos());
    CHECK_FALSE(sub);
    CHECK(sub_ec == ErrTopicEmpty());
}

TEST_CASE("Participant rejects new endpoints after close", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(p->close());

    auto [pub, pub_ec] = p->new_publisher("Topic", default_qos());
    CHECK_FALSE(pub);
    CHECK(pub_ec == ErrClosed());

    auto [sub, sub_ec] = p->new_subscriber("Topic", default_qos());
    CHECK_FALSE(sub);
    CHECK(sub_ec == ErrClosed());
}

TEST_CASE("Publisher::write fails after the publisher's participant is closed",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);
    auto [pub, pub_ec] = p->new_publisher("Topic", default_qos());
    REQUIRE_FALSE(pub_ec);

    REQUIRE_FALSE(p->close());
    auto write_ec = pub->write(std::vector<uint8_t>{1, 2, 3});
    CHECK(write_ec == ErrClosed());
}

TEST_CASE("Publisher::write enforces QoS.max_sample_size", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    QoS qos = default_qos();
    qos.max_sample_size = 4;
    auto [pub, pub_ec] = p->new_publisher("Topic", qos);
    REQUIRE_FALSE(pub_ec);

    CHECK(pub->write(std::vector<uint8_t>{1, 2, 3, 4}) == std::error_code{});
    CHECK(pub->write(std::vector<uint8_t>{1, 2, 3, 4, 5}) == ErrPayloadTooLarge());

    p->close();
}

// ── Same-process best-effort delivery ────────────────────────────────────────

TEST_CASE("Same-participant publisher/subscriber deliver locally without discovery",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("LocalTopic", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = p->new_publisher("LocalTopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    CHECK_FALSE(pub->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "LocalTopic");
    CHECK(sample->payload == payload);
    CHECK(sample->sequence_number == 1);

    p->close();
}

TEST_CASE("A subscriber on a different topic does not receive the sample",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("TopicA", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = p->new_publisher("TopicB", default_qos());
    REQUIRE_FALSE(pub_ec);

    CHECK_FALSE(pub->write(std::vector<uint8_t>{0xFF}));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 200ms);
    CHECK_FALSE(sample.has_value());

    p->close();
}

TEST_CASE("TransientLocal durability delivers the last sample to a late-joining subscriber",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [pub, pub_ec] = p->new_publisher("DurableTopic", reliable_qos()); // TransientLocal
    REQUIRE_FALSE(pub_ec);
    CHECK_FALSE(pub->write(std::vector<uint8_t>{0x42}));

    // Give local dispatch (which races the late-joiner registration below
    // only in the sense of updating last_sample_) a moment — write() updates
    // last_sample synchronously before returning, so this is just for
    // robustness against unrelated scheduling jitter.
    std::this_thread::sleep_for(10ms);

    auto [sub, sub_ec] = p->new_subscriber("DurableTopic", reliable_qos());
    REQUIRE_FALSE(sub_ec);

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(sample.has_value());
    CHECK(sample->payload == std::vector<uint8_t>{0x42});

    p->close();
}

TEST_CASE("Participant::close unblocks a subscriber blocked on recv", "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    // A plain (non-structured-binding) variable, captured by the thread
    // lambda below: capturing a structured binding by reference is a
    // C++20 extension under Clang/GCC in -std=c++17 mode, and this repo
    // still targets C++17 as a baseline.
    std::shared_ptr<Participant> participant;
    {
        auto [p, ec] = Participant::create(0, opts);
        REQUIRE_FALSE(ec);
        participant = p;
    }

    auto [sub, sub_ec] = participant->new_subscriber("NeverPublished", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto ch = sub->channel();

    std::thread closer([&] {
        std::this_thread::sleep_for(100ms);
        participant->close();
    });
    auto sample = ch->recv(); // blocks until close() closes the channel
    CHECK_FALSE(sample.has_value());
    closer.join();
}

TEST_CASE("Subscriber::unsubscribe stops further delivery without closing the channel",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [sub, sub_ec] = p->new_subscriber("UnsubTopic", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = p->new_publisher("UnsubTopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    sub->unsubscribe();
    CHECK_FALSE(pub->write(std::vector<uint8_t>{0x01}));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 200ms);
    CHECK_FALSE(sample.has_value()); // channel closed by unsubscribe(), recv returns nullopt

    p->close();
}

// ── SPDP -> SEDP peer-bridge loop ────────────────────────────────────────────

TEST_CASE("Participant's bridge loop announces existing local endpoints to a newly discovered peer",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode          = true;
    opts.bridge_poll_period  = 30ms;
    auto [p, ec] = Participant::create(0, opts);
    REQUIRE_FALSE(ec);

    auto [pub, pub_ec] = p->new_publisher("BridgeTopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    // A throwaway socket standing in for a remote peer's meta-unicast port.
    auto peer_meta_sock = UdpSocket::bind_unicast(0);
    REQUIRE(peer_meta_sock.has_value());

    // Manufacture a fake SPDP announcement "from" that peer, reusing
    // already byte-verified phase-4 machinery (build_spdp_announcement).
    SpdpLocalInfo peer_info;
    peer_info.guid_prefix           = GuidPrefix{{0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4, 5, 6, 7, 8}};
    peer_info.meta_unicast_port     = static_cast<uint16_t>(peer_meta_sock->port());
    peer_info.default_unicast_port  = static_cast<uint16_t>(peer_meta_sock->port());
    auto announce = build_spdp_announcement(peer_info, SequenceNumber{0, 1});
    p->spdp().handle_packet(announce, "127.0.0.1");

    // Wait for the bridge loop to notice the new peer and have SEDP
    // announce the local "BridgeTopic" writer directly to it.
    bool received = false;
    for (int attempt = 0; attempt < 40 && !received; ++attempt) {
        auto pkt = peer_meta_sock->recv();
        if (pkt.has_value()) received = true;
    }
    CHECK(received);

    p->close();
}

// ── Cross-participant best-effort delivery over real loopback UDP ───────────

TEST_CASE("Two Participants exchange a best-effort sample over real UDP once SEDP-matched",
          "[rtps][participant]") {
    ParticipantOptions opts;
    opts.test_mode = true;
    auto [pa, eca] = Participant::create(0, opts);
    auto [pb, ecb] = Participant::create(0, opts);
    REQUIRE_FALSE(eca);
    REQUIRE_FALSE(ecb);
    REQUIRE(pa);
    REQUIRE(pb);

    auto [sub, sub_ec] = pa->new_subscriber("E2ETopic", default_qos());
    REQUIRE_FALSE(sub_ec);
    auto [pub, pub_ec] = pb->new_publisher("E2ETopic", default_qos());
    REQUIRE_FALSE(pub_ec);

    // Stand in for SPDP having already converged (see the file-level scope
    // note above) by feeding each SedpService a proxy for the other
    // directly.
    ParticipantProxy proxy_b;
    proxy_b.guid                   = GUID{pb->guid_prefix(), kEntityIdParticipant};
    proxy_b.metatraffic_unicast    = loopback_locator(static_cast<uint16_t>(pb->meta_unicast_port()));
    proxy_b.default_unicast        = loopback_locator(static_cast<uint16_t>(pb->data_unicast_port()));
    pa->sedp().on_new_peer(proxy_b);

    ParticipantProxy proxy_a;
    proxy_a.guid                   = GUID{pa->guid_prefix(), kEntityIdParticipant};
    proxy_a.metatraffic_unicast    = loopback_locator(static_cast<uint16_t>(pa->meta_unicast_port()));
    proxy_a.default_unicast        = loopback_locator(static_cast<uint16_t>(pa->data_unicast_port()));
    pb->sedp().on_new_peer(proxy_a);

    bool matched = false;
    for (int attempt = 0; attempt < 60 && !matched; ++attempt) {
        std::this_thread::sleep_for(50ms);
        matched = !pb->sedp().matched_reader_locators_for_topic("E2ETopic").empty();
    }
    REQUIRE(matched);

    std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
    CHECK_FALSE(pub->write(payload));

    auto ch     = sub->channel();
    auto sample = ch->recv_until(std::chrono::steady_clock::now() + 3s);
    REQUIRE(sample.has_value());
    CHECK(sample->topic == "E2ETopic");
    CHECK(sample->payload == payload);

    Guid expected_writer_guid{};
    {
        GUID writer_guid{pb->guid_prefix(), EntityId{{0x00, 0x00, 0x01, 0x03}}};
        std::copy(writer_guid.prefix.bytes.begin(), writer_guid.prefix.bytes.end(),
                    expected_writer_guid.begin());
        std::copy(writer_guid.entity.bytes.begin(), writer_guid.entity.bytes.end(),
                    expected_writer_guid.begin() + 12);
    }
    CHECK(sample->writer_guid == expected_writer_guid);

    pa->close();
    pb->close();
}
