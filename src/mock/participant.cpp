// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/mock/participant.hpp>
#include <dds/mock/loan.hpp>
#include <dds/pool/pool.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// fusa:req REQ-MOCK-001 REQ-MOCK-002 REQ-MOCK-003 REQ-MOCK-004 REQ-MOCK-005
// fusa:req REQ-METRICS-001 REQ-METRICS-002 REQ-METRICS-003
// fusa:req REQ-METRICS-004 REQ-METRICS-005 REQ-METRICS-006
// fusa:req REQ-HEALTH-001 REQ-HEALTH-002 REQ-HEALTH-003
// fusa:req REQ-DDS-010 REQ-SEC-004 REQ-SEC-005 REQ-LIFECYCLE-001 REQ-LIFECYCLE-002
// fusa:req REQ-LIFECYCLE-003 REQ-LIFECYCLE-004 REQ-LIFECYCLE-005
// fusa:req REQ-SAFETY-001 REQ-SAFETY-002 REQ-SAFETY-003

namespace dds::mock {

// ── Forward declarations ──────────────────────────────────────────────────────

class Broker;
class MockPublisher;
class MockSubscriber;
class MockParticipantImpl;

// ── Per-participant metrics counters ──────────────────────────────────────────

// fusa:req REQ-METRICS-001 REQ-METRICS-002
struct ParticipantCounters {
    std::atomic<uint64_t> write_count{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> deliver_count{0};
    std::atomic<uint64_t> drop_count{0};
    std::atomic<uint64_t> bytes_delivered{0};
    std::atomic<uint64_t> error_count{0};
};

// Per-topic breakdown of the same counters, keyed by topic name within a
// single participant (dds::ITopicMetricsProvider, go-DDS:
// mock.mockTopicCounter). Scoped per-participant to match ParticipantCounters
// above — go-DDS's mock keeps a single topic-counter table on the
// process-global broker instead (shared identically by every participant
// sharing it), but cpp-DDS's mock already gives each participant its own
// aggregate ParticipantCounters rather than a shared one, so per-topic
// counters follow the same already-established scope for consistency.
// fusa:req REQ-METRICS-006
struct TopicCounters {
    std::atomic<uint64_t> write_count{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> deliver_count{0};
    std::atomic<uint64_t> drop_count{0};
    std::atomic<uint64_t> bytes_delivered{0};
};

// ── Broker ────────────────────────────────────────────────────────────────────

struct DeliveryStats {
    int      delivered{0};
    int      dropped{0};
    uint64_t bytes{0};
};

// Broker is the process-global in-memory pub/sub hub.
// All mock participants in the same process share one broker.
struct SubscriptionEntry {
    std::shared_ptr<dds::Chan<Sample>>  ch;
    relay::BackPressurePolicy           back_pressure{relay::BackPressurePolicy::DropNewest};
    std::string                         topic;
    std::function<bool(const Sample&)>  filter; // null = accept all
};

class Broker {
public:
    static Broker& instance() {
        static Broker b;
        return b;
    }

    // publish delivers a sample to all subscribers on sample.topic.
    // Returns delivery statistics for metrics tracking.
    // fusa:req REQ-MOCK-003 REQ-DDS-010
    DeliveryStats publish(const Sample& sample) {
        DeliveryStats stats;
        stats.bytes = sample.payload.size();

        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = subs_.find(sample.topic);
        if (it == subs_.end()) return stats;

        for (auto& entry : it->second) {
            if (entry.filter && !entry.filter(sample)) continue;

            bool delivered = false;
            switch (entry.back_pressure) {
            case relay::BackPressurePolicy::DropNewest:
                delivered = (entry.ch->try_send(sample) == dds::Chan<Sample>::SendResult::Ok);
                break;
            case relay::BackPressurePolicy::DropOldest:
                delivered = entry.ch->send_drop_oldest(sample);
                break;
            case relay::BackPressurePolicy::Block:
                delivered = entry.ch->send(sample);
                break;
            }
            if (delivered) {
                stats.delivered++;
            } else {
                stats.dropped++;
            }
        }
        return stats;
    }

