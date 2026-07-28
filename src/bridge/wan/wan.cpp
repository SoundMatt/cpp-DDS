// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/wan/wan.hpp>

// C++ port of github.com/SoundMatt/go-DDS bridge/wan/wan.go. See
// include/dds/bridge/wan/wan.hpp for scope notes and deliberate deviations
// from a literal line-for-line port. Platform socket setup mirrors
// src/bridge/grpc/transport.cpp's / src/rtps/transport.cpp's WinsockGuard /
// close_native pattern (this module's own copy, per this repo's established
// per-module self-containment convention).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

namespace dds::bridge::wan {

// ── Error category ────────────────────────────────────────────────────────────

namespace {

class WanErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "dds.bridge.wan"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::frame_too_large: return "wan: frame too large";
        case Errc::unauthorized:    return "wan: unauthorized: token mismatch";
        case Errc::invalid_frame:   return "wan: decode frame: invalid JSON";
        default:                    return "wan: unknown error";
        }
    }
};

} // anonymous namespace

const std::error_category& error_category() noexcept {
    static WanErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

// ── JSON codec (own copy — see wan.hpp's file-level scope note) ──────────────

namespace {

void append_json_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '<':  out += "\\u003c"; break;
        case '>':  out += "\\u003e"; break;
        case '&':  out += "\\u0026"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    std::size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out += "=";
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<std::vector<uint8_t>> base64_decode(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        s.push_back(c);
    }
    if (s.empty()) return std::vector<uint8_t>{};
    if (s.size() % 4 != 0) return std::nullopt;

    std::size_t pad = 0;
    if (s.size() >= 1 && s[s.size() - 1] == '=') pad++;
    if (s.size() >= 2 && s[s.size() - 2] == '=') pad++;

    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k) {
            char c = s[i + static_cast<std::size_t>(k)];
            if (c == '=') { v[k] = 0; continue; }
            v[k] = b64_val(c);
            if (v[k] < 0) return std::nullopt;
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        bool last_block = (i + 4 == s.size());
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        if (!(last_block && pad >= 2)) out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        if (!(last_block && pad >= 1)) out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    return out;
}

// Minimal recursive-descent JSON reader, scoped to exactly what WireFrame
// needs: a flat object of one string field ("t") and one base64-string
// field ("p"). Not a general-purpose parser (see wan.hpp's file-level
// scope note).
class JsonReader {
public:
    explicit JsonReader(const std::string& text) : s_(text) {}

    bool parse_object(std::unordered_map<std::string, std::string>& fields) {
        skip_ws();
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            std::string raw;
            if (!parse_raw_value(raw)) return false;
            fields[key] = raw;
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (i_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char esc = s_[i_++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return false; // unterminated string
    }

private:
    const std::string& s_;
    std::size_t         i_{0};

    void skip_ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }

    bool consume(char c) {
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }

    bool parse_raw_value(std::string& raw) {
        std::size_t start = i_;
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '"') {
            std::string tmp;
            if (!parse_string(tmp)) return false;
        } else if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']';
            int depth = 0;
            bool in_str = false;
            for (; i_ < s_.size(); ++i_) {
                char cc = s_[i_];
                if (in_str) {
                    if (cc == '\\') { ++i_; continue; }
                    if (cc == '"') in_str = false;
                    continue;
                }
                if (cc == '"') { in_str = true; continue; }
                if (cc == open) ++depth;
                else if (cc == close) {
                    --depth;
                    if (depth == 0) { ++i_; break; }
                }
            }
            if (depth != 0) return false;
        } else {
            while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
                   !std::isspace(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
            }
        }
        raw = s_.substr(start, i_ - start);
        return !raw.empty();
    }
};

bool decode_string_field(const std::unordered_map<std::string, std::string>& fields,
                          const char* key, std::string& out) {
    auto it = fields.find(key);
    if (it == fields.end()) { out.clear(); return true; }
    if (it->second == "null") { out.clear(); return true; }
    JsonReader r(it->second);
    return r.parse_string(out);
}

bool decode_bytes_field(const std::unordered_map<std::string, std::string>& fields,
                         const char* key, std::vector<uint8_t>& out) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second == "null") { out.clear(); return true; }
    std::string b64;
    JsonReader r(it->second);
    if (!r.parse_string(b64)) return false;
    auto decoded = base64_decode(b64);
    if (!decoded) return false;
    out = std::move(*decoded);
    return true;
}

} // anonymous namespace

