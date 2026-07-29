// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/grpc/grpc.hpp — gRPC gateway bridging a dds::IParticipant to
// RPC clients using JSON-encoded messages.
//
// C++ port of github.com/SoundMatt/go-DDS's bridge/grpc package
// (ROADMAP.md, "Tier 4 — bridges"). The DDSBridge service exposes three
// RPCs, ported one-for-one from go-DDS's grpc.go:
//
//   - Subscribe(SubscribeRequest) -> stream(Sample): server-streaming,
//     delivers DDS samples as they arrive.
//   - Publish(PublishRequest) -> PublishAck: unary publish of one sample.
//   - StreamPublish(stream PublishRequest) -> PublishAck: client-streaming
//     for high-throughput writes; returns the total count on close.
//
// ── Scope notes (deliberate deviations from a literal line-for-line port) ──
//
//   - Wire transport: go-DDS's bridge rides on google.golang.org/grpc,
//     i.e. real HTTP/2 framing plus HPACK header compression, and its
//     messages are protobuf-shaped internally even though this bridge
//     itself uses go-DDS's own JSONCodec on top of that transport. cpp-DDS
//     has no HTTP/2 or protobuf dependency anywhere in the repo (the whole
//     codebase's convention — RTPS wire types, CDR, xtypes, TSN netlink —
//     is to hand-roll wire protocols rather than pull in a matching
//     reference library), so pulling in grpc++ (itself dependent on
//     protobuf and abseil) purely for this one bridge would be a large,
//     slow-to-build outlier inconsistent with every other module in this
//     repo. Instead, dds::bridge::grpc::transport (transport.hpp) hand-
//     rolls a minimal call-oriented protocol over plain TCP: a short
//     text header block (method name + optional "authorization: Bearer
//     <token>" line, terminated by a blank line — the header-carrying
//     subset of what an HTTP/2 HEADERS frame would carry for this
//     service) followed by one or more *real* gRPC length-prefixed
//     message frames (RFC-identical 1-byte compression-flag + 4-byte
//     big-endian length prefix — see transport.hpp's kFrameHeaderSize).
//     Each TCP connection carries exactly one RPC call (no multiplexing),
//     which is the one place this deviates from a byte-for-byte gRPC
//     transport. True interop with a stock grpc-go/grpc++ client would
//     require adding the grpc++ dependency and swapping transport.cpp's
//     socket loop for a real HTTP/2 stack — deliberately deferred, exactly
//     like the CycloneDDS backend and the WAN bridge's TLS deep dive are
//     deferred in ROADMAP.md's "Future" section. Everything *above* the
//     transport — the message JSON shape, RPC semantics, Options
//     (AuthToken/QoS/Filter/Transform), and Config — is behavior-exact
//     with go-DDS, verified against reference vectors captured from a
//     real go-DDS process (see tests/test_bridge_grpc.cpp).
//   - JSON codec: hand-rolled per this repo's established convention (see
//     src/xtypes/xtypes.cpp, src/tsn/tsn.cpp, cli/json_lite.hpp — none of
//     which share a JSON implementation with each other either). Encoding
//     matches Go's encoding/json exactly for the field shapes this module
//     needs: object keys in Go struct-field order, HTML-safe string
//     escaping (<,>,& as </>/&, matching Go's default
//     escapeHTML=true), and standard padded base64 for []byte fields. One
//     documented gap: Go's json.Marshal emits `null` for a nil []byte and
//     `""` for a non-nil empty []byte — a distinction C++'s
//     std::vector<uint8_t> cannot represent — so this encoder always
//     emits `""` for an empty payload/writer_guid. The decoder accepts
//     both `null` and `""` for these fields, so round-tripping with a
//     real go-DDS peer's *encoder* output is unaffected either way.
//   - Options::qos: go-DDS's Options.qos() special-cases "was QoS left at
//     its Go zero value" via reflect.DeepEqual against dds.QoS{} (needed
//     there only because Go structs can't default-initialize to a named
//     constant). dds::QoS's default member initializers already make a
//     default-constructed dds::QoS equal dds::default_qos() field-for-
//     field, so that zero-value special case is structurally unreachable
//     here — Options::qos simply defaults to dds::default_qos() and is
//     used as-is; no runtime check is needed to reproduce the same
//     behavior.
//   - checkAuth: go-DDS's checkAuth distinguishes "no gRPC metadata object
//     on the context at all" (Unauthenticated: "missing metadata") from
//     "metadata present but no/wrong authorization value" (Unauthenticated:
//     "invalid token") because Go's context.Context can carry either
//     state. This port's header block is always a (possibly empty) parsed
//     map, so there is no analogous "no metadata object" state; both
//     collapse to Bridge::check_auth's single "missing or invalid
//     authorization header" outcome. Every client-visible behavior (the
//     Unauthenticated status code, and the fact that a missing or wrong
//     token is always rejected) is unchanged.

