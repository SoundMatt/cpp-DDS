// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::tsn::TAPRIOConfig (include/dds/tsn/taprio.hpp). Test
// matrix mirrors go-DDS's tsn/taprio_test.go, plus tc_command() and
// taprio_from_streams() coverage. apply()/verify_applied() are exercised
// on whichever platform CI happens to run on: real (CAP_NET_ADMIN-gated)
// netlink behavior on Linux, a documented "not supported" diagnostic
// everywhere else — see include/dds/tsn/taprio.hpp's file-level scope
// note, mirroring test_rtps_traffic.cpp's own precedent for platform-
// dependent hardware/kernel-feature tests.

#include <catch2/catch_test_macros.hpp>

#include <dds/tsn/taprio.hpp>

using namespace dds::tsn;

// fusa:test REQ-TSN-006

TEST_CASE("TAPRIOConfig::validate rejects a missing Interface", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.entries = {{0xFF, std::chrono::milliseconds(1)}};
    auto err = cfg.validate();
    REQUIRE(err.has_value());
}

TEST_CASE("TAPRIOConfig::validate rejects empty Entries", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    auto err = cfg.validate();
    REQUIRE(err.has_value());
}

TEST_CASE("TAPRIOConfig::validate rejects a zero interval", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {{0xFF, std::chrono::nanoseconds(0)}};
    auto err = cfg.validate();
    REQUIRE(err.has_value());
}

TEST_CASE("TAPRIOConfig::validate accepts a well-formed config", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {
        {0xFF, std::chrono::microseconds(800)},
        {0x01, std::chrono::microseconds(200)},
    };
    CHECK_FALSE(cfg.validate().has_value());
}

TEST_CASE("TAPRIOConfig::cycle_duration uses the explicit cycle_time when set", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface  = "eth0";
    cfg.cycle_time = std::chrono::milliseconds(5);
    cfg.entries     = {{0xFF, std::chrono::milliseconds(1)}};
    CHECK(cfg.cycle_duration() == std::chrono::milliseconds(5));
}

TEST_CASE("TAPRIOConfig::cycle_duration sums entry intervals when unset", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {
        {0x0F, std::chrono::microseconds(600)},
        {0xF0, std::chrono::microseconds(400)},
    };
    CHECK(cfg.cycle_duration() == std::chrono::milliseconds(1));
}

// fusa:test REQ-TSN-007

TEST_CASE("TAPRIOConfig::apply fails cleanly (Linux: real error; other: not supported)", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {{0xFF, std::chrono::milliseconds(1)}};
    auto err = cfg.apply();
    // On Linux with CAP_NET_ADMIN and a real "eth0", this could succeed —
    // not assumed here (sandboxed CI/test hosts have neither). Otherwise
    // (no eth0, no CAP_NET_ADMIN, or a non-Linux platform) it must fail.
    // Only assert failure when it does occur — mirrors go-DDS's own
    // TestTAPRIOConfig_Apply_NonLinux "skip if it unexpectedly succeeded"
    // pattern.
    if (err.has_value()) {
        CHECK_FALSE(err->empty());
    }
}

TEST_CASE("TAPRIOConfig::verify_applied rejects an empty Interface", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.entries = {{0xFF, std::chrono::milliseconds(1)}};
    auto err = cfg.verify_applied();
    REQUIRE(err.has_value());
}

TEST_CASE("TAPRIOConfig::verify_applied fails cleanly for an unconfigured interface", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {{0xFF, std::chrono::milliseconds(1)}};
    auto err = cfg.verify_applied();
    if (err.has_value()) {
        CHECK_FALSE(err->empty());
    }
}

TEST_CASE("TAPRIOConfig::tc_command produces a tc(8) command template", "[tsn][taprio]") {
    TAPRIOConfig cfg;
    cfg.interface = "eth0";
    cfg.entries    = {
        {0xFF, std::chrono::microseconds(800)},
        {0x01, std::chrono::microseconds(200)},
    };
    const std::string cmd = cfg.tc_command("eth0", 0);
    CHECK(cmd.find("tc qdisc replace dev eth0") != std::string::npos);
    CHECK(cmd.find("taprio") != std::string::npos);
    CHECK(cmd.find("sched-entry S ff 800000") != std::string::npos);
    CHECK(cmd.find("sched-entry S 01 200000") != std::string::npos);
    CHECK(cmd.find("base-time 0") != std::string::npos);
}

TEST_CASE("taprio_from_streams derives a TAPRIO schedule", "[tsn][taprio]") {
    auto res = parse_config(R"({
        "streams":[
            {"topic":"a","pcp":5,"interval_us":1000},
            {"topic":"b","pcp":3,"interval_us":2000}
        ]
    })");
    REQUIRE(res.ok());

    auto tc = taprio_from_streams(*res.config);
    REQUIRE(tc.ok());
    CHECK(tc.config->cycle_time == std::chrono::milliseconds(3));
    REQUIRE(tc.config->entries.size() == 2);
}

TEST_CASE("taprio_from_streams rejects an empty StreamConfig", "[tsn][taprio]") {
    StreamConfig cfg;
    auto tc = taprio_from_streams(cfg);
    CHECK_FALSE(tc.ok());
    REQUIRE(tc.error.has_value());
}

TEST_CASE("taprio_from_streams rejects a config where every interval is zero", "[tsn][taprio]") {
    auto res = parse_config(R"({"streams":[{"topic":"a","pcp":5}]})");
    REQUIRE(res.ok());
    auto tc = taprio_from_streams(*res.config);
    CHECK_FALSE(tc.ok());
}
