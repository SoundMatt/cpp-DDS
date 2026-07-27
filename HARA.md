# Hazard Analysis and Risk Assessment (HARA)

**Project:** cpp-DDS
**Standard:** ISO 26262-3, project target ASIL-B
**Machine-readable source of truth:** [`.fusa-hara.json`](.fusa-hara.json) (validated by `cpfusa check` — rules HARA002-005)

This document is the human-readable companion to `.fusa-hara.json`. It exists
per RELAY spec §20.4 ("A conformant implementation MUST carry ... a HARA").
Regenerate the summary table below with `cpfusa hara show` after editing
`.fusa-hara.json`; both files MUST stay in sync.

## Operational situations

| ID | Situation |
|---|---|
| OS-001 | Normal operation — participants, publishers, and subscribers actively exchanging samples. |
| OS-002 | Participant shutdown or reconfiguration — `Close()`/`close_with_drain()` called while subscriptions are open. |
| OS-003 | Concurrent multi-threaded access — Send/Publish/Close invoked from different threads without external synchronisation (RELAY spec §6 req 6, 7). |
| OS-004 | Resource-constrained / overload — publish rate exceeds subscriber channel capacity (back-pressure engaged). |

## Hazards and risk assessment

| ID | Hazard | Situations | S | E | C | ASIL | Safety goal |
|---|---|---|---|---|---|---|---|
| H-001 | `IParticipant::close()` did not close previously-returned subscriber channels, so a consumer blocked in `recv()` (directly, or via `dds::adapt()`'s forwarder thread) never observed shutdown. **Fixed** in `src/mock/participant.cpp` `MockParticipantImpl::close()` — see `tests/test_mock.cpp` "participant close: previously-returned subscriber channels are closed" / "... unblocks a subscriber blocked in recv()". | OS-002 | S2 | E2 | C2 | ASIL-B | SG-001 |
| H-002 | `IPublisher::write()` rejects an oversized payload (`ErrPayloadTooLarge`), but a caller ignoring the returned `std::error_code` silently drops a safety-relevant command. | OS-001, OS-004 | S2 | E1 | C2 | ASIL-A | SG-002 |
| H-003 | `validate_domain()` only enforces the numeric range `[0,232]`; it cannot detect a participant joining a range-valid but semantically wrong domain, risking cross-subsystem data leakage. | OS-001 | S3 | E1 | C2 | ASIL-B | SG-003 |
| H-004 | `QoS.Deadline` expiry is only surfaced if the caller registers `with_deadline_missed()`; without it, a stale sample looks fresh. | OS-001, OS-004 | S2 | E2 | C1 | ASIL-A | SG-004 |
| H-005 | A data race between concurrent Send/Publish/Close on the same participant/channel could corrupt an in-flight sample or crash the process (RELAY spec §6 req 6-7). | OS-003 | S2 | E1 | C1 | QM | SG-005 |

## Safety goals

| ID | ASIL | Goal | Safe state |
|---|---|---|---|
| SG-001 | ASIL-B | `IParticipant::close()` MUST close every subscriber channel already returned via `new_subscriber()`, unblocking any thread blocked in `recv()`. | `is_closed() == true` on every returned channel; `recv()` returns `std::nullopt` within one scheduling interval of `close()`. |
| SG-002 | ASIL-A | Every `IPublisher::write()` call site MUST propagate and act on a non-empty `std::error_code`. | Caller observes the error and does not treat the write as delivered. |
| SG-003 | ASIL-B | Domain selection MUST be governed by application-level configuration review / integration testing in addition to `validate_domain()`'s numeric check. | Deployment configuration is reviewed so isolated subsystems use disjoint domains (documented residual risk — see README). |
| SG-004 | ASIL-A | Applications with a freshness requirement MUST register `with_deadline_missed()` on every safety-relevant subscriber. | The callback fires and the application discards/flags the stale sample before acting on it. |
| SG-005 | QM | Shared participant/channel state MUST remain race-free under concurrent Send/Publish/Close (RELAY spec §6 req 6-7). | No race reported by the `sanitizers` CI job (ASan+UBSan) across the concurrency test suite (`REQ-SAFETY-001`); no crash or corruption. |

## Process

- New hazards are added to `.fusa-hara.json` first (machine-readable), then
  reflected here.
- `cpfusa check` gates on HARA002 (missing S/E/C), HARA003 (hazard not linked
  to a safety goal), HARA004 (safety goal missing ASIL), and HARA005 (hazard
  ASIL exceeding the project ASIL declared in `.fusa.json`).
- Related evidence: [`TARA`](tara.md) (ISO/SAE 21434 threat scenarios),
  [`dFMEA`](fmea.csv) (generated failure-mode analysis),
  [`requirements/requirements.json`](requirements/requirements.json).
