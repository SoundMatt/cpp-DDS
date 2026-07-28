// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/rest/transport.hpp>

// Real HTTP/1.1-over-TCP transport for dds::bridge::rest. See
// transport.hpp for the wire protocol shape and file-level scope note.
// Platform socket setup mirrors dds::bridge::wan's / dds::bridge::grpc::
// transport's WinsockGuard / close_native pattern (this module's own
// copy, per this repo's established per-module self-containment
// convention).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
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

namespace dds::bridge::rest {

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
void shutdown_both(NativeSocket s) { ::shutdown(s, SD_BOTH); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void ensure_winsock_initialized() {}
void close_native(NativeSocket s) { ::close(s); }
// shutdown_both (not just close()) is required to reliably interrupt a
// *different* thread's concurrent blocking send()/recv() on the same fd —
// see dds::bridge::wan::wan.cpp's shutdown_both doc comment for the full
// Linux-vs-macOS rationale (this module hits the identical scenario: a
// worker thread possibly blocked inside recv() waiting for the next
// request, or send() while streaming SSE chunks, when close() force-
// closes its connection from a different thread).
void shutdown_both(NativeSocket s) { ::shutdown(s, SHUT_RDWR); }
#endif

constexpr std::size_t kMaxHeaderLine = 8192;
constexpr std::size_t kMaxHeaders    = 64;
constexpr uint64_t    kMaxBodyBytes  = 16ull * 1024ull * 1024ull; // 16 MiB — see
                                                                   // rest.hpp's file-level
                                                                   // scope note on oversized
                                                                   // bodies.

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

bool send_all(NativeSocket s, const std::string& data) {
    return send_all(s, reinterpret_cast<const uint8_t*>(data.data()), data.size());
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

// ── Minimal HTTP/1.1 request line/header reader ───────────────────────────

bool read_line(NativeSocket s, std::string& out) {
    out.clear();
    char c;
    while (out.size() < kMaxHeaderLine) {
        if (!recv_one_byte(s, c)) return false;
        if (c == '\n') {
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        out.push_back(c);
    }
    return false; // line too long
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_leading_space(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

struct HttpRequest {
    std::string                                method;
    std::string                                path; // query string already stripped
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    std::vector<uint8_t>                       body;
};

enum class ParseOutcome { ok, connection_error, bad_request };

// read_request parses one HTTP/1.1 request off `s`. On ParseOutcome::
// bad_request, `detail` describes the problem (used verbatim in the 400
// response body — see rest.hpp's file-level scope note: exact wording
// parity with go-DDS isn't a goal here, only status-code parity is).
ParseOutcome read_request(NativeSocket s, HttpRequest& out, std::string& detail) {
    std::string request_line;
    if (!read_line(s, request_line)) return ParseOutcome::connection_error;
    if (request_line.empty()) return ParseOutcome::connection_error;

    auto sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) {
        detail = "malformed request line";
        return ParseOutcome::bad_request;
    }
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        detail = "malformed request line";
        return ParseOutcome::bad_request;
    }
    out.method          = request_line.substr(0, sp1);
    std::string target  = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty()) {
        detail = "malformed request line";
        return ParseOutcome::bad_request;
    }

    auto qpos = target.find('?');
    out.path  = (qpos == std::string::npos) ? target : target.substr(0, qpos);

    for (std::size_t n = 0; n < kMaxHeaders; ++n) {
        std::string line;
        if (!read_line(s, line)) return ParseOutcome::connection_error;
        if (line.empty()) break; // blank line: end of headers
        auto cpos = line.find(':');
        if (cpos == std::string::npos) {
            detail = "malformed header line";
            return ParseOutcome::bad_request;
        }
        std::string key = to_lower(line.substr(0, cpos));
        std::string val = trim_leading_space(line.substr(cpos + 1));
        out.headers[key] = val;
    }

    if (auto it = out.headers.find("content-length"); it != out.headers.end()) {
        long long   len      = -1;
        std::size_t consumed = 0;
        try {
            len = std::stoll(it->second, &consumed);
        } catch (...) {
            detail = "invalid content-length";
            return ParseOutcome::bad_request;
        }
        if (consumed != it->second.size() || len < 0) {
            detail = "invalid content-length";
            return ParseOutcome::bad_request;
        }
        if (static_cast<uint64_t>(len) > kMaxBodyBytes) {
            detail = "content-length exceeds 16 MiB cap";
            return ParseOutcome::bad_request;
        }
        out.body.assign(static_cast<std::size_t>(len), 0);
        if (len > 0 && !recv_exact(s, out.body.data(), static_cast<std::size_t>(len))) {
            detail = "read body: connection error";
            return ParseOutcome::bad_request;
        }
    }

    return ParseOutcome::ok;
}

// ── Minimal HTTP/1.1 response writer ──────────────────────────────────────

const char* reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

bool write_json_response(NativeSocket s, int status, const std::string& content_type, const std::string& body) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out += " ";
    out += reason_phrase(status);
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return send_all(s, out);
}

// write_error_response mirrors go's http.Error: Content-Type: text/plain;
// charset=utf-8, X-Content-Type-Options: nosniff, body = message + "\n".
bool write_error_response(NativeSocket s, int status, const std::string& message) {
    std::string body = message + "\n";
    std::string out  = "HTTP/1.1 ";
    out += std::to_string(status);
    out += " ";
    out += reason_phrase(status);
    out += "\r\nContent-Type: text/plain; charset=utf-8\r\nX-Content-Type-Options: nosniff\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return send_all(s, out);
}

bool write_no_content_response(NativeSocket s) {
    return send_all(s, std::string("HTTP/1.1 204 No Content\r\n\r\n"));
}

bool write_sse_headers(NativeSocket s) {
    return send_all(s, std::string("HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/event-stream\r\n"
                                    "Cache-Control: no-cache\r\n"
                                    "Connection: keep-alive\r\n"
                                    "X-Accel-Buffering: no\r\n"
                                    "Transfer-Encoding: chunked\r\n"
                                    "\r\n"));
}

bool write_chunk(NativeSocket s, const std::string& data) {
    char hex[2 * sizeof(std::size_t) + 4];
    int  n = std::snprintf(hex, sizeof(hex), "%zx\r\n", data.size());
    if (n <= 0) return false;
    std::string out(hex, static_cast<std::size_t>(n));
    out += data;
    out += "\r\n";
    return send_all(s, out);
}

bool write_chunk_terminator(NativeSocket s) { return send_all(s, std::string("0\r\n\r\n")); }

// ── SseSink backed by a real socket ───────────────────────────────────────

class TcpSseSink final : public SseSink {
public:
    TcpSseSink(NativeSocket s, std::shared_ptr<std::atomic<bool>> stopping)
        : s_(s), stopping_(std::move(stopping)) {}

    std::error_code send_message(uint64_t id, const std::vector<uint8_t>& payload) override {
        if (!write_chunk(s_, sse_message_event(id, payload))) return relay::ErrNotConnected();
        return {};
    }
    std::error_code send_keepalive() override {
        if (!write_chunk(s_, sse_keepalive_comment())) return relay::ErrNotConnected();
        return {};
    }
    bool cancelled() const override { return stopping_->load(); }

private:
    NativeSocket                       s_;
    std::shared_ptr<std::atomic<bool>> stopping_;
};

// parse_host_port splits "host:port" on the last ':', matching the only
// address form this bridge's API surface and tests use (IPv4 host:port).
bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port) {
    auto pos = addr.rfind(':');
    if (pos == std::string::npos) return false;
    host                     = addr.substr(0, pos);
    std::string port_str = addr.substr(pos + 1);
    if (port_str.empty()) return false;
    try {
        std::size_t consumed = 0;
        int         v        = std::stoi(port_str, &consumed);
        if (consumed != port_str.size() || v < 0 || v > 65535) return false;
        port = static_cast<uint16_t>(v);
    } catch (...) {
        return false;
    }
    return true;
}

std::optional<std::pair<NativeSocket, uint16_t>> listen_on(const std::string& host, uint16_t port) {
    ensure_winsock_initialized();
    NativeSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) return std::nullopt;

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_native(fd);
        return std::nullopt;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_native(fd);
        return std::nullopt;
    }
    if (::listen(fd, 64) != 0) {
        close_native(fd);
        return std::nullopt;
    }

    socklen_t bound_len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &bound_len);
    uint16_t bound_port = ntohs(addr.sin_port);

    return std::make_pair(fd, bound_port);
}

} // namespace

