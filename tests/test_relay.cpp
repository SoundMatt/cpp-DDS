// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-RELAY-001 REQ-RELAY-002 REQ-RELAY-003 REQ-RELAY-004 REQ-RELAY-005
// fusa:test REQ-RELAY-006 REQ-RELAY-007 REQ-RELAY-008 REQ-RELAY-009 REQ-RELAY-010
// fusa:test REQ-RELAY-011 REQ-RELAY-012 REQ-RELAY-013 REQ-RELAY-014 REQ-RELAY-015
// fusa:test REQ-RELAY-016 REQ-RELAY-017 REQ-RELAY-018 REQ-RELAY-019 REQ-RELAY-020
// fusa:test REQ-RELAY-021 REQ-RELAY-022 REQ-RELAY-023 REQ-RELAY-024 REQ-RELAY-025
// fusa:test REQ-RELAY-026 REQ-RELAY-027 REQ-RELAY-028 REQ-RELAY-029
// fusa:test REQ-RELAY-051 REQ-RELAY-056 REQ-RELAY-059 REQ-RELAY-060

#include <dds/relay.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <type_traits>

using namespace relay;

// ── Protocol enum ─────────────────────────────────────────────────────────────

TEST_CASE("Protocol integer type with CAN=1 through SOMEIP=6", "[relay][REQ-RELAY-001][REQ-RELAY-002]") {
    CHECK(static_cast<int>(Protocol::CAN)    == 1);
    CHECK(static_cast<int>(Protocol::DDS)    == 2);
    CHECK(static_cast<int>(Protocol::LIN)    == 3);
    CHECK(static_cast<int>(Protocol::MQTT)   == 4);
    CHECK(static_cast<int>(Protocol::RCP)    == 5);
    CHECK(static_cast<int>(Protocol::SOMEIP) == 6);
}

TEST_CASE("to_string: all protocols", "[relay][REQ-RELAY-003]") {
    CHECK(to_string(Protocol::CAN)    == "CAN");
    CHECK(to_string(Protocol::DDS)    == "DDS");
    CHECK(to_string(Protocol::LIN)    == "LIN");
    CHECK(to_string(Protocol::MQTT)   == "MQTT");
    CHECK(to_string(Protocol::RCP)    == "RCP");
    CHECK(to_string(Protocol::SOMEIP) == "SOMEIP");
    CHECK(to_string(static_cast<Protocol>(99)) == "unknown");
}

TEST_CASE("try_parse_protocol: all protocols round-trip", "[relay][REQ-RELAY-059]") {
    Protocol p{};
    CHECK(try_parse_protocol("CAN",    p)); CHECK(p == Protocol::CAN);
    CHECK(try_parse_protocol("DDS",    p)); CHECK(p == Protocol::DDS);
    CHECK(try_parse_protocol("LIN",    p)); CHECK(p == Protocol::LIN);
    CHECK(try_parse_protocol("MQTT",   p)); CHECK(p == Protocol::MQTT);
    CHECK(try_parse_protocol("RCP",    p)); CHECK(p == Protocol::RCP);
    CHECK(try_parse_protocol("SOMEIP", p)); CHECK(p == Protocol::SOMEIP);
    CHECK(try_parse_protocol("SOME/IP",p)); CHECK(p == Protocol::SOMEIP);
}

TEST_CASE("try_parse_protocol: case-insensitive", "[relay][REQ-RELAY-059]") {
    Protocol p{};
    CHECK(try_parse_protocol("dds", p)); CHECK(p == Protocol::DDS);
    CHECK(try_parse_protocol("Dds", p)); CHECK(p == Protocol::DDS);
}

TEST_CASE("try_parse_protocol: unknown returns false", "[relay][REQ-RELAY-059]") {
    Protocol p{};
    CHECK_FALSE(try_parse_protocol("UNKNOWN", p));
    CHECK_FALSE(try_parse_protocol("",        p));
}

TEST_CASE("parse_protocol: throws on unknown", "[relay][REQ-RELAY-059]") {
    REQUIRE_THROWS_AS(parse_protocol("BAD"), std::invalid_argument);
}

// ── Version ───────────────────────────────────────────────────────────────────

TEST_CASE("Version::to_string", "[relay][REQ-RELAY-005]") {
    Version v{1, 2, 3};
    CHECK(v.to_string() == "1.2.3");
    CHECK((Version{0, 0, 0}).to_string() == "0.0.0");
}