    // subscribe registers a channel for a given topic and returns it.
    // fusa:req REQ-MOCK-004 REQ-DDS-010
    std::shared_ptr<dds::Chan<Sample>>
    subscribe(const std::string& topic, int depth,
              relay::BackPressurePolicy bp,
              const QoS& qos,
              const std::optional<Sample>& last_sample,
              std::function<bool(const Sample&)> filter = {})
    {
        auto ch = std::make_shared<dds::Chan<Sample>>(static_cast<std::size_t>(depth));
        SubscriptionEntry entry{ch, bp, topic, std::move(filter)};

        std::unique_lock<std::shared_mutex> lk(mu_);
        subs_[topic].push_back(entry);

        // TransientLocal: deliver the last sample to the new subscriber.
        if (qos.durability == DurabilityKind::TransientLocal && last_sample) {
            lk.unlock();
            if (!entry.filter || entry.filter(*last_sample))
                ch->try_send(*last_sample);
        }
        return ch;
    }

    // unsubscribe removes a channel from a topic's subscription list.
    // fusa:req REQ-MOCK-005 REQ-LIFECYCLE-002
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
    mutable std::shared_mutex                                        mu_;
    std::unordered_map<std::string, std::vector<SubscriptionEntry>> subs_;

    mutable std::shared_mutex                    last_mu_;
    std::unordered_map<std::string, Sample>      last_samples_;
};

// ── MockPublisher ──────────────────────────────────────────────────────────────

class MockPublisher : public IPublisher {
public:
    MockPublisher(std::string topic, QoS qos, Broker& broker,
                  std::atomic<bool>& participant_closed,
                  std::atomic<uint64_t>& seq,
                  std::shared_ptr<ParticipantCounters> ctrs,
                  std::shared_ptr<TopicCounters> topic_ctrs)
        : topic_(std::move(topic))
        , qos_(std::move(qos))
        , broker_(broker)
        , participant_closed_(participant_closed)
        , seq_(seq)
        , ctrs_(std::move(ctrs))
        , topic_ctrs_(std::move(topic_ctrs))
    {}

    std::error_code write(const std::vector<uint8_t>& payload) override {
        return write(relay::Context::background(), payload);
    }

    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override {
        if (closed_.load())             { ctrs_->error_count++; return ErrClosed(); }
        if (participant_closed_.load()) { ctrs_->error_count++; return ErrClosed(); }
        if (ctx.done())                 { ctrs_->error_count++; return ErrTimeout(); }

        if (qos_.max_sample_size > 0 &&
            static_cast<int>(payload.size()) > qos_.max_sample_size) {
            ctrs_->error_count++;
            return ErrPayloadTooLarge();
        }

        Sample s;
        s.topic           = topic_;
        s.payload         = payload;
        s.timestamp       = std::chrono::system_clock::now();
        s.sequence_number = seq_.fetch_add(1);

        if (qos_.durability == DurabilityKind::TransientLocal)
            broker_.update_last_sample(topic_, s);

        auto stats = broker_.publish(s);

        ctrs_->write_count++;
        ctrs_->bytes_written   += payload.size();
        ctrs_->deliver_count   += static_cast<uint64_t>(stats.delivered);
        ctrs_->drop_count      += static_cast<uint64_t>(stats.dropped);
        ctrs_->bytes_delivered += static_cast<uint64_t>(stats.bytes)
                                  * static_cast<uint64_t>(stats.delivered);

        topic_ctrs_->write_count++;
        topic_ctrs_->bytes_written   += payload.size();
        topic_ctrs_->deliver_count   += static_cast<uint64_t>(stats.delivered);
        topic_ctrs_->drop_count      += static_cast<uint64_t>(stats.dropped);
        topic_ctrs_->bytes_delivered += static_cast<uint64_t>(stats.bytes)
                                         * static_cast<uint64_t>(stats.delivered);
        return {};
    }

    std::error_code close() override {
        closed_.store(true);
        return {};
    }

    // is_closed reports whether close() has already run on this publisher
    // itself (not the owning participant). Used by MockLoaningPublisher's
    // own loan_buffer closed-check, mirroring rtps::Writer::is_closed()
    // (src/rtps/participant.cpp) exactly.
    bool is_closed() const noexcept { return closed_.load(); }

private:
    std::string                           topic_;
    QoS                                   qos_;
    Broker&                               broker_;
    std::atomic<bool>&                    participant_closed_;
    std::atomic<uint64_t>&                seq_;
    std::shared_ptr<ParticipantCounters>  ctrs_;
    std::shared_ptr<TopicCounters>        topic_ctrs_;
    std::atomic<bool>                     closed_{false};
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

