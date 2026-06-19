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

// fusa:req REQ-MOCK-001
// fusa:req REQ-MOCK-002
// fusa:req REQ-MOCK-003
// fusa:req REQ-MOCK-004
// fusa:req REQ-MOCK-005

namespace dds::mock {

// create returns a new in-process participant joined to the given domain.
// Returns ErrDomainOutOfRange if domain is outside 0–232.
// fusa:req REQ-MOCK-001 REQ-MOCK-002
std::pair<std::shared_ptr<IParticipant>, std::error_code>
create(Domain domain = 0);

} // namespace dds::mock