std::string to_json(const WireFrame& f) {
    std::string out = "{\"t\":";
    append_json_string(out, f.topic);
    out += ",\"p\":\"";
    out += base64_encode(f.payload);
    out += "\"}";
    return out;
}

bool from_json(const std::string& text, WireFrame& out) {
    JsonReader r(text);
    std::unordered_map<std::string, std::string> fields;
    if (!r.parse_object(fields)) return false;
    return decode_string_field(fields, "t", out.topic) &&
           decode_bytes_field(fields, "p", out.payload);
}

// ── Frame codec (public; testable with in-memory WriteFn/ReadFn doubles) ─────

namespace {

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u; // matches go-DDS's maxFrameBytes
constexpr uint32_t kMaxTokenBytes = 4096u;                // matches go-DDS's maxTokenBytes

void put_be32(uint8_t hdr[4], uint32_t v) {
    hdr[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    hdr[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    hdr[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    hdr[3] = static_cast<uint8_t>(v & 0xFF);
}

uint32_t get_be32(const uint8_t hdr[4]) {
    return (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
}

} // anonymous namespace

std::error_code write_frame(const WriteFn& w, const WireFrame& f) {
    std::string data = to_json(f);
    uint8_t hdr[4];
    put_be32(hdr, static_cast<uint32_t>(data.size()));
    if (auto ec = w(hdr, sizeof(hdr)); ec) return ec;
    if (data.empty()) return {};
    return w(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::error_code read_frame(const ReadFn& r, WireFrame& out) {
    uint8_t hdr[4];
    if (auto ec = r(hdr, sizeof(hdr)); ec) return ec;
    uint32_t len = get_be32(hdr);
    if (len > kMaxFrameBytes) return ErrFrameTooLarge();
    std::string buf(len, '\0');
    if (len > 0) {
        if (auto ec = r(reinterpret_cast<uint8_t*>(buf.data()), len); ec) return ec;
    }
    if (!from_json(buf, out)) return ErrInvalidFrame();
    return {};
}

std::error_code write_auth(const WriteFn& w, const std::string& token) {
    uint8_t hdr[4];
    put_be32(hdr, static_cast<uint32_t>(token.size()));
    if (auto ec = w(hdr, sizeof(hdr)); ec) return ec;
    if (token.empty()) return {};
    return w(reinterpret_cast<const uint8_t*>(token.data()), token.size());
}

std::error_code read_auth(const ReadFn& r, std::string& out) {
    uint8_t hdr[4];
    if (auto ec = r(hdr, sizeof(hdr)); ec) return ec;
    uint32_t len = get_be32(hdr);
    if (len > kMaxTokenBytes) return ErrFrameTooLarge();
    out.assign(len, '\0');
    if (len == 0) return {};
    return r(reinterpret_cast<uint8_t*>(out.data()), len);
}

// ── Socket plumbing ────────────────────────────────────────────────────────

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
// shutdown_both is required (close() alone is not enough) to reliably
// unblock a *different* thread's concurrent blocking recv()/send() on the
// same fd — e.g. Bridge::close() force-closing a server connection whose
// handle_connection thread is blocked in read_frame()'s recv_exact()
// waiting for the next frame. On Linux, close()-from-another-thread does
// not wake a thread blocked in recv() on that fd (the fd may even be
// silently reused by a racing open() before the blocked thread notices);
// shutdown(fd, SHUT_RDWR) does, per POSIX. macOS/BSD happen to wake a
// blocked recv() on close() too, which is why this only reproduced on
// Linux (gcc-12 CI) and not local macOS testing.
void shutdown_both(NativeSocket s) { ::shutdown(s, SHUT_RDWR); }
#endif

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

WriteFn socket_writer(NativeSocket s) {
    return [s](const uint8_t* data, std::size_t len) -> std::error_code {
        return send_all(s, data, len) ? std::error_code{} : relay::ErrNotConnected();
    };
}

ReadFn socket_reader(NativeSocket s) {
    return [s](uint8_t* data, std::size_t len) -> std::error_code {
        return recv_exact(s, data, len) ? std::error_code{} : relay::ErrNotConnected();
    };
}

// constant_time_equal mirrors Go's crypto/subtle.ConstantTimeCompare
// exactly, including its own documented behavior of returning immediately
// (not in constant time) when lengths differ.
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

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

// listen_on binds host:port (port 0 lets the OS assign a free port) and
// returns the listening socket plus the actually-bound port.
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

} // anonymous namespace

// ── Bridge::Impl ───────────────────────────────────────────────────────────

struct Bridge::Impl {
    std::shared_ptr<dds::IParticipant> p;
    Options                            opts;

    bool        is_server{false};
    std::string bound_host;
    uint16_t    bound_port{0};

    NativeSocket listen_fd{kInvalidSocket};   // server only
    NativeSocket client_conn{kInvalidSocket}; // client only: the single dial

    std::shared_ptr<std::atomic<bool>> stopping{std::make_shared<std::atomic<bool>>(false)};

    std::mutex                conns_mu;
    std::vector<NativeSocket> conns; // connections force-closed by close()

    struct WorkerEntry {
        std::thread                        th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex               workers_mu;
    std::vector<WorkerEntry> workers;
    std::thread              accept_thread; // server only

    // client only: one subscriber per opts.topics entry; a write failure on
    // any topic's sender thread closes all of them (mirrors go's
    // closeAllSubs()), unblocking sibling sender threads at their next poll.
    std::vector<std::shared_ptr<dds::ISubscriber>> subs;
    std::mutex                                     write_mu;

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
            // shutdown() before close(): see shutdown_both's doc comment —
            // required to reliably interrupt another thread possibly
            // blocked in recv()/send() on this same fd right now.
            shutdown_both(c);
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

// ── Server-side connection handling ───────────────────────────────────────

namespace {

// handle_connection mirrors go-DDS's receiveLoop: optionally authenticate,
// then read frames until a transport error, oversized frame, malformed
// JSON, or publisher error, lazily caching one publisher per topic *for
// this connection only* (matches go's per-call `pubs := make(map[...])`).
void handle_connection(NativeSocket conn, std::shared_ptr<dds::IParticipant> p, Options opts,
                        Bridge::Impl* impl) {
    if (!opts.token.empty()) {
        std::string token;
        if (read_auth(socket_reader(conn), token)) {
            close_native(conn);
            impl->remove_conn(conn);
            return;
        }
        if (!constant_time_equal(token, opts.token)) {
            close_native(conn); // unauthorized: drop the connection silently
            impl->remove_conn(conn);
            return;
        }
    }

    std::unordered_map<std::string, std::shared_ptr<dds::IPublisher>> pubs;
    while (true) {
        WireFrame frame;
        if (read_frame(socket_reader(conn), frame)) break;

        std::shared_ptr<dds::IPublisher> pub;
        auto                              it = pubs.find(frame.topic);
        if (it == pubs.end()) {
            auto [new_pub, err] = p->new_publisher(frame.topic, opts.qos);
            if (err) break;
            pubs.emplace(frame.topic, new_pub);
            pub = new_pub;
        } else {
            pub = it->second;
        }
        if (pub->write(frame.payload)) break;
    }

    for (auto& [topic, pub] : pubs) {
        (void)topic;
        pub->close();
    }
    close_native(conn);
    impl->remove_conn(conn);
}

void run_accept_loop(Bridge::Impl* impl) {
    while (!impl->stopping->load()) {
        if (!wait_readable(impl->listen_fd, 200)) continue;
        if (impl->stopping->load()) break;
        NativeSocket conn = ::accept(impl->listen_fd, nullptr, nullptr);
        if (conn == kInvalidSocket) {
            if (impl->stopping->load()) break;
            continue;
        }
        impl->add_conn(conn);

        auto                                done = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<dds::IParticipant> p    = impl->p;
        Options                             opts = impl->opts;

        std::lock_guard<std::mutex> lk(impl->workers_mu);
        impl->reap_finished_locked();
        impl->workers.push_back(Bridge::Impl::WorkerEntry{
            std::thread([conn, p, opts, impl, done] {
                handle_connection(conn, p, opts, impl);
                done->store(true);
            }),
            done});
    }
}

// run_sender_loop mirrors go-DDS's per-topic sendLoop goroutine (see
// wan.hpp's file-level scope note on why this polls rather than selecting).
void run_sender_loop(Bridge::Impl* impl, std::string topic, std::shared_ptr<dds::ISubscriber> sub) {
    auto                  chan = sub->channel();
    constexpr auto        kPollInterval = std::chrono::milliseconds(20);
    while (true) {
        if (impl->stopping->load()) return;

        auto item = chan->recv_until(std::chrono::steady_clock::now() + kPollInterval);
        if (!item) {
            if (chan->is_closed()) return;
            continue;
        }

        WireFrame f{topic, item->payload};
        std::error_code werr;
        {
            std::lock_guard<std::mutex> lk(impl->write_mu);
            werr = write_frame(socket_writer(impl->client_conn), f);
        }
        if (werr) {
            // Mirror go's closeAllSubs(): unblock sibling sender threads at
            // their next poll by closing every topic's subscriber.
            for (auto& s : impl->subs) s->close();
            return;
        }
    }
}

} // anonymous namespace

// ── Bridge ─────────────────────────────────────────────────────────────────

Bridge::Bridge() : impl_(std::make_unique<Impl>()) {}

Bridge::~Bridge() { close(); }

std::pair<std::shared_ptr<Bridge>, std::error_code>
Bridge::serve(std::shared_ptr<dds::IParticipant> p, const std::string& addr, Options opts) {
    std::string host;
    uint16_t    port = 0;
    if (!parse_host_port(addr, host, port)) return {nullptr, relay::ErrNotConnected()};

    auto bound = listen_on(host, port);
    if (!bound) return {nullptr, relay::ErrNotConnected()};

    std::shared_ptr<Bridge> bridge(new Bridge());
    Impl*                   impl = bridge->impl_.get();
    impl->p                      = std::move(p);
    impl->opts                   = std::move(opts);
    impl->is_server               = true;
    impl->listen_fd               = bound->first;
    impl->bound_host               = host.empty() ? "0.0.0.0" : host;
    impl->bound_port               = bound->second;

    impl->accept_thread = std::thread([impl] { run_accept_loop(impl); });

    return {bridge, std::error_code{}};
}

std::pair<std::shared_ptr<Bridge>, std::error_code>
Bridge::connect(std::shared_ptr<dds::IParticipant> p, const std::string& addr, Options opts) {
    std::vector<std::shared_ptr<dds::ISubscriber>> subs;
    for (const auto& topic : opts.topics) {
        auto [sub, err] = p->new_subscriber(topic, opts.qos);
        if (err) {
            for (auto& s : subs) s->close();
            return {nullptr, err};
        }
        subs.push_back(sub);
    }

    std::string host;
    uint16_t    port = 0;
    if (!parse_host_port(addr, host, port)) {
        for (auto& s : subs) s->close();
        return {nullptr, relay::ErrNotConnected()};
    }

    auto conn_opt = connect_to(host, port);
    if (!conn_opt) {
        for (auto& s : subs) s->close();
        return {nullptr, relay::ErrNotConnected()};
    }
    NativeSocket conn = *conn_opt;

    // Authenticate before streaming any samples.
    if (!opts.token.empty()) {
        if (auto ec = write_auth(socket_writer(conn), opts.token); ec) {
            close_native(conn);
            for (auto& s : subs) s->close();
            return {nullptr, ec};
        }
    }

    std::shared_ptr<Bridge> bridge(new Bridge());
    Impl*                   impl = bridge->impl_.get();
    impl->p                      = std::move(p);
    impl->is_server               = false;
    impl->client_conn             = conn;
    impl->subs                    = subs;
    impl->add_conn(conn);

    for (std::size_t i = 0; i < opts.topics.size(); ++i) {
        auto                                done  = std::make_shared<std::atomic<bool>>(false);
        std::string                         topic = opts.topics[i];
        std::shared_ptr<dds::ISubscriber>   sub   = subs[i];
        impl->workers.push_back(Impl::WorkerEntry{
            std::thread([impl, topic, sub, done] {
                run_sender_loop(impl, topic, sub);
                done->store(true);
            }),
            done});
    }

    impl->opts = std::move(opts);

    return {bridge, std::error_code{}};
}

std::string Bridge::addr() const {
    if (!impl_->is_server) return "";
    return impl_->bound_host + ":" + std::to_string(impl_->bound_port);
}

void Bridge::close() {
    std::call_once(impl_->close_once, [this] {
        impl_->stopping->store(true);

        if (impl_->is_server && impl_->listen_fd != kInvalidSocket) {
            close_native(impl_->listen_fd);
            impl_->listen_fd = kInvalidSocket;
        }
        impl_->close_all_conns(); // force-closes accepted conns (server) / the dial (client)
        for (auto& s : impl_->subs) s->close(); // client: unblock sender poll loops promptly

        if (impl_->accept_thread.joinable()) impl_->accept_thread.join();

        std::lock_guard<std::mutex> lk(impl_->workers_mu);
        for (auto& w : impl_->workers) {
            if (w.th.joinable()) w.th.join();
        }
        impl_->workers.clear();
    });
}

} // namespace dds::bridge::wan
