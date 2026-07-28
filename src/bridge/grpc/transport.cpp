// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/grpc/transport.hpp>

// TCP transport for dds::bridge::grpc. See transport.hpp for the wire
// protocol shape. Platform socket setup mirrors src/rtps/transport.cpp's
// WinsockGuard / close_native pattern (this module's own copy, per this
// repo's established per-module self-containment convention — see
// grpc.hpp's file-level scope note).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace dds::bridge::grpc {

namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

struct WinsockGuard {
    WinsockGuard() {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
    }
    ~WinsockGuard() { WSACleanup(); }
};
void ensure_winsock_initialized() {
    static WinsockGuard guard;
    (void)guard;
}
void close_native(NativeSocket s) { ::closesocket(s); }
void shutdown_write(NativeSocket s) { ::shutdown(s, SD_SEND); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void ensure_winsock_initialized() {}
void close_native(NativeSocket s) { ::close(s); }
void shutdown_write(NativeSocket s) { ::shutdown(s, SHUT_WR); }
#endif

constexpr std::size_t kFrameHeaderSize = 5;             // 1-byte flag + 4-byte BE length
constexpr uint32_t    kMaxFrameSize    = 64u * 1024u * 1024u; // 64 MiB safety cap
constexpr std::size_t kMaxHeaderLine   = 8192;

bool send_all(NativeSocket s, const uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = ::send(s, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(s, data + sent, len - sent,
#  if defined(MSG_NOSIGNAL)
                            MSG_NOSIGNAL
#  else
                            0
#  endif
        );
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_exact(NativeSocket s, uint8_t* data, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
#if defined(_WIN32)
        int n = ::recv(s, reinterpret_cast<char*>(data + got), static_cast<int>(len - got), 0);
#else
        ssize_t n = ::recv(s, data + got, len - got, 0);
#endif
        if (n <= 0) return false; // 0 = orderly shutdown; <0 = error
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_one_byte(NativeSocket s, char& out) {
#if defined(_WIN32)
    int n = ::recv(s, &out, 1, 0);
#else
    ssize_t n = ::recv(s, &out, 1, 0);
#endif
    return n == 1;
}

bool wait_readable(NativeSocket fd, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    int n = ::select(0, &set, nullptr, nullptr, &tv);
#else
    int n = ::select(static_cast<int>(fd) + 1, &set, nullptr, nullptr, &tv);
#endif
    return n > 0;
}

// ── Frame I/O (real gRPC message-framing: 1-byte flag + 4-byte BE length) ────

bool write_frame(NativeSocket s, const std::string& payload) {
    uint8_t hdr[kFrameHeaderSize];
    hdr[0]        = 0; // uncompressed
    uint32_t len  = static_cast<uint32_t>(payload.size());
    hdr[1] = static_cast<uint8_t>((len >> 24) & 0xFF);
    hdr[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
    hdr[3] = static_cast<uint8_t>((len >> 8) & 0xFF);
    hdr[4] = static_cast<uint8_t>(len & 0xFF);
    if (!send_all(s, hdr, sizeof(hdr))) return false;
    if (len == 0) return true;
    return send_all(s, reinterpret_cast<const uint8_t*>(payload.data()), len);
}

bool read_frame(NativeSocket s, std::string& out) {
    uint8_t hdr[kFrameHeaderSize];
    if (!recv_exact(s, hdr, sizeof(hdr))) return false;
    uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16) |
                   (uint32_t(hdr[3]) << 8) | uint32_t(hdr[4]);
    if (len > kMaxFrameSize) return false;
    out.assign(len, '\0');
    if (len == 0) return true;
    return recv_exact(s, reinterpret_cast<uint8_t*>(out.data()), len);
}

// ── Header block I/O (method line + optional metadata + blank terminator) ───

bool write_line(NativeSocket s, const std::string& line) {
    std::string l = line;
    l.push_back('\n');
    return send_all(s, reinterpret_cast<const uint8_t*>(l.data()), l.size());
}

bool read_line(NativeSocket s, std::string& out) {
    out.clear();
    char c;
    while (out.size() < kMaxHeaderLine) {
        if (!recv_one_byte(s, c)) return false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
    }
    return false;
}

struct RequestHeader {
    std::string                method;
    std::optional<std::string> authorization;
};

bool write_request_header(NativeSocket s, const std::string& method, const std::string& auth_token) {
    if (!write_line(s, "RPC " + method)) return false;
    if (!auth_token.empty() && !write_line(s, "authorization: Bearer " + auth_token)) return false;
    return write_line(s, "");
}

bool read_request_header(NativeSocket s, RequestHeader& out) {
    std::string line;
    if (!read_line(s, line)) return false;
    if (line.rfind("RPC ", 0) != 0) return false;
    out.method = line.substr(4);
    while (true) {
        if (!read_line(s, line)) return false;
        if (line.empty()) return true;
        auto pos = line.find(':');
        if (pos == std::string::npos) return false;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "authorization") out.authorization = val;
    }
}

// Status line: "STATUS <code> <message>" where an empty message is sent as
// "-" to keep the line well-formed (single space-delimited token).
bool write_status(NativeSocket s, const Status& st) {
    std::string msg = st.message;
    std::replace(msg.begin(), msg.end(), '\n', ' ');
    if (msg.empty()) msg = "-";
    if (!write_line(s, "STATUS " + std::to_string(static_cast<int>(st.code)) + " " + msg)) return false;
    return write_line(s, "");
}

bool read_status(NativeSocket s, Status& out) {
    std::string line;
    if (!read_line(s, line)) return false;
    if (line.rfind("STATUS ", 0) != 0) return false;
    std::string rest = line.substr(7);
    auto        sp   = rest.find(' ');
    std::string code_str = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    std::string msg      = (sp == std::string::npos) ? std::string{} : rest.substr(sp + 1);
    if (msg == "-") msg.clear();
    try {
        std::size_t consumed = 0;
        int         code_val = std::stoi(code_str, &consumed);
        if (consumed != code_str.size()) return false;
        out.code = static_cast<StatusCode>(code_val);
    } catch (...) {
        return false;
    }
    out.message = msg;
    std::string blank;
    return read_line(s, blank) && blank.empty();
}

std::optional<NativeSocket> connect_to(const std::string& host, uint16_t port) {
    ensure_winsock_initialized();
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo*   res      = nullptr;
    std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        return std::nullopt;
    }
    NativeSocket fd = kInvalidSocket;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kInvalidSocket) continue;
        if (::connect(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen)) == 0) break;
        close_native(fd);
        fd = kInvalidSocket;
    }
    ::freeaddrinfo(res);
    if (fd == kInvalidSocket) return std::nullopt;
    return fd;
}

