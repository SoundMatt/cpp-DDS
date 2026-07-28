// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/mock/participant.hpp — in-process DDS participant.
//
// All mock participants sharing the same process-global Broker exchange
// samples synchronously. Use for unit tests; replace with a real transport
// (RTPS/CycloneDDS) for multi-process or multi-host operation.

#pragma once

#include <dds/dds.hpp>
#include <memory>

// fusa:req REQ-MOCK-001 REQ-MOCK-002 REQ-MOCK-003 REQ-MOCK-004 REQ-MOCK-005
// fusa:req REQ-METRICS-001 REQ-METRICS-002 REQ-METRICS-003
// fusa:req REQ-METRICS-004 REQ-METRICS-005 REQ-METRICS-006
// fusa:req REQ-HEALTH-001 REQ-HEALTH-002 REQ-HEALTH-003

namespace dds::mock {

// IMockParticipant extends IParticipant with the optional RELAY capability
// interfaces (relay::IMetricsProvider, relay::IHealthProvider,
// relay::IDrainer — RELAY spec §9/§12.2) plus the DDS-package-scoped metrics
// and health providers mirroring go-DDS's dds.go (dds::IMetricsProvider,
// dds::IDiscoveryMetricsProvider, dds::ITopicMetricsProvider,
// dds::IHealthProvider — see dds.hpp's "Metrics providers" and "Health
// provider" sections for why the latter use `dds_metrics()`/`dds_health()`
// rather than `metrics()`/`health()`). Callers can use the concrete type
// directly without dynamic_cast.
// fusa:req REQ-MOCK-001 REQ-METRICS-003 REQ-METRICS-004 REQ-METRICS-005
// fusa:req REQ-METRICS-006 REQ-HEALTH-001 REQ-HEALTH-003
class IMockParticipant
    : public IParticipant
    , public relay::IMetricsProvider
    , public relay::IHealthProvider
    , public relay::IDrainer
    , public IMetricsProvider
    , public IDiscoveryMetricsProvider
    , public ITopicMetricsProvider
    , public IHealthProvider
{
public:
    virtual ~IMockParticipant() = default;
};

// create returns a new in-process participant joined to the given domain.
// Returns ErrDomainOutOfRange if domain is outside 0–232.
// fusa:req REQ-MOCK-001 REQ-MOCK-002
std::pair<std::shared_ptr<IMockParticipant>, std::error_code>
create(Domain domain = 0);

} // namespace dds::mock
