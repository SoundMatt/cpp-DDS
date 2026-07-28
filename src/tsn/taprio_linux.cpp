// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Linux TAPRIO qdisc netlink implementation. C++ port of
// github.com/SoundMatt/go-DDS's tsn/taprio_linux.go — programs (apply())
// and verifies (verify_applied()) the kernel `taprio` qdisc via
// RTM_NEWQDISC/RTM_GETQDISC over NETLINK_ROUTE, using hand-built raw
// netlink message buffers exactly as go-DDS does (rather than pulling in
// libnl or any other netlink helper library), matching this repo's
// traffic_linux.cpp precedent (mirrored TCA/rtnetlink numeric constants
// as literal values rather than kernel-header struct layouts, since those
// headers' exact availability isn't guaranteed across every glibc/musl
// build environment). This file is only added to the build when
// CMAKE_SYSTEM_NAME is "Linux" — see CMakeLists.txt.

#include <dds/tsn/taprio.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

// <linux/netlink.h> supplies `struct sockaddr_nl` and `NETLINK_ROUTE`;
// present on any Linux dev system (part of linux-libc-dev, a transitive
// dependency of libc6-dev). AF_NETLINK/SOCK_RAW/SOCK_CLOEXEC come from
// <sys/socket.h>.
#include <linux/netlink.h>

namespace dds::tsn {

// fusa:req REQ-TSN-007

namespace {

// ── rtnetlink / TCA numeric constants (mirrored as literals, matching
// go-DDS's taprio_linux.go and this repo's own traffic_linux.cpp
// convention — see the file-level comment above). ──────────────────────

constexpr uint16_t kRtmNewQdisc = 36;
constexpr uint16_t kRtmGetQdisc = 38;
constexpr uint16_t kNlmsgError  = 2;
constexpr uint16_t kNlmsgDone   = 3;

constexpr uint16_t kNlmFRequest = 0x01;
constexpr uint16_t kNlmFAck     = 0x04;
constexpr uint16_t kNlmFRoot    = 0x100;
constexpr uint16_t kNlmFMatch   = 0x200;
constexpr uint16_t kNlmFCreate  = 0x400;
constexpr uint16_t kNlmFReplace = 0x100;
constexpr uint16_t kNlmFDump    = kNlmFRoot | kNlmFMatch;

constexpr uint32_t kTcHRoot = 0xFFFFFFFFu;

constexpr uint16_t kTcaKind    = 1;
constexpr uint16_t kTcaOptions = 2;

constexpr uint16_t kTcaTaprioAttrSchedEntryList = 2;
constexpr uint16_t kTcaTaprioAttrSchedBaseTime  = 3;
constexpr uint16_t kTcaTaprioAttrSchedClockid   = 6;
constexpr uint16_t kTcaTaprioAttrSchedCycleTime = 9;
constexpr uint16_t kTcaTaprioAttrFlags          = 11;

constexpr uint16_t kTcaTaprioSchedSingleEntry = 2;
constexpr uint16_t kTcaTaprioSchedEntryCmd    = 3;
constexpr uint16_t kTcaTaprioSchedEntryGateMask = 4;
constexpr uint16_t kTcaTaprioSchedEntryInterval = 5;

constexpr uint32_t kNlaFNested = 0x8000;

constexpr int32_t kClockTai = 11;

// ── Netlink attribute builder — mirrors go-DDS's nlBuf. ─────────────────

class NlBuf {
public:
    void add_raw(const uint8_t* data, std::size_t len) { b_.insert(b_.end(), data, data + len); }

