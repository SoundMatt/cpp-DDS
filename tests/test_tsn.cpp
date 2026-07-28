// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::tsn::Stream/StreamConfig (include/dds/tsn/tsn.hpp). Test
// matrix mirrors go-DDS's tsn/tsn_test.go.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <dds/tsn/tsn.hpp>
#include <fstream>

using namespace dds::tsn;

namespace {
const char* kSampleJson = R"({
  "streams": [
    {
      "topic": "vehicle/speed",
      "vid": 100,
      "pcp": 5,
      "dscp": 46,
      "max_frame_size": 1500,
      "max_interval_frames": 1,
      "interval_us": 125,
      "tx_offset_us": 50,
      "talker_id": "ecu-cluster-1"
    },
    {
      "topic": "vehicle/steering",
      "vid": 100,
      "pcp": 4,
      "dscp": 34,
      "max_frame_size": 1500,
      "max_interval_frames": 2,
      "interval_us": 250,
      "tx_offset_us": 0,
      "talker_id": "ecu-cluster-2"
    }
  ]
})";
} // namespace

// fusa:test REQ-TSN-001

TEST_CASE("parse_config parses a valid stream config", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    REQUIRE(res.config->streams.size() == 2);
    const auto& s = res.config->streams[0];
    CHECK(s.topic == "vehicle/speed");
    CHECK(s.pcp == 5);
    CHECK(s.dscp == 46);
    CHECK(s.vid == 100);
    CHECK(s.max_frame_size == 1500);
    CHECK(s.max_interval_frames == 1);
    CHECK(s.interval_us == 125);
    CHECK(s.tx_offset_us == 50);
    CHECK(s.talker_id == "ecu-cluster-1");
}

TEST_CASE("Stream::interval/tx_offset duration helpers", "[tsn]") {
    Stream s;
    s.interval_us  = 125;
    s.tx_offset_us = 50;
    CHECK(s.interval() == std::chrono::microseconds(125));
    CHECK(s.tx_offset() == std::chrono::microseconds(50));
}

TEST_CASE("Stream::max_frag_payload", "[tsn]") {
    struct Case { int max_frame_size; int want; };
    const Case cases[] = {
        {1500, 1452}, // 1500 - 48
        {64, 16},     // 64 - 48
        {47, 0},      // <= overhead -> 0
        {0, 0},       // unset -> 0
    };
    for (const auto& tc : cases) {
        Stream s;
        s.max_frame_size = tc.max_frame_size;
        CHECK(s.max_frag_payload() == tc.want);
    }
}

TEST_CASE("StreamConfig::stream_for_topic", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    const Stream* s = res.config->stream_for_topic("vehicle/speed");
    REQUIRE(s != nullptr);
    CHECK(s->pcp == 5);
    CHECK(res.config->stream_for_topic("unknown") == nullptr);
}

TEST_CASE("StreamConfig::topics", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    auto topics = res.config->topics();
    REQUIRE(topics.size() == 2);
    CHECK(((topics[0] == "vehicle/speed" && topics[1] == "vehicle/steering") ||
           (topics[1] == "vehicle/speed" && topics[0] == "vehicle/steering")));
}

TEST_CASE("load_config reads from a file", "[tsn]") {
    const std::string path = "test_tsn_load_config.json";
    {
        std::ofstream f(path, std::ios::binary);
        f << kSampleJson;
    }
    auto res = load_config(path);
    REQUIRE(res.ok());
    CHECK(res.config->streams.size() == 2);
    std::remove(path.c_str());
}

TEST_CASE("load_config on a missing file returns an error", "[tsn]") {
    auto res = load_config("/nonexistent/path/tsn.json");
    CHECK_FALSE(res.ok());
    REQUIRE(res.error.has_value());
}

TEST_CASE("load_config on malformed JSON returns an error", "[tsn]") {
    const std::string path = "test_tsn_bad.json";
    {
        std::ofstream f(path, std::ios::binary);
        f << "{broken json";
    }
    auto res = load_config(path);
    CHECK_FALSE(res.ok());
    std::remove(path.c_str());
}

TEST_CASE("load_config on a validation failure returns an error", "[tsn]") {
    const std::string path = "test_tsn_invalid.json";
    {
        std::ofstream f(path, std::ios::binary);
        f << R"({"streams":[{"topic":"","pcp":1}]})";
    }
    auto res = load_config(path);
    CHECK_FALSE(res.ok());
    std::remove(path.c_str());
}

TEST_CASE("parse_config on malformed JSON returns an error", "[tsn]") {
    auto res = parse_config("{broken");
    CHECK_FALSE(res.ok());
    REQUIRE(res.error.has_value());
}

TEST_CASE("parse_config rejects an empty topic", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"","pcp":1}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects PCP out of range", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","pcp":8}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects DSCP out of range", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","dscp":64}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config -> to_json -> parse_config round trip", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    const std::string serialized = res.config->to_json();
    auto res2 = parse_config(serialized);
    REQUIRE(res2.ok());
    REQUIRE(res2.config->streams.size() == res.config->streams.size());
    for (std::size_t i = 0; i < res.config->streams.size(); ++i) {
        CHECK(res2.config->streams[i].topic == res.config->streams[i].topic);
        CHECK(res2.config->streams[i].pcp == res.config->streams[i].pcp);
        CHECK(res2.config->streams[i].dscp == res.config->streams[i].dscp);
    }
}

TEST_CASE("parse_config rejects negative max_frame_size", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","max_frame_size":-1}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects negative max_interval_frames", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","max_interval_frames":-1}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects negative interval_us", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","interval_us":-1}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects negative tx_offset_us", "[tsn]") {
    auto res = parse_config(R"({"streams":[{"topic":"x","tx_offset_us":-1}]})");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config accepts an empty streams array", "[tsn]") {
    auto res = parse_config(R"({"streams":[]})");
    REQUIRE(res.ok());
    CHECK(res.config->streams.empty());
}

TEST_CASE("parse_config rejects a non-object top-level value", "[tsn]") {
    auto res = parse_config(R"([1,2,3])");
    CHECK_FALSE(res.ok());
}

TEST_CASE("parse_config rejects a non-array \"streams\" field", "[tsn]") {
    auto res = parse_config(R"({"streams":"nope"})");
    CHECK_FALSE(res.ok());
}

// fusa:test REQ-TSN-002

TEST_CASE("StreamConfigAdapter resolves a matching topic into TSNParams", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    auto cfg     = std::make_shared<StreamConfig>(std::move(*res.config));
    auto adapter = with_stream_config(cfg);

    dds::rtps::TSNParams params;
    REQUIRE(adapter->stream_for_topic("vehicle/speed", params));
    CHECK(params.priority == 5);
    CHECK(params.dscp == 46);
    CHECK(params.interval == std::chrono::microseconds(125));
    CHECK(params.tx_offset == std::chrono::microseconds(50));
    CHECK(params.max_frag_payload == 1452);
}

TEST_CASE("StreamConfigAdapter returns false for an unconfigured topic", "[tsn]") {
    auto res = parse_config(kSampleJson);
    REQUIRE(res.ok());
    auto cfg     = std::make_shared<StreamConfig>(std::move(*res.config));
    auto adapter = with_stream_config(cfg);

    dds::rtps::TSNParams params;
    CHECK_FALSE(adapter->stream_for_topic("unknown/topic", params));
}
