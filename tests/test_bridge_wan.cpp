// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// fusa:test REQ-BRIDGE-WAN-001 REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-003
// fusa:test REQ-BRIDGE-WAN-004 REQ-BRIDGE-WAN-005 REQ-BRIDGE-WAN-006
// fusa:test REQ-BRIDGE-WAN-007

#include <dds/bridge/wan/wan.hpp>
#include <dds/mock/participant.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace bwan = dds::bridge::wan;

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// make_participant returns an isolated-broker mock participant (REQ-MOCK-006)
// so publishing on one participant never echoes directly to a subscriber on
// another — mirrors go-DDS's wan_test.go newPart() helper, which uses
// mock.IsolatedBroker() for exactly the same reason: "prevents the global
// mock broker from echoing samples published by the server back to the WAN
// client's subscribers, which would create a feedback loop and a data race."
std::shared_ptr<dds::mock::IMockParticipant> make_participant() {
    auto [p, ec] = dds::mock::create(0, /*isolated_broker=*/true);
    REQUIRE_FALSE(ec);
    REQUIRE(p);
    return p;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 2s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// failAfterNSubs wraps a Participant and makes new_subscriber fail after N
// successful calls — C++ port of go-DDS's wan_test.go failAfterNSubs, used
// to exercise Connect's multi-topic subscription cleanup.
class FailAfterNSubs : public dds::IParticipant {
public:
    FailAfterNSubs(std::shared_ptr<dds::IParticipant> inner, int limit)
        : inner_(std::move(inner)), limit_(limit) {}

    std::pair<std::shared_ptr<dds::IPublisher>, std::error_code>
    new_publisher(const std::string& topic, dds::QoS qos) override {
        return inner_->new_publisher(topic, qos);
    }

    std::pair<std::shared_ptr<dds::ISubscriber>, std::error_code>
    new_subscriber(const std::string& topic, dds::QoS qos,
                   std::vector<relay::SubscriberOption> opts) override {
        int n = ++count_;
        if (n > limit_) return {nullptr, relay::ErrNotConnected()};
        return inner_->new_subscriber(topic, qos, std::move(opts));
    }

    dds::Domain domain() const noexcept override { return inner_->domain(); }
    std::error_code close() override { return inner_->close(); }

private:
    std::shared_ptr<dds::IParticipant> inner_;
    int                                  limit_;
    std::atomic<int>                    count_{0};
};

// ── Raw-socket helpers for the malformed-frame tests (mirrors go-DDS's
// wan_test.go net.Dial-based tests, which poke the wire protocol directly). ──

#if defined(_WIN32)
struct WinsockInitGuard {
    WinsockInitGuard() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
void ensure_ws_init() {
    static WinsockInitGuard g;
    (void)g;
}
using RawSocket = SOCKET;
#else
void ensure_ws_init() {}
using RawSocket = int;
#endif

RawSocket raw_dial(const std::string& host, uint16_t port) {
    ensure_ws_init();
    RawSocket s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        ::closesocket(s);
        return INVALID_SOCKET;
#else
        ::close(s);
        return -1;
#endif
    }
    return s;
}

void raw_close(RawSocket s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void raw_send_be32(RawSocket s, uint32_t v) {
    uint8_t hdr[4] = {
        static_cast<uint8_t>((v >> 24) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 8) & 0xFF),
        static_cast<uint8_t>(v & 0xFF),
    };
#if defined(_WIN32)
    ::send(s, reinterpret_cast<const char*>(hdr), sizeof(hdr), 0);
#else
    ::send(s, hdr, sizeof(hdr), 0);
#endif
}

void raw_send(RawSocket s, const std::string& body) {
#if defined(_WIN32)
    ::send(s, body.data(), static_cast<int>(body.size()), 0);
#else
    ::send(s, body.data(), body.size(), 0);
#endif
}

std::pair<std::shared_ptr<bwan::Bridge>, uint16_t> must_serve(std::shared_ptr<dds::IParticipant> p,
                                                               bwan::Options opts = {}) {
    auto [srv, ec] = bwan::Bridge::serve(p, "127.0.0.1:0", std::move(opts));
    REQUIRE_FALSE(ec);
    REQUIRE(srv);
    auto addr = srv->addr();
    auto pos  = addr.rfind(':');
    REQUIRE(pos != std::string::npos);
    return {srv, static_cast<uint16_t>(std::stoi(addr.substr(pos + 1)))};
}

} // namespace

