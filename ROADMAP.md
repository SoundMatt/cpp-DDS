# cpp-DDS Roadmap

## Continuous conformance (RELAY spec §20 — ongoing, not version-gated)

- [x] `relay conform --strict` CI gate
- [x] Full x-FuSa lifecycle CI gate: check, requirements traceability, cybersecurity
      analysis (`cpfusa cyber`), dependency vulnerability scan (`cpfusa vuln`), and the
      tool qualification suite (`cpfusa qualify`) — §20.1 item 2
- [x] Safety evidence: requirements registry, HARA, TARA, dFMEA — §20.4 (see README)
- [x] SBOM + build provenance attached to every release tag — §20.5
- [x] `convert --protocol DDS` implements the §11.2 wire contract and reproduces the
      embedded `dds-sample` golden vector byte-for-byte; `relay interop --protocol DDS
      --strict ./build/cli/cpp-dds` gates `relay-conform` in CI — §20.1 item 3, §20.2

## v0.1.0 — Foundation (complete)

- [x] Core interfaces: `IParticipant`, `IPublisher`, `ISubscriber`
- [x] Canonical types: `Sample`, `QoS`, `Guid`, `Domain`
- [x] Error sentinels mapping to relay sentinels
- [x] `dds::mock` in-process participant
- [x] `dds::adapt()` relay::INode bridge
- [x] `relay::Context` (background, with_timeout, with_deadline)
- [x] `cpp-dds` CLI (version, conform, convert)
- [x] CI matrix: Ubuntu / macOS / Windows, C++17 and C++20
- [x] Coverage, DCO, SARIF upload
- [x] RELAY spec v1.10 conformance (current pin: v1.11, see §20 continuous conformance above)

---

## Parity gap vs. go-DDS

cpp-DDS today is ~2,200 LOC across 10 files (`dds.hpp/cpp`, `channel.hpp`, `relay.hpp/cpp`,
`mock/participant.hpp/cpp`, plus the `cpp-dds` CLI) implementing exactly one transport:
an in-process mock. go-DDS — the reference implementation this roadmap tracks — is ~17,000
non-test LOC (~40,000+ including tests) across 24 packages. Concretely, go-DDS's `rtps`
package alone (pure Go, no CGo — 17 non-test files, 4,106 LOC non-test / 10,776 LOC
including tests) is already larger than the whole of cpp-DDS today, and it's only one of
five architectural groups. Everything below is currently absent from cpp-DDS:

| Missing capability | go-DDS package(s) | Approx. LOC (non-test) |
|---|---|---|
| RTPS/UDP wire transport, discovery, reliability | `rtps` | 4,106 |
| Shared-memory transport | `shmem` | — (part of 776 incl. tests) |
| Security (auth, encryption, ACL) | `security` | — (part of 639 incl. tests) |
| E2E safety protection | `safety` | — (part of 658 incl. tests) |
| DDS-XTypes Dynamic Data | `xtypes` | 460 |
| TSN QoS integration | `tsn` | — (part of 824 incl. tests) |
| IDL parser + codegen | `idl`, `cmd/ddstool` | 1,382 + part of `cmd`'s 1,462 |
| CDR/XCDR1 general-purpose serialization | `cdr` | — (part of 348 incl. tests) |
| Protocol bridges | `bridge/{grpc,wan,rest}` | 1,235 |
| Observability | `otel`, `admin`, `monitor`, `record`, `services` | 64+197+504+395+231 = 1,391 |
| Automatic transport selection, pooling | `auto`, `pool` | — (part of 129+139 incl. tests) |
| Optional CycloneDDS backend | `cyclone` | 413 |

This roadmap sequences closing that gap. Per explicit direction, **Tier 1 targets full
native RTPS wire-protocol parity** — real interop with other DDS implementations over
UDP — not a CycloneDDS-binding shortcut and not a deferral of RTPS in favor of easier
tiers.

## Target architecture — 5-library split

go-DDS is executing its own split into build-independent groups under
[go-DDS#71](https://github.com/SoundMatt/go-DDS/issues/71) (multi-module Go layout, one
`go.mod` per group). cpp-DDS mirrors the same grouping as separate CMake targets rather
than separate repos, since this is a single-repo C++ library. Proposed target names and
their mapping onto the existing `include/dds/` + `src/` layout:

