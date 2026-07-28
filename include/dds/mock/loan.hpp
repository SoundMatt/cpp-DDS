// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// mock/loan.hpp — zero-copy loaned-sample publishing wired into the mock
// (in-process) participant's writer path, backed by dds::pool::BytePool
// (dds/pool/pool.hpp).
//
// This is the "Also within ddscore but not RTPS-specific" list's
// `ILoaningPublisher` item (see ROADMAP.md): a mock-participant-backed
// implementation of dds::ILoaningPublisher, the general interface already
// concretely implemented once (Tier-1 phase 9, RTPS side only — see
// dds/rtps/loan.hpp). Internal, additive scaffolding, matching that
// phase's own scope note: NOT wired into dds::adapt() or any
// automatic-transport-selection surface — callers construct
// new_loaning_publisher(...) explicitly, exactly as they construct
// dds::rtps::new_loaning_publisher(...) or dds::mock::create(...)
// explicitly.
//
// C++ port of github.com/SoundMatt/go-DDS's mock/loan.go (66 LOC):
// NewLoaningPublisher and the loaningPublisher type — itself a
// near-verbatim mirror of rtps/loan.go's loaningWriter/NewLoaningPublisher
// (the same pool.BytePool-backed Loan/Commit pair, wrapping a mock
// *publisher instead of an *rtpsWriter). This header and its
// implementation (src/mock/participant.cpp) follow rtps/loan.hpp's own
// precedent exactly, including its scope-note rationale below adapted to
// the mock side:
//
//   - go-DDS's loaningPublisher embeds *publisher directly and, being in
//     the same package, reaches into publisher's private `mu`/`closed`
//     fields for its own Loan() closed-check. dds::mock::MockPublisher
//     (src/mock/participant.cpp) is a .cpp-local type not exposed via any
//     header — matching this codebase's established Publisher-
//     encapsulation convention (see participant.hpp's own file-level
//     scope note and rtps/loan.hpp's identical note for the RTPS side) —
//     so this file's MockLoaningPublisher and new_loaning_publisher are
//     implemented in participant.cpp itself, the one translation unit
//     where the concrete MockPublisher type is visible, rather than in a
//     separate loan.cpp; only the public factory function is declared
//     here.
//   - dds::ILoaningPublisher::loan_buffer/write_loaned/return_loan operate
//     on `std::vector<uint8_t>*` rather than go-DDS's `[]byte` slice view
//     — BytePool (dds/pool/pool.hpp) therefore owns std::vector<uint8_t>
//     objects directly rather than a backing array a slice merely views.
//     See pool.hpp's own file-level note for the ownership consequence (a
//     caller that never returns a loaned buffer leaks it, unlike Go's
//     GC-backed sync.Pool).
//   - go-DDS's Commit (this port's write_loaned) does not validate that
//     buf was actually issued by this same LoaningPublisher's pool before
//     publishing it and returning it to the pool — the "must be a slice
//     previously returned by Loan on this same publisher" contract in
//     both dds.go's and dds.hpp's LoaningPublisher/ILoaningPublisher doc
//     comments is documentation of caller responsibility, not enforced
//     behavior. This port matches that exactly (no validation), since
//     go-DDS is the behavioral oracle here and this is a deliberate
//     "trust the caller" design, not an oversight — identical to
//     rtps/loan.hpp's own note.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>

namespace dds::mock {

// new_loaning_publisher creates an ILoaningPublisher for topic using the
// given QoS, backed by a dds::pool::BytePool of buf_size bytes (0 => pool
// default, 4096 — matching go-DDS's pool.New(0) default). p must be a
// dds::mock participant (i.e. constructed via dds::mock::create); any
// other IParticipant implementation returns ErrLoanBuffer, matching
// go-DDS's mock.NewLoaningPublisher failing its `mpub, ok :=
// pub.(*publisher)` type assertion the same way. Returns whatever error
// new_publisher itself returns (e.g. ErrClosed, ErrTopicEmpty) if
// publisher creation fails.
std::pair<std::shared_ptr<ILoaningPublisher>, std::error_code>
new_loaning_publisher(const std::shared_ptr<IParticipant>& p, const std::string& topic, QoS qos,
                       std::size_t buf_size = 0);

} // namespace dds::mock
