// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/safety/e2e.hpp — end-to-end (E2E) data protection wire header.
//
// C++ port of github.com/SoundMatt/go-DDS's `safety` package (safety/e2e.go):
// E2EPublisher prepends an 18-byte protection header to every payload before
// writing; E2ESubscriber strips the header and validates CRC, sequence
// counter, and sample freshness on every received sample. See ROADMAP.md,
// "Tier 2 — safety and security", `ddssafety` / E2E protection.
//
// Wire format (little-endian, 18 bytes followed by the original payload) —
// byte-for-byte identical to go-DDS's, since this is cross-language interop
// exactly like the RTPS wire format:
//
//   Bytes  0-1   DataID (uint16)
//   Bytes  2-3   SourceID (uint16)
//   Bytes  4-7   SequenceCounter (uint32, monotonically increasing per publisher)
//   Bytes  8-15  Timestamp (int64, Unix nanoseconds at time of Write)
//   Bytes 16-17  CRC-16/CCITT-FALSE over bytes 0-15 plus the original payload
//   Bytes 18+    Original payload
//
// The CRC slot is treated as zero when computing the CRC. Byte layout and
// the CRC-16/CCITT-FALSE algorithm (polynomial 0x1021, init 0xFFFF, no
// reflection) are verified against reference vectors independently derived
// from a fresh go-DDS clone — see tests/test_safety_e2e.cpp.
//
// go-DDS's safety.SafetyMetricsProvider / safety.SafetyEvent / monitor
// integration (safety/metrics.go) are Tier-5-adjacent observability
// surfaces layered on top of the E2E subscriber and are out of scope here —
// ROADMAP.md's Tier 2 item scopes only the wire header plus
// CRC/sequence/freshness validation. E2ESubscriber below reports violations
// via its errors() channel only, matching the subset of go-DDS's behavior
// this port targets.
//
// go-DDS's E2EPublisher/E2ESubscriber do not, in practice, satisfy the full
// dds.Publisher/dds.Subscriber interfaces despite their doc comments
// claiming so (E2EPublisher has no WriteCtx; E2ESubscriber has no TryRead or
// Unsubscribe) — this port's E2EPublisher fully implements dds::IPublisher
// (including the context-bounded write() overload, per REQ-SAFETY-003)
// since doing so is a strict improvement with no wire-format cost.
// E2ESubscriber mirrors go-DDS's actual (partial) surface — channel() /
// errors() / close() — rather than being forced into dds::ISubscriber,
// since go-DDS's own type was never a full Subscriber either.

#pragma once

#include <dds/dds.hpp>
#include <dds/relay.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace dds::safety {

// fusa:req REQ-E2E-001
inline constexpr std::size_t kHeaderSize = 18;

// E2EConfig configures end-to-end protection parameters shared by a
// publisher and subscriber pair (go-DDS: safety.E2EConfig).
struct E2EConfig {
    // data_id identifies the logical data element (0-65535).
    uint16_t data_id{0};
    // source_id identifies the sender (0-65535).
    uint16_t source_id{0};
    // max_age is the maximum permitted age of a received sample, measured
    // from the timestamp written by the publisher. Zero disables freshness
    // checking.
    std::chrono::nanoseconds max_age{0};
    // topic is the DDS topic name; informational only. Optional.
    std::string topic;
};

// E2EErrorKind categorises safety check failures reported by E2ESubscriber
// (go-DDS: safety.E2EErrorKind).
enum class E2EErrorKind : int {
    CRCMismatch    = 0, // the CRC in the frame header did not match.
    SequenceGap    = 1, // one or more sequence numbers were skipped.
    StaleSample    = 2, // the sample's timestamp is older than max_age.
    HeaderTooShort = 3, // the payload is shorter than the 18-byte header.
};

// E2EError is delivered over E2ESubscriber::errors() when a safety check
// fails (go-DDS: safety.E2EError).
struct E2EError {
    E2EErrorKind kind{E2EErrorKind::CRCMismatch};
    uint32_t     counter{0};
    std::string  message;
};

// ── E2EPublisher ─────────────────────────────────────────────────────────────

// E2EPublisher wraps a dds::IPublisher and prepends an E2E protection
// header to every write() payload. Thread-safe (fusa:req REQ-SAFETY-001).
// fusa:req REQ-E2E-001 REQ-E2E-002
class E2EPublisher : public dds::IPublisher {
public:
    E2EPublisher(std::shared_ptr<dds::IPublisher> pub, E2EConfig cfg);
    ~E2EPublisher() override = default;

    E2EPublisher(const E2EPublisher&)            = delete;
    E2EPublisher& operator=(const E2EPublisher&) = delete;

    // write prepends an E2E header to payload and writes the frame to the
    // underlying publisher. fusa:req REQ-E2E-002
    std::error_code write(const std::vector<uint8_t>& payload) override;

    // write with context — bounded execution per REQ-SAFETY-003.
    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override;

    // close closes the underlying publisher.
    std::error_code close() override;

private:
    std::vector<uint8_t> make_frame(uint32_t counter, const std::vector<uint8_t>& payload) const;

    std::shared_ptr<dds::IPublisher> pub_;
    E2EConfig                        cfg_;
    std::mutex                       mu_;
    uint32_t                         counter_{0};
};

// new_e2e_publisher wraps pub with end-to-end protection configured by cfg
// (go-DDS: safety.NewE2EPublisher).
std::shared_ptr<E2EPublisher> new_e2e_publisher(std::shared_ptr<dds::IPublisher> pub, E2EConfig cfg);

// ── E2ESubscriber ────────────────────────────────────────────────────────────

// E2ESubscriber wraps a dds::ISubscriber, strips the E2E header from each
// received sample, and validates CRC, sequence counter, and freshness.
// Valid samples are forwarded to channel(); failures are reported on
// errors(). A background pump thread starts on construction and exits when
// close() is called or the underlying subscriber's channel closes.
// fusa:req REQ-E2E-001 REQ-E2E-003 REQ-E2E-004 REQ-E2E-005 REQ-E2E-006
class E2ESubscriber {
public:
    E2ESubscriber(std::shared_ptr<dds::ISubscriber> sub, E2EConfig cfg);
    ~E2ESubscriber();

    E2ESubscriber(const E2ESubscriber&)            = delete;
    E2ESubscriber& operator=(const E2ESubscriber&) = delete;

    // channel returns the channel that delivers validated, header-stripped
    // samples.
    std::shared_ptr<dds::Chan<dds::Sample>> channel() const noexcept { return ch_; }

    // errors returns a channel that receives one E2EError per safety check
    // failure. Buffered (32); overflows are silently dropped.
    std::shared_ptr<dds::Chan<E2EError>> errors() const noexcept { return err_ch_; }

    // close stops the pump thread and closes the underlying subscriber.
    // Idempotent.
    std::error_code close();

private:
    void pump();
    void process(const dds::Sample& raw);
    void emit_error(E2EError e);

    std::shared_ptr<dds::ISubscriber> sub_;
    E2EConfig                         cfg_;
    uint32_t                          last_counter_{0};
    bool                              has_first_{false};

    std::shared_ptr<dds::Chan<dds::Sample>> ch_;
    std::shared_ptr<dds::Chan<E2EError>>    err_ch_;
    std::thread                             pump_thread_;
    std::once_flag                          close_once_;
};

// new_e2e_subscriber wraps sub with E2E validation using cfg
// (go-DDS: safety.NewE2ESubscriber).
std::shared_ptr<E2ESubscriber> new_e2e_subscriber(std::shared_ptr<dds::ISubscriber> sub, E2EConfig cfg);

} // namespace dds::safety