// ── JSON reference vectors (fusa:test REQ-BRIDGE-WAN-001) ────────────────────
//
// Captured verbatim from a real go-DDS process (encoding/json.Marshal on
// github.com/SoundMatt/go-DDS/bridge/wan's actual wireFrame type) — not
// hand-typed — via a throwaway `go test` program (a temporary in-package
// _test.go calling json.Marshal directly on wireFrame, since the type is
// unexported) against a fresh go-DDS clone (github.com/SoundMatt/go-DDS,
// v0.63.0), per this repo's established "never hardcode without deriving
// from a fresh go-DDS clone" convention. See wan.hpp's file-level scope
// note for the one documented encoder gap (nil vs. empty []byte).

TEST_CASE("to_json: matches go-DDS reference vectors", "[bridge][wan][json][REQ-BRIDGE-WAN-001]") {
    CHECK(bwan::to_json(bwan::WireFrame{"sensors/temperature", {'h', 'e', 'l', 'l', 'o'}}) ==
          R"({"t":"sensors/temperature","p":"aGVsbG8="})");

    bwan::WireFrame empty_payload;
    empty_payload.topic = "t";
    CHECK(bwan::to_json(empty_payload) == R"({"t":"t","p":""})");

    CHECK(bwan::to_json(bwan::WireFrame{"bin", {0x00, 0x01, 0xFF, 0x7F, 0x80}}) ==
          R"({"t":"bin","p":"AAH/f4A="})");

    CHECK(bwan::to_json(bwan::WireFrame{"wan/fwd", {'v', 'i', 'a', '-', 'w', 'a', 'n'}}) ==
          R"({"t":"wan/fwd","p":"dmlhLXdhbg=="})");
}

TEST_CASE("to_json: HTML-unsafe / non-ASCII characters match go's escapeHTML default",
          "[bridge][wan][json][REQ-BRIDGE-WAN-001]") {
    bwan::WireFrame f{"t/<script>&\"quote\"/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", {'x'}};
    CHECK(bwan::to_json(f) ==
          "{\"t\":\"t/\\u003cscript\\u003e\\u0026\\\"quote\\\"/"
          "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\",\"p\":\"eA==\"}");
}

TEST_CASE("from_json: round-trips to_json output", "[bridge][wan][json][REQ-BRIDGE-WAN-001]") {
    bwan::WireFrame f{"roundtrip", {1, 2, 3, 250, 251}};
    bwan::WireFrame decoded;
    REQUIRE(bwan::from_json(bwan::to_json(f), decoded));
    CHECK(decoded.topic == f.topic);
    CHECK(decoded.payload == f.payload);
}

TEST_CASE("from_json: accepts a literal `null` for the payload field, matching Go's nil []byte encoding",
          "[bridge][wan][json][REQ-BRIDGE-WAN-001]") {
    bwan::WireFrame f;
    REQUIRE(bwan::from_json(R"({"t":"t","p":null})", f));
    CHECK(f.topic == "t");
    CHECK(f.payload.empty());
}

TEST_CASE("from_json: rejects malformed JSON", "[bridge][wan][json][REQ-BRIDGE-WAN-001]") {
    bwan::WireFrame f;
    CHECK_FALSE(bwan::from_json("not json", f));
    CHECK_FALSE(bwan::from_json(R"({"t":)", f));
    CHECK_FALSE(bwan::from_json(R"({"t":"x","p":"not-valid-base64!!"})", f));
}

// ── Frame codec unit tests (fusa:test REQ-BRIDGE-WAN-002 REQ-BRIDGE-WAN-004) ──
//
// C++ port of go-DDS's wan_internal_test.go, using in-memory WriteFn/ReadFn
// doubles instead of bytes.Buffer / a write-failing io.Writer.

