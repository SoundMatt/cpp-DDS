// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/dds.hpp>
#include <iomanip>
#include <sstream>
#include <thread>

// fusa:req REQ-DDS-001 REQ-DDS-002 REQ-DDS-003 REQ-DDS-004 REQ-DDS-005
// fusa:req REQ-DDS-006 REQ-DDS-007 REQ-DDS-008 REQ-DDS-009 REQ-DDS-010
// fusa:req REQ-SEC-001 REQ-SEC-003 REQ-SEC-004 REQ-SAFETY-002
// fusa:req REQ-RELAY-VEC-001

namespace dds {

// ── Error category ────────────────────────────────────────────────────────────

namespace {

class DdsErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "dds"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::topic_empty:         return "dds: topic name must not be empty";
        case Errc::qos_mismatch:        return "dds: QoS incompatibility between publisher and subscriber";
        case Errc::deadline_missed:     return "dds: deadline missed — no sample within QoS.Deadline period";
        case Errc::sample_rejected:     return "dds: sample rejected — resource limits exceeded";
        case Errc::resource_limits:     return "dds: resource limit exceeded";
        case Errc::loan_buffer:         return "dds: loan buffer unavailable or invalid";
        case Errc::domain_out_of_range: return "dds: domain out of range [0,232]";
        default:                        return "dds: unknown error";
        }
    }

    // §5.2: map dds::Errc to relay::Errc sentinels via error_condition equivalence.
    bool equivalent(int code, const std::error_condition& cond) const noexcept override {
        if (cond.category() != relay::error_category())
            return false;
        auto dc = static_cast<Errc>(code);
        auto re = static_cast<relay::Errc>(cond.value());
        switch (dc) {
        case Errc::topic_empty:         return re == relay::Errc::not_connected;
        case Errc::qos_mismatch:        return re == relay::Errc::not_connected;
        case Errc::domain_out_of_range: return re == relay::Errc::not_connected;
        case Errc::deadline_missed:     return re == relay::Errc::timeout;
        case Errc::sample_rejected:     return re == relay::Errc::payload_too_large;
        case Errc::resource_limits:     return re == relay::Errc::payload_too_large;
        case Errc::loan_buffer:         return re == relay::Errc::closed;
        default:                        return false;
        }
    }
};

} // anonymous namespace

const std::error_category& error_category() noexcept {
    static DdsErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

// ── Domain validation ─────────────────────────────────────────────────────────

std::error_code validate_domain(Domain d) noexcept {
    if (d < kDomainMin || d > kDomainMax)
        return ErrDomainOutOfRange();
    return {};
}

// ── Sample::to_message ────────────────────────────────────────────────────────

static std::string guid_to_hex(const Guid& g) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : g) oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

relay::Message Sample::to_message() const {
    relay::Message m;
    m.protocol  = relay::Protocol::DDS;
    m.id        = topic;
    m.payload   = payload;
    m.timestamp = timestamp;
    m.seq       = sequence_number;
    m.meta["dds.writer_guid"] = guid_to_hex(writer_guid);
    return m;
}

// ── from_message ──────────────────────────────────────────────────────────────

static bool hex_to_guid(const std::string& hex, Guid& out) {
    if (hex.size() != 32) return false;
    for (std::size_t i = 0; i < 16; ++i) {
        auto h = hex.substr(i * 2, 2);
        try {
            out[i] = static_cast<uint8_t>(std::stoul(h, nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::pair<Sample, std::error_code> from_message(const relay::Message& m) {
    Sample s;
    s.topic           = m.id;
    s.payload         = m.payload;
    s.timestamp       = m.timestamp;
    s.sequence_number = m.seq;

    auto it = m.meta.find("dds.writer_guid");
    if (it != m.meta.end())
        hex_to_guid(it->second, s.writer_guid);

    return {s, {}};
}

// ── adapt — relay::INode wrapper ──────────────────────────────────────────────

namespace {

class NodeAdapter : public relay::INode {
public:
    explicit NodeAdapter(std::shared_ptr<IParticipant> p) : p_(std::move(p)) {}

    relay::Protocol protocol() const noexcept override {
        return relay::Protocol::DDS;
    }

    std::error_code send(relay::Context ctx, const relay::Message& msg) override {
        if (ctx.done()) return relay::ErrTimeout();
        if (msg.id.empty()) return ErrTopicEmpty();

        auto [pub, err] = p_->new_publisher(msg.id, default_qos());
        if (err) return err;

        auto ec = pub->write(ctx, msg.payload);
        pub->close();
        return ec;
    }

    std::pair<std::shared_ptr<dds::Chan<relay::Message>>, std::error_code>
    subscribe(std::vector<relay::SubscriberOption> opts) override {
        relay::SubscriberConfig cfg = relay::apply_options(opts);
        int depth = cfg.effective_depth(static_cast<int>(kDefaultChanDepth));
        std::string topic = cfg.topic_name;

        if (topic.empty())
            return {nullptr, ErrTopicEmpty()};

        auto [new_sub, err] = p_->new_subscriber(topic, default_qos(), opts);
        if (err) return {nullptr, err};

        auto msg_ch    = std::make_shared<dds::Chan<relay::Message>>(static_cast<std::size_t>(depth));
        auto sample_ch = new_sub->channel();

        // Forward samples; new_sub captured by value keeps subscriber alive for
        // the thread's lifetime. Thread is detached — channel close signals exit.
        std::thread([sample_ch, msg_ch, kept_sub = std::move(new_sub)]() mutable {
            (void)kept_sub;
            while (true) {
                auto s = sample_ch->recv();
                if (!s) break;
                relay::Message m = s->to_message();
                if (!msg_ch->send(std::move(m))) break;
            }
            msg_ch->close();
        }).detach();

        return {msg_ch, {}};
    }

    std::error_code close() noexcept override {
        return p_->close();
    }

private:
    std::shared_ptr<IParticipant> p_;
};

} // anonymous namespace

std::unique_ptr<relay::INode> adapt(std::shared_ptr<IParticipant> p) {
    return std::make_unique<NodeAdapter>(std::move(p));
}

} // namespace dds