// ── Server-side stream adapters ───────────────────────────────────────────────

class TcpSampleSender final : public SampleSender {
public:
    TcpSampleSender(NativeSocket s, std::shared_ptr<std::atomic<bool>> stopping)
        : s_(s), stopping_(std::move(stopping)) {}

    std::error_code send(const Sample& sample) override {
        if (!write_frame(s_, to_json(sample))) return relay::ErrNotConnected();
        return {};
    }
    bool cancelled() const override { return stopping_->load(); }

private:
    NativeSocket                       s_;
    std::shared_ptr<std::atomic<bool>> stopping_;
};

class TcpPublishReceiver final : public PublishReceiver {
public:
    explicit TcpPublishReceiver(NativeSocket s) : s_(s) {}

    bool recv(PublishRequest& out, Status& err) override {
        std::string raw;
        if (!read_frame(s_, raw)) {
            // Treated as clean end-of-stream: the client half-closes its
            // write side (shutdown(SHUT_WR)) when done sending, which is
            // indistinguishable at this layer from any other short read.
            // See grpc.hpp's file-level scope note.
            err = Status::make_ok();
            return false;
        }
        if (!from_json(raw, out)) {
            err = Status::invalid_argument("malformed request");
            return false;
        }
        return true;
    }

private:
    NativeSocket s_;
};

} // namespace

// ── Server ─────────────────────────────────────────────────────────────────

struct Server::Impl {
    NativeSocket                       listen_fd{kInvalidSocket};
    std::shared_ptr<Bridge>            bridge;
    std::shared_ptr<std::atomic<bool>> stopping{std::make_shared<std::atomic<bool>>(false)};
    uint16_t                           bound_port{0};

    struct WorkerEntry {
        std::thread                        th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex                workers_mu;
    std::vector<WorkerEntry>  workers;
    std::thread               accept_thread;

