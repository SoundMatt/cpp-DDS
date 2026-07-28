// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/rest/transport.hpp — real HTTP/1.1-over-TCP Server for
// dds::bridge::rest.
//
// Hand-rolls a minimal HTTP/1.1 request/response parser+writer over raw
// TCP sockets (see rest.hpp's file-level scope note for why: no HTTP
// library dependency exists anywhere in this repo). Unlike
// dds::bridge::grpc::transport (which deliberately does *not* speak real
// HTTP/2), this is intended to be a genuinely valid, byte-exact-verified
// HTTP/1.1 server: request-line + header parsing, Content-Length-framed
// unary responses, and Transfer-Encoding: chunked for the SSE stream
// (matching a real go-DDS process's actual wire bytes exactly — captured
// via a raw net.Dial, not net/http's client, which transparently
// de-chunks responses). The one deliberate simplification, carried over
// from the grpc bridge's own precedent, is "one HTTP request per TCP
// connection" — no keep-alive/pipelining of multiple requests on the same
// connection.
//
// Native socket handles never appear in this header (IPv4 TCP via BSD
// sockets on POSIX / Winsock on Windows, mirroring dds::bridge::wan and
// dds::bridge::grpc::transport's platform split) — Server hides all
// socket and thread state behind a pimpl.

#pragma once

#include <dds/bridge/rest/rest.hpp>

#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace dds::bridge::rest {

// Server accepts TCP connections on "host:port" and serves them as
// HTTP/1.1 requests dispatched to a Bridge.
// fusa:req REQ-BRIDGE-REST-009
class Server {
public:
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // serve binds addr ("host:port"; port 0 lets the OS assign a free
    // port — use addr() to discover it) and starts accepting connections
    // in a background thread (one worker thread per accepted connection).
    // Returns a non-OK error_code if the bind/listen fails.
    static std::pair<std::shared_ptr<Server>, std::error_code>
        serve(std::shared_ptr<Bridge> bridge, const std::string& addr);

    // addr returns the bound "host:port" listen address.
    std::string addr() const;

    // close stops accepting new connections, force-closes every open
    // connection (unblocking any in-flight SSE stream at its next poll —
    // see Bridge::run_sse_loop), and joins every worker thread. Idempotent;
    // also called by the destructor.
    void close();

    // Impl is opaque outside transport.cpp (forward-declared only) —
    // public here solely so transport.cpp's free-function connection
    // handler (not a Server member, to keep it out of the public header)
    // can take `Impl*` by name; nothing outside this translation unit can
    // do anything with the incomplete type.
    struct Impl;

private:
    Server();

    std::unique_ptr<Impl> impl_;
};

} // namespace dds::bridge::rest