| Group | CMake target (proposed) | `include/dds/` subtree (proposed) | go-DDS equivalents |
|---|---|---|---|
| ddscore | `cppdds_core` (supersedes today's `cppdds_lib`) | `dds.hpp`, `channel.hpp`, `relay.hpp`, `mock/`, `rtps/`, `shmem/`, `security/`, `pool/`, `auto/` | `dds`, `rtps`, `mock`, `shmem`, `auto`, `pool`, `security` |
| ddsbridges | `cppdds_bridges` | `bridge/{grpc,wan,rest,mqtt,domain}/` | `bridge/{grpc,wan,rest}` (mqtt, domain not yet in go-DDS either — see Tier 4) |
| ddstools | `cppdds_tools` | `idl/`, `cdr/`, `xtypes/` + a standalone `ddstool` CLI target (distinct from the existing `cpp-dds` conformance CLI) | `idl`, `cdr`, `xtypes`, `cmd/ddstool` |
| ddsobservability | `cppdds_observability` | `otel/`, `admin/`, `monitor/`, `record/`, `services/` | `otel`, `admin`, `monitor`, `record`, `services` |
| ddssafety | `cppdds_safety` | `safety/`, `tsn/` + safety evidence already at repo root (`HARA.md`, `tara.md`, `fmea.*`) | `safety`, `tsn`, `cert/` |

`cppdds_core` stays the only mandatory target — mock-only builds (today's default) must
keep working with zero new dependencies. The other four are opt-in `CPPDDS_BUILD_*`
CMake options, same pattern as the existing `CPPDDS_BUILD_TESTS` / `CPPDDS_BUILD_CLI`.
An umbrella `cppdds::all` INTERFACE target can link all five for convenience once they
exist. This is a build-graph reorganization only — no target splitting happens until a
tier actually has code to put in it; **Tier 1 is scoped entirely within `cppdds_core`**
(RTPS lives under `dds/rtps/`, not a separate target, since go-DDS's own grouping keeps
`rtps` in its core module too).

**Naming caveat:** the group names above (`ddscore`, `ddsbridges`, `ddstools`,
`ddsobservability`, `ddssafety`) and the per-concern directory names (`rtps`, `xtypes`,
`tsn`, `idl`, `cdr`, `shmem`, and bridge names `mqttbr`/`wanbr`/`restbr`/`grpcbridge`/
`domainbr`) are **proposed pending RELAY spec §13.7.2 ratification**, tracked at
[RELAY#59](https://github.com/SoundMatt/RELAY/issues/59). That issue's registry addition
is in draft/review, not yet normative — treat every directory and CMake target name in
this roadmap as provisional and expect a rename pass once §13.7.2 lands.

---

## Tier 1 — RTPS wire protocol (v0.2.x — highest priority)

Goal: real network interop over UDP, wire-compatible with other RTPS 2.3 implementations
(go-DDS, CycloneDDS, rust-DDS once it exists) — not just mock-to-mock in one process.
go-DDS's `rtps` package (pure Go, no CGo, targets any platform with UDP sockets) is the
correctness oracle: port its *behavior* against the OMG RTPS 2.3 spec sections its own
doc comments cite, not its Go idioms. Sub-phases, in dependency order (each independently
buildable and testable against go-DDS's file of the same concern):

1. [x] **Wire types & framing** — `GuidPrefix`/`GUID` (RTPS 2.3 §9.3.1), `Locator_t`
   (§9.3.2), `Header`/`SubmessageHeader`/DATA submessage (§9.4). Reference: `guid.go`
   (81 LOC), `locator.go` (136 LOC), `message.go` (365 LOC). **Done (v0.3.0):**
   `include/dds/rtps/types.hpp` + `src/rtps/types.cpp`, verified byte-for-byte
   against go-DDS reference vectors in `tests/test_rtps_types.cpp`. Internal to
   `cppdds_lib` — not yet wired into `dds::IParticipant`/`relay::INode`.
2. [x] **Discovery-scoped CDR/PL_CDR encoding** — little-endian only (the de-facto
   standard for modern RTPS), used to encode/decode SPDP/SEDP parameter lists
   (§10.2–§10.3). Reference: `cdr.go` (193 LOC). This is a *minimal* subset sufficient
   for discovery submessages — do not conflate it with Tier 3's general-purpose XCDR1
   `cdr` library for arbitrary IDL-defined user payload types; go-DDS keeps these as two
   separate packages (`rtps/cdr.go` vs. top-level `cdr/`) and cpp-DDS should too.
   **Done:** `include/dds/rtps/cdr.hpp` + `src/rtps/cdr.cpp` (`PLCDREncoder`/
   `PLCDRDecoder` plus the `cdr_wrap_payload`/`cdr_unwrap_payload` CDR_LE encapsulation
   helpers ported from `message.go`), verified byte-for-byte against go-DDS reference
   vectors in `tests/test_rtps_cdr.cpp`. Internal to `cppdds_lib`, under `dds/rtps/` —
   not yet wired into `dds::IParticipant`/`relay::INode` or consumed by SPDP/SEDP
   (later phases).
3. [x] **UDP transport** — socket send/recv, the RTPS 2.3 §9.6.1 port-assignment formula
   (`metaMulticast(domain) = 7400 + 250*domain`, `metaUnicast/dataUnicast` offsets),
   multicast group `239.255.0.1`, platform-specific socket tuning (go-DDS splits this
   Linux vs. other via `traffic_linux.go` / `traffic_other.go`, 154 + 28 LOC — expect a
   similar `#ifdef __linux__` split in cpp-DDS). Reference: `transport.go` (205 LOC).
   **Done (v0.5.0):** `include/dds/rtps/transport.hpp` + `src/rtps/transport.cpp`
   (IPv4-only `UdpSocket`: unicast bind with go-DDS's port+0..+15 retry, multicast
   receive with unicast fallback, synchronous send/recv with a 250ms poll timeout) plus
   `include/dds/rtps/traffic.hpp` with a real `src/rtps/traffic_linux.cpp` (SO_PRIORITY /
   IP_TOS / SO_TXTIME / CLOCK_TAI via raw syscalls, `sendmsg` + `SCM_TXTIME` cmsg) and a
   no-op `src/rtps/traffic_other.cpp` for every other platform, matching go-DDS's own
   file split — CMakeLists.txt selects between them on `CMAKE_SYSTEM_NAME`. Builds and
   passes on Windows/MSVC (Winsock2) and macOS/Linux (BSD sockets); verified locally on
   macOS (AppleClang) and Linux/gcc-12 (Release + Debug ASan/UBSan, 161/161 tests,
   exercising the real Linux syscall path). Port formula verified against go-DDS's own
   `rtps/wire_test.go` `TestPortFormula` values plus additional vectors reproduced by
   calling go-DDS's actual `metaMulticastPort`/`metaUnicastPort`/`userUnicastPort`
   functions (`tests/test_rtps_transport.cpp` documents the exact reproduction steps).
   Internal to `cppdds_lib`, under `dds/rtps/` — not yet wired into
   `dds::IParticipant`/`relay::INode` or consumed by SPDP (phase 4, next).
4. [x] **SPDP** — Simple Participant Discovery Protocol (§8.5.3/§9.6.1): periodic (2s)
   multicast self-announcement plus a known-participants table. Reference: `spdp.go`
   (379 LOC). **Done (v0.6.0):** `include/dds/rtps/spdp.hpp` + `src/rtps/spdp.cpp`
   (`SpdpService`: announce/receive/eviction threads over `rtps::UdpSocket`, a
   known-peers table keyed by `GuidPrefix` with 10s-default lease-based eviction,
   configurable announce period/jitter) plus standalone
   `build_participant_data`/`parse_participant_data` (PL_CDR_LE `ParticipantProxy`
   encode/decode) and `wrap_in_rtps_message`/`build_spdp_announcement` (RTPS message
   framing) functions. New `kVendorIdCppDDS` in `types.hpp` for locally-originated
   messages. Verified byte-for-byte against go-DDS reference vectors (calling
   go-DDS's actual `buildParticipantData`/`parseParticipantData`/
   `wrapInRTPSMessage`/`marshalDataSubmessage` functions directly) plus behavioral
   tests (self-announcement filtering, non-SPDP-writer filtering, lease eviction,
   a live two-`SpdpService` loopback discovery test) in `tests/test_rtps_spdp.cpp`;
   verified locally with Release C++17/C++20 builds and a Debug ASan/UBSan pass on
   macOS/AppleClang, 177/177 tests. Scoped down from a full port of go-DDS's
   `spdpService` (a method on the not-yet-ported `participant` type, phase 6): the
   SEDP-notification hooks, liveliness callback, and optional `DiscoveryPlugin`
   authentication-token exchange are deliberately omitted — see spdp.hpp's
   file-level scope note. Internal to `cppdds_lib`, under `dds/rtps/` — not yet
   wired into `dds::IParticipant`/`relay::INode` or consumed by SEDP (phase 5, next).
5. [x] **SEDP** — Simple Endpoint Discovery Protocol (§8.5.4/§9.6.2): per-endpoint
   publication/subscription announcement sent unicast to every known participant's
   meta-unicast port; incoming announcements are topic-name matched against local
   endpoints to link readers to writers. Reference: `sedp.go` (343 LOC). **Done
   (v0.7.0):** `include/dds/rtps/sedp.hpp` + `src/rtps/sedp.cpp` (`SedpService`:
   local writer/reader registration and event-driven unicast announcement, a
   receive thread over `rtps::UdpSocket`, remote-endpoint tables keyed by `GUID`
   with topic-name matching, `on_new_peer`/`on_peer_evicted` hooks fed by SPDP's
   known-peers table) plus standalone `build_endpoint_data`/`parse_endpoint_data`
   (PL_CDR_LE `EndpointData` encode/decode) and `build_sedp_announcement` (RTPS
   message framing shared by publication and subscription announcements).
   Verified byte-for-byte against go-DDS reference vectors (calling go-DDS's
   actual `buildEndpointData`/`marshalDataSubmessage`/`wrapInRTPSMessage`
   functions directly) plus behavioral tests (self-announcement filtering, topic
   matching in both discovery orders, remote reader/writer locator tracking,
   peer eviction purge, a live two-`SedpService` unicast convergence test) in
   `tests/test_rtps_sedp.cpp`; verified locally with Release C++17/C++20 builds
   and a Debug ASan/UBSan pass on macOS/AppleClang, 195/195 tests. Scoped down
   from a full port of go-DDS's `sedpService` (a method on the not-yet-ported
   `participant` type, phase 6): rather than a live reference to `SpdpService`
   or a not-yet-existing `rtpsReader` type, this phase maintains its own
   known-peers table (fed by explicit `on_new_peer`/`on_peer_evicted` calls) and
   exposes matched-writer/locator query methods for phase 6 to consume — see
   sedp.hpp's file-level scope note. Internal to `cppdds_lib`, under
   `dds/rtps/` — not yet wired into `dds::IParticipant`/`relay::INode` or
   consumed by entities (phase 6, next).
6. [x] **Entities & history cache** — participant/writer/reader entity lifecycle and the
   per-endpoint HistoryCache that everything else plugs into. This is go-DDS's single
   largest file by far (`participant.go`, 1,505 LOC — more than a third of the whole
   `rtps` package) and is where phases 1–5 get wired together into working best-effort
   pub/sub. Expect cpp-DDS's equivalent to be its largest new file too; scope it as its
   own point release rather than folding it into the phase-4/5 release. **Done (v0.8.0):**
   `include/dds/rtps/participant.hpp` + `src/rtps/participant.cpp` (`dds::rtps::Participant`:
   a new, complete `dds::IParticipant` implementation over real RTPS/UDP — binds
   meta/data unicast sockets via the participant-index 0..15 retry go-DDS's
   `newParticipant` uses, owns an `SpdpService` + `SedpService` pair, bridges
   SPDP-discovered peers into SEDP via a poll loop that diffs `SpdpService::peers()`
   and calls the existing `on_new_peer`/`on_peer_evicted` hooks, and runs a data
   receive thread that decodes inbound DATA submessages and dispatches them to
   matching local readers) plus `include/dds/rtps/history_cache.hpp`
   (`dds::rtps::HistoryCache`: a bounded, sequence-number-indexed per-endpoint
   store, matching go-DDS's `maxHistoryDepth`-256 window — populated by every
   writer write, not yet consumed by anything since retransmission doesn't exist
   until phase 7). `Participant::new_publisher`/`new_subscriber` allocate
   `EntityId`s via the new `entity_id_for_writer`/`entity_id_for_reader`
   (`types.hpp`, matching go-DDS's kind-byte convention) and a random
   `new_guid_prefix()` (matching go-DDS's `newGuidPrefix`: random entropy +
   process-ID bytes). `Writer::write` composes only already byte-verified wire
   primitives from phases 1–2/4–5 (`DataSubmessage::encode`, `cdr_wrap_payload`,
   `wrap_in_rtps_message`) — this phase introduces no new wire encoding of its
   own — delivering unconditionally to local (same-participant) readers by topic
   name and, via a new `SedpService::matched_reader_locators_for_topic` query
   (the write-path mirror of the existing `matched_writer_guids_for_reader`),
   unicast to every SEDP-matched remote reader locator. Scope: **best-effort
   delivery only** — Reliable QoS is accepted but behaves identically to
   BestEffort until phase 7 lands; no fragmentation, loan integration, IPv6,
   security, or TSN (later phases/tiers); no INFO_TS-carried publish timestamps
   (`Sample::timestamp` is always local wall-clock time). `dds::rtps::Participant`
   is a new, separate `dds::IParticipant` implementation living alongside
   `dds::mock`'s — deliberately **not** wired into `dds::adapt()`'s default
   selection or any automatic-transport-selection surface (still the unchecked
   `dds/auto/` item below); callers construct it explicitly, exactly as they
   would construct `dds::mock::create(...)`. Verified with a live two-`Participant`
   test that exchanges a real sample over loopback UDP once SEDP-matched (feeding
   each `SedpService` a `ParticipantProxy` for the other directly, standing in for
   SPDP convergence — already covered at the `SpdpService` level in
   `test_rtps_spdp.cpp` — so this test exercises the phase-6-specific path: a
   real `Writer::write()` producing UDP bytes a real `Participant::data_loop()`
   receives, decodes, SEDP-matches, and delivers into a real `Reader`'s channel),
   plus same-process delivery, topic isolation, TransientLocal late-joiner
   delivery, QoS.max_sample_size enforcement, and the SPDP→SEDP bridge loop
   (221/221 tests). Verified locally with Release C++17/C++20 builds (the
   project's C++20 CI leg is affected by a pre-existing `CMakeLists.txt` quirk —
   `set(CMAKE_CXX_STANDARD 17)` unconditionally shadows any `-DCMAKE_CXX_STANDARD=`
   passed on the command line — so the new files were additionally verified to
   compile warning-clean under a genuine `-std=c++20` invocation directly) and a
   Debug ASan+UBSan pass on macOS/AppleClang, 221/221 tests; CI additionally
   exercises Linux/gcc-12 ASan+UBSan. Internal to `cppdds_lib`, under `dds/rtps/`
   — phase 7 (reliable delivery, next) depends on this phase's `HistoryCache`.
7. [x] **Reliable delivery** — HEARTBEAT/ACKNACK sliding-window retransmission (§8.4.9–
   §8.4.12): a reliable writer sends HEARTBEAT after every write and periodically,
   advertising its send-history window; a reliable reader tracks received sequence
   numbers and sends ACKNACK on a detected gap; the writer retransmits the requested
   range from history. Depends on phase 6's history cache. Reference: `reliable.go`
   (231 LOC) + `persist.go` (87 LOC, TransientLocal-style durability persistence).
   **Done (v0.9.0):** `Heartbeat`/`AckNack`/`Gap` submessage types added to
   `include/dds/rtps/types.hpp` + `src/rtps/types.cpp` (the well-known submessage-ID
   constants phase 1 already reserved for this phase), verified byte-for-byte against
   go-DDS reference vectors (calling go-DDS's actual `marshalHeartbeat`/
   `marshalAckNack`/`marshalGAP` functions directly) in the new
   `tests/test_rtps_reliable.cpp`. New `include/dds/rtps/reliable.hpp`
   (`dds::rtps::RecvTracker`, `sn_to_u64`/`u64_to_sn`, `kHeartbeatPeriod`,
   `kMaxReorderAhead`): a header-only C++ port of `reliable.go`'s *receiver*-side
   sliding-window gap tracker, matching `history_cache.hpp`'s existing header-only
   precedent — `reliable.go`'s *sender*-side `sendHistory` is deliberately not
   re-ported, since phase 6's `HistoryCache` already serves that role exactly as its
   own file-level scope note anticipated ("be the storage phase 7 wraps for
   retransmission"). New `include/dds/rtps/persist.hpp` + `src/rtps/persist.cpp`: a
   byte-for-byte port of `persist.go`'s `persistLoad`/`persistFlush`/`persistPath`
   file format (4-byte LE length prefix + payload at
   `<dir>/topic-<sanitised(topic)>.bin`). `include/dds/rtps/participant.hpp` +
   `src/rtps/participant.cpp` wire these together: `Writer` gains a per-instance
   background HEARTBEAT thread (started when reliable, joined in `close()`/
   `~Writer()`, period configurable via `ParticipantOptions::heartbeat_period`),
   sends HEARTBEAT immediately after every reliable write, and — on receipt of
   ACKNACK — retransmits every still-retained requested sequence number (re-encoding
   a fresh DATA submessage from the matching `HistoryCache::CacheChange`, broadcasting
   to every SEDP-matched reader locator, not just the ACKNACK sender) plus a GAP for
   any requested range already evicted from history, matching go-DDS's
   `handleAckNack` exactly — including its asymmetry that GAP is sent but never
   parsed on receipt, since go-DDS itself has no `parseGAP` either. `Reader` gains a
   `RecvTracker` per matched remote writer GUID, updated on every DATA arrival and
   HEARTBEAT receipt, sending ACKNACK back to the sender whenever a gap is detected —
   both `Reliable QoS` and `BestEffort QoS` share the same dispatch path (delivery is
   never blocked or reordered by reliability bookkeeping, matching go-DDS). A new
   `writers_` weak_ptr registry on `Participant` (mirroring the existing `readers_`
   registry) lets `Participant::close()` close every still-registered writer (stopping
   every heartbeat thread) before tearing down sockets, matching go-DDS's
   `participant.Close()` snapshot-and-close-every-`rtpsWriter` behavior.
   `ParticipantOptions::persist_dir` (this port's `WithPersistentHistory` equivalent)
   flushes every writer's every publish to disk when set, and a `TransientLocal`
   subscriber falls back to the on-disk copy when no in-memory `last_sample` exists
   yet. Verified with byte-exact HEARTBEAT/ACKNACK/GAP reference-vector tests,
   `RecvTracker`/persistence behavioral unit tests, and five end-to-end tests driving
   a real `Participant` over real loopback UDP against a raw-socket stand-in for the
   remote peer (gap-from-DATA and gap-from-HEARTBEAT detection + ACKNACK + delivery
   of the simulated retransmit; writer HEARTBEAT-after-write-and-periodic; writer
   retransmit-from-history on ACKNACK; writer GAP-for-evicted-range on ACKNACK) —
   248/248 tests, verified locally with Release C++17/C++20 builds and a Debug
   ASan/UBSan pass on macOS/AppleClang; CI additionally exercises Linux/gcc-12
   ASan+UBSan. `go-DDS`'s `waitDrain`/`CloseWithDrain` (blocking until all writes are
   ACKed) is out of scope — not required by this phase's roadmap text and not exposed
   anywhere yet. Internal to `cppdds_lib`, under `dds/rtps/` — not yet wired into
   `dds::IParticipant`'s public surface beyond `dds::rtps::Participant` itself, nor
   into any automatic-transport-selection surface.
8. [x] **Fragmentation** — `FragmentedData` submessages for payloads over 64 KB. Reference:
   `fragment.go` (231 LOC). **Done (v0.10.0):** `DataFrag` submessage type added to
   `include/dds/rtps/types.hpp` + `src/rtps/types.cpp` (the well-known
   `kSubmessageIdDataFrag` = 0x16 constant phase 1 already reserved a slot for),
   verified byte-for-byte against go-DDS reference vectors (calling go-DDS's actual
   `marshalDataFrag`/`splitIntoFragmentsN`/`wrapInRTPSMessage` functions directly) in
   the new `tests/test_rtps_fragment.cpp`. New `include/dds/rtps/fragment.hpp`
   (`dds::rtps::FragmentAssembler`, `split_into_fragments`/`split_into_fragmentsN`,
   `kMaxFragmentPayload`, `kMaxReassemblyBytes`, `kStaleFragAge`): a header-only C++
   port of `fragment.go`'s producer (`splitIntoFragments`/`splitIntoFragmentsN`) and
   receiver (`fragmentAssembler`), matching `reliable.hpp`'s existing precedent of
   keeping submessage wire types in `types.hpp` and their bookkeeping in their own
   header. `include/dds/rtps/participant.hpp` + `src/rtps/participant.cpp` wire these
   together: `Writer::write` fragments the CDR-wrapped payload into DATA_FRAG
   submessages whenever it exceeds `kMaxFragmentPayload` (matching go-DDS's `Write()`
   threshold check — cpp-DDS has no TSN writer yet, so unlike go-DDS's
   `fragmentSize()` this always uses the plain constant), `Writer::handle_ack_nack`
   re-fragments from `HistoryCache` on ACKNACK-triggered retransmit (a deliberate
   correctness improvement over go-DDS's own known limitation — its `sendHistory`
   retains only the first fragment's wire bytes for a fragmented write, so its own
   ACKNACK retransmit of a fragmented sample only ever resends fragment #1), and
   `Participant::handle_data_packet` gains a `kSubmessageIdDataFrag` case that
   decodes and reassembles incoming fragments via a new `Participant::frag_assembler_`
   member before dispatching — completing the round trip even though go-DDS's own
   `participant.go` never wires its own (otherwise fully working) `fragmentAssembler`
   into an equivalent switch case; see `fragment.hpp`'s file-level scope note for the
   full rationale on both deviations. `FragmentAssembler` keys reassembly by the full
   writer GUID (`GuidPrefix` + `EntityId`) plus the full 64-bit sequence number rather
   than go-DDS's own `EntityId` + low-32-bits-only key, a correctness improvement with
   no wire-format consequence (the key is never serialized). Verified with byte-exact
   `DataFrag`/`split_into_fragments_n` reference-vector tests, `FragmentAssembler`
   behavioral unit tests (in-order and out-of-order reassembly reproducing the go-DDS
   vectors, partial-reassembly, malformed/oversized rejection, cross-writer key
   isolation), and two end-to-end tests driving a real `Participant` over real
   loopback UDP (a 5000-byte `Writer::write` fragmented on send and reassembled by a
   second `Participant`'s real `data_loop()`; a reliable writer re-fragmenting a
   5000-byte payload correctly on ACKNACK from a raw-socket stand-in peer) —
   264/264 tests, verified locally with Release C++17/C++20 builds (plus the new
   files additionally compiled warning-clean under a genuine `-std=c++20` invocation
   directly, per phase 6's precedent for this repo's C++20 CI-leg quirk) and a Debug
   ASan/UBSan pass on macOS/AppleClang; CI additionally exercises Linux/gcc-12
   ASan+UBSan. Internal to `cppdds_lib`, under `dds/rtps/` — not yet wired into
   `dds::IParticipant`'s public surface beyond `dds::rtps::Participant` itself, nor
   into any automatic-transport-selection surface.
9. [x] **Loan integration** (stretch, can slip to a `v0.2.x` point release without blocking
   the rest of Tier 1) — zero-copy loaned-sample publishing wired into the RTPS writer
   path, backed by a pool allocator. Reference: `loan.go` (66 LOC); pairs with the
   `pool`/`ILoaningPublisher` work below. **Done (v0.12.0):** new
   `include/dds/pool/pool.hpp` (header-only): `dds::pool::BytePool`, a thread-safe
   fixed-capacity byte-buffer allocator — a C++ port of the `BytePool` portion of
   go-DDS's `pool/pool.go` (139 LOC; `SampleBuffer`, the other half of that file, is
   left for the separate, still-unchecked ddscore item below since loaned *writes*
   don't need it). New `include/dds/rtps/loan.hpp` declares
   `dds::rtps::new_loaning_publisher(participant, topic, qos, buf_size)`, a C++ port
   of go-DDS's `rtps/loan.go` `NewLoaningPublisher`; its `LoaningWriter`
   implementation of `dds::ILoaningPublisher` (`dds.hpp`'s pre-existing interface —
   see the ddscore item below) lives in `src/rtps/participant.cpp` alongside `Writer`,
   the one translation unit where that `.cpp`-local type is visible (see loan.hpp's
   file-level scope note for why this deviates from a literal separate-file port).
   `LoaningWriter::loan_buffer`/`write_loaned`/`return_loan` wrap a `Writer` plus a
   `BytePool`: `loan_buffer` rejects on a closed writer (a new `Writer::is_closed()`
   accessor) or an oversized request (`ErrLoanBuffer`), `write_loaned` calls the same
   already byte-verified `Writer::write` every plain publisher uses and returns the
   buffer to the pool, and `return_loan` discards a buffer without publishing —
   matching go-DDS's `loaningWriter.Loan`/`Commit` exactly, including its "no
   ownership validation on Commit" behavior (a documented caller contract, not
   enforced code, in both the Go and C++ interfaces). This phase introduces no new
   wire encoding: `write_loaned` composes only already-verified primitives, so a
   loaned publish is byte-identical on the wire to a plain `Writer::write` of the
   same payload. Verified with `BytePool` behavioral unit tests (capacity/reuse/
   undersized-discard/default-sizing/no-reallocation/concurrency, mirroring go-DDS's
   own `pool_test.go` coverage) plus loan-integration tests covering same-process
   loan/commit round-tripping, pool exhaustion, closed-writer rejection,
   discard-without-publish, direct `write()` passthrough, the two go-DDS-mirrored
   `NewLoaningPublisher` error paths (wrong participant type, publisher-creation
   failure), and an end-to-end test driving a loaned publish across two real
   `Participant`s over real loopback UDP once SEDP-matched — 279/279 tests, verified
   locally with Release C++17/C++20 builds (plus the new files additionally compiled
   warning-clean under a genuine `-std=c++20` invocation directly, per phase 6's
   precedent for this repo's C++20 CI-leg quirk) and a Debug ASan/UBSan pass on
   macOS/AppleClang; CI additionally exercises Linux/gcc-12 ASan+UBSan. Scope:
   internal, additive — `new_loaning_publisher` is NOT wired into `dds::adapt()` or
   any automatic-transport-selection surface, matching every prior RTPS phase; a
   mock-participant-backed `ILoaningPublisher` implementation remains out of scope
   here (see the still-unchecked ddscore item immediately below, which this phase
   only partially advances).
10. [x] **IPv6 / wildcard locators** (best-effort, non-gating) — go-DDS's own docs flag IPv6
    transport as having "limited interop testing"; treat cpp-DDS's IPv6 support the same
    way — implement it, don't gate a release on it. **Done (v0.13.0):** wildcard
    (0.0.0.0-address) locator fill-in was already implemented and tested as of phases
    4-5 (SpdpService/SedpService fill a received all-zero-address Locator's trailing 4
    bytes with the sender's real IPv4 address — see spdp.hpp/sedp.hpp's own scope notes),
    and Locator_t (types.hpp) has encoded/decoded UDPv6-kind values byte-identically to
    go-DDS since phase 1/2 (the 24-byte kind+port+address layout is kind-agnostic; see
    "Locator::encode matches go-DDS UDPv6 reference vector" in tests/test_rtps_types.cpp)
    — so this phase's actual new work is entirely at the socket/transport-primitive and
    participant-integration layers. `dds::rtps::UdpSocket` (transport.hpp/transport.cpp)
    gained `bind_unicast_v6`/`bind_multicast_receive_v6`, C++ ports of go-DDS's
    `newUnicastSocketV6`/`newMulticastReceiveSocketV6` (binds `[::]:port` with
    `IPV6_V6ONLY` set, matching Go's IPv6-only `"udp6"` network semantics exactly); a new
    `AddressFamily` tag records which family a given socket is bound to; `send_to`
    auto-detects the destination family from the address string itself, and `recv`
    reports the sender's address in the socket's own bound family (an IPv6 sender address
    is reported as uncompressed colon-hex — valid `inet_pton` input, not necessarily RFC
    5952 canonical form). `dds::rtps::Participant` gained `ParticipantOptions::ipv6`, a
    C++ port of go-DDS's `WithIPv6()` option — deliberately matching its exact (shallow)
    integration depth rather than an idealized dual-stack rewrite: when set, three
    additional IPv6 sockets are bound at the same RTPS §9.6.1 port numbers the IPv4
    sockets use (a UDP port is scoped per address family, so this isn't a collision),
    every bind failure is soft (participant creation still succeeds IPv4-only, matching
    go-DDS's own "Optional IPv6 sockets. Failures are soft" comment verbatim), and only
    the user-data socket is wired into a receive path (`data_loop_v6`, feeding the exact
    same `handle_data_packet` as the IPv4 path) — the SPDP-multicast and SEDP-meta IPv6
    sockets are bound-but-unconsumed, exactly matching go-DDS's own `mcastSockV6`/
    `metaSockV6` (bound in `newParticipant`, closed in `Close`, never read from anywhere
    else). Outbound replies/retransmits always go via the IPv4 socket regardless of which
    socket a packet arrived on, matching go-DDS's `participant.go` verbatim (every one of
    `handleDataPacket`/`handleAckNack`/`handleHeartbeat`'s sends unconditionally calls
    `p.dataSock.send(...)`, never `p.dataSockV6`) — this is a faithful port of a real,
    documented limitation, not an oversight; go-DDS's own IPv6 tests (`rtps_test.go`'s
    `TestRTPS_WithIPv6_StartsCleanly`, `rtps_coverage_test.go`'s
    `TestRTPS_WithIPv6_creates_participant`) likewise only assert that `WithIPv6()`
    doesn't prevent participant creation and that ordinary same-process pub/sub keeps
    working — this port's own tests mirror both, plus (going one step further than
    go-DDS's own coverage) a real end-to-end test that injects a wire-correct DATA
    submessage from a raw `::1` UdpSocket directly into a Participant's IPv6 data port
    and confirms it reaches a subscriber via `data_loop_v6`. SPDP/SEDP discovery and
    Locator advertisement remain IPv4-only on both sides, matching go-DDS's own
    `buildParticipantData`, which never advertises a UDPv6-kind Locator either — a peer
    has no way to learn this participant's IPv6 address regardless, so inventing
    IPv6-kind proxy advertisement here would just be untested, unreachable code (SEDP's
    `locator_to_dest` was, however, extended to *format* a UDPv6-kind Locator's address
    correctly for the send path, for symmetry with Locator's own generality and in case
    a genuinely wire-conformant peer ever supplies one — even though nothing in go-DDS
    exercises that branch today). 8 new tests (5 transport-level: ephemeral IPv6 bind,
    loopback IPv6 send/recv round-trip, cross-family send rejection, IPv6 multicast
    receive with unicast fallback, the `ff03::1` group constant; 3 participant-level: the
    two go-DDS-mirrored option-coverage tests plus the raw-socket wire-injection test) —
    287/287 tests, verified locally with Release C++17/C++20 builds (plus the changed and
    new files additionally compiled warning-clean under a genuine `-std=c++20`
    invocation directly, per phase 6's precedent for this repo's C++20 CI-leg quirk) and
    a Debug ASan/UBSan pass; CI additionally exercises Linux/gcc-12 ASan+UBSan. Scope:
    internal, additive — `ParticipantOptions::ipv6` is opt-in and off by default, and (as
    with every prior RTPS phase) none of this is wired into `dds::adapt()` or any
    automatic-transport-selection surface.

Suggested version sequencing (land discovery and best-effort delivery before reliable
delivery, since reliable QoS depends on phase 6's scaffolding):

- `v0.2.0` — phases 1–3 (wire types, discovery CDR, UDP transport)
- `v0.2.1` — phases 4–5 (SPDP + SEDP discovery converges between two cpp-DDS participants)
- `v0.2.2` — phase 6 (entities + history cache; best-effort pub/sub works end to end)
- `v0.2.3` — phase 7 (reliable delivery)
- `v0.2.4` — phases 8–10 (fragmentation, loan integration, IPv6)

(Actual tags diverged from this original `v0.2.x` suggestion once phase 1 landed —
each phase shipped as its own incrementing `v0.MINOR.0` release instead:
v0.3.0=phase 1, v0.4.0=phase 2, v0.5.0=phase 3, v0.6.0=phase 4, v0.7.0=phase 5,
v0.8.0=phase 6, v0.9.0=phase 7, v0.10.0=phase 8, v0.12.0=phase 9, v0.13.0=phase 10 —
see each phase's own "Done (vX.Y.0)" note above. v0.11.0 landed RTPS wire-level interop
testing infrastructure, a cross-cutting need rather than a numbered roadmap phase — see
that release's own CHANGELOG.md entry. Tier 1's ten dependency-ordered RTPS sub-phases
are now all complete.)

Also within `ddscore` but not RTPS-specific, carried forward from the previous roadmap
draft and small enough to slot in opportunistically alongside Tier 1 rather than blocking
it:

- [x] `IMetricsProvider` / `IDiscoveryMetricsProvider` / `ITopicMetricsProvider`
      implementation on mock and RTPS participants (go-DDS: these interfaces live in the
      core `dds` package, `dds.go`, implemented by `mock`, `rtps`, and exported by
      `monitor`/`admin` — Tier 5). **Done:** `dds::IMetricsProvider` / `dds::Metrics`,
      `dds::IDiscoveryMetricsProvider` / `dds::DiscoveryMetrics`, and
      `dds::ITopicMetricsProvider` / `dds::TopicMetrics` added to `dds.hpp`, mirroring
      go-DDS's `dds.go` field-for-field. Implemented by `dds::mock::IMockParticipant`
      (aggregate counters already tracked for `relay::IMetricsProvider` reused verbatim;
      `discovery_metrics()` always returns zero values — no real network discovery,
      matching go-DDS's mock doc comment verbatim; new per-topic counter table added for
      `topic_metrics()`) and `dds::rtps::Participant` (new participant-level aggregate
      atomics incremented in `Writer::write()`/`Participant::dispatch()`, mirroring
      go-DDS's `rtpsWriter.Write`/`participant.deliverToReader` counter split exactly;
      `discovery_metrics()` reports live `SpdpService`/`SedpService` counters that were
      already tracked and exposed since phases 4-5 — `announces_sent()`,
      `announces_received()`, `peers()`, `peer_evictions()`, `endpoint_matches()` — this
      phase just surfaces them through the new interface; new per-topic counter table
      keyed by reader topic, matching go-DDS's `topicCounterFor`). Because C++ virtual
      dispatch (unlike Go's structural interface satisfaction) cannot have two unrelated
      base classes both declare a `metrics()` method with different return types on the
      same derived class, and `dds::mock::IMockParticipant` already implements
      `relay::IMetricsProvider::metrics()` (RELAY spec §9.1, added in v1.13), the new
      DDS-package-scoped accessor is named `dds_metrics()` instead — see dds.hpp's
      "Metrics providers" section for the full rationale.
      `discovery_metrics()`/`topic_metrics()` have no RELAY-generic counterpart and keep
      the direct go-DDS names. 10 new tests (`tests/test_mock.cpp`,
      `tests/test_rtps_participant.cpp`) cover the interface hierarchy on both
      participants plus write/deliver/drop/byte counting, per-topic breakdown, and (RTPS)
      live discovery counters; verified locally with Release C++17 and a Debug ASan/UBSan
      pass (297/297 tests), 0 warnings under `-Wall -Wextra -Wpedantic`. Internal/additive
      only — not yet wired into `dds::adapt()`'s `relay::INode` bridge or the `cpp-dds`
      CLI's `optional_interfaces` capabilities list; that consumption/export layer is
      go-DDS's `monitor`/`admin` equivalent (Tier 5, still deferred).
- [x] `IHealthProvider` reporting participant and transport health (same source: go-DDS's
      core `dds` package, `dds.go`, implemented by `mock`, `rtps`, and exported by
      `monitor`/`admin` — Tier 5). **Done (v0.15.0):** `dds::HealthStatus`/`dds::Health`/
      `dds::IHealthProvider` added to `dds.hpp`, field-for-field mirrors of go-DDS's
      `dds.go` `HealthStatus`/`Health`/`HealthProvider` (`details` carries both
      participant- and transport-level state via a free-form JSON string, matching
      go-DDS's single-struct approach — no separate transport-health type exists in
      go-DDS either). Implemented by `dds::mock::IMockParticipant` (`dds_health()`
      mirrors go-DDS's `mock.participant.Health()` exactly, including its
      `{"state":"closed"}` details string) and `dds::rtps::Participant` (`dds_health()`
      mirrors go-DDS's `rtps.participant.Health()` exactly, folding live writer/reader
      counts into `{"writers":N,"readers":N}` while open). Because C++ virtual dispatch
      cannot have two unrelated base classes both declare a `health()` method with
      different return types on the same derived class, and `dds::mock::IMockParticipant`
      already implements `relay::IHealthProvider::health()` (RELAY spec §9.2), the new
      DDS-package-scoped accessor is named `dds_health()` instead — the same collision
      and naming precedent as `dds_metrics()` above. 7 new tests (`tests/test_mock.cpp`,
      `tests/test_rtps_participant.cpp`) cover the interface hierarchy on both
      participants plus OK/Down transitions and details-string content; verified locally
      with Release C++17 and a Debug ASan/UBSan pass (304/304 tests), 0 warnings under
      `-Wall -Wextra -Wpedantic`. Internal/additive only — not yet wired into
      `dds::adapt()`'s `relay::INode` bridge or the `cpp-dds` CLI's `optional_interfaces`
      capabilities list; that consumption/export layer is go-DDS's `monitor`/`admin`
      equivalent (Tier 5, still deferred).
- [ ] `IDrainer::close_with_drain()` on mock participant
- [x] `ILoaningPublisher` (zero-copy loan/commit) backed by a pool allocator;
      `ErrLoanBuffer` for exhausted or mismatched loans (go-DDS: `pool`, 139 LOC).
      **Done:** Tier-1 phase 9 ("Loan integration", v0.12.0) implemented
      `dds::ILoaningPublisher` once already, but scoped to the RTPS writer path only
      (`dds::rtps::new_loaning_publisher`/`LoaningWriter`) and ported only the
      `BytePool` half of go-DDS's `pool/pool.go` — this item completes both remaining
      pieces. `include/dds/pool/pool.hpp` gains `dds::pool::SampleBuffer`, a
      thread-safe fixed-capacity ring buffer of `dds::Sample` values, a C++ port of
      `pool.go`'s `SampleBuffer` (Push/Pop/Len/Cap ported as push/pop/len/cap,
      operating purely by value with no ownership divergence from go-DDS, unlike
      `BytePool`). New `include/dds/mock/loan.hpp` declares
      `dds::mock::new_loaning_publisher(participant, topic, qos, buf_size)`, a C++
      port of go-DDS's `mock/loan.go` `NewLoaningPublisher`; its
      `MockLoaningPublisher` implementation lives in `src/mock/participant.cpp`
      alongside `MockPublisher`, the one translation unit where that `.cpp`-local
      type is visible — the same translation-unit-boundary precedent
      `rtps/loan.hpp`/`participant.cpp` established for the RTPS side. Mirrors
      `LoaningWriter` exactly: `loan_buffer` rejects on a closed publisher (a new
      `MockPublisher::is_closed()` accessor) or an oversized request
      (`ErrLoanBuffer`), `write_loaned` calls the same `MockPublisher::write` every
      plain mock publisher uses and returns the buffer to the pool, `return_loan`
      discards a buffer without publishing, and `new_loaning_publisher` rejects a
      non-mock participant with `ErrLoanBuffer` (matching go-DDS's `mpub, ok :=
      pub.(*publisher)` type-assertion failure) or propagates `new_publisher`'s own
      error (e.g. `ErrClosed`) — matching go-DDS's `mock.loaningPublisher`/
      `NewLoaningPublisher` exactly, including its "no ownership validation on
      Commit" behavior. Verified with 9 new `SampleBuffer` behavioral unit tests
      (push/pop round-trip, pop-on-empty, push-on-full, wraparound ordering, len
      tracking, cap reporting, zero-size defaulting, full-then-drain, concurrency —
      mirroring go-DDS's own `pool_test.go` `TestSampleBuffer_*` coverage) plus 7 new
      mock-loan-integration tests (`tests/test_mock_loan.cpp`) covering same-process
      loan/commit round-tripping, pool exhaustion, closed-publisher rejection,
      discard-without-publish, direct `write()` passthrough, and the two
      go-DDS-mirrored `NewLoaningPublisher` error paths — 320/320 tests, verified
      locally with Release C++17/C++20 builds and a Debug ASan/UBSan pass on
      macOS/AppleClang; CI additionally exercises Linux/gcc-12 ASan+UBSan. Scope:
      internal, additive — `dds::mock::new_loaning_publisher` is NOT wired into
      `dds::adapt()` or any automatic-transport-selection surface, matching the RTPS
      side's own precedent.
- [ ] `dds/auto/` — automatic transport selection (shmem first, fall back to RTPS/UDP;
      go-DDS `auto`, 129 LOC) — only meaningful once both shmem and RTPS exist, so this
      is naturally a late Tier-1-adjacent item

## Tier 2 — safety and security (v0.3.0)

- [x] **`ddssafety` / E2E protection** — byte-compatible port of go-DDS's E2E wire header:
  `E2EPublisher` prepends an 18-byte protection header (little-endian: 2-byte DataID,
  plus CRC, sequence counter, and freshness fields) to every payload before writing;
  `E2ESubscriber` strips the header and validates CRC, sequence counter, and sample
  freshness on receipt (go-DDS `safety/e2e.go`, package ~658 LOC incl. tests). Wire
  compatibility with go-DDS's header format is required here for the same reason it's
  required for RTPS — this is cross-language interop, not just a local feature port.
  **Done (v0.17.0):** `dds::safety::E2EPublisher`/`E2ESubscriber`
  (`include/dds/safety/e2e.hpp`, `src/safety/e2e.cpp`) — byte-exact 18-byte header
  layout and CRC-16/CCITT-FALSE algorithm verified against reference vectors
  independently derived from a fresh go-DDS clone (see `tests/test_safety_e2e.cpp`).
  go-DDS's `safety.SafetyMetricsProvider`/`SafetyEvent`/monitor-integration surface
  (`safety/metrics.go`) is a separate, Tier-5-adjacent observability layer and is out
  of scope for this item, which covers the wire header plus
  CRC/sequence/freshness validation only.
- [x] **`security`** — HMAC-SHA-256 message authentication, AES-256-GCM encryption layer,
  topic ACL (per-participant/per-topic `Permission` bitfield), anti-replay sequence
  number enforcement (go-DDS `security/`, 639 LOC). Carried forward from the previous
  roadmap draft's v0.3.0 scope, now explicitly ordered as Tier 2 rather than Tier 3.
  **Done (v0.18.0):** `dds::security::Plugin`/`NullPlugin`/`HMACPlugin`/`AESGCMPlugin`
  (`include/dds/security/security.hpp`, `src/security/security.cpp`),
  `dds::security::AccessPolicy` (`include/dds/security/access.hpp`,
  `src/security/access.cpp`), and `dds::security::ReplayGuard`
  (`include/dds/security/replay.hpp`, `src/security/replay.cpp`) — byte-exact
  `HMACPlugin`/`AESGCMPlugin` wire formats (`plaintext || HMAC-SHA-256[32]` and
  `nonce[12] || AES-256-GCM ciphertext || tag[16]` respectively) verified against
  reference vectors independently derived from a fresh go-DDS clone (see
  `tests/test_security_hmac.cpp`, `tests/test_security_aesgcm.cpp`), plus
  independent FIPS/RFC/NIST known-answer cross-checks of the underlying
  SHA-256/HMAC/AES-256/GCM primitives. No external crypto dependency is fetched
  for this project (`cmake/FetchDeps.cmake` — Catch2 only), so SHA-256, HMAC,
  AES-256, and GCM are implemented from scratch as an internal, non-public detail
  under `src/security/crypto/`, verified byte-exact against Go's `crypto/aes` +
  `crypto/cipher` + `crypto/hmac` stdlib output (which is what go-DDS's plugins
  wrap) as well as the classic McGrew-Viega/NIST AES-256-GCM all-zero test
  vector and RFC 4231 HMAC-SHA-256 test vectors. `AccessPolicy`'s glob matching
  is a faithful byte-oriented port of Go's `path.Match` (behavioral parity, no
  wire format — see `tests/test_security_access.cpp`); `ReplayGuard` is a
  sliding-time-window anti-replay tracker (`tests/test_security_replay.cpp`).
  go-DDS's `security.cert`/`security.discovery` (PKI-based mutual authentication
  and DDS-discovery security wrapping, `security/cert.go` + `security/discovery.go`)
  are separate surfaces beyond this item's stated scope and are out of scope here,
  matching the `ddssafety`/E2E item's precedent of scoping out `safety/metrics.go`.
  Scope: internal, additive — `dds::security::Plugin` is a standalone seal/open
  library, not wired into `dds::adapt()`, `dds::mock`, or the RTPS transport,
  matching `dds::safety::E2EPublisher`'s own precedent.

## Tier 3 — xtypes, tsn, idl, cdr (v0.4.0)

- [x] **`xtypes`** — DDS-XTypes Dynamic Data support (go-DDS `xtypes/`, 460 LOC).
  **Done (v0.19.0):** `include/dds/xtypes/xtypes.hpp` + `src/xtypes/xtypes.cpp` —
  a faithful C++ port of go-DDS's `tools/xtypes/xtypes.go`: `TypeKind`/
  `FieldDescriptor`/`TypeDescriptor` (the type-descriptor model), `identify()`
  (content-addressed `TypeIdentifier`, order-independent over field
  declaration), `TypeObject`/`new_type_object()`, `DynamicData` (schema-
  validated property map with `to_json()`/`from_json()`), `TypeRegistry`
  (thread-safe, name-keyed, `ErrTypeMismatch` on structural conflict), and
  `check_compatibility()` (forward/backward schema-evolution rules).
  `TypeIdentifier::hash` is SHA-256 over a canonical JSON encoding of the
  descriptor, verified byte-for-byte against reference vectors independently
  derived from a fresh go-DDS clone (calling go-DDS's actual unexported
  `canonical()`/`canonicalField()`/`Identify()` functions directly, per this
  repo's established white-box vector-derivation convention) — including Go's
  default HTML-safe JSON string-escaping rule, confirmed against real go-DDS
  output rather than assumed. No external crypto dependency is fetched for
  this project, so SHA-256 is implemented from scratch under
  `src/xtypes/detail/` (an internal, non-public detail, independent of
  `dds::security`'s own scratch SHA-256 — that header is explicitly scoped to
  `security.cpp` only) — verified against FIPS 180-4 known-answer vectors and,
  end-to-end, against the go-DDS-derived hashes above. `DynamicData::from_json`
  fully parses and syntactically validates the entire input document before
  applying any field (matching go-DDS's `json.Unmarshal(&raw map[string]
  json.RawMessage)` up-front-parse behavior exactly, including failing under a
  malformed value hidden behind an unknown/skipped key) via a small internal
  recursive-descent JSON reader; unknown fields are silently skipped (forward
  compatibility) and JSON numbers always decode to `Value::Kind::Double`
  regardless of textual shape, matching Go's `json.Unmarshal`-into-`any`
  behavior exactly. Two deliberate, documented scope decisions vs. go-DDS's
  untyped `any`: nested JSON object/array values are syntactically validated
  but not stored, even under a declared field name (`dds::xtypes::Value` does
  not model arbitrary nesting — go-DDS's own test suite never exercises nested
  `DynamicData` field values either); and `Value::of_bytes()` follows Go's
  real, asymmetric `[]byte` JSON quirk faithfully — `to_json()` emits
  base64 (matching Go's marshal special-case for `[]byte`) but `from_json()`
  has no corresponding decode path (matching Go's `json.Unmarshal`-into-`any`,
  which never re-derives `[]byte` from a base64 string either). Not wired into
  RTPS discovery or `dds::adapt()` — this item's roadmap scope is the
  standalone type system and dynamic-data package only, matching every prior
  Tier-2/Tier-1-adjacent internal-library port's own precedent (`dds::safety`,
  `dds::security`). Verified with 36 new tests (`tests/test_xtypes.cpp`) —
  byte-exact canonical-JSON/hash reference-vector tests (mirroring
  `test_rtps_cdr.cpp`'s/`test_safety_e2e.cpp`'s derivation convention) plus
  full behavioral parity with go-DDS's own `xtypes_test.go` test matrix
  (`TypeKind.String`, `Identify` determinism/field-order-independence/
  struct/seq hashing, `NewTypeObject`, `DynamicData` set/get/JSON round-trip/
  unknown-field forward-compat/malformed-input rejection, `TypeRegistry`
  register/lookup/idempotent-re-registration/name-conflict/sorted snapshot,
  `CheckCompatibility`'s five rule cases) — 417/417 tests total, verified
  locally with Release C++17/C++20 builds (the new files additionally
  compiled warning-clean under a genuine `-std=c++20` invocation directly,
  per phase 6's precedent for this repo's C++20 CI-leg quirk) and a Debug
  ASan/UBSan pass on macOS/AppleClang; CI additionally exercises Linux/gcc-12
  ASan+UBSan. `REQ-XTYPE-001`..`006` and `REQ-TREG-001`..`003` added, traced
  and tested.
- [x] **`cdr`** — full XCDR1 encode/decode per OMG DDS-XTypes 1.3, for arbitrary
  IDL-defined user payload types (go-DDS `cdr/`, 348 LOC incl. tests) — general-purpose,
  and deliberately separate from Tier 1's discovery-only CDR subset (see Tier 1 phase 2).
  **Done (v0.20.0):** `include/dds/cdr/cdr.hpp` + `src/cdr/cdr.cpp` — a faithful C++
  port of go-DDS's `tools/cdr/cdr.go`: `Encoder`/`Decoder` for all CDR primitives
  (bool/int8/uint8/int16/uint16/int32/uint32/int64/uint64/float32/float64) with
  natural alignment, strings (4-byte-aligned uint32 length + UTF-8 bytes + null
  terminator), and byte sequences (4-byte-aligned uint32 count + raw bytes, no null
  terminator). `Encoder` prepends and `Decoder::create()` validates the 4-byte CDR_LE
  encapsulation header, accepting both the CDR_LE (0x0001) and CDR_BE (0x0000) scheme
  values on decode while always decoding little-endian, matching go-DDS's own
  `NewDecoder()` quirk exactly. Deliberately a SEPARATE library from Tier 1 phase 2's
  discovery-scoped `dds::rtps::PLCDREncoder`/`PLCDRDecoder` — not reused, not extended;
  go-DDS keeps `rtps/cdr.go` and top-level `tools/cdr/` as two independent packages and
  cpp-DDS mirrors that split with this new top-level `dds/cdr/`. Every method verified
  byte-exact against reference vectors independently derived from a fresh go-DDS clone
  (calling go-DDS's actual exported `Encoder`/`Decoder` methods directly from a scratch
  `_test.go` file, never committed upstream, per this repo's established white-box
  vector-derivation convention). Verified with 45 new tests (`tests/test_cdr.cpp`) —
  byte-exact reference-vector tests plus full behavioral parity with go-DDS's own
  `cdr_test.go` matrix (primitive/string/bytes round trips, alignment, multi-field
  struct simulation, encapsulation header acceptance/rejection, `Len`/`Remaining`
  introspection, buffer-underrun detection on every typed read) — 443/443 tests total,
  verified locally with a Release C++17 build (the new files additionally compiled
  warning-clean under a genuine `-std=c++20` invocation directly, per phase 6's
  precedent for this repo's C++20 CI-leg quirk) and a Debug ASan/UBSan pass on
  macOS/AppleClang; CI additionally exercises Linux/gcc-12 ASan+UBSan. `REQ-CDR-001`
  through `REQ-CDR-005` added, traced and tested. Landed as `include/dds/cdr/` +
  `src/cdr/`, internal to `cppdds_lib` — no CMake target-splitting yet (per this
  tier's own caveat, that happens once `ddstools`/`cppdds_tools` has multiple
  packages to house, not before).
- [x] **`idl`** — OMG IDL parser plus a C++ code generator, exposed via a standalone
  `ddstool` CLI target under `cppdds_tools` (go-DDS `idl/`, 1,382 LOC, plus
  `cmd/ddstool`, part of `cmd`'s 1,462 LOC — go-DDS's single largest package outside
  `rtps`).
  **Done (v0.21.0):** `include/dds/idl/idl.hpp` + `src/idl/parser.cpp` +
  `src/idl/gen.cpp` — a faithful C++ port of go-DDS's `tools/idl` package
  (`ast.go`/`idl.go`/`parser.go`/`gen.go`): a hand-written lexer/recursive-descent
  parser covering module (nestable)/struct/enum/typedef declarations, basic types,
  bounded/unbounded sequences, bounded strings, fixed-size arrays, qualified
  `Module::Name` references, and the `@key` field annotation; and a code generator
  emitting a self-contained C++ header — a struct/`enum class`/`using`-alias per IDL
  declaration (field/type names verbatim, no PascalCase conversion) plus a
  `<Name>Codec` per struct with `marshal()`/`unmarshal()` built on
  `dds::cdr::Encoder`/`Decoder` (this tier's own `cdr`, v0.20.0) and `key_fields()`,
  nested struct/enum references inlined recursively at the call site with no wrapper
  header (matching CDR wire semantics and go-DDS's own generator exactly) and a cycle
  guard for self-referential struct graphs. Exposed via a new standalone `ddstool` CLI
  executable (`ddstool/cli.hpp`+`cli.cpp`+`main.cpp`, `ddstool idl [-out <file>]
  [-namespace <name>] <input.idl>`) — distinct from the existing `cpp-dds`
  RELAY-conformance CLI under `cli/`, per this item's own scope, and landed as a plain
  top-level `ddstool/` directory (the full `cppdds_tools` target-group split, like
  `cdr`'s/`xtypes`'s own precedent, is deferred until the group has enough members to
  warrant it — see this tier's own caveat above `cdr`). Two behaviors were
  independently confirmed against a fresh go-DDS clone before porting, since neither is
  stated in go-DDS's own `idl_test.go` matrix: `Generate`'s emitted package/namespace
  name is always `idlgen` unless the caller overrides the root `Module`'s name first
  (a top-level `module X { ... }` declaration becomes a *sub*-module, never the root
  itself), and a parenthesized annotation argument (`@id(5)`) is a parse error rather
  than being silently accepted (go-DDS's lexer has no `(`/`)` token, so its
  `(...)`-skip branch in `parseStruct`'s annotation loop never actually triggers) —
  both faithfully preserved here, not "fixed". A related generator-only divergence was
  found and deliberately kept: go-DDS's own `Generate()` actually *fails* on a
  self-referential-via-sequence struct (a `// TODO: ... (cyclic struct)` comment lands
  mid-line in its semicolon-joined single-line field-expansion style and comments out
  a loop's closing `}`, leaving gofmt-rejected unbalanced braces) — this port emits one
  statement per source line, so the analogous comment never swallows subsequent code
  and generation succeeds with valid (if functionally incomplete for the guarded
  field) C++; treated as an accidental go-DDS formatting bug, not a semantic to
  reproduce. Two further deliberate, documented scope differences from the go-DDS
  reference generator: C++ field/struct/enum names are emitted exactly as written in
  the IDL source (no PascalCase conversion — C++ has no public/private-by-case rule to
  satisfy), and no `TypedPublisher`/`TypedSubscriber` factory functions are emitted
  (cpp-DDS has no `dds::Codec<T>`/`TypedPublisher<T>` abstraction yet to target); and
  `ddstool` itself scopes down to the `idl` subcommand only (go-DDS's reference
  `cmd/ddstool` also has `pub`/`sub`/`discover`, out of scope for this item — cpp-DDS's
  own `cpp-dds` CLI under `cli/` already covers protocol-level conformance concerns).
  Every `marshal()` byte sequence verified byte-exact against reference vectors
  independently derived from a fresh go-DDS clone (calling go-DDS's actual generated
  `HeaderCodec{}.Marshal`/`TelemetryCodec{}.Marshal` — themselves produced by go-DDS's
  real `idl.Generate()` — from a scratch `_test.go` file, never committed upstream, per
  this repo's established white-box vector-derivation convention). A checked-in
  round-trip fixture (`tests/idl_roundtrip/schema.idl` + `schema_gen.hpp`, the latter
  produced by actually running the `ddstool` binary this item adds, not hand-written)
  mirrors go-DDS's own `tools/idl/roundtrip/` package shape field-for-field, giving
  genuine end-to-end confidence the generator's *output* both compiles and behaves
  correctly, not just that its source text contains the right tokens. Verified with
  101 new tests — `tests/test_idl.cpp` (44, parser/generator behavior mirroring
  go-DDS's `idl_test.go` matrix plus the independently confirmed behaviors above and
  additional parse-error/cycle-guard coverage), `tests/test_ddstool_cli.cpp` (13,
  subcommand dispatch/flag parsing/stdout-vs-`-out`/error paths, exercised in-process
  via a `ddstool_cli` static library rather than subprocess spawn), and
  `tests/idl_roundtrip/test_idl_roundtrip.cpp` (18, byte-exact vectors, round trips,
  `key_fields()`, buffer-truncation rejection at every field boundary, mirroring
  go-DDS's `roundtrip/schema_test.go` matrix) — 500/500 tests total, verified locally
  with Release C++17 and C++20 builds (the new files additionally compiled
  warning-clean under a genuine `-std=c++20` invocation directly, per phase 6's
  precedent for this repo's C++20 CI-leg quirk) and a Debug ASan/UBSan pass on
  macOS/AppleClang; CI additionally exercises Linux/gcc-12 ASan+UBSan. `REQ-IDL-001`
  through `REQ-IDL-009` added, traced and tested.
- [x] **`tsn`** — TSN (802.1) QoS fields: transport priority, latency budget, TAPRIO
  integration. go-DDS's `tsn` package is Linux-specific with a `!linux` build-tag stub
  for other platforms — cpp-DDS follows the same platform-gated pattern, not attempting
  TAPRIO on macOS or Windows.
  **Done (v0.22.0):** `include/dds/tsn/tsn.hpp` + `src/tsn/tsn.cpp` — `dds::tsn::Stream`/
  `StreamConfig`, a C++ port of go-DDS's `tsn/tsn.go`, with `load_config()`/
  `parse_config()` parsing the identical JSON `tsn_streams` shape (topic/vid/pcp/dscp/
  max_frame_size/max_interval_frames/interval_us/tx_offset_us/talker_id) via a small
  internal recursive-descent JSON reader scoped to exactly this config shape (not a
  general-purpose library dependency, matching `dds::idl`'s own lexer/`cli/json_lite.hpp`
  precedent), plus `to_json()` for round trips. `include/dds/rtps/tsn.hpp` mirrors
  go-DDS's `rtps/tsn.go` split exactly: a dependency-leaf `TSNParams`/`TSNStreamConfig`
  interface inside `dds::rtps` itself, so `dds::rtps` never depends on `dds::tsn`;
  `dds::tsn::StreamConfigAdapter`/`with_stream_config()` is the supported adapter,
  assigned to the new `ParticipantOptions::tsn_config` field. `include/dds/tsn/
  taprio.hpp` + `src/tsn/taprio.cpp` — `TAPRIOEntry`/`TAPRIOConfig` (gate control list,
  `cycle_duration()`/`validate()`/`tc_command()`) plus `taprio_from_streams()`, all
  portable; `src/tsn/taprio_linux.cpp` (compiled only when `CMAKE_SYSTEM_NAME` is
  `"Linux"`, mirroring Tier-1 phase 3's `traffic_linux.cpp`/`traffic_other.cpp`
  `CMakeLists.txt` split) programs and verifies the real Linux `taprio` qdisc via
  hand-built `RTM_NEWQDISC`/`RTM_GETQDISC` netlink messages over `NETLINK_ROUTE` — a
  byte-for-byte port of go-DDS's `tsn/taprio_linux.go` attribute layout (TCA_KIND/
  TCA_OPTIONS/TCA_TAPRIO_ATTR_\*/TCA_TAPRIO_SCHED_ENTRY_\* IDs, NLM flags, message
  ordering all cross-checked field-by-field against a fresh go-DDS clone — there is no
  RTPS wire format involved here, so netlink-attribute-layout exactness is this item's
  equivalent of a byte-exact vector); `src/tsn/taprio_other.cpp` returns a documented
  "requires Linux" diagnostic everywhere else. Neither `apply()` nor any other TSN code
  is called automatically by `dds::rtps::Participant` for qdisc configuration — matching
  go-DDS's own participant.go, which never calls `TAPRIOConfig.Apply` either; that stays
  an explicit, privileged, out-of-band operator action. Real `Participant`/`Writer`
  wiring (not just types): `ParticipantOptions::tsn_config` set → `new_publisher`
  resolves each writer's topic against it (config match takes priority,
  `QoS::transport_priority` — a field that already existed in `dds.hpp` — is the
  fallback PCP selector when no entry matches, exactly mirroring go-DDS's `NewPublisher`
  "Wire TSN stream" block) and lazily creates/reuses one dedicated `UdpSocket` per
  distinct PCP value (`Participant::tsn_socket_for_pcp`, matching go-DDS's
  `tsnSocketForPCP`), configured via the *already-existing* (Tier-1 phase 3)
  `dds::rtps::traffic.hpp` hooks (`set_sock_priority`/`set_sock_tos`/`enable_tx_time`) —
  those hooks had no caller anywhere in the codebase before this item, by design (see
  `traffic.hpp`'s own file-level scope note: "these hooks are unused... they exist for
  later TSN integration... to call into"). `Writer::write()`/`send_heartbeat()` route
  through that dedicated socket via a new `send_via` helper (falling back to the shared
  `data_sock_` when unconfigured) and, for a stream with a nonzero `tx_offset`, compute a
  `SO_TXTIME` launch time (next interval boundary + `tx_offset`, `CLOCK_TAI`) and send via
  `scheduled_send` — matching go-DDS's `Write()`/`sendHeartbeatLocked()` exactly, down to
  ACKNACK-triggered retransmission and GAP deliberately *not* routing through the TSN
  socket (confirmed against a fresh go-DDS clone: `handleAckNack` unconditionally uses
  `p.dataSock`, never `sendSock()`). A TSN stream's `max_frag_payload()` (`max_frame_size
  - 48`, 0 when unset) overrides `fragment.hpp`'s default `kMaxFragmentPayload` as this
  writer's fragmentation threshold end-to-end (`build_data_messages` gained a `frag_size`
  parameter). Verified with 4 new cross-participant, real-loopback-UDP end-to-end tests
  (`tests/test_rtps_tsn.cpp`) proving delivery still succeeds correctly when routed
  through the dedicated TSN socket (config match, `QoS::transport_priority` fallback,
  TSN-forced `DATA_FRAG` fragmentation and reassembly, and PCP-socket sharing across
  writers) plus 45 unit tests (`tests/test_tsn.cpp` mirroring go-DDS's `tsn_test.go`
  matrix, `tests/test_taprio.cpp` mirroring `taprio_test.go` plus `tc_command`/
  `taprio_from_streams` coverage) — 540/540 tests total, verified locally with Release
  C++17/C++20 builds and a Debug ASan/UBSan pass on both macOS and a real Ubuntu-22.04/
  gcc-12 Docker container (exercising the actual Linux netlink code path, not just the
  portable stub); CI additionally exercises Linux/gcc-12 ASan+UBSan and the full
  cross-platform matrix (ubuntu/macos/windows). `REQ-TSN-001` through `REQ-TSN-007`
  added, traced and tested. This is the last remaining item of Tier 3 (`xtypes` v0.19.0,
  `idl` v0.21.0, `cdr` v0.20.0, `tsn` v0.22.0 — all now done).

## Tier 4 — bridges (v0.5.0)

go-DDS itself currently ships only `grpc`, `wan`, and `rest` bridges (`bridge/`, 1,235
LOC total) — `mqtt` and `domain` bridges named in RELAY#59's draft registry (`mqttbr`,
`domainbr`) don't exist upstream in go-DDS yet either. cpp-DDS's Tier 4 target is
therefore grpc + wan + rest first, matching what the reference implementation actually
has, with mqtt/domain added opportunistically once go-DDS lands them rather than
inventing a bridge spec ahead of the reference implementation.

- [x] **`grpc`** — gRPC gateway bridging a `dds::IParticipant` to RPC clients over
  JSON-encoded messages (go-DDS `bridge/grpc`, 586 LOC across `grpc.go`+`config.go`).
  **Done (v0.23.0):** `include/dds/bridge/grpc/{grpc.hpp,transport.hpp,config.hpp}` +
  `src/bridge/grpc/{grpc.cpp,transport.cpp,config.cpp}` — a C++ port of go-DDS's
  `bridge/grpc` package, landed as the first member of a new `cppdds_bridges` CMake
  target (opt-in via `CPPDDS_BUILD_BRIDGES`, default `ON`), the first concrete
  materialization of the `ddsbridges` group proposed in the "Target architecture —
  5-library split" table above — `cppdds_lib` itself is untouched/unrenamed. Message
  types (`SubscribeRequest`, `PublishRequest`, `Sample`, `PublishAck`) and their JSON
  codec (`to_json()`/`from_json()`) are field-for-field and byte-exact with go-DDS's
  `encoding/json.Marshal` output — object key order, HTML-safe `<`/`>`/`&` escaping,
  standard padded base64 for byte fields — verified against reference vectors captured
  from a real go-DDS process (a throwaway `go run` program against a fresh
  `github.com/SoundMatt/go-DDS` clone calling the actual `bridge/grpc` struct types,
  per this repo's established white-box vector-derivation convention), including the
  one documented codec gap: Go's nil-vs-empty-`[]byte` distinction (`null` vs. `""`)
  isn't representable in `std::vector<uint8_t>`, so the encoder always emits `""` and
  the decoder accepts both. `Bridge` implements all three RPCs as plain C++ methods
  (`subscribe`/`publish`/`stream_publish`) against `SampleSender`/`PublishReceiver`
  abstractions — enough surface to unit-test business logic with in-memory doubles,
  mirroring go-DDS's own `grpc_internal_test.go` mock-stream pattern, independent of
  any networking — plus lazy subscriber/publisher caching, `Options`
  (`auth_token`/`qos`/`filter`/`transform`), and `check_auth()`, all behavior-matched
  to go-DDS's `Bridge`/`checkAuth`/`FilterFunc`/`TransformFunc`. `transport.hpp`'s
  `Server`/`Client` speak a real, working TCP wire protocol — a text header block
  (RPC method name + optional `authorization: Bearer <token>`) followed by message
  bodies framed with real gRPC length-prefixed-message framing (1-byte compression
  flag + 4-byte big-endian length) — rather than a real HTTP/2 gRPC transport: this
  repo has no HTTP/2 or protobuf dependency anywhere (every other module hand-rolls
  its wire protocol rather than adopting a matching reference library — RTPS, CDR,
  xtypes, TSN netlink), so adding grpc++ (itself dependent on protobuf and abseil)
  for this one bridge would be a large, slow-to-build outlier; true stock-grpc-go/
  grpc++ wire interop is deliberately deferred, exactly like the CycloneDDS backend
  and the WAN bridge's TLS deep dive below — documented in full in `grpc.hpp`'s
  file-level scope note, along with the `Options::qos`/`checkAuth` behavioral
  simplifications the C++ port makes (both structurally forced by the language, not
  fidelity gaps in anything client-observable). `config.hpp`/`config.cpp` hand-roll a
  minimal YAML-subset parser (top-level scalar keys plus one block-form `topics:`
  list) rather than a general YAML library dependency, matching this module's own
  "own small parser" convention — `TopicConfig::effective_qos()` deliberately starts
  from `dds::default_qos()` rather than go-DDS's raw zero-value `dds.QoS{Reliability:
  ...}` literal (`HistoryDepth==0`, a field cpp-DDS's own `rtps` package doesn't
  consume either — see `include/dds/rtps/history_cache.hpp`), a documented low-risk
  improvement over reproducing what reads as an accidental Go zero-value default.
  Verified with 41 new tests (`tests/test_bridge_grpc.cpp`) — byte-exact JSON
  reference-vector tests, `Bridge` unit tests against in-memory `SampleSender`/
  `PublishReceiver` doubles covering every RPC's success/error/auth/filter/transform
  path (mirroring go-DDS's own `grpc_test.go`/`grpc_internal_test.go` scenario
  matrix), `Config` load/apply tests, and real loopback-TCP end-to-end tests
  (`Server`+`Client`) covering Publish/Subscribe/StreamPublish, auth accept/reject,
  and idempotent `Server::stop()` unblocking a live Subscribe call — 581/581 tests
  total, verified locally with Release C++17/C++20 builds and a Debug ASan+UBSan pass
  (clang, macOS) with zero new warnings under `-Wall -Wextra -Wpedantic`. `REQ-BRIDGE-
  GRPC-001` through `REQ-BRIDGE-GRPC-011` added, traced and tested.
- [x] **`wan`** — WAN bridge (go-DDS `bridge/wan`). Baseline scope (TLS deep dive is
  Tier 4's explicitly-deferred stretch item, see "Future" below). Done (v0.24.0):
  `dds::bridge::wan::Bridge`, a second member of `cppdds_bridges`, ported one-for-one
  from `wan.go` — `Bridge::serve` accepts TCP connections and publishes received
  samples to a local `dds::IParticipant`, with a fresh per-connection publisher cache
  (mirroring go's per-call `receiveLoop` map exactly); `Bridge::connect` synchronously
  subscribes to `Options::topics` before dialing (closing every already-created
  subscription on a partial failure, matching `Connect`'s cleanup order exactly) and
  streams each topic's samples to the server from an independent per-topic sender
  thread. Wire format is byte-exact with go's `encoding/json.Marshal` output for the
  unexported `wireFrame` struct — a 4-byte big-endian length prefix (capped at 16 MiB,
  `ErrFrameTooLarge`) followed by `{"t":"<topic>","p":"<base64-payload>"}` — verified
  against reference vectors captured from a real go-DDS process (a throwaway `go test`
  program against a fresh clone, `github.com/SoundMatt/go-DDS` v0.63.0, since
  `wireFrame` is unexported). Per the roadmap's own scope note, the shared-token auth
  path is in scope alongside the framing it depends on: `Options::token`, when set, is
  sent as the client's first frame (`write_auth`/`read_auth`, capped at 4096 bytes) and
  constant-time compared server-side (mirroring `crypto/subtle.ConstantTimeCompare`
  exactly, including its own documented immediate-`false`-on-length-mismatch behavior),
  silently dropping the connection on a mismatch; a real TLS/mTLS transport remains the
  explicitly-deferred stretch item (see "Future" below). Per-topic sample fan-out uses
  one sender thread per topic polling `recv_until` on a short deadline rather than a
  native multi-channel select (`relay::Channel<T>` has none — the same pattern already
  established by `dds::bridge::grpc::Bridge::subscribe`), with a write failure on any
  sender closing every topic's subscriber to unblock the others, mirroring go's
  `closeAllSubs()`. Testing two independently-addressable participants in one process
  (the client's local domain and the server's local domain) surfaced a real conformance
  gap versus go-DDS's `mock` package: `dds::mock::create` had no equivalent of
  `mock.IsolatedBroker()`, so a same-process client/server pair sharing the global mock
  broker would echo samples directly between them, bypassing the bridge under test
  entirely (exactly the race go-DDS's own doc comment warns about) — fixed by adding
  `dds::mock::create`'s own `isolated_broker` parameter (`REQ-MOCK-006`, additive/
  default-`false`, every existing call site unaffected), verified by its own
  `TestIsolatedBroker_NoEcho`-equivalent test. Verified with 34 new tests
  (`tests/test_bridge_wan.cpp`) — byte-exact JSON reference-vector tests, an
  internal-test-style frame/auth codec suite against in-memory `WriteFn`/`ReadFn`
  doubles (mirroring `wan_internal_test.go`), and real loopback-TCP end-to-end tests
  covering forwarding (single- and multi-topic), auth accept/reject, oversized-frame and
  malformed-JSON rejection, topic filtering, and cleanup-on-partial-failure — 617/617
  tests total, verified locally with Release C++17/C++20 builds and a Debug ASan+UBSan
  pass (gcc 16, macOS) clean. `REQ-BRIDGE-WAN-001` through `REQ-BRIDGE-WAN-007` and
  `REQ-MOCK-006` added, traced and tested.
- [x] **`rest`** — REST bridge (go-DDS `bridge/rest`). **Done (v0.25.0):**
  `dds::bridge::rest::Bridge` + `dds::bridge::rest::Server`, a third member of
  `cppdds_bridges`, ported one-for-one from `rest.go` — `GET /topics` returns a JSON
  array of currently-subscribed topic names, `GET /topics/{topic}` opens a Server-Sent
  Events (SSE) stream of samples published on that topic, and `POST /topics/{topic}`
  publishes the request body as one sample. Split the same way as the grpc bridge
  (`grpc.hpp`/`transport.hpp`): `rest.hpp`/`rest.cpp` hold pure business logic (JSON/SSE
  codec, `classify_request` routing, `Bridge`'s lazy subscriber/publisher caching,
  `authorize()`, and `run_sse_loop`/`handle_publish` driven through an abstract
  `SseSink` — enough surface to unit-test directly with an in-memory double, mirroring
  go-DDS's own `grpc_internal_test.go` mock-stream pattern, independent of any
  networking); `transport.hpp`/`transport.cpp` hand-roll a minimal but genuinely valid
  HTTP/1.1 request/response parser+writer over raw TCP sockets (this repo has no HTTP
  library dependency anywhere — every other module hand-rolls its own wire protocol:
  RTPS, CDR, xtypes, TSN netlink, and the grpc/wan bridges' own framing). Unlike the
  grpc bridge (which deliberately does *not* speak real HTTP/2), this transport is a
  byte-exact-verified HTTP/1.1 server: Content-Length-framed unary responses for the
  topic list/publish/error paths, and `Transfer-Encoding: chunked` for the SSE stream
  (one chunk per sample or keepalive comment, terminated by a final zero-length chunk),
  matching a real go-DDS process's actual wire bytes exactly — captured via a raw
  `net.Dial` (not `net/http`'s client, which transparently de-chunks responses) against
  a fresh `github.com/SoundMatt/go-DDS` clone, per this repo's established
  vector-derivation convention. The one deliberate simplification, carried over from the
  grpc bridge's own precedent, is "one HTTP request per TCP connection" — no
  keep-alive/pipelining of multiple requests on the same connection. `topics_to_json()`
  (a compact JSON array of HTML-safe-escaped topic name strings, matching Go's default
  `escapeHTML=true`, with the trailing `"\n"` `encoding/json.Encoder.Encode` appends)
  and the SSE `"id: <n>\nevent: message\ndata: <base64>\n\n"` / `": keepalive\n\n"` wire
  formats are verified against reference vectors captured from a real go-DDS process,
  including go's actual (if oddly documented) `Options.keepalive()` behavior: an
  explicit zero does *not* disable the keepalive, despite its doc comment's claim
  otherwise — `effective_keepalive()` reproduces the real function, not the misleading
  comment. `classify_request` reproduces go's `strings.TrimPrefix`-based `ServeHTTP`
  routing byte-for-byte, including its documented quirks (`GET /topics/` also lists
  topics; a path not literally starting with `/topics` is still routed by its
  remainder) and its one practically-unreachable `Route::bad_request` branch (kept for
  fidelity, untested for the same reason go-DDS's own equivalent is untested — no input
  can reach it). One documented, low-risk departure from a literal port: go's
  `handlePublish` silently truncates a request body over its 16 MiB cap
  (`io.LimitReader` never errors); every other wire boundary in this repo treats an
  oversized declared length as a rejected error instead of a silent truncation
  (`dds::bridge::wan`'s `ErrFrameTooLarge`, `dds::bridge::grpc::transport`'s
  `kMaxFrameSize`), so this bridge does too. Verified with 48 new tests
  (`tests/test_bridge_rest.cpp`) — byte-exact JSON/SSE reference-vector tests,
  `classify_request` routing-quirk unit tests, `Bridge` business-logic unit tests against
  a real (no-socket) mock participant, `run_sse_loop` unit tests against an in-memory
  `SseSink` double covering delivery/keepalive/cancellation/write-failure paths, and real
  loopback-TCP end-to-end tests (`Server`) covering the topic list, publish, SSE chunked
  streaming, auth accept/reject, method-not-allowed, oversized/truncated bodies,
  malformed request lines, and idempotent `Server::close()` unblocking a live SSE
  stream — 665/665 tests total, verified locally with Release C++17/C++20 builds and a
  Debug ASan+UBSan pass clean. `REQ-BRIDGE-REST-001` through `REQ-BRIDGE-REST-009` added,
  traced and tested. This is the last item of Tier 4 (`grpc` v0.23.0, `wan` v0.24.0,
  `rest` v0.25.0 — all now done).

## Tier 5 — observability (v0.6.0)

`otel`/`admin`/`monitor`/`record`/`services` equivalents (go-DDS: `otel` 64 LOC, `admin`
197 LOC, `monitor` 504 LOC, `record` 395 LOC, `services` 231 LOC — 1,391 LOC total).
Lowest priority per the agreed tier order; scope in detail only after Tiers 1–4 land,
since these packages largely consume the `IMetricsProvider`/`IHealthProvider` surface
scoped under Tier 1.

---

## Interop testing infrastructure (new need — not just golden vectors)

RELAY's `interop` CLI command and golden vectors (e.g. the embedded `dds-sample` vector
used by `convert --protocol DDS`, already gated in CI via the `relay-conform` job per
§20.1/§20.2) validate **Message-level conversion**: `dds.Sample` ⇄ `relay.Message` JSON
round-tripping. That's necessary and already works today, but it exercises zero RTPS wire
bytes — it's a serialization-contract test, not a network-protocol test. Tier 1 needs a
different kind of testing infrastructure entirely:

- **A live second RTPS implementation on the wire.** go-DDS is the natural first target
  since it already speaks pure-Go RTPS with no CGo dependency: bring up a go-DDS
  participant and a cpp-DDS participant on the same host/domain and verify SPDP/SEDP
  discovery converges and samples cross in both directions.
- **Mirror go-DDS's own `interop` package pattern.** go-DDS gates its CycloneDDS
  wire-compat tests behind a `go:build interop` tag (`interop/interop_test.go`,
  `interop/doc.go`), documents prerequisites (a live CycloneDDS peer, reachable via
  `interop/docker-compose.yml`), and configures domain/timeout via environment
  variables (`INTEROP_DOMAIN`, `INTEROP_TIMEOUT`). cpp-DDS needs the CMake/CTest
  equivalent: a `CPPDDS_INTEROP_TESTS` option (default `OFF`) producing a separately
  labeled CTest suite that is excluded from the default `ctest` invocation, plus its own
  `docker-compose.yml` bringing up a CycloneDDS peer.
- **A dedicated, opt-in CI job.** Real UDP sockets and multicast don't fit the existing
  build-and-test matrix unmodified — this needs network-capable CI (host or bridge
  networking) and should run as its own job (or a scheduled/nightly job, matching how
  go-DDS keeps it out of the default `go test` run) rather than blocking every PR.
- **Eventual 3-way matrix**: cpp-DDS ⇄ go-DDS, cpp-DDS ⇄ CycloneDDS, cpp-DDS ⇄ rust-DDS
  (once it exists). Land cpp-DDS ⇄ go-DDS first — both are RELAY-authored, so protocol
  mismatches are debuggable from either side.

- [x] **Done (v0.11.0):** the CycloneDDS-peer harness — go-DDS's own first interop
  target and reference pattern — is built, under a new `interop/` directory:
  `interop/test_interop.cpp` brings up a real (non-`test_mode`, real-multicast) `dds::
  rtps::Participant` and exercises three directions against a live CycloneDDS peer
  (cpp-DDS publisher → CycloneDDS subscriber, CycloneDDS publisher → cpp-DDS
  subscriber, and a full bidirectional echo), skipping — not failing — when no peer
  container answers within `INTEROP_TIMEOUT`, since a missing peer is a setup problem,
  not a wire-compatibility regression. `interop/docker-compose.yml` brings up the
  CycloneDDS peer (`cyclone-peer`, plus single-direction `cyclone-sub`/`cyclone-pub`
  profile services), same shape as go-DDS's own `interop/docker-compose.yml`
  (`network_mode: host` for real SPDP/SEDP multicast, `ddsperf`-driven CycloneDDS
  containers). A new `CPPDDS_INTEROP_TESTS` CMake option (default `OFF`) is the
  CMake/CTest equivalent of go-DDS's `go:build interop` tag: with it off — the default
  for every normal build, including the CI `build-and-test` matrix — this directory's
  tests are never compiled, linked, or registered with CTest at all, not merely
  filtered out afterward; the resulting `cppdds_interop_tests` target additionally
  tags every test with the CTest label `interop` (`ctest -L interop` /
  `ctest -LE interop`) and `SKIP_RETURN_CODE 4` (Catch2's own "every test case was
  skipped" exit code), so CTest reports genuinely-skipped runs as "Not Run" rather
  than "Failed". `INTEROP_DOMAIN`/`INTEROP_TIMEOUT` environment variables match
  go-DDS's own convention exactly. A new opt-in `test-interop` CI job builds with
  `-DCPPDDS_INTEROP_TESTS=ON`, brings up the `cyclone-peer` service, and runs
  `ctest -L interop` — probe-gated to skip (green) rather than fail if the CycloneDDS
  Docker image is unavailable in the runner's registry, and deliberately not a
  `needs:` dependency of, or depended on by, any other job, mirroring go-DDS's own
  `test-interop` job exactly. `interop/README.md` documents prerequisites and the
  local quick-start. Verified locally: default (`CPPDDS_INTEROP_TESTS=OFF`) Release
  C++17/C++20 builds are unaffected (264/264 existing tests still pass, zero new
  build artifacts); with the option on, the new suite builds warning-clean under a
  genuine `-std=c++20` invocation and passes (1 real pass + 2 correctly-reported
  skips with no live peer) under both a plain build and a Debug ASan+UBSan pass;
  CI's `test-interop` job additionally exercises this against a genuine CycloneDDS
  container on Linux. The go-DDS-peer and rust-DDS-peer legs of the 3-way matrix
  above remain open — this item covers the CycloneDDS leg and the harness
  infrastructure itself.

---

## Future (outside the 5-tier priority order)

- **CycloneDDS backend** (`dds/cyclone/` in cpp-DDS terms) — go-DDS keeps this as an
  *optional secondary* backend (`cyclone/`, 413 LOC) alongside its pure-Go RTPS
  implementation, not a replacement for it. Explicitly out of scope until Tiers 1–5
  land: RTPS is the sanctioned wire protocol for this roadmap, not a CycloneDDS-binding
  shortcut. Revisit afterward as an interoperability nice-to-have, optional build
  (`-DCPPDDS_CYCLONE=ON`).
- WAN bridge deep dive (TLS + shared-token auth) beyond the Tier 4 baseline
- Shared-memory transport (`dds/shmem/`, zero-copy, same-host — go-DDS `shmem`, 776 LOC
  incl. tests)
- ASIL-D uplift
- ROS 2 rmw adapter
