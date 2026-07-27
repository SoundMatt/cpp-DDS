# Changelog

All notable changes to cpp-DDS are documented here.

Format: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

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
