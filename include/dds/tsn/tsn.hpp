// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// tsn/tsn.hpp — Time-Sensitive Networking (IEEE 802.1) stream descriptors
// and configuration utilities, following the OMG DDS-TSN specification
// (OMG Document ptc/2021-03-01). C++ port of
// github.com/SoundMatt/go-DDS's `tsn` package (`tsn.go`), the last item of
// ROADMAP.md's "Tier 3 — xtypes, tsn, idl, cdr".
//
// A TSN stream maps a DDS topic to a bounded-latency IEEE 802.1 network
// flow. Stream descriptors control VLAN tagging, DSCP marking, frame-size
// bounds, and scheduled-transmit timing (SO_TXTIME / ETF-or-taprio qdisc —
// see include/dds/rtps/traffic.hpp, already wired to these fields through
// this header's `with_stream_config` adapter and
// `ParticipantOptions::tsn_config`; see rtps/participant.hpp).
//
// Usage — load from a JSON config file:
//
//   auto res = dds::tsn::load_config("tsn_streams.json");
//   if (res.ok()) {
//       dds::rtps::ParticipantOptions opts;
//       opts.tsn_config = dds::tsn::with_stream_config(
//           std::make_shared<dds::tsn::StreamConfig>(std::move(*res.config)));
//       auto [p, err] = dds::rtps::Participant::create(0, opts);
//   }
//
// Config file format (JSON), identical field names to go-DDS's tsn.go:
//
//   {
//     "streams": [
//       {
//         "topic":               "vehicle/speed",
//         "vid":                 100,
//         "pcp":                 5,
//         "dscp":                46,
//         "max_frame_size":      1500,
//         "max_interval_frames": 1,
//         "interval_us":         125,
//         "tx_offset_us":        50,
//         "talker_id":           "ecu-cluster-1"
//       }
//     ]
//   }
//
// JSON parsing: this header's .cpp uses a small internal recursive-descent
// JSON reader scoped to exactly this config shape (object/array/string/
// number), not a general-purpose JSON library — matching this repo's
// established convention of writing small, purpose-built parsers instead
// of pulling in a dependency (see dds::idl's own lexer/parser, cli/
// json_lite.hpp).
//
// Scope note — QoS fields: `dds::QoS::transport_priority` and
// `dds::QoS::latency_budget` (include/dds/dds.hpp) already existed before
// this item landed. `transport_priority` is now wired as a fallback PCP
// selector (see rtps/participant.hpp's ParticipantOptions::tsn_config doc)
// when no `Stream` entry matches a writer's topic — mirroring go-DDS's
// `rtpsWriter` construction exactly (config match takes priority,
// QoS.TransportPriority is the fallback). `latency_budget` remains purely
// informational, matching go-DDS's own dds.go comment
// ("Informational in v0.5; future releases may enforce it via qdisc
// admission control") — nothing in go-DDS's rtps package reads it either.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dds/rtps/tsn.hpp>

namespace dds::tsn {

// fusa:req REQ-TSN-001

// Stream is a DDS-TSN stream descriptor as defined in the OMG DDS-TSN
// specification. It binds a DDS topic to a TSN flow with specific
// IEEE 802.1 network scheduling constraints. C++ port of go-DDS's
// tsn.Stream.
struct Stream {
    std::string topic; // exact-match DDS topic name this stream applies to

    uint16_t vid{0};  // IEEE 802.1Q VLAN ID (0 = untagged)
    uint8_t  pcp{0};  // VLAN Priority Code Point (0-7); Linux maps this to SO_PRIORITY
    uint8_t  dscp{0}; // IP Differentiated Services Code Point (0-63); IP_TOS = dscp << 2

    int max_frame_size{0};      // max Ethernet payload bytes per frame (e.g. 1500)
    int max_interval_frames{0}; // max frames allowed per interval_us

    int64_t interval_us{0};  // TSN transmit interval, microseconds (125us = AVB Class A)
    int64_t tx_offset_us{0}; // transmit offset within the interval, microseconds

    std::string talker_id; // informational talker identifier (e.g. IEEE 802.1Qcc TalkerID)

