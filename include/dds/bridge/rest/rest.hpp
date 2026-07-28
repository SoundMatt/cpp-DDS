// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/rest/rest.hpp — HTTP/SSE gateway bridging a dds::IParticipant
// to HTTP clients.
//
// C++ port of github.com/SoundMatt/go-DDS's bridge/rest package
// (ROADMAP.md, "Tier 4 — bridges", the third and last item of Tier 4's
// grpc+wan+rest set). Three routes, ported one-for-one from go's rest.go
// ServeHTTP:
//
//   - GET  /topics       -> JSON array of currently-subscribed topic names.
//   - GET  /topics/{t}   -> Server-Sent Events (SSE) stream of samples
//                           published on topic t.
//   - POST /topics/{t}   -> publishes the request body as one sample on
//                           topic t.
//
// This header is the business-logic layer, with no networking in it at
// all: Bridge exposes list_topics/get_subscriber/get_publisher/authorize/
// run_sse_loop/handle_publish as plain C++ methods against an abstract
// SseSink, so it can be unit-tested with in-memory doubles exactly like
// dds::bridge::grpc::Bridge is tested against SampleSender/PublishReceiver
// doubles (grpc_internal_test.go's mock-stream pattern) — see
// tests/test_bridge_rest.cpp's "Bridge unit tests" section.
// transport.hpp's Server is the real HTTP/1.1-over-TCP wire layer that
// drives these methods from actual sockets.
//
// ── Scope notes (deliberate deviations from a literal line-for-line port) ──
//
//   - Wire transport: go-DDS's rest.Bridge is a plain http.Handler, meant
//     to be handed to a real net/http server (http.ListenAndServe) or
//     httptest.Server in tests — it does no socket work itself, relying
//     entirely on Go's standard library HTTP/1.1 implementation (request
//     parsing, chunked Transfer-Encoding for a streamed response body
//     without a Content-Length, connection lifecycle). cpp-DDS has no HTTP
//     library dependency anywhere (this repo's established convention:
//     RTPS, CDR, xtypes, TSN netlink, and the grpc/wan bridges' own
//     hand-rolled framing — see grpc.hpp's file-level scope note for why
//     that convention exists), so transport.hpp's Server hand-rolls a
//     minimal HTTP/1.1 request/response parser+writer over raw TCP
//     sockets instead: it correctly speaks Content-Length-framed unary
//     responses and Transfer-Encoding: chunked for the SSE stream (each
//     flush — one sample or one keepalive comment — is its own chunk),
//     verified byte-exact against a real go-DDS process's actual wire
//     bytes captured via a raw net.Dial (not net/http's client, which
//     transparently de-chunks) — see transport.hpp's file-level scope
//     note and tests/test_bridge_rest.cpp's reference-vector tests for
//     specifics. Per this repo's grpc-bridge precedent of "one call per
//     TCP connection, no multiplexing", this parser is deliberately not a
//     general-purpose HTTP/1.1 server: each connection carries exactly one
//     request (no keep-alive/pipelining across multiple requests on the
//     same connection — matching grpc.hpp's own documented "no
//     multiplexing" simplification), and request paths are not
//     percent-decoded (topic names in every test, on both sides, are
//     plain ASCII path segments with no percent-escaping or query
//     strings).
//   - JSON codec: hand-rolled per this repo's established convention (see
//     wan.hpp's file-level scope note for the list of modules that each
//     own their own minimal JSON codec). topics_to_json only needs to
//     *encode* a sorted array of plain strings (the wire protocol never
//     carries incoming JSON — SSE bodies and publish bodies are raw
//     bytes) — verified byte-exact, including the trailing "\n"
//     `encoding/json.Encoder.Encode` appends after every value (unlike
//     `json.Marshal`), against reference vectors captured from a real
//     go-DDS process.
//   - Options::sse_keepalive "disable" quirk: go-DDS's doc comment for
//     Options.SSEKeepalive claims "Zero disables", but its actual
//     Options.keepalive() method returns the 15s default whenever
//     SSEKeepalive == 0 — there is no way to actually disable the
//     keepalive comment in go-DDS's real behavior, despite what the
//     comment says. effective_keepalive() below mirrors the actual
//     *function*, not the misleading comment, so behavior (not prose) is
//     byte-for-byte reproduced; Options::sse_keepalive defaults directly
//     to 15s (rather than 0 routed through a fallback) for the same
//     reason dds::bridge::wan::Options::qos defaults directly to
//     dds::default_qos() instead of reproducing a Go zero-value special
//     case that C++'s default member initializers make structurally
//     unreachable (see grpc.hpp's file-level scope note) — an explicit
//     zero still hits the fallback and still resolves to 15s, exactly
//     like go-DDS.
//   - "streaming not supported" (go's http.Flusher type-assertion
//     failure): this exists in go-DDS purely because an arbitrary
//     http.ResponseWriter isn't guaranteed to implement http.Flusher.
//     transport.hpp's Server always writes directly to a raw socket it
//     owns, so an analogous "can't stream" failure mode does not exist —
//     there is nothing to port here, not a fidelity gap.
//   - Route::bad_request ("topic name required"): go's `topic ==
//     "" -> 400` branch is, on inspection, unreachable — not just through
//     any real HTTP path, but through classify_request's own logic taken
//     as a pure function of (method, raw_path): raw_path reaching the
//     topic-extraction step is guaranteed non-empty and not equal to "/"
//     (both are handled by the earlier branch), so stripping at most one
//     more leading "/" from it can never yield "". go-DDS's own
//     rest_test.go has no test exercising its equivalent branch either,
//     for the identical reason. classify_request below reproduces the
//     same TrimPrefix-based logic byte-for-byte anyway (including this
//     same unreachability) rather than silently dropping dead code from a
//     port — Route::bad_request and its 400 response remain wired up
//     end-to-end (transport.cpp) for defense-in-depth, but, matching
//     go-DDS's own untested equivalent, no test claims to exercise it,
//     since none can.
//   - Oversized publish bodies: go's handlePublish reads via
//     `io.ReadAll(io.LimitReader(r.Body, maxBody))` (maxBody = 16 MiB),
//     which *silently truncates* a longer body without ever returning an
//     error — an accidental-looking Go behavior nothing in go-DDS's own
//     test suite exercises. Every other wire boundary in cpp-DDS treats
//     "declared size exceeds the cap" as an explicit rejected error
//     instead of a silent truncation (dds::bridge::wan's
//     ErrFrameTooLarge, dds::bridge::grpc::transport's kMaxFrameSize
//     check), so transport.hpp's Server does the same here: a
//     Content-Length above 16 MiB is rejected with 400 rather than
//     silently truncated — a documented, low-risk improvement over what
//     reads as an accidental Go default, exactly like
//     dds::bridge::grpc::TopicConfig::effective_qos()'s documented
//     departure from a raw Go zero-value literal (see ROADMAP.md's grpc
//     entry).