namespace {

bwan::WriteFn buffer_writer(std::string& buf) {
    return [&buf](const uint8_t* data, std::size_t len) -> std::error_code {
        buf.append(reinterpret_cast<const char*>(data), len);
        return {};
    };
}

bwan::ReadFn buffer_reader(const std::string& data, std::size_t& pos) {
    return [&data, &pos](uint8_t* out, std::size_t len) -> std::error_code {
        if (pos + len > data.size()) return relay::ErrNotConnected();
        std::memcpy(out, data.data() + pos, len);
        pos += len;
        return {};
    };
}

// FailFirstWrite fails on the first call, mimicking a broken header send.
bwan::WriteFn fail_first_write() {
    auto calls = std::make_shared<int>(0);
    return [calls](const uint8_t*, std::size_t) -> std::error_code {
        ++*calls;
        if (*calls == 1) return relay::ErrNotConnected();
        return {};
    };
}

} // namespace

TEST_CASE("write_frame/read_frame: round-trip via an in-memory buffer",
          "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    std::string buf;
    REQUIRE_FALSE(bwan::write_frame(buffer_writer(buf), bwan::WireFrame{"t", {'h', 'i'}}));

    std::size_t pos = 0;
    bwan::WireFrame out;
    REQUIRE_FALSE(bwan::read_frame(buffer_reader(buf, pos), out));
    CHECK(out.topic == "t");
    CHECK(out.payload == std::vector<uint8_t>{'h', 'i'});
}

TEST_CASE("write_frame: header write error propagates", "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    auto ec = bwan::write_frame(fail_first_write(), bwan::WireFrame{"x", {'y'}});
    CHECK(ec);
}

TEST_CASE("read_frame: oversized declared length is ErrFrameTooLarge",
          "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    std::string buf;
    buf.push_back(char(0x02)); // 0x02000000 bytes — well above the 16 MiB cap
    buf.push_back(char(0x00));
    buf.push_back(char(0x00));
    buf.push_back(char(0x00));
    std::size_t     pos = 0;
    bwan::WireFrame out;
    auto            ec = bwan::read_frame(buffer_reader(buf, pos), out);
    CHECK(ec == bwan::ErrFrameTooLarge());
}

TEST_CASE("read_frame: short header is an error", "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    std::string     buf{char(0x00)};
    std::size_t     pos = 0;
    bwan::WireFrame out;
    CHECK(bwan::read_frame(buffer_reader(buf, pos), out));
}

TEST_CASE("read_frame: short body is an error", "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    std::string buf;
    buf.push_back(char(0));
    buf.push_back(char(0));
    buf.push_back(char(0));
    buf.push_back(char(8)); // claims 8 bytes, supplies none
    std::size_t     pos = 0;
    bwan::WireFrame out;
    CHECK(bwan::read_frame(buffer_reader(buf, pos), out));
}

TEST_CASE("read_frame: malformed JSON body is ErrInvalidFrame", "[bridge][wan][frame][REQ-BRIDGE-WAN-002]") {
    std::string body = "{{{{not json";
    std::string buf;
    buf.push_back(static_cast<char>((body.size() >> 24) & 0xFF));
    buf.push_back(static_cast<char>((body.size() >> 16) & 0xFF));
    buf.push_back(static_cast<char>((body.size() >> 8) & 0xFF));
    buf.push_back(static_cast<char>(body.size() & 0xFF));
    buf += body;
    std::size_t     pos = 0;
    bwan::WireFrame out;
    CHECK(bwan::read_frame(buffer_reader(buf, pos), out) == bwan::ErrInvalidFrame());
}

TEST_CASE("write_auth/read_auth: round-trip via an in-memory buffer",
          "[bridge][wan][frame][REQ-BRIDGE-WAN-004]") {
    std::string buf;
    REQUIRE_FALSE(bwan::write_auth(buffer_writer(buf), "s3cret"));

    std::size_t pos = 0;
    std::string tok;
    REQUIRE_FALSE(bwan::read_auth(buffer_reader(buf, pos), tok));
    CHECK(tok == "s3cret");
}