// ── Server::Impl ───────────────────────────────────────────────────────────

struct Server::Impl {
    std::shared_ptr<Bridge> bridge;

    NativeSocket listen_fd{kInvalidSocket};
    std::string  bound_host;
    uint16_t     bound_port{0};

    std::shared_ptr<std::atomic<bool>> stopping{std::make_shared<std::atomic<bool>>(false)};

    std::mutex                conns_mu;
    std::vector<NativeSocket> conns;

    struct WorkerEntry {
        std::thread                        th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex                workers_mu;
    std::vector<WorkerEntry>  workers;
    std::thread               accept_thread;

    std::once_flag close_once;

    void add_conn(NativeSocket c) {
        std::lock_guard<std::mutex> lk(conns_mu);
        conns.push_back(c);
    }
    void remove_conn(NativeSocket c) {
        std::lock_guard<std::mutex> lk(conns_mu);
        conns.erase(std::remove(conns.begin(), conns.end(), c), conns.end());
    }
    void close_all_conns() {
        std::lock_guard<std::mutex> lk(conns_mu);
        for (auto c : conns) {
            shutdown_both(c); // see shutdown_both's doc comment above
            close_native(c);
        }
        conns.clear();
    }
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

// handle_connection parses exactly one HTTP/1.1 request (see transport.hpp's
// file-level scope note on "one request per connection"), authorizes it,
// routes it, and — for Route::subscribe — streams the SSE response until
// the loop ends for any reason, then closes the connection.
void handle_connection(NativeSocket conn, std::shared_ptr<Bridge> bridge,
                        std::shared_ptr<std::atomic<bool>> stopping, Server::Impl* impl) {
    HttpRequest  req;
    std::string  detail;
    ParseOutcome outcome = read_request(conn, req, detail);

    if (outcome == ParseOutcome::connection_error) {
        close_native(conn);
        impl->remove_conn(conn);
        return;
    }
    if (outcome == ParseOutcome::bad_request) {
        write_error_response(conn, 400, detail);
        close_native(conn);
        impl->remove_conn(conn);
        return;
    }

    std::optional<std::string> auth_header;
    if (auto it = req.headers.find("authorization"); it != req.headers.end()) auth_header = it->second;
    if (!bridge->authorize(auth_header)) {
        write_error_response(conn, 401, "unauthorized");
        close_native(conn);
        impl->remove_conn(conn);
        return;
    }

    RouteResult route = classify_request(req.method, req.path);
    switch (route.route) {
    case Route::list: {
        write_json_response(conn, 200, "application/json", topics_to_json(bridge->list_topics()));
        break;
    }
    case Route::publish: {
        Result result = bridge->handle_publish(route.topic, req.body);
        if (!result.ok) {
            write_error_response(conn, 500, result.message);
        } else {
            write_no_content_response(conn);
        }
        break;
    }
    case Route::subscribe: {
        auto [sub, err] = bridge->get_subscriber(route.topic);
        if (err) {
            write_error_response(conn, 500, "subscriber: " + err.message());
            break;
        }
        if (write_sse_headers(conn)) {
            TcpSseSink sink(conn, stopping);
            (void)bridge->run_sse_loop(sub, sink);
            write_chunk_terminator(conn); // best-effort; ignore failure
        }
        break;
    }
    case Route::bad_request:
        write_error_response(conn, 400, "topic name required");
        break;
    case Route::method_not_allowed:
        write_error_response(conn, 405, "method not allowed");
        break;
    }

    close_native(conn);
    impl->remove_conn(conn);
}

void run_accept_loop(Server::Impl* impl) {
    while (!impl->stopping->load()) {
        if (!wait_readable(impl->listen_fd, 200)) continue;
        if (impl->stopping->load()) break;
        NativeSocket conn = ::accept(impl->listen_fd, nullptr, nullptr);
        if (conn == kInvalidSocket) {
            if (impl->stopping->load()) break;
            continue;
        }
        impl->add_conn(conn);

        auto                    done     = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<Bridge> bridge   = impl->bridge;
        auto                    stopping = impl->stopping;

        std::lock_guard<std::mutex> lk(impl->workers_mu);
        impl->reap_finished_locked();
        impl->workers.push_back(Server::Impl::WorkerEntry{
            std::thread([conn, bridge, stopping, impl, done] {
                handle_connection(conn, bridge, stopping, impl);
                done->store(true);
            }),
            done});
    }
}

} // namespace

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() { close(); }

std::pair<std::shared_ptr<Server>, std::error_code>
Server::serve(std::shared_ptr<Bridge> bridge, const std::string& addr) {
    std::string host;
    uint16_t    port = 0;
    if (!parse_host_port(addr, host, port)) return {nullptr, relay::ErrNotConnected()};

    auto bound = listen_on(host, port);
    if (!bound) return {nullptr, relay::ErrNotConnected()};

    std::shared_ptr<Server> server(new Server());
    Impl*                   impl = server->impl_.get();
    impl->bridge                 = std::move(bridge);
    impl->listen_fd               = bound->first;
    impl->bound_host               = host.empty() ? "0.0.0.0" : host;
    impl->bound_port               = bound->second;

    impl->accept_thread = std::thread([impl] { run_accept_loop(impl); });

    return {server, std::error_code{}};
}

std::string Server::addr() const { return impl_->bound_host + ":" + std::to_string(impl_->bound_port); }

void Server::close() {
    std::call_once(impl_->close_once, [this] {
        impl_->stopping->store(true);

        if (impl_->listen_fd != kInvalidSocket) {
            close_native(impl_->listen_fd);
            impl_->listen_fd = kInvalidSocket;
        }
        impl_->close_all_conns();

        if (impl_->accept_thread.joinable()) impl_->accept_thread.join();

        std::lock_guard<std::mutex> lk(impl_->workers_mu);
        for (auto& w : impl_->workers) {
            if (w.th.joinable()) w.th.join();
        }
        impl_->workers.clear();
    });
}

} // namespace dds::bridge::rest