#pragma once

#include <dds/dds.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dds::bridge::rest {

// ── Options ────────────────────────────────────────────────────────────────

// fusa:req REQ-BRIDGE-REST-003
struct Options {
    // auth_token, if non-empty, requires every request to carry the header
    //   Authorization: Bearer <auth_token>
    // Requests without a valid token receive 401 Unauthorized.
    std::string auth_token;

    // qos is applied to all subscribers and publishers created by the
    // bridge. Defaults to dds::default_qos() (see file-level scope note on
    // why no Go-zero-value special case is needed here).
    dds::QoS qos{dds::default_qos()};

    // sse_keepalive is how often a comment (": keepalive") line is written
    // to idle SSE streams to prevent proxy timeouts. Defaults to 15s. See
    // effective_keepalive() and the file-level scope note: an explicit
    // zero here does *not* disable the keepalive (matching go-DDS's actual
    // behavior, despite its doc comment claiming otherwise) — it still
    // resolves to 15s.
    std::chrono::nanoseconds sse_keepalive{std::chrono::seconds(15)};
};

// effective_keepalive returns opts.sse_keepalive, or 15s if it is zero —
// see Options::sse_keepalive's doc comment and the file-level scope note.
// fusa:req REQ-BRIDGE-REST-003
std::chrono::nanoseconds effective_keepalive(const Options& opts) noexcept;