TEST_CASE("write_auth: header write error propagates", "[bridge][wan][frame][REQ-BRIDGE-WAN-004]") {
    CHECK(bwan::write_auth(fail_first_write(), "tok"));
}

TEST_CASE("read_auth: oversized token is ErrFrameTooLarge", "[bridge][wan][frame][REQ-BRIDGE-WAN-004]") {
    std::string buf;
    buf.push_back(char(0x00));
    buf.push_back(char(0x01));
    buf.push_back(char(0x00));
    buf.push_back(char(0x01)); // 0x00010001 = 65537 bytes, above the 4096 cap
    std::size_t pos = 0;
    std::string tok;
    CHECK(bwan::read_auth(buffer_reader(buf, pos), tok) == bwan::ErrFrameTooLarge());
}

TEST_CASE("read_auth: short header is an error", "[bridge][wan][frame][REQ-BRIDGE-WAN-004]") {
    std::string buf{char(0x00)};
    std::size_t pos = 0;
    std::string tok;
    CHECK(bwan::read_auth(buffer_reader(buf, pos), tok));
}

TEST_CASE("read_auth: short body is an error", "[bridge][wan][frame][REQ-BRIDGE-WAN-004]") {
    std::string buf;
    buf.push_back(char(0));
    buf.push_back(char(0));
    buf.push_back(char(0));
    buf.push_back(char(8));
    std::size_t pos = 0;
    std::string tok;
    CHECK(bwan::read_auth(buffer_reader(buf, pos), tok));
}

// ── ErrFrameTooLarge / ErrUnauthorized / ErrInvalidFrame sentinels ────────────

TEST_CASE("error sentinels are distinct and usable as std::error_code",
          "[bridge][wan][REQ-BRIDGE-WAN-002][REQ-BRIDGE-WAN-004]") {
    CHECK(bwan::ErrFrameTooLarge());
    CHECK(bwan::ErrUnauthorized());
    CHECK(bwan::ErrInvalidFrame());
    CHECK(bwan::ErrFrameTooLarge() != bwan::ErrUnauthorized());
    CHECK(bwan::ErrFrameTooLarge() != bwan::ErrInvalidFrame());
}

// ── Basic forwarding (fusa:test REQ-BRIDGE-WAN-005 REQ-BRIDGE-WAN-006) ───────
//
// C++ port of go-DDS's wan_test.go TestWANBridge_ForwardsSample /
// TestWANBridge_MultipleTopics.

TEST_CASE("Bridge: forwards a sample from client-side src to server-side dst",
          "[bridge][wan][REQ-BRIDGE-WAN-005][REQ-BRIDGE-WAN-006]") {
    auto src   = make_participant();
    auto dst   = make_participant();
    const std::string topic = "wan/fwd";

    auto [sub, sec] = dst->new_subscriber(topic, dds::default_qos());
    REQUIRE_FALSE(sec);

    auto [srv, sport] = must_serve(dst);
    (void)sport;

    auto [cli, cec] = bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), ""});
    REQUIRE_FALSE(cec);
    REQUIRE(cli);

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'v', 'i', 'a', '-', 'w', 'a', 'n'}));

    auto item = sub->channel()->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(item.has_value());
    CHECK(std::string(item->payload.begin(), item->payload.end()) == "via-wan");

    pub->close();
    cli->close();
    srv->close();
    sub->close();
}