    // interval() returns interval_us as a duration.
    std::chrono::microseconds interval() const noexcept {
        return std::chrono::microseconds(interval_us);
    }

    // tx_offset() returns tx_offset_us as a duration.
    std::chrono::microseconds tx_offset() const noexcept {
        return std::chrono::microseconds(tx_offset_us);
    }

    // max_frag_payload returns the maximum RTPS fragment payload bytes for
    // this stream, reserving space for RTPS headers (~48 bytes). Returns 0
    // when max_frame_size is unset (no fragmentation bound configured).
    int max_frag_payload() const noexcept;
};

// StreamConfig is the top-level structure for a tsn_streams JSON file.
// C++ port of go-DDS's tsn.StreamConfig.
struct StreamConfig {
    std::vector<Stream> streams;

    // stream_for_topic returns a pointer to the first Stream whose topic
    // matches, or nullptr if no stream is configured for topic. The
    // pointer is invalidated by any mutation of `streams`.
    const Stream* stream_for_topic(const std::string& topic) const noexcept;

    // topics returns the set of all configured topic names, in
    // declaration order.
    std::vector<std::string> topics() const;

    // to_json serializes this config back to the same JSON shape
    // parse_config()/load_config() accept — for save/reload round trips.
    std::string to_json() const;
};

// ── Config loading ──────────────────────────────────────────────────────

// LoadResult is returned by load_config()/parse_config(): exactly one of
// `config`/`error` is engaged (mirrors dds::idl::ParseResult's shape —
// see include/dds/idl/idl.hpp for the established free-form-diagnostic
// rationale over std::error_code).
// fusa:req REQ-TSN-001
struct LoadResult {
    std::optional<StreamConfig> config;
    std::optional<std::string>  error;

    bool ok() const noexcept { return config.has_value(); }
};

// load_config reads and parses a TSN stream configuration from a JSON
// file. The file must contain a top-level "streams" array of Stream
// objects; every stream's fields are validated (topic non-empty, PCP
// 0-7, DSCP 0-63, MaxFrameSize/MaxIntervalFrames/IntervalUS/TxOffsetUS
// >= 0). fusa:req REQ-TSN-001
LoadResult load_config(const std::string& path);

// parse_config parses TSN stream configuration from a JSON string. Same
// validation as load_config. fusa:req REQ-TSN-001
LoadResult parse_config(const std::string& json_text);

// ── rtps::TSNStreamConfig adapter ───────────────────────────────────────

// StreamConfigAdapter adapts a StreamConfig to dds::rtps::TSNStreamConfig
// — the supported wiring point for TSN-aware participants (see
// ParticipantOptions::tsn_config in include/dds/rtps/participant.hpp).
// C++ analog of go-DDS's tsn.WithStreamConfig / streamConfigAdapter.
// fusa:req REQ-TSN-002
class StreamConfigAdapter : public dds::rtps::TSNStreamConfig {
public:
    explicit StreamConfigAdapter(std::shared_ptr<StreamConfig> cfg) : cfg_(std::move(cfg)) {}

    bool stream_for_topic(const std::string& topic, dds::rtps::TSNParams& out) const override {
        if (!cfg_) return false;
        const Stream* s = cfg_->stream_for_topic(topic);
        if (!s) return false;
        out.priority         = s->pcp;
        out.dscp              = s->dscp;
        out.interval           = s->interval();
        out.tx_offset          = s->tx_offset();
        out.max_frag_payload   = s->max_frag_payload();
        return true;
    }

private:
    std::shared_ptr<StreamConfig> cfg_;
};

// with_stream_config returns a dds::rtps::TSNStreamConfig that resolves
// topics against cfg — assign the result to
// ParticipantOptions::tsn_config:
//
//   opts.tsn_config = dds::tsn::with_stream_config(cfg);
//
// fusa:req REQ-TSN-002
inline std::shared_ptr<dds::rtps::TSNStreamConfig> with_stream_config(std::shared_ptr<StreamConfig> cfg) {
    return std::make_shared<StreamConfigAdapter>(std::move(cfg));
}

} // namespace dds::tsn