#pragma once

#include <dds/dds.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dds::bridge::grpc {

// ── Message types (fusa:req REQ-BRIDGE-GRPC-001) ──────────────────────────────
// fusa:req REQ-BRIDGE-GRPC-001

// SubscribeRequest is the request message for the Subscribe RPC.
struct SubscribeRequest {
    std::string topic;
};

// PublishRequest is the request message for Publish and StreamPublish.
struct PublishRequest {
    std::string          topic;
    std::vector<uint8_t> payload;
};

// Sample is a DDS sample delivered by the Subscribe RPC.
struct Sample {
    std::string           topic;
    std::vector<uint8_t>  payload;
    uint64_t               sequence_number{0};
    int64_t                timestamp_ns{0};
    std::vector<uint8_t>  writer_guid;
};

// PublishAck is the response from Publish and StreamPublish.
struct PublishAck {
    uint32_t count{0};
};

// ── JSON codec (fusa:req REQ-BRIDGE-GRPC-002) ─────────────────────────────────
// fusa:req REQ-BRIDGE-GRPC-002
//
// Field names and shapes match go-DDS's grpc.go struct tags exactly:
// SubscribeRequest{topic}, PublishRequest{topic,payload}, Sample{topic,
// payload,seq_num,timestamp_ns,writer_guid}, PublishAck{count}.

std::string to_json(const SubscribeRequest& v);
std::string to_json(const PublishRequest& v);
std::string to_json(const Sample& v);
std::string to_json(const PublishAck& v);

// from_json parses `text` into `out`, returning false (leaving `out`
// unspecified) on malformed JSON or a type mismatch on a known field.
// Unknown fields are ignored, matching Go's encoding/json.Unmarshal.
bool from_json(const std::string& text, SubscribeRequest& out);
bool from_json(const std::string& text, PublishRequest& out);
bool from_json(const std::string& text, Sample& out);
bool from_json(const std::string& text, PublishAck& out);

// ── Status (fusa:req REQ-BRIDGE-GRPC-003) ─────────────────────────────────────
// fusa:req REQ-BRIDGE-GRPC-003
//
// Numeric values match google.golang.org/grpc/codes exactly, so a payload
// carrying one of these is drop-in compatible with a real gRPC status
// trailer if this bridge's transport is ever upgraded to real HTTP/2.
enum class StatusCode : int {
    OK                  = 0,
    Cancelled           = 1,
    InvalidArgument     = 3,
    Internal            = 13,
    Unauthenticated     = 16,
};

struct Status {
    StatusCode  code{StatusCode::OK};
    std::string message;

    bool ok() const noexcept { return code == StatusCode::OK; }

    static Status make_ok() { return Status{}; }
    static Status invalid_argument(std::string msg) { return Status{StatusCode::InvalidArgument, std::move(msg)}; }
    static Status internal_error(std::string msg)   { return Status{StatusCode::Internal, std::move(msg)}; }
    static Status unauthenticated(std::string msg)  { return Status{StatusCode::Unauthenticated, std::move(msg)}; }
    static Status cancelled(std::string msg)        { return Status{StatusCode::Cancelled, std::move(msg)}; }
};

// ── Filter / Transform (fusa:req REQ-BRIDGE-GRPC-004) ─────────────────────────
// fusa:req REQ-BRIDGE-GRPC-004

// FilterFunc decides whether to forward a sample. Return false to drop it.
using FilterFunc = std::function<bool(const std::string& topic, const std::vector<uint8_t>& payload)>;

// TransformFunc rewrites a sample payload before forwarding. Return
// std::nullopt to drop the sample (mirrors go-DDS's TransformFunc
// returning a non-nil error to drop).
using TransformFunc = std::function<std::optional<std::vector<uint8_t>>(
    const std::string& topic, const std::vector<uint8_t>& payload)>;

// ── Options (fusa:req REQ-BRIDGE-GRPC-004) ────────────────────────────────────
// fusa:req REQ-BRIDGE-GRPC-004

struct Options {
    // auth_token, if non-empty, requires every RPC to carry the header
    //   authorization: Bearer <auth_token>
    std::string auth_token;

    // qos is applied to all subscribers and publishers created by the
    // bridge. Defaults to dds::default_qos() (see file-level scope note).
    dds::QoS qos{dds::default_qos()};

    // filter, if set, is called for each outbound sample (Subscribe path).
    // Return false to drop the sample.
    FilterFunc filter;

    // transform, if set, rewrites the payload of each outbound sample
    // (Subscribe path) before delivery to the client.
    TransformFunc transform;
};