TEST_CASE("Bridge: forwards samples on multiple topics independently",
          "[bridge][wan][REQ-BRIDGE-WAN-005][REQ-BRIDGE-WAN-006]") {
    auto src = make_participant();
    auto dst = make_participant();
    const std::string topic_a = "wan/multi/a";
    const std::string topic_b = "wan/multi/b";

    auto [sub_a, sa] = dst->new_subscriber(topic_a, dds::default_qos());
    REQUIRE_FALSE(sa);
    auto [sub_b, sb] = dst->new_subscriber(topic_b, dds::default_qos());
    REQUIRE_FALSE(sb);

    auto [srv, sport] = must_serve(dst);
    (void)sport;

    auto [cli, cec] =
        bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic_a, topic_b}, dds::default_qos(), ""});
    REQUIRE_FALSE(cec);

    auto [pub_a, pa] = src->new_publisher(topic_a, dds::default_qos());
    REQUIRE_FALSE(pa);
    auto [pub_b, pb] = src->new_publisher(topic_b, dds::default_qos());
    REQUIRE_FALSE(pb);

    REQUIRE_FALSE(pub_a->write({'m', 's', 'g', '-', 'a'}));
    REQUIRE_FALSE(pub_b->write({'m', 's', 'g', '-', 'b'}));

    auto item_a = sub_a->channel()->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(item_a.has_value());
    CHECK(std::string(item_a->payload.begin(), item_a->payload.end()) == "msg-a");

    auto item_b = sub_b->channel()->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(item_b.has_value());
    CHECK(std::string(item_b->payload.begin(), item_b->payload.end()) == "msg-b");

    pub_a->close();
    pub_b->close();
    cli->close();
    srv->close();
    sub_a->close();
    sub_b->close();
}

// ── addr() ────────────────────────────────────────────────────────────────────

TEST_CASE("Bridge::serve: addr() is non-empty", "[bridge][wan][REQ-BRIDGE-WAN-007]") {
    auto p           = make_participant();
    auto [srv, port] = must_serve(p);
    (void)port;
    CHECK_FALSE(srv->addr().empty());
    srv->close();
}

TEST_CASE("Bridge::connect: addr() is empty", "[bridge][wan][REQ-BRIDGE-WAN-007]") {
    auto src         = make_participant();
    auto [srv, port] = must_serve(make_participant());
    (void)port;

    auto [cli, ec] = bwan::Bridge::connect(src, srv->addr(), bwan::Options{});
    REQUIRE_FALSE(ec);
    CHECK(cli->addr().empty());

    cli->close();
    srv->close();
}

// ── Close idempotency ─────────────────────────────────────────────────────────

TEST_CASE("Bridge::close: idempotent", "[bridge][wan][REQ-BRIDGE-WAN-007]") {
    auto p           = make_participant();
    auto [srv, port] = must_serve(p);
    (void)port;
    srv->close();
    srv->close(); // must not hang or crash
}

// ── Error paths ───────────────────────────────────────────────────────────────

TEST_CASE("Bridge::serve: invalid address is an error", "[bridge][wan][REQ-BRIDGE-WAN-005]") {
    auto p         = make_participant();
    auto [srv, ec] = bwan::Bridge::serve(p, "127.0.0.1:99999", bwan::Options{});
    CHECK(ec);
    CHECK_FALSE(srv);
}

TEST_CASE("Bridge::connect: dial error (no listener)", "[bridge][wan][REQ-BRIDGE-WAN-006]") {
    auto p         = make_participant();
    auto [cli, ec] = bwan::Bridge::connect(p, "127.0.0.1:1", bwan::Options{});
    CHECK(ec);
    CHECK_FALSE(cli);
}

TEST_CASE("Bridge::connect: dial error with topics cleans up subscriptions",
          "[bridge][wan][REQ-BRIDGE-WAN-006]") {
    auto p         = make_participant();
    auto [cli, ec] = bwan::Bridge::connect(p, "127.0.0.1:1", bwan::Options{{"cleanup/topic"}, dds::default_qos(), ""});
    CHECK(ec);
    CHECK_FALSE(cli);
}

TEST_CASE("Bridge::connect: closed participant is an error", "[bridge][wan][REQ-BRIDGE-WAN-006]") {
    auto src         = make_participant();
    auto [srv, port] = must_serve(make_participant());
    (void)port;

    src->close();
    auto [cli, ec] =
        bwan::Bridge::connect(src, srv->addr(), bwan::Options{{"some/topic"}, dds::default_qos(), ""});
    CHECK(ec);
    CHECK_FALSE(cli);
    srv->close();
}

