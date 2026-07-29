// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/wan/wan.hpp — WAN bridge forwarding DDS samples between two
// dds::IParticipant domains over a real TCP connection.
//
// C++ port of github.com/SoundMatt/go-DDS's bridge/wan package (ROADMAP.md,
// "Tier 4 — bridges", the item immediately after dds::bridge::grpc). A
// server Bridge (Bridge::serve) accepts TCP connections and publishes every
// received sample to a local participant; a client Bridge (Bridge::connect)
// subscribes to a configured set of local topics and streams samples to a
// server. Bidirectional bridging is two such pairs, one per direction —
// there is no single "bidirectional" mode, matching go-DDS exactly.
//
// Wire format: each frame is a 4-byte big-endian length prefix (capped at
// 16 MiB — see ErrFrameTooLarge) followed by a JSON object
// {"t":"<topic>","p":"<base64-payload>"}, byte-exact with what go's
// encoding/json.Marshal produces for wan.go's unexported wireFrame struct
// (Topic string `json:"t"`, Payload []byte `json:"p"` — json.Marshal
// base64-encodes []byte automatically). See tests/test_bridge_wan.cpp's
// "JSON reference vectors" section for vectors captured from a real go-DDS
// process, per this repo's established "never hardcode without deriving
// from a fresh go-DDS clone" convention.
//
// ── Scope notes (deliberate deviations / this PR's baseline-vs-deferred split) ──
//
//   - Baseline vs. TLS deep dive: ROADMAP.md's Tier 4 entry for this bridge
//     is explicitly scoped to a "baseline" — plain-TCP wire framing plus the
//     shared-token auth path (Options::token below), since token auth is
//     already part of go-DDS's base bridge/wan package (wan.go, not
//     tls_test.go) and cheap to port alongside the framing it depends on.
//     A full TLS/mTLS integration (go-DDS's bridge/wan/tls_test.go —
//     encrypting the connection via a tls.Config, certificate/key loading,
//     mTLS client-cert verification) is deliberately deferred to
//     ROADMAP.md's "Future" section ("WAN bridge deep dive (TLS +
//     shared-token auth) beyond the Tier 4 baseline"), exactly like the
//     grpc bridge deferred a full HTTP/2/protobuf-grpc transport for the
//     same "large, slow-to-build outlier" reason — see grpc.hpp's file-level
//     scope note for the established rationale style this mirrors. Nothing
//     here forecloses adding a `tls` field to Options later: Bridge's
//     internal socket setup is centralized in wan.cpp's connect_to()/
//     listen_on() helpers, the one place a future TLS wrap would go.
//   - JSON codec: hand-rolled per this repo's established convention (see
//     grpc.hpp's file-level scope note for the list of modules that each
//     own their own minimal JSON codec rather than sharing one). Same
//     documented gap as the grpc bridge: Go's json.Marshal emits `null` for
//     a nil []byte and `""` for a non-nil empty []byte, a distinction
//     std::vector<uint8_t> cannot represent, so to_json always emits `""`
//     for an empty payload; from_json accepts both `null` and `""`.
//   - Per-topic send fan-out: go-DDS's sendLoop uses `select { case
//     s := <-sc.ch: ...; case <-b.done: return }` to multiplex N topic
//     channels and a done-signal without polling. relay::Channel<T> (this
//     repo's channel primitive, see channel.hpp) has no native multi-channel
//     select, so each topic gets its own sender thread that polls
//     recv_until() on a short deadline and checks a shared stop flag between
//     polls — the same "synchronous primitive polled from a loop" pattern
//     already established by dds::bridge::grpc::Bridge::subscribe (see
//     grpc.hpp's file-level scope note) and by rtps/transport.cpp's recv().
//     A write failure on any topic's sender thread closes every topic's
//     subscriber (mirroring go's closeAllSubs()), which unblocks the other
//     sender threads at their next poll.
//   - Native sockets never appear in this header (IPv4 TCP via BSD sockets
//     on POSIX / Winsock on Windows, mirroring dds::bridge::grpc::transport
//     and rtps/transport.hpp's platform split) — Bridge hides all socket
//     and thread state behind a pimpl.

