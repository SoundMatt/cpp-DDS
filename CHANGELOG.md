# Changelog

All notable changes to cpp-DDS are documented here.

Format: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.19.0] — 2026-07-27

### Added

- `dds::xtypes` (`include/dds/xtypes/xtypes.hpp`, `src/xtypes/xtypes.cpp`) —
  DDS-XTypes Dynamic Data support, a faithful C++ port of go-DDS's
  `tools/xtypes/xtypes.go`, the first item of `ROADMAP.md`'s "Tier 3 —
  xtypes, tsn, idl, cdr". `TypeKind`/`FieldDescriptor`/`TypeDescriptor`
  describe a runtime schema; `identify()` derives a content-addressed
  `TypeIdentifier` (SHA-256 over a canonical JSON encoding, order-independent
  over field declaration) byte-for-byte identical to go-DDS's, verified
  against reference vectors independently derived from a fresh go-DDS clone
  including Go's HTML-safe JSON string-escaping rule; `DynamicData` is a
  schema-validated property map with JSON serialization (unknown fields
  skipped on read for forward compatibility, matching go-DDS exactly);
  `TypeRegistry` is a thread-safe name-keyed store with structural-conflict
  detection (`ErrTypeMismatch`); `check_compatibility()` implements the
  standard forward/backward schema-evolution rules.
- No external crypto dependency is fetched for this project, so SHA-256 is
  implemented from scratch under `src/xtypes/detail/` (independent of
  `dds::security`'s own scratch SHA-256, which is explicitly scoped to
  `security.cpp` only) and verified against FIPS 180-4 known-answer vectors.
- 36 new tests (`tests/test_xtypes.cpp`) — 417/417 total. Adds
  `REQ-XTYPE-001` through `REQ-XTYPE-006` and `REQ-TREG-001` through
  `REQ-TREG-003`, traced and tested.

Verified locally: Release C++17/C++20 builds clean (the new files
additionally compiled warning-clean under a genuine `-std=c++20` invocation
directly), ctest 417/417, Debug ASan+UBSan pass on macOS/AppleClang.
ROADMAP.md checked off.

---

## [0.18.0] — 2026-07-27

### Added

- `dds::security::Plugin` / `NullPlugin` / `HMACPlugin` / `AESGCMPlugin`
  (`include/dds/security/security.hpp`, `src/security/security.cpp`) — a
  byte-compatible C++ port of go-DDS's `security` package
  (`security/security.go`), the second item of `ROADMAP.md`'s "Tier 2 —
  safety and security". `HMACPlugin::seal()` appends a 32-byte
  HMAC-SHA-256 authentication tag (`plaintext || HMAC[32]`); `AESGCMPlugin`
  encrypts with AES-256-GCM using a fresh random 12-byte nonce per call
  (`nonce[12] || ciphertext || GCM-tag[16]`, 28 bytes of overhead). Both
  wire formats are byte-for-byte identical to go-DDS's, verified against
  reference vectors independently derived from a fresh go-DDS clone (see
  `tests/test_security_hmac.cpp`, `tests/test_security_aesgcm.cpp`).
- `dds::security::AccessPolicy` / `Permission` / `Rule`
  (`include/dds/security/access.hpp`, `src/security/access.cpp`) — a
  per-participant/per-topic ACL: rules are evaluated in declaration order
  with first-match-wins glob semantics, a faithful byte-oriented port of
  Go's `path.Match` (`security/access.go`). No wire format — behavioral
  parity verified against go-DDS's actual match decisions for the same
  (pattern, topic) case matrix (see `tests/test_security_access.cpp`).
- `dds::security::ReplayGuard`
  (`include/dds/security/replay.hpp`, `src/security/replay.cpp`) — a
  sliding-time-window anti-replay sequence-number tracker
  (`security/replay.go`); `check()` returns `ErrReplay` for any sequence
  number already seen within the configured window (default 30s).
