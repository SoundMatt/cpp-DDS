// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/mock/participant.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// fusa:req REQ-MOCK-001 REQ-MOCK-002 REQ-MOCK-003 REQ-MOCK-004 REQ-MOCK-005

namespace dds::mock {

// ── Forward declarations ──────────────────────────────────────────────────────

class Broker;
class MockPublisher;
class MockSubscriber;
class MockParticipant;

// ── Broker ────────────────────────────────────────────────────────────────────

// Broker is the process-global in-memory pub/sub hub.
// All mock participants in the same process share one broker.
struct SubscriptionEntry {
    std::shared_ptr<dds::Chan<Sample>> ch;
    relay::BackPressurePolicy          back_pressure{relay::BackPressurePolicy::DropNewest};
    std::string                        topic;
};

class Broker {
public:
    static Broker& instance() {
        static Broker b;
        return b;
    }

    // publish delivers a sample to all subscribers on sample.topic.
    // fusa:req REQ-MOCK-003
    void publish(const Sample& sample) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = subs_.find(sample.topic);
        if (it == subs_.end()) return;

        for (auto& entry : it->second) {
            switch (entry.back_pressure) {
            case relay::BackPressurePolicy::DropNewest:
                entry.ch->try_send(sample);
                break;
            case relay::BackPressurePolicy::DropOldest:
                entry.ch->send_drop_oldest(sample);
                break;
            case relay::BackPressurePolicy::Block:
                entry.ch->send(sample);
                break;
            }
        }
    }

    // subscribe registers a channel for a given topic and returns it.
    // fusa:req REQ-MOCK-004
    std::shared_ptr<dds::Chan<Sample>>
    subscribe(const std::string& topic, int depth,
              relay::BackPressurePolicy bp,
              const QoS& qos,
              const std::optional<Sample>& last_sample)
    {
        auto ch = std::make_shared<dds::Chan<Sample>>(static_cast<std::size_t>(depth));
        SubscriptionEntry entry{ch, bp, topic};

        std::unique_lock<std::shared_mutex> lk(mu_);
        subs_[topic].push_back(entry);

        // TransientLocal: deliver the last sample to the new subscriber.
        if (qos.durability == DurabilityKind::TransientLocal && last_sample) {
            lk.unlock();
            ch->try_send(*last_sample);
        }
        return ch;
    }

    // unsubscribe removes a channel from a topic's subscription list.
    // fusa:req REQ-MOCK-005
    void unsubscribe(const std::string& topic,
                     const std::shared_ptr<dds::Chan<Sample>>& ch)
    {
        std::unique_lock<std::shared_mutex> lk(mu_);
        auto it = subs_.find(topic);
        if (it == subs_.end()) return;
        auto& list = it->second;
        list.erase(
            std::remove_if(list.begin(), list.end(),
                [&](const SubscriptionEntry& e){ return e.ch == ch; }),
            list.end());
    }

    // update_last_sample stores the most recent sample per topic (TransientLocal).
    void update_last_sample(const std::string& topic, const Sample& s) {
        std::unique_lock<std::shared_mutex> lk(last_mu_);
        last_samples_[topic] = s;
    }

