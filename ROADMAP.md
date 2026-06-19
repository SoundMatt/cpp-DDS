# cpp-DDS Roadmap

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
- [x] RELAY spec v1.7 conformance

## v0.2.0 — RTPS transport

- [ ] `dds/rtps/participant.hpp` — pure C++ RTPS/UDP transport (no third-party DDS required)
- [ ] SPDP discovery (multicast participant announcement)
- [ ] SEDP endpoint matching
- [ ] CDR serialization for payload framing
- [ ] Reliable delivery: sliding-window ACK/NACK
- [ ] FragmentedData for payloads > 64 KB

## v0.3.0 — Security

- [ ] `dds/security/` — HMAC-SHA-256 message authentication
- [ ] AES-256-GCM encryption layer
- [ ] Topic ACL (access control per participant)
- [ ] Anti-replay sequence number enforcement

## v0.4.0 — Metrics and Health

- [ ] `IMetricsProvider` implementation on mock and RTPS participants
- [ ] `IHealthProvider` reporting participant and transport health
- [ ] `IDrainer::close_with_drain()` on mock participant
- [ ] Prometheus-compatible metrics export

## v0.5.0 — LoaningPublisher

- [ ] `ILoaningPublisher` interface (zero-copy loan/commit)
- [ ] Pool allocator backing the loaning publisher
- [ ] `ErrLoanBuffer` for exhausted or mismatched loans

## v0.6.0 — CycloneDDS backend

- [ ] `dds/cyclone/participant.hpp` — CycloneDDS C-API wrapper
- [ ] Optional build: `-DCPPDDS_CYCLONE=ON`
- [ ] Interoperability tests vs go-DDS cyclone backend

## Future

- WAN bridge (TLS + shared-token auth)
- Shared-memory transport (zero-copy, same-host)
- TSN QoS fields (transport priority, latency budget, TAPRIO)
- ASIL-D uplift
- IDL parser + C++ codegen
- ROS 2 rmw adapter