    void add_attr(uint16_t attr_type, const uint8_t* val, std::size_t len) {
        const uint16_t length = static_cast<uint16_t>(4 + len);
        push_u16(length);
        push_u16(attr_type);
        b_.insert(b_.end(), val, val + len);
        const std::size_t pad = (4 - (len % 4)) % 4;
        for (std::size_t i = 0; i < pad; ++i) b_.push_back(0);
    }
    void add_attr_u8(uint16_t attr_type, uint8_t v) { add_attr(attr_type, &v, 1); }
    void add_attr_u32(uint16_t attr_type, uint32_t v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4); // host order — matches go-DDS's binary.LittleEndian on little-endian hosts; see file note below.
        add_attr(attr_type, b, 4);
    }
    void add_attr_s32(uint16_t attr_type, int32_t v) { add_attr_u32(attr_type, static_cast<uint32_t>(v)); }
    void add_attr_s64(uint16_t attr_type, int64_t v) {
        uint8_t b[8];
        std::memcpy(b, &v, 8);
        add_attr(attr_type, b, 8);
    }
    void add_nested_attr(uint16_t attr_type, const std::vector<uint8_t>& nested) {
        add_attr(static_cast<uint16_t>(attr_type | kNlaFNested), nested.data(), nested.size());
    }