TEST_CASE("Bridge::connect: second-of-two subscriptions failing cleans up the first",
          "[bridge][wan][REQ-BRIDGE-WAN-006]") {
    auto base        = make_participant();
    auto [srv, port] = must_serve(make_participant());
    (void)port;

    auto wrapped = std::make_shared<FailAfterNSubs>(base, 1);
    auto [cli, ec] =
        bwan::Bridge::connect(wrapped, srv->addr(), bwan::Options{{"wan/topic-a", "wan/topic-b"}, dds::default_qos(), ""});
    CHECK(ec);
    CHECK_FALSE(cli);
    srv->close();
}

// ── Large-frame / malformed-frame rejection ───────────────────────────────────

TEST_CASE("Bridge::serve: oversized frame header is rejected and the connection is dropped",
          "[bridge][wan][REQ-BRIDGE-WAN-002][REQ-BRIDGE-WAN-005]") {
    auto p             = make_participant();
    auto srv_port_pair = must_serve(p);
    // Not a structured binding: `srv` is captured by reference in the
    // lambda below, and clang (unlike gcc) correctly rejects capturing a
    // structured-binding name in C++17 (only C++20 permits it).
    std::shared_ptr<bwan::Bridge> srv  = srv_port_pair.first;
    uint16_t                      port = srv_port_pair.second;

    RawSocket conn = raw_dial("127.0.0.1", port);
    REQUIRE(conn >= 0);
    raw_send_be32(conn, 32u * 1024u * 1024u); // 32 MiB — exceeds the 16 MiB cap
    raw_close(conn);

    // srv.close() must complete promptly (no goroutine/thread leak).
    std::atomic<bool> done{false};
    std::thread       th([&] {
        srv->close();
        done.store(true);
    });
    REQUIRE(wait_until([&] { return done.load(); }, 2s));
    th.join();
}

TEST_CASE("Bridge::serve: malformed JSON frame body closes the connection",
          "[bridge][wan][REQ-BRIDGE-WAN-002][REQ-BRIDGE-WAN-005]") {
    auto p           = make_participant();
    auto [srv, port] = must_serve(p);

    RawSocket conn = raw_dial("127.0.0.1", port);
    REQUIRE(conn >= 0);
    std::string body = "this is not valid JSON {{{{";
    raw_send_be32(conn, static_cast<uint32_t>(body.size()));
    raw_send(conn, body);

    // The server closes the connection after the malformed-JSON error; a
    // subsequent read should observe EOF/closed promptly.
    std::this_thread::sleep_for(100ms);
    char buf[1];
#if defined(_WIN32)
    int n = ::recv(conn, buf, 1, 0);
#else
    ssize_t n = ::recv(conn, buf, 1, 0);
#endif
    CHECK(n <= 0);

    raw_close(conn);
    srv->close();
}

// ── Shared-token authentication (fusa:test REQ-BRIDGE-WAN-004) ───────────────
//
// C++ port of go-DDS's tls_test.go TestWANBridge_TokenAuth_Accepts/_Rejects
// (the shared-token auth path only — TLS itself is out of scope for this
// baseline, see wan.hpp's file-level scope note).

TEST_CASE("Bridge: matching token authenticates and forwards", "[bridge][wan][REQ-BRIDGE-WAN-004]") {
    auto src = make_participant();
    auto dst = make_participant();
    const std::string topic = "wan/auth/ok";

    auto [sub, sec] = dst->new_subscriber(topic, dds::default_qos());
    REQUIRE_FALSE(sec);

    auto [srv, sec2] = bwan::Bridge::serve(dst, "127.0.0.1:0", bwan::Options{{}, dds::default_qos(), "s3cret"});
    REQUIRE_FALSE(sec2);

    auto [cli, cec] =
        bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), "s3cret"});
    REQUIRE_FALSE(cec);

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'a', 'u', 't', 'h', 'e', 'd'}));

    auto item = sub->channel()->recv_until(std::chrono::steady_clock::now() + 2s);
    REQUIRE(item.has_value());
    CHECK(std::string(item->payload.begin(), item->payload.end()) == "authed");

    pub->close();
    cli->close();
    srv->close();
    sub->close();
}

