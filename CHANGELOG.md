# Changelog

All notable changes to cpp-DDS are documented here.

Format: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
