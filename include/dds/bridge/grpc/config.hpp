// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/grpc/config.hpp — YAML-file configuration for a gRPC bridge.
//
// C++ port of github.com/SoundMatt/go-DDS's bridge/grpc/config.go.
//
// Example:
//
//   listen: ":9090"
//   auth_token: "shared-secret"
//   topics:
//     - name: "sensors/temperature"
//       qos: "reliable"
//     - name: "vehicle/speed"
//       qos: "best_effort"
//
// Scope notes (deliberate deviations from a literal line-for-line port):
//
//   - go-DDS depends on gopkg.in/yaml.v3, a general-purpose YAML parser.
//     This port hand-rolls a minimal parser scoped to exactly the schema
//     above (top-level `key: scalar` pairs, plus one block-form key
//     `topics:` whose value is an indented `- name: ...` / `qos: ...`
//     list) — the same "own small parser, not a general library" pattern
//     already used throughout this repo for JSON (see grpc.hpp's scope
//     note). It does not support YAML features this schema never uses:
//     flow-style collections (`{a: 1}` / `[1, 2]`), anchors/aliases,
//     multi-line scalars, or comments.
//   - TopicConfig::effective_qos()'s "reliable"/"best_effort" branches
//     mirror go-DDS's TopicConfig.qos() by starting from
//     dds::default_qos() and overriding only `reliability`, rather than a
//     raw zero-valued dds::QoS{Reliability: ...} (what go-DDS's literal
//     `dds.QoS{Reliability: dds.Reliable}` produces, with HistoryDepth==0).
//     dds::QoS's default member initializers make a true field-by-field
//     zero value inexpressible without manually overwriting every field,
//     and history_depth is documented dead weight in cpp-DDS today (see
//     include/dds/rtps/history_cache.hpp's scope note — go-DDS's own rtps
//     package doesn't consume it either), so reproducing Go's
//     HistoryDepth==0 byte-for-byte would only add risk for zero
//     behavioral benefit. This is a deliberate, low-risk improvement over
//     a literal port, not a fidelity gap in anything actually observable.

#pragma once

#include <dds/bridge/grpc/grpc.hpp>

#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dds::bridge::grpc {

// TopicConfig configures a single pre-subscribed topic.
// fusa:req REQ-BRIDGE-GRPC-010
struct TopicConfig {
    std::string name;
    std::string qos; // "reliable" | "best_effort" | "besteffort" | "" (default)

    // effective_qos maps `qos` to a dds::QoS value (see file-level scope
    // note for the one deliberate deviation from a literal Go zero-value
    // struct literal).
    dds::QoS effective_qos() const;
};

// Config is the bridge configuration loaded from a YAML file.
// fusa:req REQ-BRIDGE-GRPC-010
struct Config {
    std::string              listen;      // gRPC listen address, e.g. ":9090"
    std::string              auth_token;  // non-empty requires a matching Bearer token
    std::vector<TopicConfig> topics;      // pre-subscribed on bridge start
};

// LoadResult is returned by load_config(): exactly one of `config`/`error`
// is engaged (mirrors dds::idl::ParseResult / dds::tsn::LoadResult's shape
// — see include/dds/idl/idl.hpp and include/dds/tsn/tsn.hpp for the
// established free-form-diagnostic convention this follows, in preference
// to shoehorning "file not found" / "malformed YAML" onto an unrelated
// std::error_code sentinel).
struct LoadResult {
    std::optional<Config>      config;
    std::optional<std::string> error;

    bool ok() const noexcept { return config.has_value(); }
};

// load_config reads and parses the YAML file at `path` (mirrors go-DDS's
// `LoadConfig(path) (*Config, error)`).
// fusa:req REQ-BRIDGE-GRPC-011
LoadResult load_config(const std::string& path);

// apply_config pre-subscribes every topic listed in cfg on bridge's
// participant. Returns the first subscription error encountered, if any.
// fusa:req REQ-BRIDGE-GRPC-011
std::error_code apply_config(Bridge& bridge, const Config& cfg);

} // namespace dds::bridge::grpc
