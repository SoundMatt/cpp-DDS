// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/relay.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>

// fusa:req REQ-RELAY-001 REQ-RELAY-002 REQ-RELAY-003 REQ-RELAY-004 REQ-RELAY-005
// fusa:req REQ-RELAY-006 REQ-RELAY-007 REQ-RELAY-008 REQ-RELAY-009 REQ-RELAY-010
// fusa:req REQ-RELAY-011 REQ-RELAY-012 REQ-RELAY-013 REQ-RELAY-014 REQ-RELAY-015
// fusa:req REQ-RELAY-016 REQ-RELAY-017 REQ-RELAY-018 REQ-RELAY-019 REQ-RELAY-020
// fusa:req REQ-RELAY-021 REQ-RELAY-022 REQ-RELAY-023 REQ-RELAY-024 REQ-RELAY-025
// fusa:req REQ-RELAY-026 REQ-RELAY-027 REQ-RELAY-028 REQ-RELAY-029
// fusa:req REQ-RELAY-051 REQ-RELAY-056 REQ-RELAY-059 REQ-RELAY-060

namespace relay {

// ── to_string / parse_protocol ───────────────────────────────────────────────

std::string to_string(Protocol p) {
    switch (p) {
    case Protocol::CAN:    return "CAN";
    case Protocol::DDS:    return "DDS";
    case Protocol::LIN:    return "LIN";
    case Protocol::MQTT:   return "MQTT";
    case Protocol::RCP:    return "RCP";
    case Protocol::SOMEIP: return "SOMEIP";
    default:               return "unknown";
    }
}

bool try_parse_protocol(std::string_view s, Protocol& out) noexcept {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    auto l = upper.find_first_not_of(" \t");
    auto r = upper.find_last_not_of(" \t");
    if (l == std::string::npos) return false;
    upper = upper.substr(l, r - l + 1);

    if (upper == "CAN")           { out = Protocol::CAN;    return true; }
    if (upper == "DDS")           { out = Protocol::DDS;    return true; }
    if (upper == "LIN")           { out = Protocol::LIN;    return true; }
    if (upper == "MQTT")          { out = Protocol::MQTT;   return true; }
    if (upper == "RCP")           { out = Protocol::RCP;    return true; }
    if (upper == "SOMEIP")        { out = Protocol::SOMEIP; return true; }
    if (upper == "SOME/IP")       { out = Protocol::SOMEIP; return true; }
    return false;
}

Protocol parse_protocol(std::string_view s) {
    Protocol p{};
    if (!try_parse_protocol(s, p))
        throw std::invalid_argument("relay: unknown protocol: " + std::string(s));
    return p;
}

// ── Version::to_string ────────────────────────────────────────────────────────

std::string Version::to_string() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// ── Error category ── REQ-RELAY-021 REQ-RELAY-022 ────────────────────────────

namespace {

class RelayErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "relay"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
        case Errc::closed:            return "relay: closed";
        case Errc::not_connected:     return "relay: not connected";
        case Errc::timeout:           return "relay: timeout";
        case Errc::payload_too_large: return "relay: payload too large";
        default:                      return "relay: unknown error";
        }
    }
};

} // anonymous namespace

const std::error_category& error_category() noexcept {
    static RelayErrorCategory cat;
    return cat;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

} // namespace relay
