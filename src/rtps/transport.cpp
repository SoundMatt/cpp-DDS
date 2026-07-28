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

// Creates an unbound IPv6 UDP socket with IPV6_V6ONLY set, matching Go's
// "udp6" network (IPv6-only, no IPv4-mapped-address dual-stack behavior).
// Returns kInvalidHandle on failure (e.g. no IPv6 stack).
NativeSocketHandle make_udp_socket_v6() {
    ensure_winsock_initialized();
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    NativeSocketHandle h = static_cast<NativeSocketHandle>(s);
#else
    NativeSocketHandle h = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (h == static_cast<NativeSocketHandle>(-1)) return h;
    int one = 1;
#if defined(_WIN32)
    ::setsockopt(static_cast<SOCKET>(h), IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#endif
    return h;
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

// IPv6 analogue of bind_any: binds [::]:port.
bool bind_any_v6(NativeSocketHandle h, int port, int* out_bound_port) {
    struct sockaddr_in6 addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(static_cast<uint16_t>(port));

#if defined(_WIN32)
    int rc = ::bind(static_cast<SOCKET>(h), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#else
    int rc = ::bind(h, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
#endif
    if (rc != 0) return false;

    if (port == 0 && out_bound_port != nullptr) {
        struct sockaddr_in6 actual;
        socklen_t len = sizeof(actual);
#if defined(_WIN32)
        if (::getsockname(static_cast<SOCKET>(h), reinterpret_cast<struct sockaddr*>(&actual), &len) == 0) {
#else
        if (::getsockname(h, reinterpret_cast<struct sockaddr*>(&actual), &len) == 0) {
#endif
            *out_bound_port = ntohs(actual.sin6_port);
        }
    } else if (out_bound_port != nullptr) {
        *out_bound_port = port;
    }
    return true;
}

// Formats a 16-byte IPv6 address as 8 colon-separated hex groups (not
// zero-compressed — valid inet_pton input, matching what recv() needs to
// hand back via UdpPacket::from_address; not intended as a pretty-printer).
std::string format_ipv6(const uint8_t (&addr)[16]) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%x:%x:%x:%x:%x:%x:%x:%x",
                  (addr[0] << 8) | addr[1], (addr[2] << 8) | addr[3],
                  (addr[4] << 8) | addr[5], (addr[6] << 8) | addr[7],
                  (addr[8] << 8) | addr[9], (addr[10] << 8) | addr[11],
                  (addr[12] << 8) | addr[13], (addr[14] << 8) | addr[15]);
    return buf;
}

// Address-family-agnostic destination resolution for send_to: tries IPv4
// first, then IPv6. Returns true and fills in *out_v6 (whether the parsed
// address is IPv6) plus the family-appropriate sockaddr on success.
bool resolve_dest(const std::string& dst_address, int dst_port, struct sockaddr_storage* out,
                   socklen_t* out_len) {
    struct sockaddr_in v4;
    std::memset(&v4, 0, sizeof(v4));
    v4.sin_family = AF_INET;
    v4.sin_port   = htons(static_cast<uint16_t>(dst_port));
#if defined(_WIN32)
    if (InetPtonA(AF_INET, dst_address.c_str(), &v4.sin_addr) == 1) {
#else
    if (::inet_pton(AF_INET, dst_address.c_str(), &v4.sin_addr) == 1) {
#endif
        std::memcpy(out, &v4, sizeof(v4));
        *out_len = sizeof(v4);
        return true;
    }

    struct sockaddr_in6 v6;
    std::memset(&v6, 0, sizeof(v6));
    v6.sin6_family = AF_INET6;
    v6.sin6_port   = htons(static_cast<uint16_t>(dst_port));
#if defined(_WIN32)
    if (InetPtonA(AF_INET6, dst_address.c_str(), &v6.sin6_addr) == 1) {
#else
    if (::inet_pton(AF_INET6, dst_address.c_str(), &v6.sin6_addr) == 1) {
#endif
        std::memcpy(out, &v6, sizeof(v6));
        *out_len = sizeof(v6);
        return true;
    }
    return false;
}

} // namespace

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_), port_(other.port_), family_(other.family_) {
    other.handle_ = kInvalidHandle;
    other.port_   = 0;
    other.family_ = AddressFamily::kIPv4;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_       = other.handle_;
        port_         = other.port_;
        family_       = other.family_;
        other.handle_ = kInvalidHandle;
        other.port_   = 0;
        other.family_ = AddressFamily::kIPv4;
    }
    return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (handle_ != kInvalidHandle) {
        close_native(handle_);
        handle_ = kInvalidHandle;
    }
    port_   = 0;
    family_ = AddressFamily::kIPv4;
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

std::optional<UdpSocket> UdpSocket::bind_unicast_v6(int port) {
    if (port == 0) {
        NativeSocketHandle h = make_udp_socket_v6();
        if (h == kInvalidHandle) return std::nullopt;
        set_reuse_addr(h);
        int bound_port = 0;
        if (!bind_any_v6(h, 0, &bound_port)) {
            close_native(h);
            return std::nullopt;
        }
        set_recv_timeout(h, kRecvTimeoutMs);
        UdpSocket sock;
        sock.handle_ = h;
        sock.port_   = bound_port;
        sock.family_ = AddressFamily::kIPv6;
        return sock;
    }

    // Matches go-DDS's newUnicastSocketV6: try port, port+1, … port+15.
    for (int i = 0; i < 16; ++i) {
        NativeSocketHandle h = make_udp_socket_v6();
        if (h == kInvalidHandle) continue;
        set_reuse_addr(h);
        int candidate    = port + i;
        int bound_port   = candidate;
        if (bind_any_v6(h, candidate, &bound_port)) {
            set_recv_timeout(h, kRecvTimeoutMs);
            UdpSocket sock;
            sock.handle_ = h;
            sock.port_   = bound_port;
            sock.family_ = AddressFamily::kIPv6;
            return sock;
        }
        close_native(h);
    }
    return std::nullopt;
}

std::optional<UdpSocket> UdpSocket::bind_multicast_receive_v6(const std::string& group, int port) {
    NativeSocketHandle h = make_udp_socket_v6();
    if (h == kInvalidHandle) return std::nullopt;
    set_reuse_addr(h);

    int bound_port = port;
    if (bind_any_v6(h, port, &bound_port)) {
        struct ipv6_mreq mreq;
        std::memset(&mreq, 0, sizeof(mreq));
#if defined(_WIN32)
        InetPtonA(AF_INET6, group.c_str(), &mreq.ipv6mr_multiaddr);
#else
        ::inet_pton(AF_INET6, group.c_str(), &mreq.ipv6mr_multiaddr);
#endif
        mreq.ipv6mr_interface = 0; // let the OS pick the default interface

#if defined(_WIN32)
        int rc = ::setsockopt(static_cast<SOCKET>(h), IPPROTO_IPV6, IPV6_JOIN_GROUP,
                               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
#else
        int rc = ::setsockopt(h, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));
#endif
        if (rc == 0) {
            set_recv_timeout(h, kRecvTimeoutMs);
            UdpSocket sock;
            sock.handle_ = h;
            sock.port_   = bound_port;
            sock.family_ = AddressFamily::kIPv6;
            return sock;
        }
    }

    // IPv6 multicast unavailable — fall back to a plain IPv6 unicast bind
    // on the same port, matching bind_multicast_receive's IPv4 fallback
    // (and go-DDS's newMulticastReceiveSocketV6).
    close_native(h);
    NativeSocketHandle h2 = make_udp_socket_v6();
    if (h2 == kInvalidHandle) return std::nullopt;
    set_reuse_addr(h2);
    int fallback_port = port;
    if (!bind_any_v6(h2, port, &fallback_port)) {
        close_native(h2);
        return std::nullopt;
    }
    set_recv_timeout(h2, kRecvTimeoutMs);
    UdpSocket sock;
    sock.handle_ = h2;
    sock.port_   = fallback_port;
    sock.family_ = AddressFamily::kIPv6;
    return sock;
}

bool UdpSocket::send_to(const std::string& dst_address, int dst_port,
                         const uint8_t* data, std::size_t len) const {
    if (!valid()) return false;

    struct sockaddr_storage addr;
    socklen_t                addr_len = 0;
    if (!resolve_dest(dst_address, dst_port, &addr, &addr_len)) return false;

#if defined(_WIN32)
    int n = ::sendto(static_cast<SOCKET>(handle_), reinterpret_cast<const char*>(data),
                      static_cast<int>(len), 0, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
#else
    ssize_t n = ::sendto(handle_, data, len, 0, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
#endif
    return n == static_cast<decltype(n)>(len);
}

std::optional<UdpPacket> UdpSocket::recv() const {
    if (!valid()) return std::nullopt;

    std::vector<uint8_t> buf(kMaxUdpSize);
    struct sockaddr_storage from;
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

    if (family_ == AddressFamily::kIPv6) {
        auto* from6 = reinterpret_cast<struct sockaddr_in6*>(&from);
        uint8_t raw[16];
        std::memcpy(raw, &from6->sin6_addr, 16);
        pkt.from_address = format_ipv6(raw);
        pkt.from_port    = ntohs(from6->sin6_port);
    } else {
        auto* from4 = reinterpret_cast<struct sockaddr_in*>(&from);
        char addr_str[INET_ADDRSTRLEN] = {0};
#if defined(_WIN32)
        InetNtopA(AF_INET, &from4->sin_addr, addr_str, sizeof(addr_str));
#else
        ::inet_ntop(AF_INET, &from4->sin_addr, addr_str, sizeof(addr_str));
#endif
        pkt.from_address = addr_str;
        pkt.from_port    = ntohs(from4->sin_port);
    }
    return pkt;
}

} // namespace dds::rtps
