// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// rtps/loan.hpp — zero-copy loaned-sample publishing wired into the RTPS
// writer path, backed by dds::pool::BytePool (dds/pool/pool.hpp).
//
// This is Tier-1 sub-phase 9 of the cpp-DDS RTPS roadmap (see ROADMAP.md,
// "Tier 1 — RTPS wire protocol", phase 9: "Loan integration"). It is
// internal, additive scaffolding, matching every prior phase's own scope
// note: NOT wired into dds::adapt() or any automatic-transport-selection
// surface — callers construct new_loaning_publisher(...) explicitly,
// exactly as they construct dds::rtps::Participant::create(...) or
// dds::mock::create(...) explicitly.
//
// C++ port of github.com/SoundMatt/go-DDS's rtps/loan.go (66 LOC):
// NewLoaningPublisher and the loaningWriter type. dds::ILoaningPublisher
// and dds::ErrLoanBuffer (dds.hpp) already existed ahead of this phase as
// scaffolding for the general, not-RTPS-specific ddscore roadmap item
// ("`ILoaningPublisher` ... backed by a pool allocator", the "Also within
// ddscore but not RTPS-specific" list in ROADMAP.md) — this phase is the
// first *concrete* implementation of that interface, scoped to the RTPS
// writer path only (a mock-participant-backed implementation remains for
// that separate, still-unchecked roadmap item).
//
// Scope notes (deliberate deviations from a literal line-for-line port):
//
//   - go-DDS's loaningWriter embeds *rtpsWriter directly and, being in the
//     same package, reaches into rtpsWriter's private `mu`/`closed` fields
//     for its own Loan() closed-check. dds::rtps::Writer (participant.cpp)
//     is a .cpp-local type not exposed via any header — matching every
//     prior phase's precedent of keeping Writer/Reader internal
//     implementation details (see participant.hpp's own file-level scope
//     note) — so this phase's LoaningWriter and new_loaning_publisher are
//     implemented in participant.cpp itself, the one translation unit
//     where the concrete Writer type is visible, rather than in a separate
//     loan.cpp; only the public factory function is declared here. This is
//     a translation-unit-boundary consequence of matching this codebase's
//     established Writer-encapsulation convention, not a behavioral
//     deviation from go-DDS.
//   - dds::ILoaningPublisher::loan_buffer/write_loaned/return_loan (already
//     fixed by dds.hpp before this phase) operate on `std::vector<uint8_t>*`
//     rather than go-DDS's `[]byte` slice view — BytePool (dds/pool/pool.hpp)
//     therefore owns std::vector<uint8_t> objects directly rather than a
//     backing array a slice merely views. See pool.hpp's own file-level
//     note for the ownership consequence (a caller that never returns a
//     loaned buffer leaks it, unlike Go's GC-backed sync.Pool).
//   - go-DDS's Commit (this port's write_loaned) does not validate that buf
//     was actually issued by this same LoaningPublisher's pool before
//     publishing it and returning it to the pool — the "must be a slice
//     previously returned by Loan on this same publisher" contract in both
//     dds.go's and dds.hpp's LoaningPublisher/ILoaningPublisher doc
//     comments is documentation of caller responsibility, not enforced
//     behavior. This port matches that exactly (no validation), since
//     go-DDS is the behavioral oracle here and this is a deliberate
//     "trust the caller" design, not an oversight.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <dds/dds.hpp>

namespace dds::rtps {

// new_loaning_publisher creates an ILoaningPublisher for topic using the
// given QoS, backed by a dds::pool::BytePool of buf_size bytes (0 => pool
// default, 4096 — matching go-DDS's pool.New(0) default). p must be a
// dds::rtps::Participant (i.e. constructed via Participant::create); any
// other IParticipant implementation returns ErrLoanBuffer, matching
// go-DDS's rtps.NewLoaningPublisher failing its `rw, ok := pub.(*rtpsWriter)`
// type assertion the same way. Returns whatever error new_publisher itself
// returns (e.g. ErrClosed, ErrTopicEmpty) if publisher creation fails.
std::pair<std::shared_ptr<ILoaningPublisher>, std::error_code>
new_loaning_publisher(const std::shared_ptr<IParticipant>& p, const std::string& topic, QoS qos,
                       std::size_t buf_size = 0);

} // namespace dds::rtps