#pragma once

#include <dds/dds.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace dds::bridge::wan {

// ── Wire frame type + JSON codec (fusa:req REQ-BRIDGE-WAN-001) ────────────────
// fusa:req REQ-BRIDGE-WAN-001

// WireFrame is the per-frame message: a topic name plus an opaque payload.
// Field names ("t", "p") and order match go-DDS's unexported wireFrame
// struct exactly.
struct WireFrame {
    std::string          topic;
    std::vector<uint8_t> payload;
};

// to_json/from_json: byte-exact with go's encoding/json.Marshal/Unmarshal
// output for wireFrame (see file-level scope note for the one documented
// nil-vs-empty-[]byte encoder gap). from_json returns false (leaving `out`
// unspecified) on malformed JSON.
std::string to_json(const WireFrame& f);
bool         from_json(const std::string& text, WireFrame& out);

// ── Errors (fusa:req REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-004) ───────────────────
// fusa:req REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-004

enum class Errc : int {
    frame_too_large = 1, // a length-prefixed frame (data or auth) exceeded its cap
    unauthorized    = 2, // server: client's auth token did not match
    invalid_frame   = 3, // frame body was not valid wireFrame JSON
};

const std::error_category& error_category() noexcept;
std::error_code             make_error_code(Errc e) noexcept;

// ErrFrameTooLarge mirrors go-DDS's exported wan.ErrFrameTooLarge sentinel:
// returned when an incoming frame's declared length exceeds the 16 MiB data
// cap, or an auth handshake frame exceeds its 4096-byte cap.
inline std::error_code ErrFrameTooLarge() noexcept { return make_error_code(Errc::frame_too_large); }
// ErrUnauthorized mirrors go-DDS's exported wan.ErrUnauthorized sentinel:
// returned (conceptually — see Bridge::serve, which drops the connection
// rather than surfacing this to any caller, matching go's receiveLoop)
// when a client's auth token does not match the server's configured token.
inline std::error_code ErrUnauthorized() noexcept { return make_error_code(Errc::unauthorized); }
inline std::error_code ErrInvalidFrame() noexcept { return make_error_code(Errc::invalid_frame); }

// ── Byte-stream abstraction (fusa:req REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-004) ──
// fusa:req REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-004
//
// Mirrors Go's io.Writer/io.Reader closely enough that the frame codec
// (write_frame/read_frame/write_auth/read_auth) can be unit-tested with
// in-memory doubles — exactly like go-DDS's wan_internal_test.go tests
// wan.go's writeFrame/readAuth/writeAuth directly against bytes.Buffer and
// a write-failing io.Writer — without a real socket. `write` must write all
// `len` bytes or return a non-OK error_code; `read` must fill exactly `len`
// bytes or return a non-OK error_code (io.ReadFull semantics). Bridge's
// socket implementation (wan.cpp) supplies real-socket WriteFn/ReadFn.
using WriteFn = std::function<std::error_code(const uint8_t* data, std::size_t len)>;
using ReadFn  = std::function<std::error_code(uint8_t* data, std::size_t len)>;

// write_frame/read_frame: the per-sample wire frame (4-byte BE length prefix
// + WireFrame JSON body, capped at 16 MiB). read_frame returns
// ErrFrameTooLarge if the declared length exceeds the cap, or
// ErrInvalidFrame if the body is not valid WireFrame JSON.
std::error_code write_frame(const WriteFn& w, const WireFrame& f);
std::error_code read_frame(const ReadFn& r, WireFrame& out);