// ── Topic-list JSON codec ─────────────────────────────────────────────────────
//
// Byte-identical to go's json.NewEncoder(w).Encode(sortedTopics): a
// compact JSON array of HTML-safe-escaped strings (matching Go's default
// escapeHTML=true for <,>,&) with a trailing "\n", verified against
// reference vectors captured from a real go-DDS process. `topics` must
// already be sorted by the caller (Bridge::list_topics does this).
// fusa:req REQ-BRIDGE-REST-001
std::string topics_to_json(const std::vector<std::string>& topics);

// ── SSE wire-format helpers ────────────────────────────────────────────────────
//
// Byte-identical to go's handleSubscribe fmt.Fprintf output. Base64 is
// standard padded encoding (matching encoding/base64.StdEncoding, the same
// encoder go-DDS's rest.go uses).
// fusa:req REQ-BRIDGE-REST-002
std::string sse_message_event(uint64_t id, const std::vector<uint8_t>& payload);
// fusa:req REQ-BRIDGE-REST-002
std::string sse_keepalive_comment();

// ── Routing ────────────────────────────────────────────────────────────────

enum class Route {
    list,               // GET /topics (or GET /topics/) -> topic list JSON
    subscribe,          // GET /topics/{topic}           -> SSE stream
    publish,            // POST /topics/{topic}          -> publish body
    bad_request,        // topic name required after stripping (see scope note)
    method_not_allowed, // any other method on either shape
};

struct RouteResult {
    Route       route{Route::bad_request};
    std::string topic; // set for subscribe/publish
};

// classify_request reproduces go-DDS's ServeHTTP routing logic exactly,
// including its strings.TrimPrefix(path, "/topics")-then-TrimPrefix(path,
// "/") behavior (see file-level scope note on Route::bad_request's
// practical unreachability). `raw_path` is a URL path with any query
// string already stripped by the caller (transport.hpp's Server) —
// classify_request itself has no notion of query strings, matching Go's
// r.URL.Path.
// fusa:req REQ-BRIDGE-REST-005
RouteResult classify_request(const std::string& method, const std::string& raw_path);

// ── Result ─────────────────────────────────────────────────────────────────

// fusa:req REQ-BRIDGE-REST-007
// fusa:req REQ-BRIDGE-REST-008
struct Result {
    bool        ok{true};
    std::string message; // empty when ok

    static Result success() noexcept { return Result{}; }
    static Result failure(std::string msg) { return Result{false, std::move(msg)}; }
};

// ── SseSink ────────────────────────────────────────────────────────────────
//
// Stands in for the real HTTP/1.1 chunked-response writer transport.hpp's
// Server drives Bridge::run_sse_loop with — enough surface to unit-test
// run_sse_loop directly with an in-memory double (mirroring go-DDS's own
// grpc_internal_test.go mock-stream pattern for an analogous streaming
// RPC), independent of any networking.
// fusa:req REQ-BRIDGE-REST-007
class SseSink {
public:
    virtual ~SseSink() = default;

    // send_message delivers one sample as an SSE "message" event. A
    // non-OK error_code aborts the loop (mirrors handleSubscribe's
    // `if writeErr != nil { return }`).
    virtual std::error_code send_message(uint64_t id, const std::vector<uint8_t>& payload) = 0;

    // send_keepalive writes an SSE keepalive comment. A non-OK error_code
    // aborts the loop, exactly like send_message.
    virtual std::error_code send_keepalive() = 0;