    void reap_finished_locked() {
        workers.erase(std::remove_if(workers.begin(), workers.end(),
                                      [](WorkerEntry& w) {
                                          if (w.done->load()) {
                                              if (w.th.joinable()) w.th.join();
                                              return true;
                                          }
                                          return false;
                                      }),
                       workers.end());
    }
};

namespace {

void handle_connection(NativeSocket conn, std::shared_ptr<Bridge> bridge,
                        std::shared_ptr<std::atomic<bool>> stopping) {
    RequestHeader hdr;
    if (!read_request_header(conn, hdr)) {
        close_native(conn);
        return;
    }
    Status auth = bridge->check_auth(hdr.authorization);

    if (hdr.method == "Publish") {
        std::string raw;
        if (!read_frame(conn, raw)) { close_native(conn); return; }
        if (!auth.ok()) { write_status(conn, auth); close_native(conn); return; }
        PublishRequest req;
        if (!from_json(raw, req)) {
            write_status(conn, Status::invalid_argument("malformed request"));
            close_native(conn);
            return;
        }
        auto [ack, st] = bridge->publish(req);
        write_status(conn, st);
        if (st.ok()) write_frame(conn, to_json(ack));
        close_native(conn);
        return;
    }

    if (hdr.method == "Subscribe") {
        std::string raw;
        if (!read_frame(conn, raw)) { close_native(conn); return; }
        if (!auth.ok()) { write_status(conn, auth); close_native(conn); return; }
        SubscribeRequest req;
        if (!from_json(raw, req)) {
            write_status(conn, Status::invalid_argument("malformed request"));
            close_native(conn);
            return;
        }
        write_status(conn, Status::make_ok());
        TcpSampleSender sender(conn, stopping);
        (void)bridge->subscribe(req, sender);
        close_native(conn);
        return;
    }

    if (hdr.method == "StreamPublish") {
        if (!auth.ok()) {
            std::string raw;
            while (read_frame(conn, raw)) {} // drain so a well-behaved client isn't RST
            write_status(conn, auth);
            close_native(conn);
            return;
        }
        TcpPublishReceiver receiver(conn);
        auto [ack, st] = bridge->stream_publish(receiver);
        write_status(conn, st);
        if (st.ok()) write_frame(conn, to_json(ack));
        close_native(conn);
        return;
    }

    write_status(conn, Status::invalid_argument("unknown method: " + hdr.method));
    close_native(conn);
}

} // namespace

Server::Server(std::shared_ptr<Bridge> bridge) : impl_(std::make_unique<Impl>()) {
    impl_->bridge = std::move(bridge);
}

Server::~Server() { stop(); }

std::pair<uint16_t, std::error_code> Server::listen(const std::string& bind_addr, uint16_t port) {
    ensure_winsock_initialized();
    NativeSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) return {0, relay::ErrNotConnected()};

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bind_addr.empty() || bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        close_native(fd);
        return {0, relay::ErrNotConnected()};
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_native(fd);
        return {0, relay::ErrNotConnected()};
    }
    if (::listen(fd, 64) != 0) {
        close_native(fd);
        return {0, relay::ErrNotConnected()};
    }

    socklen_t bound_len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &bound_len);
    uint16_t bound_port = ntohs(addr.sin_port);

    impl_->listen_fd  = fd;
    impl_->bound_port = bound_port;

    impl_->accept_thread = std::thread([this] {
        while (!impl_->stopping->load()) {
            if (!wait_readable(impl_->listen_fd, 200)) continue;
            if (impl_->stopping->load()) break;
            NativeSocket conn = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (conn == kInvalidSocket) {
                if (impl_->stopping->load()) break;
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::shared_ptr<Bridge>            bridge   = impl_->bridge;
            std::shared_ptr<std::atomic<bool>> stopping = impl_->stopping;
            std::lock_guard<std::mutex>        lk(impl_->workers_mu);
            impl_->reap_finished_locked();
            impl_->workers.push_back(Impl::WorkerEntry{
                std::thread([conn, bridge, stopping, done] {
                    handle_connection(conn, bridge, stopping);
                    done->store(true);
                }),
                done});
        }
    });

    return {bound_port, std::error_code{}};
}