    // Destructor automatically unsubscribes (fusa:req REQ-LIFECYCLE-005).
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

// ── MockParticipantImpl ────────────────────────────────────────────────────────

class MockParticipantImpl : public IMockParticipant {
public:
    // isolated_broker: C++ port of go-DDS's mock.IsolatedBroker() Option
    // (see mock/participant.hpp's file-level comment). When true, this
    // participant owns a private Broker instance instead of referencing
    // the process-global singleton, so it shares no subscription state
    // with any other participant (isolated or not).
    // fusa:req REQ-MOCK-006
    MockParticipantImpl(Domain domain, bool isolated_broker)
        : domain_(domain)
        , owned_broker_(isolated_broker ? std::make_unique<Broker>() : nullptr)
        , broker_(owned_broker_ ? *owned_broker_ : Broker::instance())
        , ctrs_(std::make_shared<ParticipantCounters>())
    {}

    // ── IParticipant ──────────────────────────────────────────────────────────

    std::pair<std::shared_ptr<IPublisher>, std::error_code>
    new_publisher(const std::string& topic, QoS qos) override {
        if (closed_.load()) return {nullptr, ErrClosed()};
        if (topic.empty())  return {nullptr, ErrTopicEmpty()};

        auto pub = std::make_shared<MockPublisher>(
            topic, qos, broker_, closed_, seq_, ctrs_, topic_counters_for(topic));
        return {pub, {}};
    }

    std::pair<std::shared_ptr<ISubscriber>, std::error_code>
    new_subscriber(const std::string& topic, QoS qos,
                   std::vector<relay::SubscriberOption> opts) override {
        if (closed_.load()) return {nullptr, ErrClosed()};
        if (topic.empty())  return {nullptr, ErrTopicEmpty()};

        relay::SubscriberConfig cfg = relay::apply_options(opts);
        int depth = cfg.effective_depth(static_cast<int>(kDefaultChanDepth));

        // Wrap relay::Message filter as Sample filter if set.
        std::function<bool(const Sample&)> sample_filter;
        if (cfg.filter) {
            sample_filter = [f = cfg.filter](const Sample& s) {
                return f(s.to_message());
            };
        }

        auto last = broker_.last_sample(topic);
        auto ch   = broker_.subscribe(topic, depth, cfg.back_pressure, qos,
                                      last, std::move(sample_filter));

        {
            std::lock_guard<std::mutex> lk(sub_mu_);
            sub_channels_.emplace_back(std::weak_ptr<dds::Chan<Sample>>(ch));
        }

        auto sub = std::make_shared<MockSubscriber>(topic, qos, ch, broker_);
        return {sub, {}};
    }

    Domain domain() const noexcept override { return domain_; }

    // fusa:req REQ-LIFECYCLE-001 REQ-LIFECYCLE-003
    std::error_code close() override {
        if (closed_.exchange(true)) return {}; // idempotent (spec §6 req 1)

        // spec §6 req 3: channels already returned via new_subscriber() MUST
        // be closed by the implementation, unblocking any blocked recv().
        std::lock_guard<std::mutex> lk(sub_mu_);
        for (auto& wch : sub_channels_) {
            if (auto ch = wch.lock()) ch->close();
        }
        return {};
    }

    // ── IMetricsProvider ──────────────────────────────────────────────────────

    // fusa:req REQ-METRICS-003
    relay::Metrics metrics() const override {
        relay::Metrics m;
        m.write_count     = ctrs_->write_count.load();
        m.bytes_written   = ctrs_->bytes_written.load();
        m.deliver_count   = ctrs_->deliver_count.load();
        m.drop_count      = ctrs_->drop_count.load();
        m.bytes_delivered = ctrs_->bytes_delivered.load();
        m.error_count     = ctrs_->error_count.load();
        return m;
    }

    // ── IHealthProvider ───────────────────────────────────────────────────────

    // fusa:req REQ-HEALTH-001 REQ-HEALTH-002
    relay::Health health() const override {
        if (closed_.load())
            return {relay::HealthStatus::Down, "participant closed"};
        return {relay::HealthStatus::OK, ""};
    }