- No external crypto dependency is fetched for this project
  (`cmake/FetchDeps.cmake` — Catch2 only): SHA-256, HMAC-SHA-256, AES-256,
  and AES-256-GCM are implemented from scratch under `src/security/crypto/`
  (an internal, non-public implementation detail) and verified byte-exact
  against FIPS 180-4/RFC 4231/NIST SP 800-38A known-answer test vectors,
  the classic McGrew-Viega/NIST AES-256-GCM all-zero test vector, and
  Go's actual `crypto/aes` + `crypto/cipher` + `crypto/hmac` stdlib output
  (which is what go-DDS's plugins wrap internally).
- `requirements/requirements.json` gains `REQ-SECURITY-001` through
  `REQ-SECURITY-009`, traced (`fusa:req`) and tested (`fusa:test`).

Internal/additive: `dds::security::Plugin` is a standalone seal/open
library, not wired into `dds::adapt()`, `dds::mock`, or the RTPS transport —
matching `dds::safety::E2EPublisher`'s own precedent from v0.17.0. go-DDS's
`security.cert`/`security.discovery` (PKI-based mutual authentication and
DDS-discovery security wrapping, `security/cert.go` + `security/discovery.go`)
are separate surfaces beyond this item's stated scope and are out of scope
here, matching the `ddssafety`/E2E item's precedent of scoping out
`safety/metrics.go`.

---

## [0.17.0] — 2026-07-27

### Added

- `dds::safety::E2EPublisher` / `dds::safety::E2ESubscriber`
  (`include/dds/safety/e2e.hpp`, `src/safety/e2e.cpp`) — a byte-compatible
  C++ port of go-DDS's end-to-end (E2E) data protection wire header
  (`safety/e2e.go`), the first item of `ROADMAP.md`'s "Tier 2 — safety and
  security". `E2EPublisher` wraps a `dds::IPublisher` and prepends an
  18-byte little-endian protection header (DataID, SourceID,
  SequenceCounter, Timestamp, CRC-16/CCITT-FALSE) to every payload before
  writing, with a per-publisher sequence counter starting at 1.
  `E2ESubscriber` wraps a `dds::ISubscriber`, strips the header from every
  received sample on a background pump thread, and validates CRC,
  sequence-counter continuity, and sample freshness against
  `E2EConfig::max_age` (zero disables freshness checking) — valid samples
  are forwarded via `channel()`; violations are reported via `errors()`
  without suppressing delivery of sequence-gapped samples (matching
  go-DDS's own behavior).
- Byte-exact wire-format vectors (18-byte header layout and the
  CRC-16/CCITT-FALSE algorithm) independently derived from a fresh go-DDS
  clone and checked in `tests/test_safety_e2e.cpp`, following the same
  reference-vector derivation convention `test_rtps_cdr.cpp` established
  for RTPS.
- `requirements/requirements.json` gains `REQ-E2E-001` through
  `REQ-E2E-006`, traced (`fusa:req`) and tested (`fusa:test`).

Internal/additive: `E2EPublisher` fully implements `dds::IPublisher`
(including the context-bounded `write()` overload, per `REQ-SAFETY-003`) —
a strict improvement over go-DDS's own `E2EPublisher`, which in practice
does not satisfy the full `dds.Publisher` interface (no `WriteCtx`).
`E2ESubscriber` is not forced into `dds::ISubscriber` for the same reason
go-DDS's own type isn't a full `Subscriber` either (no `TryRead`/
`Unsubscribe`); it exposes `channel()`/`errors()`/`close()` instead,
mirroring go-DDS's actual surface. go-DDS's `safety.SafetyMetricsProvider`/
`SafetyEvent`/monitor-integration layer (`safety/metrics.go`) is
Tier-5-adjacent observability and out of scope for this item.

Verified locally: Release C++17/C++20 clean, all tests passing, Debug
ASan+UBSan clean. `ROADMAP.md` checked off.

## [0.16.0] — 2026-07-28

### Added

- `dds::mock::new_loaning_publisher()` (see `ROADMAP.md`, "Also within
  `ddscore` but not RTPS-specific"): a mock-participant-backed
  implementation of `dds::ILoaningPublisher`, completing the interface's
  second (and, alongside the RTPS side landed in v0.12.0, final planned)
  concrete implementation for zero-copy loan/commit publishing.
- `include/dds/pool/pool.hpp` gains `dds::pool::SampleBuffer`, a
  thread-safe fixed-capacity ring buffer of `dds::Sample` values — a C++
  port of the other half of go-DDS's `pool/pool.go` (`BytePool` was
  ported alone in v0.12.0, since Tier-1 phase 9 only needed that piece).
  `push`/`pop`/`len`/`cap` mirror go-DDS's `Push`/`Pop`/`Len`/`Cap`
  exactly, operating purely by value with no pointer-ownership divergence
  from go-DDS (unlike `BytePool`, which owns `std::vector<uint8_t>`
  objects directly per `dds::ILoaningPublisher`'s pointer-based shape).
- New `include/dds/mock/loan.hpp` declares
  `dds::mock::new_loaning_publisher(participant, topic, qos, buf_size)`,
  a C++ port of go-DDS's `mock/loan.go` `NewLoaningPublisher`. Its
  `MockLoaningPublisher` implementation lives in
  `src/mock/participant.cpp` alongside `MockPublisher`, the one
  translation unit where that `.cpp`-local type is visible — following
  `dds/rtps/loan.hpp`/`src/rtps/participant.cpp`'s own precedent for the
  RTPS side. `MockPublisher` gains an `is_closed()` accessor mirroring
  `rtps::Writer::is_closed()`.
- `loan_buffer` rejects on a closed publisher (`ErrClosed`) or an
  oversized pool request (`ErrLoanBuffer`); `write_loaned` publishes via
  the same `MockPublisher::write` every plain mock publisher uses and
  returns the buffer to the pool; `return_loan` discards a buffer without
  publishing; `new_loaning_publisher` rejects a non-mock participant with
  `ErrLoanBuffer` (matching go-DDS's `mpub, ok := pub.(*publisher)`
  type-assertion failure) or propagates `new_publisher`'s own error —
  matching go-DDS's `mock.loaningPublisher`/`NewLoaningPublisher` exactly,
  including its "no ownership validation on Commit" behavior (a
  documented caller contract, not enforced code, in both languages).
- 9 new `SampleBuffer` unit tests (`tests/test_pool.cpp`) and 7 new
  mock-loan-integration tests (`tests/test_mock_loan.cpp`) — 320/320
  total. Verified locally with Release C++17/C++20 builds and a Debug
  ASan/UBSan pass; CI additionally exercises Linux/gcc-12 ASan+UBSan.
- Scope: internal, additive — `dds::mock::new_loaning_publisher` is NOT
  wired into `dds::adapt()` or any automatic-transport-selection surface,
  matching the RTPS side's own precedent (v0.12.0).

---

## [0.15.0] — 2026-07-28

### Added

- `dds::IHealthProvider` (see `ROADMAP.md`, "Also within `ddscore` but not
  RTPS-specific"): a DDS-package-scoped health-reporting interface mirroring
  go-DDS's core `dds` package (`dds.go`), implemented by both
  `dds::mock::IMockParticipant` and `dds::rtps::Participant`.
- `dds.hpp` gains `dds::HealthStatus`/`dds::Health`/`dds::IHealthProvider`,
  field-for-field mirrors of go-DDS's `dds.go` `HealthStatus`/`Health`/
  `HealthProvider`. The `details` string carries both participant- and
  transport-level state (no separate transport-health type exists in
  go-DDS either). Because C++ virtual dispatch (unlike Go's structural
  interface satisfaction) cannot have two unrelated base classes both
  declare a `health()` method with different return types on the same
  derived class, and `dds::mock::IMockParticipant` already implements
  `relay::IHealthProvider::health()` (RELAY spec §9.2), the new DDS-scoped
  accessor is named `dds_health()` instead — the same collision and naming
  precedent as `dds_metrics()` in v0.14.0.
- `dds::mock::IMockParticipant` implements it: `dds_health()` mirrors
  go-DDS's `mock.participant.Health()` exactly, including its
  `{"state":"closed"}` details string on close.
- `dds::rtps::Participant` implements it: `dds_health()` mirrors go-DDS's
  `rtps.participant.Health()` exactly, folding live writer/reader counts
  into `{"writers":N,"readers":N}` while open and `{"state":"closed"}`
  once closed.
- 7 new tests (`tests/test_mock.cpp`, `tests/test_rtps_participant.cpp`)
  covering the interface hierarchy on both participants plus OK/Down
  transitions and details-string content — 304/304 total. Verified locally
  with Release C++17 and a Debug ASan/UBSan pass.
- Scope: internal, additive — not yet wired into `dds::adapt()`'s
  `relay::INode` bridge or the `cpp-dds` CLI's `optional_interfaces`
  capabilities list; that consumption/export layer is go-DDS's
  `monitor`/`admin` equivalent (Tier 5, still deferred).

---

## [0.14.0] — 2026-07-28

### Added

- `dds::IMetricsProvider` / `dds::IDiscoveryMetricsProvider` /
  `dds::ITopicMetricsProvider` (see `ROADMAP.md`, "Also within `ddscore` but
  not RTPS-specific"): DDS-package-scoped metrics interfaces mirroring
  go-DDS's core `dds` package (`dds.go`), implemented by both
  `dds::mock::IMockParticipant` and `dds::rtps::Participant`.
- `dds.hpp` gains `dds::Metrics`/`IMetricsProvider`,
  `dds::DiscoveryMetrics`/`IDiscoveryMetricsProvider`, and
  `dds::TopicMetrics`/`ITopicMetricsProvider`, field-for-field mirrors of
  go-DDS's `dds.go` structs/interfaces. Because C++ virtual dispatch
  (unlike Go's structural interface satisfaction) cannot have two unrelated
  base classes both declare a `metrics()` method with different return
  types on the same derived class, and `dds::mock::IMockParticipant`
  already implements `relay::IMetricsProvider::metrics()` (RELAY spec
  §9.1), the new DDS-scoped accessor is named `dds_metrics()` instead.
- `dds::mock::IMockParticipant` implements all three: `dds_metrics()`
  reuses the aggregate counters already tracked for
  `relay::IMetricsProvider`; `discovery_metrics()` always returns zero
  values (no real network discovery, matching go-DDS's mock doc comment
  verbatim); a new per-topic counter table backs `topic_metrics()`.
- `dds::rtps::Participant` implements all three: new participant-level
  aggregate atomics incremented in `Writer::write()` /
  `Participant::dispatch()`, mirroring go-DDS's `rtpsWriter.Write` /
  `participant.deliverToReader` counter split exactly; `discovery_metrics()`
  surfaces the `SpdpService`/`SedpService` counters already tracked and
  exposed since Tier-1 phases 4-5; a new per-topic counter table keyed by
  reader topic backs `topic_metrics()`.
- 10 new tests (`tests/test_mock.cpp`, `tests/test_rtps_participant.cpp`)
  covering the interface hierarchy on both participants plus
  write/deliver/drop/byte counting, per-topic breakdown, and (RTPS) live
  discovery counters — 297/297 total. Verified locally with Release C++17
  and a Debug ASan/UBSan pass.
- Scope: internal, additive — not yet wired into `dds::adapt()`'s
  `relay::INode` bridge or the `cpp-dds` CLI's `optional_interfaces`
  capabilities list; that consumption/export layer is go-DDS's
  `monitor`/`admin` equivalent (Tier 5, still deferred).

---

## [0.13.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 10 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 10 "IPv6 / wildcard locators" — best-effort, non-gating, per
  go-DDS's own "limited interop testing" framing): IPv6 UDP socket support
  and an opt-in `ParticipantOptions::ipv6` flag. This was the last of
  Tier 1's ten dependency-ordered RTPS sub-phases — Tier 1 is now complete.
- `dds::rtps::UdpSocket::bind_unicast_v6` / `bind_multicast_receive_v6`
  (`include/dds/rtps/transport.hpp`, `src/rtps/transport.cpp`): C++ ports
  of go-DDS's `newUnicastSocketV6` / `newMulticastReceiveSocketV6`, plus a
  new `AddressFamily` tag and family-aware `send_to`/`recv`. No wire-format
  change: `Locator_t` has encoded/decoded UDPv6-kind values
  byte-identically to go-DDS since Tier-1 phase 1/2 (kind-agnostic 24-byte
  layout), and wildcard (0.0.0.0-address) locator fill-in was already
  implemented in phases 4-5 — this phase is purely the socket/transport
  primitive and its participant-level wiring.
- `dds::rtps::ParticipantOptions::ipv6` (`include/dds/rtps/participant.hpp`,
  `src/rtps/participant.cpp`): a C++ port of go-DDS's `WithIPv6()` option,
  deliberately matching its exact integration depth rather than an
  idealized rewrite — binds three additional IPv6 sockets (soft-fail
  tolerated), of which only the user-data socket is wired into a receive
  path (a new `data_loop_v6`, feeding the same `handle_data_packet` as the
  IPv4 path); outbound replies/retransmits always go via the IPv4 socket
  regardless of arrival socket, and SPDP/SEDP discovery remains IPv4-only —
  both are faithful ports of go-DDS's own `participant.go` behavior, not
  oversights (see `ROADMAP.md` phase 10's "Done" note for the full
  rationale, including why go-DDS's own IPv6 tests are similarly shallow).
- 8 new tests (5 transport-level IPv6 socket tests, 3 participant-level —
  including a real end-to-end test injecting a wire-correct DATA
  submessage from a raw `::1` UdpSocket into a Participant's IPv6 data
  port) — 287/287 total. Verified locally with Release C++17/C++20 builds
  (plus the changed and new files additionally compiled warning-clean
  under a genuine `-std=c++20` invocation) and a Debug ASan+UBSan pass; CI
  additionally exercises Linux/gcc-12 ASan+UBSan.
- Scope: internal, additive — `ParticipantOptions::ipv6` is opt-in and off
  by default, and (as with every prior RTPS phase) none of this is wired
  into `dds::adapt()` or any automatic-transport-selection surface.

---

## [0.12.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 9 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 9 "Loan integration"): zero-copy loaned-sample publishing wired into
  the RTPS writer path, backed by a pool allocator.
- `include/dds/pool/pool.hpp` (header-only): `dds::pool::BytePool`, a
  thread-safe fixed-capacity byte-buffer allocator — a C++ port of the
  `BytePool` portion of go-DDS's `pool/pool.go`.
- `include/dds/rtps/loan.hpp`: `dds::rtps::new_loaning_publisher`, a C++
  port of go-DDS's `rtps/loan.go` `NewLoaningPublisher`; its
  `LoaningWriter` implementation of `dds::ILoaningPublisher` (already
  declared in `dds.hpp` ahead of this phase) lives in
  `src/rtps/participant.cpp`, wrapping a `Writer` with a `BytePool` for
  `loan_buffer`/`write_loaned`/`return_loan`. Introduces no new wire
  encoding — `write_loaned` calls the same already byte-verified
  `Writer::write` every plain publisher uses.
- Verified with `BytePool` behavioral unit tests
  (`tests/test_pool.cpp`, mirroring go-DDS's own `pool_test.go` coverage)
  and loan-integration tests (`tests/test_rtps_loan.cpp`) covering
  same-process round-tripping, pool exhaustion, closed-writer rejection,
  discard-without-publish, direct `write()` passthrough, both
  go-DDS-mirrored `NewLoaningPublisher` error paths, and an end-to-end test
  over real loopback UDP once SEDP-matched — 279/279 total. Verified
  locally with Release C++17/C++20 builds (plus the new files additionally
  compiled warning-clean under a genuine `-std=c++20` invocation) and a
  Debug ASan+UBSan pass on macOS/AppleClang; CI additionally exercises
  Linux/gcc-12 ASan+UBSan.
- Scope: internal to `cppdds_lib` — `new_loaning_publisher` is not wired
  into `dds::adapt()` or any automatic-transport-selection surface, and a
  mock-participant-backed `ILoaningPublisher` implementation remains a
  separate, still-unchecked roadmap item.

---

## [0.11.0] — 2026-07-27

### Added

- RTPS wire-level interop testing infrastructure (see `ROADMAP.md`, "Interop testing
  infrastructure (new need — not just golden vectors)"): a new `interop/` directory,
  mirroring go-DDS's own `interop/` package pattern exactly.
- `interop/test_interop.cpp`: brings up a real (non-`test_mode`, real-multicast)
  `dds::rtps::Participant` and verifies SPDP/SEDP discovery converges and samples
  cross in both directions against a live CycloneDDS peer — the CycloneDDS-peer leg
  of the roadmap's eventual cpp-DDS ⇄ go-DDS / cpp-DDS ⇄ CycloneDDS / cpp-DDS ⇄
  rust-DDS 3-way matrix, and go-DDS's own first interop target and reference pattern.
- `interop/docker-compose.yml`: brings up a CycloneDDS peer container
  (`cyclone-peer`, plus single-direction `cyclone-sub`/`cyclone-pub` profile
  services), same shape as go-DDS's own `interop/docker-compose.yml`.
- New `CPPDDS_INTEROP_TESTS` CMake option (default `OFF`): the CMake/CTest
  equivalent of go-DDS's `go:build interop` tag. With it off — the default for every
  normal build, including CI's `build-and-test` matrix — `interop/`'s tests are
  never compiled, linked, or registered with CTest. With it on, the resulting
  `cppdds_interop_tests` target tags every test with the CTest label `interop` and
  `SKIP_RETURN_CODE 4`, so a run with no live peer reports "Not Run" rather than
  "Failed".
- `INTEROP_DOMAIN` / `INTEROP_TIMEOUT` environment variables, matching go-DDS's own
  convention exactly.
- New opt-in `test-interop` CI job: builds with `-DCPPDDS_INTEROP_TESTS=ON`, brings
  up the CycloneDDS peer, and runs the interop CTest suite — probe-gated to skip
  gracefully if the CycloneDDS Docker image is unavailable, and not a dependency of
  (or depended on by) any other job, mirroring go-DDS's own `test-interop` job.
- `interop/README.md`: prerequisites, local quick-start, and what each test verifies.
- Verified locally: default (`CPPDDS_INTEROP_TESTS=OFF`) Release C++17/C++20 builds
  unaffected (264/264 existing tests still pass); with the option on, the new suite
  builds warning-clean under a genuine `-std=c++20` invocation and passes under both
  a plain build and a Debug ASan+UBSan pass.
- Scope: additive testing infrastructure only — no changes to `cppdds_lib`'s public
  or internal API surface.

## [0.10.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 8 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 8 "Fragmentation"): `DataFrag` submessage type added to
  `include/dds/rtps/types.hpp` + `src/rtps/types.cpp`, verified
  byte-for-byte against go-DDS reference vectors in the new
  `tests/test_rtps_fragment.cpp`.
- `include/dds/rtps/fragment.hpp` (header-only): `dds::rtps::FragmentAssembler`
  (receive-side reassembly) and `split_into_fragments`/`split_into_fragments_n`
  (send-side splitting) — a C++ port of go-DDS's `fragment.go`.
- `dds::rtps::Participant`'s `Writer::write` now fragments a CDR-wrapped
  payload over `kMaxFragmentPayload` into DATA_FRAG submessages;
  `Writer::handle_ack_nack` re-fragments from `HistoryCache` on
  ACKNACK-triggered retransmit (a correctness improvement over go-DDS's own
  first-fragment-only retransmit limitation); `Participant::handle_data_packet`
  reassembles incoming DATA_FRAG submessages via a new `frag_assembler_`
  member and dispatches the completed sample — completing the round trip
  even though go-DDS's own `participant.go` never wires its own
  `fragmentAssembler` into an equivalent receive path.
- Verified with byte-exact `DataFrag`/`split_into_fragments_n` reference-vector
  tests, `FragmentAssembler` behavioral unit tests, and two end-to-end tests
  driving a real `Participant` over real loopback UDP (264/264 total).
  Verified locally with Release C++17/C++20 builds (plus the new files
  additionally compiled warning-clean under a genuine `-std=c++20`
  invocation) and a Debug ASan+UBSan pass on macOS/AppleClang; CI
  additionally exercises Linux/gcc-12 ASan+UBSan.
- Scope: internal to `cppdds_lib`, under `dds/rtps/` — not yet wired into
  `dds::IParticipant`'s public surface beyond `dds::rtps::Participant`
  itself, nor into any automatic-transport-selection surface.

---

## [0.9.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 7 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 7 "Reliable delivery"): `Heartbeat`/`AckNack`/`Gap` submessage types
  in `include/dds/rtps/types.hpp` + `src/rtps/types.cpp`, verified
  byte-for-byte against go-DDS reference vectors in the new
  `tests/test_rtps_reliable.cpp`.
- `dds::rtps::RecvTracker` (`include/dds/rtps/reliable.hpp`, header-only): a
  reader-side sliding-window gap tracker — port of go-DDS's `reliable.go`
  `recvTracker`. `reliable.go`'s sender-side `sendHistory` is not
  separately re-ported: phase 6's `HistoryCache` already serves that role.
- `include/dds/rtps/persist.hpp` + `src/rtps/persist.cpp`
  (`persist_load`/`persist_flush`/`persist_path`): byte-for-byte port of
  go-DDS's `persist.go` TransientLocal-style disk durability file format.
- `dds::rtps::Participant`'s `Writer` now sends HEARTBEAT immediately after
  every reliable write and periodically from a per-writer background thread
  (`ParticipantOptions::heartbeat_period`), and retransmits (or sends GAP
  for an evicted range of) requested sequence numbers on receipt of
  ACKNACK, broadcasting to every SEDP-matched reader locator. `Reader` now
  tracks a `RecvTracker` per matched remote writer and sends ACKNACK on a
  detected gap, from both DATA arrival and HEARTBEAT receipt.
  `ParticipantOptions::persist_dir` flushes every publish to disk and backs
  `TransientLocal` late-joiner delivery when no in-memory sample exists.
  `Participant::close()` now closes every still-registered writer (stopping
  its heartbeat thread) via a new `writers_` registry, mirroring the
  existing `readers_` registry.
- Verified with byte-exact HEARTBEAT/ACKNACK/GAP reference-vector tests,
  `RecvTracker`/persistence behavioral unit tests, and five end-to-end
  tests driving a real `Participant` over real loopback UDP (248/248
  total). Verified locally with Release C++17/C++20 builds and a Debug
  ASan+UBSan pass on macOS/AppleClang; CI additionally exercises
  Linux/gcc-12 ASan+UBSan.
- Scope: internal to `cppdds_lib`, under `dds/rtps/` — not yet wired into
  `dds::IParticipant`'s public surface beyond `dds::rtps::Participant`
  itself, nor into any automatic-transport-selection surface. go-DDS's
  `waitDrain`/`CloseWithDrain` (blocking until all writes are ACKed) is out
  of scope for this phase.

---

## [0.8.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 6 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 6 "Entities & history cache"): `dds::rtps::Participant` under
  `include/dds/rtps/participant.hpp` + `src/rtps/participant.cpp` — a new,
  complete `dds::IParticipant` implementation over real RTPS/UDP wiring
  together phases 1–5 (wire types, discovery CDR, UDP transport, SPDP, SEDP)
  into working best-effort pub/sub: participant/writer/reader entity
  lifecycle, an SPDP→SEDP peer-bridge poll loop, and a data receive thread
  that decodes inbound DATA submessages and dispatches them to matching
  local readers.
- `dds::rtps::HistoryCache` (`include/dds/rtps/history_cache.hpp`): a
  bounded, sequence-number-indexed per-endpoint change store (matching
  go-DDS's `maxHistoryDepth`-256 window), populated by every writer write —
  scaffolding for phase 7 (reliable delivery) to consume, not yet used for
  retransmission.
- `entity_id_for_writer`/`entity_id_for_reader`/`new_guid_prefix` added to
  `include/dds/rtps/types.hpp` (entity/participant identity allocation,
  matching go-DDS's `guid.go` kind-byte convention and PID-seeded random
  prefix).
- `SedpService::matched_reader_locators_for_topic` added to
  `include/dds/rtps/sedp.hpp` (the write-path mirror of the existing
  `matched_writer_guids_for_reader`): every remote reader locator matched
  to a topic, deduplicated.
- Writer::write composes only already byte-verified wire primitives from
  phases 1–2/4–5 (`DataSubmessage::encode`, `cdr_wrap_payload`,
  `wrap_in_rtps_message`) — this phase introduces no new wire encoding of
  its own. Verified with a live two-`Participant` test exchanging a real
  sample over loopback UDP once SEDP-matched, plus same-process delivery,
  topic isolation, TransientLocal late-joiner delivery,
  `QoS.max_sample_size` enforcement, and SPDP→SEDP bridge-loop tests
  (221/221 total). Verified locally with Release C++17/C++20 builds and a
  Debug ASan+UBSan pass on macOS/AppleClang; CI additionally exercises
  Linux/gcc-12 ASan+UBSan.
- Scope: best-effort delivery only (Reliable QoS is accepted but behaves
  identically to BestEffort until phase 7 lands); no fragmentation, loan
  integration, IPv6, security, or TSN. `dds::rtps::Participant` is a new,
  separate `dds::IParticipant` implementation living alongside
  `dds::mock`'s — deliberately not wired into `dds::adapt()`'s default
  selection or any automatic-transport-selection surface.

---

## [0.7.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 5 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 5 "SEDP"): `dds::rtps::SedpService` under `include/dds/rtps/sedp.hpp`
  + `src/rtps/sedp.cpp` — local writer/reader registration with event-driven
  unicast publication/subscription announcement to every known participant's
  meta-unicast port, a receive thread, and remote-endpoint tables that
  topic-name match incoming announcements against local endpoints. Also the
  standalone `build_endpoint_data`/`parse_endpoint_data` (PL_CDR_LE
  `EndpointData` encode/decode) and `build_sedp_announcement` (RTPS message
  framing shared by publication and subscription announcements) functions,
  independently testable without a running service.
- `on_new_peer`/`on_peer_evicted` hooks so a later phase can wire
  `SpdpService`'s known-peers table into `SedpService` without either class
  holding a live reference to the other, plus `matched_writer_guids_for_reader`/
  `writer_locator`/`reader_locator` query methods for phase 6's reader/writer
  wiring to consume.
- Every wire-format path verified byte-for-byte against reference vectors
  produced by calling go-DDS's actual (unexported)
  `sedpService.buildEndpointData`/`marshalDataSubmessage`/`wrapInRTPSMessage`
  functions directly (`tests/test_rtps_sedp.cpp` documents the exact
  reproduction steps), plus behavioral tests (self-announcement filtering,
  topic matching in both discovery orders, remote locator tracking, peer
  eviction purge) and a live two-`SedpService` unicast convergence test.
  Verified locally with Release C++17/C++20 builds and a Debug ASan+UBSan
  pass on macOS/AppleClang; CI additionally exercises Linux/gcc-12
  ASan+UBSan.
- Internal, additive scaffolding within `cppdds_lib`, under `dds/rtps/` —
  not yet wired into the public `dds::IParticipant` / `relay::INode`
  surface. Deliberately scoped down from a full port of go-DDS's
  `sedpService` (a method set on the not-yet-ported `participant` type,
  phase 6) — see `sedp.hpp`'s file-level scope note for the specific
  deviations.

### Fixed

- `src/mock/participant.cpp` was missing `#include <algorithm>` for
  `std::remove_if`, relying on a transitive include that not every
  standard library provides — added explicitly.

---

## [0.6.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 4 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 4 "SPDP"): `dds::rtps::SpdpService` under `include/dds/rtps/spdp.hpp`
  + `src/rtps/spdp.cpp` — periodic (2s, configurable) multicast
  self-announcement of the local participant plus a known-participants
  table populated from received SPDP announcements, with lease-based peer
  eviction (once per second, 10s default lease, matching go-DDS). Also the
  standalone `build_participant_data`/`parse_participant_data`
  (PL_CDR_LE `ParticipantProxy` encode/decode) and
  `wrap_in_rtps_message`/`build_spdp_announcement` (RTPS message framing)
  functions, independently testable without a running service.
- New `dds::rtps::kVendorIdCppDDS` (`include/dds/rtps/types.hpp`) — cpp-DDS's
  own (unregistered) RTPS vendor ID for locally-originated messages,
  distinct from `kVendorIdGoDDS`.
- Every wire-format path verified byte-for-byte against reference vectors
  produced by calling go-DDS's actual (unexported)
  `spdpService.buildParticipantData`/`parseParticipantData`/
  `wrapInRTPSMessage`/`marshalDataSubmessage` functions directly
  (`tests/test_rtps_spdp.cpp` documents the exact reproduction steps), plus
  behavioral tests of the known-peers table (self-announcement filtering,
  non-SPDP-writer filtering, lease-based eviction with and without an
  advertised lease PID) and a live two-`SpdpService` loopback discovery
  test. Verified locally with Release C++17/C++20 builds and a Debug
  ASan+UBSan pass on macOS/AppleClang; CI additionally exercises Linux/gcc-12
  ASan+UBSan.
- Internal, additive scaffolding within `cppdds_lib`, under `dds/rtps/` —
  not yet wired into the public `dds::IParticipant` / `relay::INode`
  surface. Deliberately scoped down from a full port of go-DDS's
  `spdpService` (a method set on the not-yet-ported `participant` type,
  Tier-1 phase 6): the SEDP-notification hooks, liveliness callback, and
  optional `DiscoveryPlugin` authentication-token exchange are omitted, as
  they depend on machinery from later phases / the public-API wiring phase
  — see `include/dds/rtps/spdp.hpp`'s file-level scope note for the full
  list of deliberate deviations.

## [0.5.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 3 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 3 "UDP transport"): `dds::rtps::UdpSocket` under
  `include/dds/rtps/transport.hpp` + `src/rtps/transport.cpp` — an IPv4 UDP
  socket wrapper with unicast bind (go-DDS's port+0..+15 retry, or an
  OS-assigned ephemeral port when `port == 0`), multicast receive with a
  same-host unicast fallback when the OS/CI sandbox has no
  multicast-capable route, and synchronous send/recv with a 250ms poll
  timeout matching go-DDS's `readLoop` interval. Also the RTPS 2.3 §9.6.1
  port-assignment formula (`meta_multicast_port`, `meta_unicast_port`,
  `data_unicast_port`, `user_multicast_port`) and the standard discovery
  multicast group `239.255.0.1`.
- Platform-specific socket tuning under `include/dds/rtps/traffic.hpp`,
  mirroring go-DDS's `rtps/traffic_linux.go` / `rtps/traffic_other.go`
  split as a CMake-selected translation-unit split: `src/rtps/traffic_linux.cpp`
  implements real `SO_PRIORITY` / `IP_TOS` / `SO_TXTIME` / `CLOCK_TAI`
  socket tuning via raw syscalls (only compiled when
  `CMAKE_SYSTEM_NAME` is `Linux`); `src/rtps/traffic_other.cpp` provides
  no-op fallbacks everywhere else (macOS, Windows).
- Cross-platform socket implementation: BSD sockets on POSIX, Winsock2 on
  Windows (`ws2_32` now linked into `cppdds_lib` on `WIN32`).
- Port formula verified against go-DDS's own `rtps/wire_test.go`
  `TestPortFormula` values, plus additional vectors reproduced by calling
  go-DDS's actual `metaMulticastPort`/`metaUnicastPort`/`userUnicastPort`
  functions directly (`tests/test_rtps_transport.cpp` documents the exact
  reproduction steps). `tests/test_rtps_traffic.cpp` covers the traffic
  tuning hooks. Verified locally with Release C++17/C++20 builds and a
  Debug ASan+UBSan pass on Linux/gcc-12 exercising the real
  `traffic_linux.cpp` syscall path, and a Release build on macOS/AppleClang.
  Internal, additive scaffolding within `cppdds_lib` — not yet wired into
  the public `dds::IParticipant` / `relay::INode` surface, and not yet
  consumed by SPDP/SEDP (later Tier-1 sub-phases).

## [0.4.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 2 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 2 "Discovery-scoped CDR/PL_CDR encoding"): `dds::rtps::PLCDREncoder`
  and `PLCDRDecoder`, plus `cdr_wrap_payload`/`cdr_unwrap_payload`, under
  `include/dds/rtps/cdr.hpp` + `src/rtps/cdr.cpp`. This is the minimal
  little-endian PL_CDR subset used to encode/decode SPDP/SEDP parameter
  lists (RTPS 2.3 §10.2–§10.3) — deliberately separate from Tier 3's
  general-purpose XCDR1 `cdr` library for arbitrary IDL-defined user
  payload types, mirroring go-DDS's own `rtps/cdr.go` vs. top-level `cdr/`
  package split. Every encode/decode path is verified byte-for-byte against
  reference vectors produced by calling go-DDS's actual (unexported)
  `plCDREncoder`/`plCDRDecoder`/`cdrWrapPayload`/`cdrUnwrapPayload`
  functions directly (`tests/test_rtps_cdr.cpp` documents the exact
  reproduction steps). Internal, additive scaffolding within `cppdds_lib`
  — not yet wired into the public `dds::IParticipant` / `relay::INode`
  surface, and not yet consumed by SPDP/SEDP (later Tier-1 sub-phases).

## [0.3.0] — 2026-07-27

### Added

- RTPS Tier-1 sub-phase 1 (see `ROADMAP.md`, "Tier 1 — RTPS wire protocol",
  phase 1 "Wire types & framing"): `dds::rtps::GuidPrefix`, `EntityId`,
  `GUID` (RTPS 2.3 §9.3.1), `Locator` (§9.3.2), `ProtocolVersion`,
  `VendorId`, `Header`, `SubmessageHeader`, `SequenceNumber`, and
  `DataSubmessage` (§9.4), under `include/dds/rtps/types.hpp` +
  `src/rtps/types.cpp`. Every `encode()`/`decode()` is verified byte-for-byte
  against reference vectors produced by calling `go-DDS`'s actual
  `rtps` package marshal functions directly (`tests/test_rtps_types.cpp`
  documents the exact reproduction steps). This is internal, additive
  scaffolding within `cppdds_lib` — not yet wired into the public
  `dds::IParticipant` / `relay::INode` surface; that lands in a later
  Tier-1 sub-phase once UDP transport and entities exist.
- Note on versioning: `ROADMAP.md`'s "suggested version sequencing" lists
  `v0.2.0` as the target for RTPS phases 1–3 combined and reserves `v0.3.0`
  for Tier 2 (safety/security). `v0.2.0` was already released for unrelated
  work before RTPS implementation began, and this release covers only
  phase 1 (wire types) of phases 1–3, so that sequencing no longer lines up
  literally; treat it as superseded illustrative guidance, not a
  compatibility promise. This is a MINOR bump (new additive capability,
  no breaking changes to the existing public surface) rather than a
  Tier-2 release — Tier 2's actual version number will be chosen when that
  work starts.

## [0.2.0] — 2026-07-27

### Fixed

- `MockParticipantImpl::close()` now closes every subscriber channel already
  returned via `new_subscriber()` (RELAY spec §6 req 3), unblocking blocked
  `recv()` callers and the `dds::adapt()` forwarder thread instead of leaking
  it. `close()` is now explicitly idempotent (§6 req 1) via `atomic::exchange`.
- `convert` now implements the RELAY spec §11.2 wire contract exactly:
  `convert --protocol P [--format json]` reads a `dds.Sample` JSON value on
  stdin and writes the resulting `relay.Message` JSON to stdout (exit `0`
  converted / `1` invalid input / `2` invalid args), reproducing the embedded
  `dds-sample` golden vector byte-for-byte and passing `relay interop`. The
  previous `convert <topic> <hex_payload>` positional form and plain-text
  summary output are removed.
- Pinned `kRelaySpecVersion`/`kSpecVersion` bumped `1.10` → `1.11`; stale
  `v1.7` references in `CLAUDE.md`/`ROADMAP.md` corrected.

### Added

- `relay::Channel<T>` (`include/dds/channel.hpp`) — the bundled RELAY core
  channel primitive now lives in `namespace relay` per spec §18.2, with a
  spec-mandated `push()` method. `dds::Chan<T>` is now a compatibility alias
  for `relay::Channel<T>`; `relay::INode::subscribe()` returns
  `relay::Channel<Message>` instead of reaching into the DDS-specific
  namespace (spec §13.7.3 self-contained bundled `relay` module).
- Safety evidence per RELAY spec §20.4: `HARA.md`/`.fusa-hara.json`,
  `tara.md`/`tara.json` (ISO/SAE 21434), `fmea.csv`/`fmea.json` (generated
  dFMEA), alongside the existing `requirements/requirements.json`.
- CI: `fusa-asil-b` job now runs the full x-FuSa lifecycle (§20.1 item 2) —
  `cpfusa cyber` (cybersecurity analysis), `cpfusa vuln` (dependency
  vulnerability scan), `cpfusa qualify` (tool qualification suite), all
  gating, in addition to the existing `check`/`lint`/`trace`.
- CI: `relay-conform` job now also gates on `relay interop --protocol DDS
  --strict` (§20.1 item 3 / §20.2 behavioural conformance).
- CI: new `release.yml` workflow attaches an SBOM and build provenance
  (`cpfusa release`) to every `v*` release tag (§20.5 supply-chain integrity).

## [0.1.0] — 2026-06-19

### Added

- Core DDS interfaces: `IParticipant`, `IPublisher`, `ISubscriber` per RELAY spec §8.2.
- `dds::Sample`, `dds::QoS`, `dds::Guid`, `dds::Domain` canonical types per RELAY spec §15.2.
- `Sample::to_message()` and `from_message()` per RELAY spec §15.7.2.
- `dds::adapt()` — wraps `IParticipant` as `relay::INode` for cross-protocol routing.
- `dds::mock::create()` — in-process participant backed by a process-global Broker.
- Mock TransientLocal durability: late subscribers receive the last published sample.
- Mock back-pressure: DropNewest (default), DropOldest, Block per `relay::BackPressurePolicy`.
- DDS-specific error sentinels: `ErrTopicEmpty`, `ErrQoSMismatch`, `ErrDeadlineMissed`,
  `ErrSampleRejected`, `ErrResourceLimits`, `ErrLoanBuffer`, `ErrDomainOutOfRange`.
- `relay::Context` with `background()`, `with_timeout()`, `with_deadline()`.
- `cpp-dds` CLI: `version`, `conform`, `convert` subcommands.
- Full test suite: `test_relay`, `test_dds`, `test_mock`.
- CI: Ubuntu (gcc-12/clang-14), macOS 14, Windows 2022; coverage; DCO; SARIF upload.
- Requirements traceability: 15 requirements in `requirements/requirements.json`.
- RELAY spec v1.7 conformance.