TEST_CASE("Version equality", "[relay][REQ-RELAY-004]") {
    Version a{1, 2, 3}, b{1, 2, 3}, c{1, 2, 4};
    CHECK(a == b);
    CHECK_FALSE(a == c);
}

// ── Message ───────────────────────────────────────────────────────────────────

TEST_CASE("Message has required fields with correct defaults", "[relay][REQ-RELAY-006][REQ-RELAY-007]") {
    Message m;
    m.protocol = Protocol::DDS;
    m.id       = "vehicle/speed";
    m.payload  = {0x01, 0x02};
    CHECK(m.seq  == 0);
    CHECK(m.meta.empty());
    CHECK(m.payload == std::vector<uint8_t>{0x01, 0x02});
}

// ── Error codes ───────────────────────────────────────────────────────────────

TEST_CASE("error sentinels are distinct", "[relay][REQ-RELAY-008][REQ-RELAY-012]") {
    CHECK(ErrClosed()          != ErrNotConnected());
    CHECK(ErrClosed()          != ErrTimeout());
    CHECK(ErrClosed()          != ErrPayloadTooLarge());
    CHECK(ErrNotConnected()    != ErrTimeout());
    CHECK(ErrNotConnected()    != ErrPayloadTooLarge());
    CHECK(ErrTimeout()         != ErrPayloadTooLarge());
}

TEST_CASE("error codes compare equal to themselves", "[relay][REQ-RELAY-008]") {
    CHECK(ErrClosed()          == ErrClosed());
    CHECK(ErrNotConnected()    == ErrNotConnected());
    CHECK(ErrTimeout()         == ErrTimeout());
    CHECK(ErrPayloadTooLarge() == ErrPayloadTooLarge());
}

TEST_CASE("error code messages are non-empty", "[relay][REQ-RELAY-008]") {
    CHECK_FALSE(ErrClosed().message().empty());
    CHECK_FALSE(ErrNotConnected().message().empty());
    CHECK_FALSE(ErrTimeout().message().empty());
    CHECK_FALSE(ErrPayloadTooLarge().message().empty());
}

TEST_CASE("ok error_code is false", "[relay][REQ-RELAY-008]") {
    std::error_code ok{};
    CHECK_FALSE(ok);
    CHECK(ErrClosed());
}

// ── SubscriberConfig ──────────────────────────────────────────────────────────

TEST_CASE("SubscriberConfig defaults", "[relay][REQ-RELAY-016]") {
    SubscriberConfig cfg{};
    CHECK(cfg.chan_depth  == 0);
    CHECK(cfg.back_pressure == BackPressurePolicy::DropNewest);
    CHECK(cfg.event_id   == 0);
    CHECK(cfg.topic_name.empty());
}

TEST_CASE("with_channel_depth option", "[relay][REQ-RELAY-017]") {
    auto cfg = apply_options({with_channel_depth(128)});
    CHECK(cfg.chan_depth == 128);
}

TEST_CASE("effective_depth uses default when chan_depth is 0", "[relay][REQ-RELAY-019]") {
    SubscriberConfig cfg{};
    CHECK(cfg.effective_depth(64) == 64);
    cfg.chan_depth = 32;
    CHECK(cfg.effective_depth(64) == 32);
}

TEST_CASE("with_back_pressure option", "[relay][REQ-RELAY-015][REQ-RELAY-017]") {
    auto cfg = apply_options({with_back_pressure(BackPressurePolicy::Block)});
    CHECK(cfg.back_pressure == BackPressurePolicy::Block);
    CHECK(static_cast<int>(BackPressurePolicy::DropNewest) == 0);
    CHECK(static_cast<int>(BackPressurePolicy::DropOldest) == 1);
    CHECK(static_cast<int>(BackPressurePolicy::Block)      == 2);
}

TEST_CASE("apply_options applies all options", "[relay][REQ-RELAY-018]") {
    auto cfg = apply_options({with_channel_depth(32), with_back_pressure(BackPressurePolicy::DropOldest)});
    CHECK(cfg.chan_depth    == 32);
    CHECK(cfg.back_pressure == BackPressurePolicy::DropOldest);
}

TEST_CASE("with_event_id option", "[relay][REQ-RELAY-051]") {
    auto cfg = apply_options({with_event_id(42)});
    CHECK(cfg.event_id == 42);
}