    // ── IDrainer ──────────────────────────────────────────────────────────────

    // close_with_drain waits until all subscriber channels are empty or timeout
    // elapses, then closes the participant.
    // Returns ErrTimeout if drain did not complete within the deadline.
    std::error_code close_with_drain(std::chrono::milliseconds timeout) override {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all_empty = true;
            {
                std::lock_guard<std::mutex> lk(sub_mu_);
                for (auto& wch : sub_channels_) {
                    if (auto ch = wch.lock()) {
                        if (ch->size() > 0) { all_empty = false; break; }
                    }
                }
            }
            if (all_empty) { return close(); }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        close();
        return ErrTimeout();
    }

    // ── dds::IMetricsProvider (DDS-package-scoped; mirrors go-DDS's
    // dds.MetricsProvider) ──────────────────────────────────────────────────

    // fusa:req REQ-METRICS-004
    dds::Metrics dds_metrics() const override {
        dds::Metrics m;
        m.write_count     = ctrs_->write_count.load();
        m.bytes_written   = ctrs_->bytes_written.load();
        m.deliver_count   = ctrs_->deliver_count.load();
        m.drop_count      = ctrs_->drop_count.load();
        m.bytes_delivered = ctrs_->bytes_delivered.load();
        m.error_count     = ctrs_->error_count.load();
        return m;
    }

    // ── dds::IDiscoveryMetricsProvider ──────────────────────────────────────

    // The mock has no real network discovery; always returns zero values,
    // matching go-DDS's mock.participant.DiscoveryMetrics doc comment
    // verbatim ("The mock has no real network discovery; this always
    // returns zero values").
    // fusa:req REQ-METRICS-005
    dds::DiscoveryMetrics discovery_metrics() const override {
        return dds::DiscoveryMetrics{};
    }

    // ── dds::ITopicMetricsProvider ────────────────────────────────────────

    // fusa:req REQ-METRICS-006
    std::vector<dds::TopicMetrics> topic_metrics() const override {
        std::vector<dds::TopicMetrics> result;
        std::lock_guard<std::mutex> lk(topic_ctrs_mu_);
        result.reserve(topic_ctrs_.size());
        for (auto& [topic, tc] : topic_ctrs_) {
            dds::TopicMetrics tm;
            tm.topic           = topic;
            tm.write_count     = tc->write_count.load();
            tm.deliver_count   = tc->deliver_count.load();
            tm.drop_count      = tc->drop_count.load();
            tm.bytes_written   = tc->bytes_written.load();
            tm.bytes_delivered = tc->bytes_delivered.load();
            result.push_back(std::move(tm));
        }
        return result;
    }

    // ── dds::IHealthProvider (DDS-package-scoped; mirrors go-DDS's
    // dds.HealthProvider) ──────────────────────────────────────────────────

    // Matches go-DDS's mock.participant.Health() verbatim, including the
    // exact JSON details string on close.
    // fusa:req REQ-HEALTH-003
    dds::Health dds_health() const override {
        if (closed_.load())
            return dds::Health{dds::HealthStatus::Down, R"({"state":"closed"})"};
        return dds::Health{dds::HealthStatus::OK, ""};
    }

private:
    // topic_counters_for returns (creating on first access) the per-topic
    // counter block for topic, scoped to this participant. Matches go-DDS's
    // broker.topicCounterFor / participant.topicCounterFor (rtps side).
    std::shared_ptr<TopicCounters> topic_counters_for(const std::string& topic) {
        std::lock_guard<std::mutex> lk(topic_ctrs_mu_);
        auto it = topic_ctrs_.find(topic);
        if (it != topic_ctrs_.end()) return it->second;
        auto tc = std::make_shared<TopicCounters>();
        topic_ctrs_.emplace(topic, tc);
        return tc;
    }

    Domain                                       domain_;
    std::unique_ptr<Broker>                      owned_broker_; // non-null only when isolated
    Broker&                                      broker_;
    std::shared_ptr<ParticipantCounters>         ctrs_;
    std::atomic<bool>                            closed_{false};
    std::atomic<uint64_t>                        seq_{0};

    mutable std::mutex                           sub_mu_;
    std::vector<std::weak_ptr<dds::Chan<Sample>>> sub_channels_;

