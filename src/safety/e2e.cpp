// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/safety/e2e.hpp>

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

// fusa:req REQ-E2E-001 REQ-E2E-002 REQ-E2E-003 REQ-E2E-004 REQ-E2E-005 REQ-E2E-006
// fusa:req REQ-SAFETY-001 REQ-SAFETY-002 REQ-SAFETY-003

namespace dds::safety {

namespace {

// ── little-endian helpers ────────────────────────────────────────────────────

void put_u16_le(std::vector<uint8_t>& buf, std::size_t off, uint16_t v) {
    buf[off]     = static_cast<uint8_t>(v & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void put_u32_le(std::vector<uint8_t>& buf, std::size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        buf[off + static_cast<std::size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

void put_u64_le(std::vector<uint8_t>& buf, std::size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf[off + static_cast<std::size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

uint16_t get_u16_le(const std::vector<uint8_t>& buf, std::size_t off) {
    return static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
}

uint32_t get_u32_le(const std::vector<uint8_t>& buf, std::size_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(buf[off + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

int64_t get_i64_le(const std::vector<uint8_t>& buf, std::size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(buf[off + static_cast<std::size_t>(i)]) << (8 * i);
    return static_cast<int64_t>(v);
}

// now_unix_nanos returns nanoseconds since the Unix epoch for the current
// instant (go-DDS: time.Now().UnixNano()).
int64_t now_unix_nanos() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

// ── crc16 — CRC-16/CCITT-FALSE (polynomial 0x1021, init 0xFFFF, no
// reflection). Byte-for-byte identical to go-DDS's safety.crc16. ────────────
// fusa:req REQ-E2E-001 REQ-E2E-003
uint16_t crc16(const std::vector<uint8_t>& data) {
    constexpr uint16_t poly = 0x1021;
    uint16_t           crc  = 0xFFFF;
    for (uint8_t b : data) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(b) << 8));
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000)
                crc = static_cast<uint16_t>((crc << 1) ^ poly);
            else
                crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// ParsedFrame is the result of parse_frame: the header fields plus the
// stripped payload, or crc_ok == false if the CRC check failed.
struct ParsedFrame {
    uint32_t              counter{0};
    int64_t               timestamp_ns{0};
    std::vector<uint8_t>  payload;
    bool                  crc_ok{false};
    uint16_t              computed_crc{0};
    uint16_t              stored_crc{0};
};

// parse_frame decodes and validates a frame (data.size() >= kHeaderSize is a
// precondition of the caller). Mirrors go-DDS's safety.parseFrame.
// fusa:req REQ-E2E-001 REQ-E2E-003
ParsedFrame parse_frame(const std::vector<uint8_t>& data) {
    ParsedFrame f;
    f.counter      = get_u32_le(data, 4);
    f.timestamp_ns = get_i64_le(data, 8);
    f.stored_crc   = get_u16_le(data, 16);
    f.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), data.end());

    std::vector<uint8_t> crc_input(16 + f.payload.size());
    std::copy(data.begin(), data.begin() + 16, crc_input.begin());
    std::copy(f.payload.begin(), f.payload.end(), crc_input.begin() + 16);
    f.computed_crc = crc16(crc_input);
    f.crc_ok       = (f.computed_crc == f.stored_crc);
    if (!f.crc_ok) f.payload.clear();
    return f;
}

} // namespace

// ── E2EPublisher ─────────────────────────────────────────────────────────────

E2EPublisher::E2EPublisher(std::shared_ptr<dds::IPublisher> pub, E2EConfig cfg)
    : pub_(std::move(pub)), cfg_(std::move(cfg)) {}

std::vector<uint8_t> E2EPublisher::make_frame(uint32_t counter,
                                               const std::vector<uint8_t>& payload) const {
    std::vector<uint8_t> buf(kHeaderSize + payload.size(), 0);
    put_u16_le(buf, 0, cfg_.data_id);
    put_u16_le(buf, 2, cfg_.source_id);
    put_u32_le(buf, 4, counter);
    put_u64_le(buf, 8, static_cast<uint64_t>(now_unix_nanos()));
    std::copy(payload.begin(), payload.end(), buf.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));

    // CRC covers header fields [0:16] (CRC slot [16:18] is zero) plus payload.
    std::vector<uint8_t> crc_input(16 + payload.size());
    std::copy(buf.begin(), buf.begin() + 16, crc_input.begin());
    std::copy(payload.begin(), payload.end(), crc_input.begin() + 16);
    put_u16_le(buf, 16, crc16(crc_input));
    return buf;
}

std::error_code E2EPublisher::write(const std::vector<uint8_t>& payload) {
    return write(relay::Context::background(), payload);
}

std::error_code E2EPublisher::write(relay::Context ctx, const std::vector<uint8_t>& payload) {
    uint32_t counter;
    {
        std::lock_guard<std::mutex> lk(mu_);
        counter = ++counter_;
    }
    return pub_->write(ctx, make_frame(counter, payload));
}

std::error_code E2EPublisher::close() { return pub_->close(); }

std::shared_ptr<E2EPublisher> new_e2e_publisher(std::shared_ptr<dds::IPublisher> pub, E2EConfig cfg) {
    return std::make_shared<E2EPublisher>(std::move(pub), std::move(cfg));
}

// ── E2ESubscriber ────────────────────────────────────────────────────────────

E2ESubscriber::E2ESubscriber(std::shared_ptr<dds::ISubscriber> sub, E2EConfig cfg)
    : sub_(std::move(sub))
    , cfg_(std::move(cfg))
    , ch_(std::make_shared<dds::Chan<dds::Sample>>(64))
    , err_ch_(std::make_shared<dds::Chan<E2EError>>(32)) {
    pump_thread_ = std::thread([this] { pump(); });
}

E2ESubscriber::~E2ESubscriber() { close(); }

void E2ESubscriber::pump() {
    auto raw_ch = sub_->channel();
    while (auto raw = raw_ch->recv()) {
        process(*raw);
    }
    ch_->close();
}

void E2ESubscriber::process(const dds::Sample& raw) {
    if (raw.payload.size() < kHeaderSize) {
        std::ostringstream oss;
        oss << "safety: payload " << raw.payload.size() << " bytes is shorter than header "
            << kHeaderSize << " bytes";
        emit_error(E2EError{E2EErrorKind::HeaderTooShort, 0, oss.str()});
        return;
    }

    ParsedFrame f = parse_frame(raw.payload);
    if (!f.crc_ok) {
        std::ostringstream oss;
        oss << "safety: CRC mismatch — got 0x" << std::hex << std::setfill('0') << std::setw(4)
            << f.computed_crc << ", want 0x" << std::setw(4) << f.stored_crc;
        emit_error(E2EError{E2EErrorKind::CRCMismatch, f.counter, oss.str()});
        return;
    }

    if (cfg_.max_age.count() > 0) {
        using namespace std::chrono;
        auto ts  = system_clock::time_point{} + nanoseconds{f.timestamp_ns};
        auto age = system_clock::now() - ts;
        if (age > cfg_.max_age) {
            std::ostringstream oss;
            oss << "safety: sample age " << duration_cast<microseconds>(age).count()
                << "us exceeds max_age " << duration_cast<microseconds>(cfg_.max_age).count() << "us";
            emit_error(E2EError{E2EErrorKind::StaleSample, f.counter, oss.str()});
            return;
        }
    }

    if (has_first_ && f.counter != last_counter_ + 1) {
        std::ostringstream oss;
        oss << "safety: sequence gap — received " << f.counter << ", expected "
            << (last_counter_ + 1);
        emit_error(E2EError{E2EErrorKind::SequenceGap, f.counter, oss.str()});
        // Still deliver the sample; the application decides how to handle gaps.
    }
    has_first_    = true;
    last_counter_ = f.counter;

    dds::Sample out;
    out.topic     = raw.topic;
    out.payload   = std::move(f.payload);
    out.timestamp = raw.timestamp;
    ch_->try_send(std::move(out));
}

void E2ESubscriber::emit_error(E2EError e) { err_ch_->try_send(std::move(e)); }

std::error_code E2ESubscriber::close() {
    std::call_once(close_once_, [this] {
        sub_->close();
        if (pump_thread_.joinable()) pump_thread_.join();
    });
    return {};
}

std::shared_ptr<E2ESubscriber> new_e2e_subscriber(std::shared_ptr<dds::ISubscriber> sub, E2EConfig cfg) {
    return std::make_shared<E2ESubscriber>(std::move(sub), std::move(cfg));
}

} // namespace dds::safety
