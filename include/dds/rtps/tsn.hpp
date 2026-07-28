// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/tsn.hpp — the subset of a TSN stream descriptor rtps needs in order
// to mark outbound traffic for a topic with the correct VLAN priority,
// DSCP value, fragmentation bound, and (optionally) a scheduled transmit
// offset. C++ port of github.com/SoundMatt/go-DDS's rtps/tsn.go: `TSNParams`
// holds only plain values, and `TSNStreamConfig` is a narrow interface, so
// that `dds::rtps` never needs to depend on `dds::tsn` to describe them
// (mirrors go-DDS's own comment: "rtps never needs to import package tsn").
//
// `dds::tsn::StreamConfigAdapter` (include/dds/tsn/tsn.hpp) is the
// supported way to plug a `dds::tsn::StreamConfig` into
// `ParticipantOptions::tsn_config` — see that header's `with_stream_config`
// factory, the C++ equivalent of go-DDS's `tsn.WithStreamConfig`.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace dds::rtps {

// TSNParams is the subset of a TSN stream descriptor's fields the RTPS
// writer path needs to mark outbound traffic for a topic. Plain-value
// struct — no ownership, no dependency on dds::tsn.
struct TSNParams {
    uint8_t priority{0};              // VLAN Priority Code Point / SO_PRIORITY (0-7)
    uint8_t dscp{0};                  // IP Differentiated Services Code Point (0-63)
    std::chrono::nanoseconds interval{0};  // TSN transmit interval
    std::chrono::nanoseconds tx_offset{0}; // transmit offset within interval; 0 = no SO_TXTIME scheduling
    int max_frag_payload{0};          // max RTPS fragment payload bytes; 0 = unbounded
};

// TSNStreamConfig resolves a DDS topic name to its configured TSN transmit
// parameters. Assign an implementation to
// `ParticipantOptions::tsn_config` to enable per-topic traffic-class
// marking. `dds::tsn::with_stream_config` adapts a `dds::tsn::StreamConfig`
// to this interface without dds::rtps depending on dds::tsn.
class TSNStreamConfig {
public:
    virtual ~TSNStreamConfig() = default;

    // Returns true and populates out with topic's TSN parameters, or
    // returns false when topic has no configured TSN stream.
    virtual bool stream_for_topic(const std::string& topic, TSNParams& out) const = 0;
};

} // namespace dds::rtps