// write_auth/read_auth: the auth handshake frame a client sends first when
// Options::token is non-empty (4-byte BE length prefix + raw token bytes,
// capped at 4096 bytes). read_auth returns ErrFrameTooLarge if the declared
// length exceeds the cap.
std::error_code write_auth(const WriteFn& w, const std::string& token);
std::error_code read_auth(const ReadFn& r, std::string& out);

// ── Options (fusa:req REQ-BRIDGE-WAN-003) ─────────────────────────────────────
// fusa:req REQ-BRIDGE-WAN-003

struct Options {
    // topics: DDS topic names to forward. Only used by client Bridges
    // (Bridge::connect); server Bridges (Bridge::serve) publish whatever
    // topic they receive and do not filter.
    std::vector<std::string> topics;

    // qos is applied to all bridged DDS endpoints. Defaults to
    // dds::default_qos() (see grpc.hpp's file-level scope note on why no
    // Go-zero-value special case is needed here).
    dds::QoS qos{dds::default_qos()};

    // token, when non-empty, enables shared-secret authentication: the
    // client sends it as the first frame on connect; the server rejects
    // any client whose token does not match (constant-time compare) by
    // silently dropping the connection. Empty disables auth. For real
    // deployments, use token together with a TLS transport (see file-level
    // scope note — TLS itself is out of scope for this baseline) so the
    // token is not sent in clear text.
    std::string token;
};

// ── Bridge (fusa:req REQ-BRIDGE-WAN-005 REQ-BRIDGE-WAN-006 REQ-BRIDGE-WAN-007) ──
// fusa:req REQ-BRIDGE-WAN-005 REQ-BRIDGE-WAN-006 REQ-BRIDGE-WAN-007

// Bridge is a WAN bridge (server or client side). Create a server Bridge
// with Bridge::serve and a client Bridge with Bridge::connect. Safe for
// concurrent use from multiple threads.
class Bridge {
public:
    ~Bridge();

    Bridge(const Bridge&)            = delete;
    Bridge& operator=(const Bridge&) = delete;

    // serve creates a WAN bridge server listening on addr ("host:port";
    // port 0 lets the OS assign a free port — use addr() to discover it).
    // Samples received from connected clients are published to p using
    // opts.qos, with a lazily-created, per-connection publisher cache (a
    // fresh cache per accepted connection, matching go-DDS's receiveLoop
    // exactly). Returns a non-OK error_code if the listen fails.
    static std::pair<std::shared_ptr<Bridge>, std::error_code>
        serve(std::shared_ptr<dds::IParticipant> p, const std::string& addr, Options opts = {});

    // connect creates a WAN bridge client that dials addr ("host:port").
    // Topic subscriptions (opts.topics) are created synchronously before
    // connect returns, so no sample published after connect returns is
    // missed; if any subscription or the dial fails, every
    // already-created subscription is closed before returning the error
    // (matching go-DDS's Connect cleanup-on-partial-failure exactly). If
    // opts.token is non-empty, the auth handshake runs before connect
    // returns; a handshake failure is reported as this call's error.
    static std::pair<std::shared_ptr<Bridge>, std::error_code>
        connect(std::shared_ptr<dds::IParticipant> p, const std::string& addr, Options opts = {});

    // addr returns the TCP address a server Bridge is listening on
    // ("host:port"). Returns an empty string for client Bridges.
    std::string addr() const;

    // close stops the bridge and waits for all worker threads to exit.
    // Idempotent; also called by the destructor.
    void close();

    // Impl is opaque outside wan.cpp (forward-declared only) — public here
    // solely so wan.cpp's free-function connection/send-loop handlers
    // (which are not Bridge members, to keep them out of the public
    // header) can take `Impl*` by name; nothing outside this translation
    // unit can do anything with the incomplete type.
    struct Impl;

private:
    Bridge();

    std::unique_ptr<Impl> impl_;
};

} // namespace dds::bridge::wan

namespace std {
template <>
struct is_error_code_enum<dds::bridge::wan::Errc> : true_type {};
} // namespace std