    mutable std::mutex                                              topic_ctrs_mu_;
    std::unordered_map<std::string, std::shared_ptr<TopicCounters>> topic_ctrs_;
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::pair<std::shared_ptr<IMockParticipant>, std::error_code>
create(Domain domain, bool isolated_broker) {
    if (auto ec = validate_domain(domain); ec)
        return {nullptr, ec};

    return {std::make_shared<MockParticipantImpl>(domain, isolated_broker), {}};
}

// ── Loan integration (mock side; "Also within ddscore but not RTPS-
//    specific" ROADMAP.md item) ─────────────────────────────────────────
//
// See mock/loan.hpp for the full file-level scope note. C++ port of
// go-DDS's mock/loan.go loaningPublisher/NewLoaningPublisher — the same
// shape as rtps/participant.cpp's own LoaningWriter/new_loaning_publisher
// (Tier-1 phase 9), wrapping a MockPublisher instead of a Writer.

namespace {

// MockLoaningPublisher wraps a MockPublisher with a dds::pool::BytePool
// for allocation-free loaned-sample publishing, matching go-DDS's
// loaningPublisher (which embeds *publisher and adds a *pool.BytePool).
// write()/close() delegate to the wrapped MockPublisher unchanged — this
// class adds only loan_buffer/write_loaned/return_loan.
class MockLoaningPublisher : public ILoaningPublisher {
public:
    MockLoaningPublisher(std::shared_ptr<MockPublisher> pub, std::size_t buf_size)
        : pub_(std::move(pub)), pool_(buf_size) {}

    std::error_code write(const std::vector<uint8_t>& payload) override { return pub_->write(payload); }

    std::error_code write(relay::Context ctx, const std::vector<uint8_t>& payload) override {
        return pub_->write(ctx, payload);
    }

    std::error_code close() override { return pub_->close(); }

    // Matches go-DDS's loaningPublisher.Loan: reject on a closed
    // publisher, then hand out a pool buffer, returning ErrLoanBuffer
    // (and putting the undersized buffer straight back) if the pool's
    // configured capacity can't satisfy the request.
    std::pair<std::vector<uint8_t>*, std::error_code> loan_buffer(std::size_t size) override {
        if (pub_->is_closed()) return {nullptr, dds::ErrClosed()};

        std::vector<uint8_t>* buf = pool_.get();
        if (buf->capacity() < size) {
            pool_.put(buf);
            return {nullptr, dds::ErrLoanBuffer()};
        }
        buf->resize(size);
        return {buf, {}};
    }

    // Matches go-DDS's loaningPublisher.Commit: publish, then return the
    // buffer to the pool regardless of whether the write succeeded (no
    // validation that buf actually came from this pool — see
    // mock/loan.hpp's file-level scope note on why that matches go-DDS's
    // own behavior).
    std::error_code write_loaned(std::vector<uint8_t>* buf) override {
        std::error_code err = pub_->write(*buf);
        pool_.put(buf);
        return err;
    }

    // return_loan has no go-DDS LoaningPublisher equivalent (dds.
    // ILoaningPublisher added it ahead of the RTPS phase that first
    // implemented the interface, as an explicit discard-without-publish
    // operation) — trivially returns the buffer to the pool without
    // writing.
    void return_loan(std::vector<uint8_t>* buf) override { pool_.put(buf); }

private:
    std::shared_ptr<MockPublisher> pub_;
    dds::pool::BytePool            pool_;
};

} // namespace

std::pair<std::shared_ptr<ILoaningPublisher>, std::error_code>
new_loaning_publisher(const std::shared_ptr<IParticipant>& p, const std::string& topic, QoS qos,
                       std::size_t buf_size) {
    auto [pub, err] = p->new_publisher(topic, qos);
    if (err) return {nullptr, err};

    // Matches go-DDS's `mpub, ok := pub.(*publisher)` type assertion: any
    // IParticipant implementation other than dds::mock (whose
    // new_publisher always returns a MockPublisher) fails this cast.
    auto mp = std::dynamic_pointer_cast<MockPublisher>(pub);
    if (!mp) {
        pub->close();
        return {nullptr, dds::ErrLoanBuffer()};
    }
    return {std::make_shared<MockLoaningPublisher>(std::move(mp), buf_size), std::error_code{}};
}

} // namespace dds::mock