void Server::stop() {
    bool already_stopping = impl_->stopping->exchange(true);
    if (!already_stopping && impl_->listen_fd != kInvalidSocket) {
        close_native(impl_->listen_fd);
        impl_->listen_fd = kInvalidSocket;
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    std::lock_guard<std::mutex> lk(impl_->workers_mu);
    for (auto& w : impl_->workers) {
        if (w.th.joinable()) w.th.join();
    }
    impl_->workers.clear();
}

uint16_t Server::port() const noexcept { return impl_->bound_port; }

// ── Client ─────────────────────────────────────────────────────────────────

Client::Client(std::string host, uint16_t port, std::string auth_token)
    : host_(std::move(host)), port_(port), auth_token_(std::move(auth_token)) {}

std::pair<PublishAck, Status> Client::publish(const PublishRequest& req) {
    auto conn = connect_to(host_, port_);
    if (!conn) return {PublishAck{}, Status::internal_error("connect failed")};
    NativeSocket s = *conn;
    if (!write_request_header(s, "Publish", auth_token_) || !write_frame(s, to_json(req))) {
        close_native(s);
        return {PublishAck{}, Status::internal_error("write request failed")};
    }
    Status st;
    if (!read_status(s, st)) {
        close_native(s);
        return {PublishAck{}, Status::internal_error("read status failed")};
    }
    PublishAck ack;
    if (st.ok()) {
        std::string raw;
        if (!read_frame(s, raw) || !from_json(raw, ack)) {
            close_native(s);
            return {PublishAck{}, Status::internal_error("read ack failed")};
        }
    }
    close_native(s);
    return {ack, st};
}

Status Client::subscribe(const SubscribeRequest& req, const std::function<bool(const Sample&)>& on_sample) {
    auto conn = connect_to(host_, port_);
    if (!conn) return Status::internal_error("connect failed");
    NativeSocket s = *conn;
    if (!write_request_header(s, "Subscribe", auth_token_) || !write_frame(s, to_json(req))) {
        close_native(s);
        return Status::internal_error("write request failed");
    }
    Status st;
    if (!read_status(s, st)) {
        close_native(s);
        return Status::internal_error("read status failed");
    }
    if (!st.ok()) {
        close_native(s);
        return st;
    }
    while (true) {
        std::string raw;
        if (!read_frame(s, raw)) break; // server ended the stream
        Sample sample;
        if (!from_json(raw, sample)) break; // malformed frame: treat as stream end
        if (on_sample && !on_sample(sample)) break; // caller requested stop
    }
    close_native(s);
    return Status::make_ok();
}

struct Client::StreamPublishCall::Impl {
    NativeSocket fd{kInvalidSocket};
    bool         closed{false};
};

Client::StreamPublishCall::StreamPublishCall() : impl_(std::make_unique<Impl>()) {}

Client::StreamPublishCall::~StreamPublishCall() {
    if (impl_ && !impl_->closed && impl_->fd != kInvalidSocket) {
        close_native(impl_->fd);
    }
}

bool Client::StreamPublishCall::send(const PublishRequest& req) {
    if (impl_->closed || impl_->fd == kInvalidSocket) return false;
    return write_frame(impl_->fd, to_json(req));
}

std::pair<PublishAck, Status> Client::StreamPublishCall::close_and_recv() {
    if (impl_->closed || impl_->fd == kInvalidSocket) {
        return {PublishAck{}, Status::internal_error("call already closed")};
    }
    shutdown_write(impl_->fd);
    Status st;
    if (!read_status(impl_->fd, st)) {
        close_native(impl_->fd);
        impl_->closed = true;
        return {PublishAck{}, Status::internal_error("read status failed")};
    }
    PublishAck ack;
    if (st.ok()) {
        std::string raw;
        if (!read_frame(impl_->fd, raw) || !from_json(raw, ack)) {
            close_native(impl_->fd);
            impl_->closed = true;
            return {PublishAck{}, Status::internal_error("read ack failed")};
        }
    }
    close_native(impl_->fd);
    impl_->closed = true;
    return {ack, st};
}

std::pair<std::unique_ptr<Client::StreamPublishCall>, std::error_code> Client::stream_publish() {
    auto conn = connect_to(host_, port_);
    if (!conn) return {nullptr, relay::ErrNotConnected()};
    NativeSocket s = *conn;
    if (!write_request_header(s, "StreamPublish", auth_token_)) {
        close_native(s);
        return {nullptr, relay::ErrNotConnected()};
    }
    // Constructed via the private default constructor (Client is a friend);
    // impl_->fd is set directly since NativeSocket is opaque to the header.
    std::unique_ptr<StreamPublishCall> call(new StreamPublishCall());
    call->impl_->fd = s;
    return {std::move(call), std::error_code{}};
}

} // namespace dds::bridge::grpc