    const std::vector<uint8_t>& bytes() const noexcept { return b_; }

private:
    void push_u16(uint16_t v) {
        uint8_t b[2];
        std::memcpy(b, &v, 2);
        b_.insert(b_.end(), b, b + 2);
    }
    std::vector<uint8_t> b_;
};
// Note: netlink attribute payloads are host-byte-order on the local host
// (rtnetlink is a local-kernel-only protocol, never sent over the wire),
// so std::memcpy of a native integer is correct on every architecture
// Linux runs cppdds_lib on — matching go-DDS's own use of
// binary.LittleEndian, which is only actually exercised on little-endian
// (amd64/arm64) CI/production hosts.

uint32_t taprio_flags(bool offload) { return offload ? 0x2u /* TC_TAPRIO_ATTR_FLAG_FULL_OFFLOAD */ : 0u; }

std::vector<uint8_t> build_taprio_msg(const TAPRIOConfig& c, unsigned ifindex) {
    NlBuf nb;

    // tcmsg: family=AF_UNSPEC, ifindex, handle=0x00010000, parent=TC_H_ROOT
    uint8_t tcmsg[20] = {};
    tcmsg[0] = AF_UNSPEC;
    uint32_t ifindex_u32 = ifindex;
    std::memcpy(tcmsg + 4, &ifindex_u32, 4);
    uint32_t handle = 0x00010000u;
    std::memcpy(tcmsg + 8, &handle, 4);
    uint32_t parent = kTcHRoot;
    std::memcpy(tcmsg + 12, &parent, 4);
    nb.add_raw(tcmsg, sizeof(tcmsg));

    // TCA_KIND = "taprio\0"
    static const uint8_t kKind[] = {'t', 'a', 'p', 'r', 'i', 'o', 0};
    nb.add_attr(kTcaKind, kKind, sizeof(kKind));

    // TCA_OPTIONS (nested TAPRIO attributes)
    NlBuf opts;
    opts.add_attr_u32(kTcaTaprioAttrFlags, taprio_flags(c.offload));
    opts.add_attr_s32(kTcaTaprioAttrSchedClockid, kClockTai);

    const int64_t base_time = c.base_time; // 0 -> kernel schedules starting from now + one cycle
    opts.add_attr_s64(kTcaTaprioAttrSchedBaseTime, base_time);

    const int64_t cycle_ns = c.cycle_duration().count();
    if (cycle_ns > 0) opts.add_attr_s64(kTcaTaprioAttrSchedCycleTime, cycle_ns);

    NlBuf entry_list;
    for (const auto& e : c.entries) {
        NlBuf entry;
        entry.add_attr_u8(kTcaTaprioSchedEntryCmd, 0x00 /* GATE_OP */);
        entry.add_attr_u32(kTcaTaprioSchedEntryGateMask, e.gate_mask);
        entry.add_attr_u32(kTcaTaprioSchedEntryInterval, static_cast<uint32_t>(e.interval.count()));
        entry_list.add_nested_attr(kTcaTaprioSchedSingleEntry, entry.bytes());
    }
    opts.add_nested_attr(kTcaTaprioAttrSchedEntryList, entry_list.bytes());

    nb.add_nested_attr(kTcaOptions, opts.bytes());

    const auto& payload = nb.bytes();
    std::vector<uint8_t> msg(16 + payload.size());
    uint32_t total = static_cast<uint32_t>(msg.size());
    uint16_t type = kRtmNewQdisc;
    uint16_t flags = kNlmFRequest | kNlmFAck | kNlmFCreate | kNlmFReplace;
    uint32_t seq = 1;
    uint32_t pid = 0;
    std::memcpy(msg.data() + 0, &total, 4);
    std::memcpy(msg.data() + 4, &type, 2);
    std::memcpy(msg.data() + 6, &flags, 2);
    std::memcpy(msg.data() + 8, &seq, 4);
    std::memcpy(msg.data() + 12, &pid, 4);
    std::memcpy(msg.data() + 16, payload.data(), payload.size());
    return msg;
}

std::vector<uint8_t> build_get_qdisc_msg(unsigned ifindex) {
    uint8_t tcmsg[20] = {};
    tcmsg[0] = AF_UNSPEC;
    uint32_t ifindex_u32 = ifindex;
    std::memcpy(tcmsg + 4, &ifindex_u32, 4);

    std::vector<uint8_t> msg(16 + sizeof(tcmsg));
    uint32_t total = static_cast<uint32_t>(msg.size());
    uint16_t type = kRtmGetQdisc;
    uint16_t flags = kNlmFRequest | kNlmFDump;
    uint32_t seq = 2;
    uint32_t pid = 0;
    std::memcpy(msg.data() + 0, &total, 4);
    std::memcpy(msg.data() + 4, &type, 2);
    std::memcpy(msg.data() + 6, &flags, 2);
    std::memcpy(msg.data() + 8, &seq, 4);
    std::memcpy(msg.data() + 12, &pid, 4);
    std::memcpy(msg.data() + 16, tcmsg, sizeof(tcmsg));
    return msg;
}

// extract_tca_kind walks a netlink attribute list and returns the value
// of TCA_KIND (type 1), or an empty string if absent.
std::string extract_tca_kind(const uint8_t* attrs, std::size_t len) {
    std::size_t off = 0;
    while (off + 4 <= len) {
        uint16_t attr_len;
        uint16_t attr_type;
        std::memcpy(&attr_len, attrs + off, 2);
        std::memcpy(&attr_type, attrs + off + 2, 2);
        attr_type &= 0x7FFFu; // mask NLA_F_NESTED
        if (attr_len < 4 || static_cast<std::size_t>(attr_len) > len - off) break;
        if (attr_type == kTcaKind) {
            std::size_t val_len = attr_len - 4;
            // Trim a trailing NUL, if present.
            while (val_len > 0 && attrs[off + 4 + val_len - 1] == '\0') --val_len;
            return std::string(reinterpret_cast<const char*>(attrs + off + 4), val_len);
        }
        const std::size_t advance = (static_cast<std::size_t>(attr_len) + 3) & ~std::size_t{3};
        if (advance == 0 || off + advance > len) break;
        off += advance;
    }
    return {};
}

std::optional<std::string> read_qdisc_kind(int fd) {
    std::vector<uint8_t> buf(32768);
    for (;;) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) return std::string("tsn: verify_applied: read: ") + std::strerror(errno);
        std::size_t off = 0;
        const std::size_t total = static_cast<std::size_t>(n);
        while (total - off >= 16) {
            uint32_t msg_len;
            uint16_t msg_type;
            std::memcpy(&msg_len, buf.data() + off, 4);
            std::memcpy(&msg_type, buf.data() + off + 4, 2);
            if (msg_type == kNlmsgDone) {
                return std::string("tsn: verify_applied: no taprio qdisc found on interface");
            }
            if (msg_type == kNlmsgError) {
                if (total - off >= 20) {
                    int32_t code;
                    std::memcpy(&code, buf.data() + off + 16, 4);
                    if (code != 0) {
                        return std::string("tsn: verify_applied: kernel error: ") + std::strerror(-code);
                    }
                }
                return std::string("tsn: verify_applied: no taprio qdisc found on interface");
            }
            if (msg_type == kRtmNewQdisc && msg_len >= 16 + 20) {
                const std::size_t attrs_off = off + 16 + 20;
                const std::size_t attrs_len = msg_len - (16 + 20);
                if (attrs_off + attrs_len <= total && attrs_off + attrs_len <= buf.size()) {
                    std::string kind = extract_tca_kind(buf.data() + attrs_off, attrs_len);
                    if (kind == "taprio") return std::nullopt;
                }
            }
            const std::size_t advance = (static_cast<std::size_t>(msg_len) + 3) & ~std::size_t{3};
            if (advance < 16 || off + advance > total) break;
            off += advance;
        }
    }
}

