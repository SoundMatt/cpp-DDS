// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// dds/security/access.hpp — topic-level access control list (ACL).
//
// C++ port of github.com/SoundMatt/go-DDS's `security` package
// (security/access.go): Permission is a per-participant/per-topic bitfield,
// and AccessPolicy evaluates an ordered list of glob-pattern Rules
// (first-match-wins; no match denies all access). See ROADMAP.md, "Tier 2
// — safety and security", `security`.
//
// Pattern matching is a byte-oriented port of Go's path.Match (see
// go/src/path/match.go) — the same glob syntax go-DDS's AccessPolicy uses:
//   - '*' matches any sequence of non-'/' characters (one path segment)
//   - '?' matches any single non-'/' character
//   - '[abc]' / '[^abc]' matches a (negated) character class, with '-'
//     ranges and '\' escaping
//   - any other character matches literally
// This is a behavioral (not wire-format) port — AccessPolicy has no on-wire
// representation, so correctness here means matching go-DDS's actual
// path.Match-derived decisions for the same (pattern, topic) pairs, verified
// against the same case matrix as go-DDS's access_test.go — see
// tests/test_security_access.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dds::security {

// Permission is a bitfield of allowed operations on a topic.
// fusa:req REQ-SECURITY-008
enum class Permission : uint8_t {
    None      = 0,
    Read      = 1u << 0,
    Write     = 1u << 1,
    ReadWrite = Read | Write,
};

inline Permission operator|(Permission a, Permission b) {
    return static_cast<Permission>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline Permission operator&(Permission a, Permission b) {
    return static_cast<Permission>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

// Rule pairs a topic glob pattern with the permissions it grants.
// fusa:req REQ-SECURITY-008
struct Rule {
    std::string pattern;
    Permission  allow{Permission::None};
};

// AccessPolicy enforces topic-level read/write permissions. Rules are
// evaluated in declaration order; the first matching rule wins. A topic
// that matches no rule is denied all access.
// fusa:req REQ-SECURITY-008
class AccessPolicy {
public:
    explicit AccessPolicy(std::vector<Rule> rules = {}) : rules_(std::move(rules)) {}

    // can_read returns true if any rule grants Read on topic.
    bool can_read(const std::string& topic) const { return allows(topic, Permission::Read); }

    // can_write returns true if any rule grants Write on topic.
    bool can_write(const std::string& topic) const { return allows(topic, Permission::Write); }

private:
    bool allows(const std::string& topic, Permission perm) const;

    std::vector<Rule> rules_;
};

// new_access_policy creates an AccessPolicy from the given rules, evaluated
// in the order given (go-DDS: security.NewAccessPolicy).
inline AccessPolicy new_access_policy(std::vector<Rule> rules) {
    return AccessPolicy(std::move(rules));
}

} // namespace dds::security