TEST_CASE("with_topic option", "[relay][REQ-RELAY-056]") {
    auto cfg = apply_options({with_topic("vehicle/speed")});
    CHECK(cfg.topic_name == "vehicle/speed");
}

// ── kSpecVersion ──────────────────────────────────────────────────────────────

TEST_CASE("kSpecVersion is 1.7", "[relay][REQ-RELAY-020]") {
    CHECK(std::string(kSpecVersion) == "1.7");
}

// ── INode / ICaller abstract ──────────────────────────────────────────────────

TEST_CASE("INode is a polymorphic abstract type", "[relay][REQ-RELAY-013]") {
    CHECK(std::is_abstract<INode>::value);
    CHECK(std::is_polymorphic<INode>::value);
}

TEST_CASE("ICaller extends INode and is abstract", "[relay][REQ-RELAY-014]") {
    CHECK(std::is_abstract<ICaller>::value);
    CHECK((std::is_base_of<INode, ICaller>::value));
}

// ── Health / Metrics / Drainer ────────────────────────────────────────────────

TEST_CASE("HealthStatus enum values OK=0 Degraded=1 Down=2", "[relay][REQ-RELAY-023]") {
    CHECK(static_cast<int>(HealthStatus::OK)       == 0);
    CHECK(static_cast<int>(HealthStatus::Degraded) == 1);
    CHECK(static_cast<int>(HealthStatus::Down)     == 2);
}

TEST_CASE("Health struct has status and details fields", "[relay][REQ-RELAY-024]") {
    Health h{HealthStatus::Degraded, "low memory"};
    CHECK(h.status  == HealthStatus::Degraded);
    CHECK(h.details == "low memory");
}

TEST_CASE("IHealthProvider is a polymorphic abstract type", "[relay][REQ-RELAY-025]") {
    CHECK(std::is_abstract<IHealthProvider>::value);
}

TEST_CASE("Metrics struct fields exist and default to zero", "[relay][REQ-RELAY-026][REQ-RELAY-029]") {
    Metrics m{};
    CHECK(m.write_count     == 0);
    CHECK(m.deliver_count   == 0);
    CHECK(m.drop_count      == 0);
    CHECK(m.bytes_written   == 0);
    CHECK(m.bytes_delivered == 0);
    CHECK(m.error_count     == 0);
}

TEST_CASE("IMetricsProvider is a polymorphic abstract type", "[relay][REQ-RELAY-027]") {
    CHECK(std::is_abstract<IMetricsProvider>::value);
}

TEST_CASE("IDrainer is a polymorphic abstract type", "[relay][REQ-RELAY-028]") {
    CHECK(std::is_abstract<IDrainer>::value);
}

// ── Error category ────────────────────────────────────────────────────────────

TEST_CASE("error_category name is relay", "[relay][REQ-RELAY-021]") {
    CHECK(std::string(error_category().name()) == "relay");
}

TEST_CASE("make_error_code returns code in relay category", "[relay][REQ-RELAY-021]") {
    auto ec = make_error_code(Errc::closed);
    CHECK(&ec.category() == &error_category());
    CHECK(ec.value()     == static_cast<int>(Errc::closed));
}

TEST_CASE("Errc implicitly converts to error_code via is_error_code_enum", "[relay][REQ-RELAY-022]") {
    std::error_code ec = Errc::timeout;
    CHECK(ec == ErrTimeout());
    CHECK(&ec.category() == &error_category());
}

// ── Context ───────────────────────────────────────────────────────────────────

TEST_CASE("Context::background is never done", "[relay][REQ-RELAY-060]") {
    auto ctx = Context::background();
    CHECK_FALSE(ctx.done());
    CHECK_FALSE(ctx.deadline().has_value());
}

TEST_CASE("Context::with_timeout expires after duration", "[relay][REQ-RELAY-060]") {
    using namespace std::chrono_literals;
    auto ctx = Context::with_timeout(1ms);
    std::this_thread::sleep_for(5ms);
    CHECK(ctx.done());
}

TEST_CASE("Context::with_timeout not yet done immediately", "[relay][REQ-RELAY-060]") {
    using namespace std::chrono_literals;
    auto ctx = Context::with_timeout(10s);
    CHECK_FALSE(ctx.done());
    CHECK(ctx.deadline().has_value());
}