// ── Sender / Receiver (streaming abstractions) ────────────────────────────────
//
// These stand in for grpc.ServerStreamingServer[Sample] and
// grpc.ClientStreamingServer[PublishRequest,PublishAck] respectively —
// enough surface for Bridge::subscribe/stream_publish to be exercised
// directly in unit tests with in-memory doubles (mirroring go-DDS's own
// grpc_internal_test.go mockSubscribeStream / mockStreamPublishServer)
// and for transport.hpp's real TCP Server to drive them over the wire.

// SampleSender receives Sample messages pushed by Bridge::subscribe().
class SampleSender {
public:
    virtual ~SampleSender() = default;

    // send delivers one Sample to the peer. A non-OK error_code aborts the
    // stream (mirrors grpc.ServerStream.SendMsg's error return).
    virtual std::error_code send(const Sample& sample) = 0;

    // cancelled reports whether the peer/call context has gone away
    // (mirrors Go's `case <-ctx.Done()`). Bridge::subscribe polls this
    // between samples since relay::Channel<T> has no native multi-channel
    // select (see relay::WaitSet for the one place that need is already
    // solved, at a granularity this bridge doesn't need).
    virtual bool cancelled() const = 0;
};

// PublishReceiver supplies PublishRequest messages to Bridge::stream_publish().
class PublishReceiver {
public:
    virtual ~PublishReceiver() = default;

    // recv returns the next PublishRequest in `out` and true, or false at
    // clean end-of-stream (`err.ok()`) or on a transport error
    // (`!err.ok()`) — mirrors Go's `req, err := stream.Recv()` where
    // `err == io.EOF` is the clean-end-of-stream case.
    virtual bool recv(PublishRequest& out, Status& err) = 0;
};

// ── Bridge (fusa:req REQ-BRIDGE-GRPC-005 REQ-BRIDGE-GRPC-006 REQ-BRIDGE-GRPC-007) ──
// fusa:req REQ-BRIDGE-GRPC-005 REQ-BRIDGE-GRPC-006 REQ-BRIDGE-GRPC-007

// Bridge wraps a dds::IParticipant and implements the three DDSBridge RPCs
// as plain C++ methods (no networking) plus lazy subscriber/publisher
// caching, matching go-DDS's Bridge exactly. transport.hpp's Server drives
// these methods from real TCP connections; tests can drive them directly.
class Bridge {
public:
    Bridge(std::shared_ptr<dds::IParticipant> participant, Options opts = {});
    ~Bridge();

    Bridge(const Bridge&)            = delete;
    Bridge& operator=(const Bridge&) = delete;

    // close closes all cached subscribers and publishers. Idempotent.
    std::error_code close();

    const Options& options() const noexcept { return opts_; }
    std::shared_ptr<dds::IParticipant> participant() const noexcept { return p_; }

    // check_auth mirrors go-DDS's checkAuth (see file-level scope note for
    // the one documented behavioral simplification). Returns Status::make_ok()
    // when options().auth_token is empty (auth disabled) regardless of
    // `authorization`.
    Status check_auth(const std::optional<std::string>& authorization) const;

    // subscribe implements the Subscribe RPC: validates req.topic,
    // resolves (or creates) a cached DDS subscriber, and pushes samples to
    // `sender` — applying options().filter / options().transform — until
    // the subscriber's channel closes (returns Status::make_ok()) or
    // sender.cancelled() (returns Status::cancelled()) or sender.send()
    // fails (returns Status::internal_error()).
    Status subscribe(const SubscribeRequest& req, SampleSender& sender);

    // publish implements the Publish RPC: validates req.topic, resolves
    // (or creates) a cached DDS publisher, and writes req.payload.
    std::pair<PublishAck, Status> publish(const PublishRequest& req);

    // stream_publish implements the StreamPublish RPC: reads PublishRequest
    // messages from `receiver` until clean end-of-stream, writing each to
    // its (cached) DDS publisher, and returns the total count.
    std::pair<PublishAck, Status> stream_publish(PublishReceiver& receiver);

private:
    std::shared_ptr<dds::IParticipant> p_;
    Options                            opts_;

    std::mutex                                                       mu_;
    std::unordered_map<std::string, std::shared_ptr<dds::ISubscriber>> subs_;
    std::unordered_map<std::string, std::shared_ptr<dds::IPublisher>>  pubs_;

    std::pair<std::shared_ptr<dds::ISubscriber>, std::error_code> get_or_create_sub(const std::string& topic);
    std::pair<std::shared_ptr<dds::IPublisher>, std::error_code>  get_or_create_pub(const std::string& topic);
};

} // namespace dds::bridge::grpc