std::optional<std::string> recv_ack(int fd) {
    uint8_t buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) return std::string("tsn: apply: recv ack: ") + std::strerror(errno);
    if (n < 20) return std::string("tsn: apply: ack too short (" + std::to_string(n) + " bytes)");
    uint16_t msg_type;
    std::memcpy(&msg_type, buf + 4, 2);
    if (msg_type != kNlmsgError) {
        return std::string("tsn: apply: unexpected ack type " + std::to_string(msg_type));
    }
    int32_t code;
    std::memcpy(&code, buf + 16, 4);
    if (code != 0) {
        return std::string("tsn: apply: kernel error: ") + std::strerror(-code);
    }
    return std::nullopt;
}

// open_bound_netlink_socket opens and binds an AF_NETLINK/NETLINK_ROUTE
// socket. Returns -1 (with *err set) on failure.
int open_bound_netlink_socket(std::optional<std::string>& err) {
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        err = std::string("tsn: netlink socket: ") + std::strerror(errno);
        return -1;
    }
    struct sockaddr_nl sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
        err = std::string("tsn: netlink bind: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

std::optional<std::string> TAPRIOConfig::apply() const {
    if (auto verr = validate()) return verr;

    const unsigned ifindex = ::if_nametoindex(interface.c_str());
    if (ifindex == 0) {
        return "tsn: taprio: interface \"" + interface + "\": not found";
    }

    const auto msg = build_taprio_msg(*this, ifindex);

    std::optional<std::string> open_err;
    const int fd = open_bound_netlink_socket(open_err);
    if (fd < 0) return open_err;

    if (::send(fd, msg.data(), msg.size(), 0) < 0) {
        std::string err = std::string("tsn: taprio: send: ") + std::strerror(errno);
        ::close(fd);
        return err;
    }

    auto ack_err = recv_ack(fd);
    ::close(fd);
    return ack_err;
}

std::optional<std::string> TAPRIOConfig::verify_applied() const {
    if (interface.empty()) return std::string("tsn: verify_applied: Interface must not be empty");

    const unsigned ifindex = ::if_nametoindex(interface.c_str());
    if (ifindex == 0) {
        return "tsn: verify_applied: interface \"" + interface + "\": not found";
    }

    std::optional<std::string> open_err;
    const int fd = open_bound_netlink_socket(open_err);
    if (fd < 0) return open_err;

    const auto req = build_get_qdisc_msg(ifindex);
    if (::send(fd, req.data(), req.size(), 0) < 0) {
        std::string err = std::string("tsn: verify_applied: send: ") + std::strerror(errno);
        ::close(fd);
        return err;
    }

    auto result = read_qdisc_kind(fd);
    ::close(fd);
    return result;
}

} // namespace dds::tsn
