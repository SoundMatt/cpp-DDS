// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Linux socket-tuning implementation. C++ port of
// github.com/SoundMatt/go-DDS's rtps/traffic_linux.go. This file is only
// added to the build when CMAKE_SYSTEM_NAME is "Linux" — see
// CMakeLists.txt — mirroring go-DDS's `//go:build linux` split. See
// include/dds/rtps/traffic.hpp for the API contract.

#include <dds/rtps/traffic.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

namespace dds::rtps {

namespace {

// Linux socket / cmsg constants mirrored as literal values rather than
// pulled from <linux/net_tstamp.h>, exactly as go-DDS's traffic_linux.go
// does — that kernel header is not guaranteed present in every glibc/musl
// build environment, and these numeric values are stable ABI (Linux
// never renumbers socket options).
constexpr int kClockTai  = 11; // CLOCK_TAI
constexpr int kSoTxTime  = 61; // SO_TXTIME
constexpr int kScmTxTime = 61; // SCM_TXTIME

// Mirrors struct sock_txtime from <linux/net_tstamp.h>.
struct SockTxTime {
    int32_t  clockid;
    uint32_t flags;
};

} // namespace

bool set_sock_priority(const UdpSocket& sock, int priority) {
    if (!sock.valid()) return false;
    return ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0;
}

bool set_sock_tos(const UdpSocket& sock, uint8_t dscp) {
    if (!sock.valid()) return false;
    int tos = static_cast<int>(dscp) << 2;
    return ::setsockopt(sock.native_handle(), IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0;
}

bool enable_tx_time(const UdpSocket& sock) {
    if (!sock.valid()) return false;
    SockTxTime cfg{kClockTai, 0};
    return ::setsockopt(sock.native_handle(), SOL_SOCKET, kSoTxTime, &cfg, sizeof(cfg)) == 0;
}

bool clock_tai_now(uint64_t& out_ns) {
    struct timespec ts;
    if (::clock_gettime(static_cast<clockid_t>(kClockTai), &ts) != 0) return false;
    out_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    return true;
}

bool scheduled_send(const UdpSocket& sock, const std::string& dst_address, int dst_port,
                     const uint8_t* data, std::size_t len, uint64_t tx_time_ns) {
    if (tx_time_ns == 0) {
        return sock.send_to(dst_address, dst_port, data, len);
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(dst_port));
    if (::inet_pton(AF_INET, dst_address.c_str(), &addr.sin_addr) != 1) {
        return false;
    }

    alignas(alignof(std::max_align_t)) uint8_t cmsg_buf[CMSG_SPACE(sizeof(uint64_t))];
    std::memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct iovec iov;
    iov.iov_base = const_cast<uint8_t*>(data);
    iov.iov_len  = len;

    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_name       = &addr;
    msg.msg_namelen    = sizeof(addr);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = kScmTxTime;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(uint64_t));
    std::memcpy(CMSG_DATA(cmsg), &tx_time_ns, sizeof(uint64_t));

    ssize_t n = ::sendmsg(sock.native_handle(), &msg, 0);
    return n == static_cast<ssize_t>(len);
}

} // namespace dds::rtps
