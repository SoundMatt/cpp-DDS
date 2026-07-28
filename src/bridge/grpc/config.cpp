// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/bridge/grpc/config.hpp>

// C++ port of github.com/SoundMatt/go-DDS bridge/grpc/config.go. See
// include/dds/bridge/grpc/config.hpp for scope notes.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace dds::bridge::grpc {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// leading_ws counts the leading space/tab run of a raw (untrimmed) line.
std::size_t leading_ws(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

bool is_key_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

// split_key_value splits "key: value" on the first colon. Returns false if
// there is no colon or the key portion is empty/invalid — both signal
// malformed YAML for this module's minimal parser (see config.hpp's scope
// note).
bool split_key_value(const std::string& content, std::string& key, std::string& value) {
    auto pos = content.find(':');
    if (pos == std::string::npos) return false;
    key = trim(content.substr(0, pos));
    if (key.empty()) return false;
    for (char c : key) {
        if (!is_key_char(c)) return false;
    }
    value = unquote(trim(content.substr(pos + 1)));
    return true;
}

} // namespace

dds::QoS TopicConfig::effective_qos() const {
    const std::string lower = to_lower(qos);
    dds::QoS          q     = dds::default_qos();
    if (lower == "reliable") {
        q.reliability = dds::ReliabilityKind::Reliable;
        return q;
    }
    if (lower == "best_effort" || lower == "besteffort") {
        q.reliability = dds::ReliabilityKind::BestEffort;
        return q;
    }
    return dds::default_qos();
}

LoadResult load_config(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return LoadResult{std::nullopt, "grpcbridge: read config \"" + path + "\": no such file"};

    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = buf.str();

    Config      cfg;
    bool        in_topics = false;
    TopicConfig* current  = nullptr;

    auto malformed = [&](const std::string& line) -> LoadResult {
        return LoadResult{std::nullopt, "grpcbridge: parse config \"" + path + "\": malformed line: " + line};
    };

    std::istringstream lines(text);
    std::string        raw_line;
    while (std::getline(lines, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) continue;

        std::size_t indent = leading_ws(raw_line);
        if (indent == 0) {
            in_topics = false;
            current   = nullptr;
            std::string key, value;
            if (!split_key_value(raw_line, key, value)) return malformed(raw_line);
            if (key == "listen") {
                cfg.listen = value;
            } else if (key == "auth_token") {
                cfg.auth_token = value;
            } else if (key == "topics") {
                if (!value.empty()) return malformed(raw_line);
                in_topics = true;
            }
            // Unknown top-level keys are ignored, matching yaml.v3's
            // default (non-KnownFields) Unmarshal behavior into a Go
            // struct with only these three known fields.
            continue;
        }

        if (!in_topics) return malformed(raw_line);

        std::string content = trim(raw_line);
        if (content.rfind("- ", 0) == 0 || content == "-") {
            cfg.topics.emplace_back();
            current       = &cfg.topics.back();
            std::string rest = trim(content.substr(1));
            if (!rest.empty()) {
                std::string key, value;
                if (!split_key_value(rest, key, value)) return malformed(raw_line);
                if (key == "name") current->name = value;
                else if (key == "qos") current->qos = value;
            }
            continue;
        }

        if (current == nullptr) return malformed(raw_line);
        std::string key, value;
        if (!split_key_value(content, key, value)) return malformed(raw_line);
        if (key == "name") current->name = value;
        else if (key == "qos") current->qos = value;
    }

    return LoadResult{std::move(cfg), std::nullopt};
}

std::error_code apply_config(Bridge& bridge, const Config& cfg) {
    auto participant = bridge.participant();
    for (const auto& tc : cfg.topics) {
        if (tc.name.empty()) continue;
        auto [sub, err] = participant->new_subscriber(tc.name, tc.effective_qos());
        (void)sub;
        if (err) return err;
    }
    return {};
}

} // namespace dds::bridge::grpc