    // cancelled reports whether the peer/connection has gone away
    // (mirrors Go's `case <-r.Context().Done()`). run_sse_loop polls this
    // between waits since relay::Channel<T> has no native multi-channel
    // select (see dds::bridge::wan's file-level scope note for the
    // established rationale behind this repo's poll-based pattern).
    virtual bool cancelled() const = 0;
};

// ── Bridge ─────────────────────────────────────────────────────────────────

// Bridge wraps a dds::IParticipant and implements the REST bridge's
// business logic as plain C++ methods (no networking) plus lazy
// subscriber/publisher caching, matching go-DDS's Bridge exactly.
// transport.hpp's Server drives these methods from real TCP connections;
// tests can drive them directly. Safe for concurrent use from multiple
// threads.
// fusa:req REQ-BRIDGE-REST-004
// fusa:req REQ-BRIDGE-REST-006
class Bridge {
public:
    explicit Bridge(std::shared_ptr<dds::IParticipant> participant, Options opts = {});
    ~Bridge();

    Bridge(const Bridge&)            = delete;
    Bridge& operator=(const Bridge&) = delete;

    // close closes every cached subscriber/publisher. Idempotent.
    // fusa:req REQ-BRIDGE-REST-006
    std::error_code close();

    const Options&                      options() const noexcept { return opts_; }
    std::shared_ptr<dds::IParticipant>  participant() const noexcept { return p_; }

    // authorize mirrors go-DDS's authorize(): true when options().auth_token
    // is empty (auth disabled), or when authorization holds exactly
    // "Bearer " + options().auth_token.
    // fusa:req REQ-BRIDGE-REST-004
    bool authorize(const std::optional<std::string>& authorization) const;

    // list_topics returns a sorted snapshot of every topic with a
    // currently-cached subscriber (i.e. every topic ever subscribed to via
    // get_subscriber and not yet closed).
    // fusa:req REQ-BRIDGE-REST-006
    std::vector<std::string> list_topics() const;

    // get_subscriber resolves or lazily creates a cached DDS subscriber for
    // topic, mirroring go's getOrCreateSub.
    // fusa:req REQ-BRIDGE-REST-006
    std::pair<std::shared_ptr<dds::ISubscriber>, std::error_code>
        get_subscriber(const std::string& topic);

    // get_publisher resolves or lazily creates a cached DDS publisher for
    // topic, mirroring go's getOrCreateSub (the publisher-side twin).
    // fusa:req REQ-BRIDGE-REST-006
    std::pair<std::shared_ptr<dds::IPublisher>, std::error_code>
        get_publisher(const std::string& topic);

    // run_sse_loop delivers samples from sub to sink — applying a
    // bridge-wide monotonically increasing sequence id to each
    // (mirroring go's atomic.Uint64 b.seq) — until sub's channel closes
    // (returns Result::success()), sink.cancelled() is observed, or a sink
    // send fails (returns Result::failure()).
    // fusa:req REQ-BRIDGE-REST-007
    Result run_sse_loop(const std::shared_ptr<dds::ISubscriber>& sub, SseSink& sink);

    // handle_publish resolves/creates a publisher for topic and writes
    // payload, matching go's handlePublish's "publisher: "/"publish: "
    // error-message prefixes exactly.
    // fusa:req REQ-BRIDGE-REST-008
    Result handle_publish(const std::string& topic, const std::vector<uint8_t>& payload);

private:
    std::shared_ptr<dds::IParticipant> p_;
    Options                            opts_;

    mutable std::mutex                                                  mu_;
    std::unordered_map<std::string, std::shared_ptr<dds::ISubscriber>> subs_;
    std::unordered_map<std::string, std::shared_ptr<dds::IPublisher>>  pubs_;

    std::atomic<uint64_t> seq_{0};
};

} // namespace dds::bridge::rest
