// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/rtps/transport.hpp>

// C++ port of github.com/SoundMatt/go-DDS rtps/transport.go. See
// include/dds/rtps/transport.hpp for the phase scope and the deliberate
// deviations from a literal line-for-line port (IPv4-only, synchronous
// recv() instead of a background readLoop goroutine, INADDR_ANY-based
// multicast interface selection).

#include <cstring>

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
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace dds::rtps {

namespace {

constexpr std::size_t kMaxUdpSize   = 65535;
constexpr int         kRecvTimeoutMs = 250; // matches go-DDS's 250ms SetReadDeadline poll

#if defined(_WIN32)

// Windows requires WSAStartup before any socket call and WSACleanup once
// the process is done with sockets. A function-local static gives
// thread-safe, exactly-once initialization (C++11 magic statics) and the
// destructor runs WSACleanup at process teardown.
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

void close_native(NativeSocketHandle h) { ::closesocket(static_cast<SOCKET>(h)); }

bool set_recv_timeout(NativeSocketHandle h, int ms) {
    DWORD timeout = static_cast<DWORD>(ms);
    return ::setsockopt(static_cast<SOCKET>(h), SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}

bool would_block_or_timeout() {
    int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
}

#else // POSIX

void ensure_winsock_initialized() {}

void close_native(NativeSocketHandle h) { ::close(h); }

bool set_recv_timeout(NativeSocketHandle h, int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool would_block_or_timeout() { return errno == EAGAIN || errno == EWOULDBLOCK; }

#endif

// Creates an unbound IPv4 UDP socket. Returns kInvalidHandle on failure.
NativeSocketHandle make_udp_socket() {
    ensure_winsock_initialized();
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return static_cast<NativeSocketHandle>(s);
#else
    return ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
}

bool set_reuse_addr(NativeSocketHandle h) {
    int one = 1;
#if defined(_WIN32)
    bool ok = ::setsockopt(static_cast<SOCKET>(h), SOL_SOCKET, SO_REUSEADDR,
                            reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
#else
    bool ok = ::setsockopt(h, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0;
#  if defined(SO_REUSEPORT)
    ::setsockopt(h, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)); // best-effort
#  endif
#endif
    return ok;
}

bool bind_any(NativeSocketHandle h, int port, int* out_bound_port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));

#if defined(_WIN32)
    int rc = ::bind(static_cast<SOCKET>(h), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#else
    int rc = ::bind(h, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#endif
    if (rc != 0) return false;

    if (port == 0 && out_bound_port != nullptr) {
        struct sockaddr_in actual;
        socklen_t len = sizeof(actual);
#if defined(_WIN32)
        if (::getsockname(static_cast<SOCKET>(h), reinterpret_cast<struct sockaddr*>(&actual), &len) == 0) {
#else
        if (::getsockname(h, reinterpret_cast<struct sockaddr*>(&actual), &len) == 0) {
#endif
            *out_bound_port = ntohs(actual.sin_port);
        }
    } else if (out_bound_port != nullptr) {
        *out_bound_port = port;
    }
    return true;
}

} // namespace

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
    other.handle_ = kInvalidHandle;
    other.port_   = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_       = other.handle_;
        port_         = other.port_;
        other.handle_ = kInvalidHandle;
        other.port_   = 0;
    }
    return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (handle_ != kInvalidHandle) {
        close_native(handle_);
        handle_ = kInvalidHandle;
    }
    port_ = 0;
}

std::optional<UdpSocket> UdpSocket::bind_unicast(int port) {
    if (port == 0) {
        NativeSocketHandle h = make_udp_socket();
        if (h == kInvalidHandle) return std::nullopt;
        set_reuse_addr(h);
        int bound_port = 0;
        if (!bind_any(h, 0, &bound_port)) {
            close_native(h);
            return std::nullopt;
        }
        set_recv_timeout(h, kRecvTimeoutMs);
        UdpSocket sock;
        sock.handle_ = h;
        sock.port_   = bound_port;
        return sock;
    }

    // Matches go-DDS's newUnicastSocket: try port, port+1, … port+15.
    for (int i = 0; i < 16; ++i) {
        NativeSocketHandle h = make_udp_socket();
        if (h == kInvalidHandle) continue;
        set_reuse_addr(h);
        int candidate = port + i;
        int bound_port = candidate;
        if (bind_any(h, candidate, &bound_port)) {
            set_recv_timeout(h, kRecvTimeoutMs);
            UdpSocket sock;
            sock.handle_ = h;
            sock.port_   = bound_port;
            return sock;
        }
        close_native(h);
    }
    return std::nullopt;
}

std::optional<UdpSocket> UdpSocket::bind_multicast_receive(const std::string& group, int port) {
    NativeSocketHandle h = make_udp_socket();
    if (h == kInvalidHandle) return std::nullopt;
    set_reuse_addr(h);

    int bound_port = port;
    if (bind_any(h, port, &bound_port)) {
        struct ip_mreq mreq;
        std::memset(&mreq, 0, sizeof(mreq));
#if defined(_WIN32)
        InetPtonA(AF_INET, group.c_str(), &mreq.imr_multiaddr);
#else
        ::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
#endif
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

#if defined(_WIN32)
        int rc = ::setsockopt(static_cast<SOCKET>(h), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
#else
        int rc = ::setsockopt(h, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
#endif
        if (rc == 0) {
            set_recv_timeout(h, kRecvTimeoutMs);
            UdpSocket sock;
            sock.handle_ = h;
            sock.port_   = bound_port;
            return sock;
        }
    }

    // Multicast unavailable (no multicast-capable route, sandboxed CI,
    // etc.) — fall back to a plain unicast bind on the same port so the
    // caller can still start. Matches go-DDS's newMulticastReceiveSocket
    // fallback: same-host delivery keeps working, only true cross-host
    // multicast discovery is disabled.
    close_native(h);
    NativeSocketHandle h2 = make_udp_socket();
    if (h2 == kInvalidHandle) return std::nullopt;
    set_reuse_addr(h2);
    int fallback_port = port;
    if (!bind_any(h2, port, &fallback_port)) {
        close_native(h2);
        return std::nullopt;
    }
    set_recv_timeout(h2, kRecvTimeoutMs);
    UdpSocket sock;
    sock.handle_ = h2;
    sock.port_   = fallback_port;
    return sock;
}

bool UdpSocket::send_to(const std::string& dst_address, int dst_port,
                         const uint8_t* data, std::size_t len) const {
    if (!valid()) return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(dst_port));
#if defined(_WIN32)
    if (InetPtonA(AF_INET, dst_address.c_str(), &addr.sin_addr) != 1) return false;
#else
    if (::inet_pton(AF_INET, dst_address.c_str(), &addr.sin_addr) != 1) return false;
#endif

#if defined(_WIN32)
    int n = ::sendto(static_cast<SOCKET>(handle_), reinterpret_cast<const char*>(data),
                      static_cast<int>(len), 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#else
    ssize_t n = ::sendto(handle_, data, len, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#endif
    return n == static_cast<decltype(n)>(len);
}

std::optional<UdpPacket> UdpSocket::recv() const {
    if (!valid()) return std::nullopt;

    std::vector<uint8_t> buf(kMaxUdpSize);
    struct sockaddr_in from;
    std::memset(&from, 0, sizeof(from));
    socklen_t from_len = sizeof(from);

#if defined(_WIN32)
    int n = ::recvfrom(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(buf.data()),
                        static_cast<int>(buf.size()), 0, reinterpret_cast<struct sockaddr*>(&from), &from_len);
#else
    ssize_t n = ::recvfrom(handle_, buf.data(), buf.size(), 0,
                            reinterpret_cast<struct sockaddr*>(&from), &from_len);
#endif
    if (n < 0) {
        if (would_block_or_timeout()) return std::nullopt; // poll timeout — caller loops
        return std::nullopt;                                // other error
    }

    UdpPacket pkt;
    pkt.data.assign(buf.begin(), buf.begin() + n);
    char addr_str[INET_ADDRSTRLEN] = {0};
#if defined(_WIN32)
    InetNtopA(AF_INET, &from.sin_addr, addr_str, sizeof(addr_str));
#else
    ::inet_ntop(AF_INET, &from.sin_addr, addr_str, sizeof(addr_str));
#endif
    pkt.from_address = addr_str;
    pkt.from_port    = ntohs(from.sin_port);
    return pkt;
}

} // namespace dds::rtps