TEST_CASE("Bridge: mismatched token is silently dropped", "[bridge][wan][REQ-BRIDGE-WAN-004]") {
    auto src = make_participant();
    auto dst = make_participant();
    const std::string topic = "wan/auth/bad";

    auto [sub, sec] = dst->new_subscriber(topic, dds::default_qos());
    REQUIRE_FALSE(sec);

    auto [srv, sec2] =
        bwan::Bridge::serve(dst, "127.0.0.1:0", bwan::Options{{}, dds::default_qos(), "correct-token"});
    REQUIRE_FALSE(sec2);

    auto [cli, cec] =
        bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), "wrong-token"});
    REQUIRE_FALSE(cec); // Connect succeeds locally regardless of the token's validity

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'n', 'o'}));

    auto item = sub->channel()->recv_until(std::chrono::steady_clock::now() + 800ms);
    CHECK_FALSE(item.has_value()); // rejected: never forwarded

    pub->close();
    cli->close();
    srv->close();
    sub->close();
}

// ── Server / write-failure edge cases ─────────────────────────────────────────

TEST_CASE("Bridge::serve: publisher write failure (QoS.max_sample_size) closes the connection",
          "[bridge][wan][REQ-BRIDGE-WAN-005]") {
    auto dst = make_participant();
    auto src = make_participant();
    const std::string topic = "wan/write-err";

    dds::QoS tiny_qos            = dds::default_qos();
    tiny_qos.max_sample_size = 1;
    auto [srv, sec] = bwan::Bridge::serve(dst, "127.0.0.1:0", bwan::Options{{}, tiny_qos, ""});
    REQUIRE_FALSE(sec);

    auto [cli, cec] = bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), ""});
    REQUIRE_FALSE(cec);

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'p', 'a', 'y', 'l', 'o', 'a', 'd', '-', 't', 'o', 'o', '-', 'b', 'i', 'g'}));

    std::this_thread::sleep_for(100ms); // let the frame reach the server and trigger the write error

    pub->close();
    cli->close();
    srv->close(); // must not hang
}

TEST_CASE("Bridge::serve: closed participant makes new_publisher fail, closing the connection",
          "[bridge][wan][REQ-BRIDGE-WAN-005]") {
    auto dst = make_participant();
    auto src = make_participant();
    const std::string topic = "wan/svc-closed";

    auto [srv, sec] = bwan::Bridge::serve(dst, "127.0.0.1:0", bwan::Options{});
    REQUIRE_FALSE(sec);
    dst->close();

    auto [cli, cec] = bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), ""});
    REQUIRE_FALSE(cec);

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'p', 'i', 'n', 'g'}));

    std::this_thread::sleep_for(100ms);

    pub->close();
    cli->close();
    srv->close(); // must not hang
}

TEST_CASE("Bridge: client write failure after server closes stops the sender without hanging",
          "[bridge][wan][REQ-BRIDGE-WAN-006]") {
    auto src = make_participant();
    auto dst = make_participant();
    const std::string topic = "wan/send-write-err";

    auto [srv, sport] = must_serve(dst);
    (void)sport;

    auto cli_ec_pair = bwan::Bridge::connect(src, srv->addr(), bwan::Options{{topic}, dds::default_qos(), ""});
    REQUIRE_FALSE(cli_ec_pair.second);
    // Not a structured binding: `cli` is captured by reference in the
    // lambda below (see the sibling fix above for why clang requires this).
    std::shared_ptr<bwan::Bridge> cli = cli_ec_pair.first;

    auto [pub, pec] = src->new_publisher(topic, dds::default_qos());
    REQUIRE_FALSE(pec);
    REQUIRE_FALSE(pub->write({'p', 'i', 'n', 'g'}));
    std::this_thread::sleep_for(30ms);

    srv->close(); // breaks the TCP connection on the server side
    std::this_thread::sleep_for(30ms);

    REQUIRE_FALSE(pub->write({'p', 'i', 'n', 'g', '2'})); // local write to src still succeeds

    // cli->close() must complete promptly even though its sender thread hit
    // a write error on the now-broken connection.
    std::atomic<bool> done{false};
    std::thread       th([&] {
        cli->close();
        done.store(true);
    });
    REQUIRE(wait_until([&] { return done.load(); }, 2s));
    th.join();

    pub->close();
}
