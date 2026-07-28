// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/bridge/grpc/transport.hpp — TCP Server/Client for dds::bridge::grpc.
//
// Speaks the minimal call-oriented wire protocol described in grpc.hpp's
// file-level scope note: a text header block (method name + optional
// "authorization: Bearer <token>") followed by real gRPC-length-prefixed
// JSON message frames. One TCP connection per RPC call. See grpc.hpp for
// why this isn't a real HTTP/2 gRPC transport and what a future upgrade
// path would look like.
//
// Native socket handles never appear in this header (IPv4 TCP via BSD
// sockets on POSIX / Winsock on Windows, mirroring rtps/transport.hpp's
// platform split) — both Server and Client hide their platform state
// behind a pimpl so this header stays platform-neutral, like every other
// public cpp-DDS header.

#pragma once

#include <dds/bridge/grpc/grpc.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace dds::bridge::grpc {

// Server accepts TCP connections on `bind_addr:port` and dispatches each
// call to a Bridge. fusa:req REQ-BRIDGE-GRPC-008
class Server {
public:
    explicit Server(std::shared_ptr<Bridge> bridge);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // listen binds bind_addr:port (port 0 lets the OS assign a free port)
    // and starts serving in a background thread (one worker thread per
    // accepted connection). Returns the bound port and an OK error_code on
    // success, or 0 + a non-OK error_code on failure. Not idempotent —
    // call at most once per Server instance.
    std::pair<uint16_t, std::error_code> listen(const std::string& bind_addr, uint16_t port);

    // stop closes the listening socket and joins every in-flight
    // connection handler. Any Subscribe call blocked in Bridge::subscribe
    // observes SampleSender::cancelled() on its next poll and returns
    // Status::cancelled(), unblocking its handler thread. Idempotent; also
    // called by the destructor.
    void stop();

    // port returns the bound port, or 0 if listen() has not succeeded.
    uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Client issues DDSBridge RPCs to a Server over the wire protocol above.
// fusa:req REQ-BRIDGE-GRPC-009
class Client {
public:
    // auth_token, if non-empty, is sent as "authorization: Bearer
    // <auth_token>" on every call (mirrors go-DDS's grpc.WithPerRPCCredentials
    // bearerToken helper used by its own test suite).
    Client(std::string host, uint16_t port, std::string auth_token = {});

    // publish issues a unary Publish RPC.
    std::pair<PublishAck, Status> publish(const PublishRequest& req);

    // subscribe opens a server-streaming Subscribe call and invokes
    // on_sample for each delivered Sample until the server ends the
    // stream (returns Status::make_ok()) or on_sample returns false
    // (caller-requested stop; also returns Status::make_ok()). Blocks the
    // calling thread for the call's duration — call from a dedicated
    // thread for a long-lived subscription. A non-OK return means the
    // call itself failed (bad topic, auth, transport error) before or
    // during streaming.
    Status subscribe(const SubscribeRequest& req,
                      const std::function<bool(const Sample&)>& on_sample);

    // StreamPublishCall is an open client-streaming StreamPublish call:
    // call send() any number of times, then close_and_recv() exactly once.
    class StreamPublishCall {
    public:
        ~StreamPublishCall();

        StreamPublishCall(const StreamPublishCall&)            = delete;
        StreamPublishCall& operator=(const StreamPublishCall&) = delete;

        // send writes one PublishRequest frame. Returns false on a
        // transport error (the call should then be abandoned).
        bool send(const PublishRequest& req);

        // close_and_recv half-closes the request stream (signaling
        // end-of-stream to the server, mirrored on the server side as a
        // clean PublishReceiver::recv EOF) and reads back the final
        // PublishAck/Status. Call exactly once.
        std::pair<PublishAck, Status> close_and_recv();

    private:
        friend class Client;
        StreamPublishCall();
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // stream_publish opens a client-streaming StreamPublish call. Returns
    // nullptr + a non-OK error_code if the initial connection/handshake
    // fails.
    std::pair<std::unique_ptr<StreamPublishCall>, std::error_code> stream_publish();

private:
    std::string host_;
    uint16_t    port_;
    std::string auth_token_;
};

} // namespace dds::bridge::grpc