    std::optional<Sample> last_sample(const std::string& topic) const {
        std::shared_lock<std::shared_mutex> lk(last_mu_);
        auto it = last_samples_.find(topic);
        if (it == last_samples_.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::shared_mutex                                 mu_;
    std::unordered_map<std::string, std::vector<SubscriptionEntry>> subs_;

    mutable std::shared_mutex                       last_mu_;
    std::unordered_map<std::string, Sample>         last_samples_;
};

// ── MockPublisher ──────────────────────────────────────────────────────────────

class MockPublisher : public IPublisher {
public:
    MockPublisher(std::string topic, QoS qos, Broker& broker,
                  std::atomic<bool>& participant_closed,
                  std::atomic<uint64_t>& seq)
        : topic_(std::move(topic))
        , qos_(std::move(qos))
        , broker_(broker)
        , participant_closed_(participant_closed)
        , seq_(seq)
    {}

    std::error_code write(const std::vector<uint8_t>& payload) override {
        return write(relay::Context::background(), payload);
    }

    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override {
        if (closed_.load()) return ErrClosed();
        if (participant_closed_.load()) return ErrClosed();
        if (ctx.done()) return ErrTimeout();

        if (qos_.max_sample_size > 0 &&
            static_cast<int>(payload.size()) > qos_.max_sample_size)
            return ErrPayloadTooLarge();

        Sample s;
        s.topic           = topic_;
        s.payload         = payload;
        s.timestamp       = std::chrono::system_clock::now();
        s.sequence_number = seq_.fetch_add(1);

        if (qos_.durability == DurabilityKind::TransientLocal)
            broker_.update_last_sample(topic_, s);

        broker_.publish(s);
        return {};
    }

    std::error_code close() override {
        closed_.store(true);
        return {};
    }

private:
    std::string           topic_;
    QoS                   qos_;
    Broker&               broker_;
    std::atomic<bool>&    participant_closed_;
    std::atomic<uint64_t>& seq_;
    std::atomic<bool>     closed_{false};
};

// ── MockSubscriber ─────────────────────────────────────────────────────────────

class MockSubscriber : public ISubscriber {
public:
    MockSubscriber(std::string topic, QoS qos,
                   std::shared_ptr<dds::Chan<Sample>> ch,
                   Broker& broker)
        : topic_(std::move(topic))
        , qos_(std::move(qos))
        , ch_(std::move(ch))
        , broker_(broker)
    {}

    ~MockSubscriber() override {
        unsubscribe();
    }

    std::shared_ptr<dds::Chan<Sample>> channel() override { return ch_; }

    std::optional<Sample> try_read() override {
        return ch_->try_recv();
    }

    void unsubscribe() override {
        bool was_open = !unsubscribed_.exchange(true);
        if (was_open) {
            broker_.unsubscribe(topic_, ch_);
            ch_->close();
        }
    }

    std::error_code close() override {
        unsubscribe();
        return {};
    }

private:
    std::string                        topic_;
    QoS                                qos_;
    std::shared_ptr<dds::Chan<Sample>> ch_;
    Broker&                            broker_;
    std::atomic<bool>                  unsubscribed_{false};
};

// ── MockParticipant ────────────────────────────────────────────────────────────

class MockParticipant : public IParticipant {
public:
    explicit MockParticipant(Domain domain)
        : domain_(domain)
        , broker_(Broker::instance())
    {}

    std::pair<std::shared_ptr<IPublisher>, std::error_code>
    new_publisher(const std::string& topic, QoS qos) override {
        if (closed_.load()) return {nullptr, ErrClosed()};
        if (topic.empty())  return {nullptr, ErrTopicEmpty()};

        auto pub = std::make_shared<MockPublisher>(
            topic, qos, broker_, closed_, seq_);
        return {pub, {}};
    }

    std::pair<std::shared_ptr<ISubscriber>, std::error_code>
    new_subscriber(const std::string& topic, QoS qos,
                   std::vector<relay::SubscriberOption> opts) override {
        if (closed_.load()) return {nullptr, ErrClosed()};
        if (topic.empty())  return {nullptr, ErrTopicEmpty()};

        relay::SubscriberConfig cfg = relay::apply_options(opts);
        int depth = cfg.effective_depth(static_cast<int>(kDefaultChanDepth));

        auto last = broker_.last_sample(topic);
        auto ch = broker_.subscribe(topic, depth, cfg.back_pressure, qos, last);
        auto sub = std::make_shared<MockSubscriber>(topic, qos, ch, broker_);
        return {sub, {}};
    }

    Domain domain() const noexcept override { return domain_; }

    std::error_code close() override {
        closed_.store(true);
        return {};
    }

private:
    Domain                domain_;
    Broker&               broker_;
    std::atomic<bool>     closed_{false};
    std::atomic<uint64_t> seq_{0};
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::pair<std::shared_ptr<IParticipant>, std::error_code>
create(Domain domain) {
    if (auto ec = validate_domain(domain); ec)
        return {nullptr, ec};

    return {std::make_shared<MockParticipant>(domain), {}};
}

} // namespace dds::mock
