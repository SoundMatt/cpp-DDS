// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Tests for dds::security::AccessPolicy — a behavioral port of
// github.com/SoundMatt/go-DDS's security.AccessPolicy (security/access.go),
// itself built on Go's path.Match. AccessPolicy has no on-wire
// representation, so correctness here is behavioral parity against
// go-DDS's actual decisions for the same (pattern, topic) case matrix as
// go-DDS's access_test.go — not a byte vector.
//
// fusa:test REQ-SECURITY-008

#include <dds/security/access.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace dds::security;

TEST_CASE("AccessPolicy: exact match, read-only rule", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"vehicle/speed", Permission::Read}});
    CHECK(p.can_read("vehicle/speed"));
    CHECK_FALSE(p.can_write("vehicle/speed"));
}

TEST_CASE("AccessPolicy: exact match, write-only rule", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"actuator/brake", Permission::Write}});
    CHECK(p.can_write("actuator/brake"));
    CHECK_FALSE(p.can_read("actuator/brake"));
}

TEST_CASE("AccessPolicy: ReadWrite rule grants both", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"sensor/temp", Permission::ReadWrite}});
    CHECK(p.can_read("sensor/temp"));
    CHECK(p.can_write("sensor/temp"));
}

TEST_CASE("AccessPolicy: '*' matches a single path segment", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"vehicle/*", Permission::Read}});
    CHECK(p.can_read("vehicle/speed"));
    CHECK(p.can_read("vehicle/rpm"));
    // '*' does not cross '/' — a multi-segment child must not match.
    CHECK_FALSE(p.can_read("vehicle/engine/rpm"));
}

TEST_CASE("AccessPolicy: bare '*' matches only top-level topics",
          "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"*", Permission::Read}});
    CHECK(p.can_read("speed"));
    CHECK_FALSE(p.can_read("vehicle/speed"));
}

TEST_CASE("AccessPolicy: no matching rule denies all access", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"allowed/topic", Permission::ReadWrite}});
    CHECK_FALSE(p.can_read("other/topic"));
    CHECK_FALSE(p.can_write("other/topic"));
}

TEST_CASE("AccessPolicy: empty policy denies all access", "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p;
    CHECK_FALSE(p.can_read("any/topic"));
    CHECK_FALSE(p.can_write("any/topic"));
}

TEST_CASE("AccessPolicy: first matching rule wins", "[security][access][REQ-SECURITY-008]") {
    // First rule grants Read; second rule (shadowed) grants Write.
    AccessPolicy p({
        Rule{"topic", Permission::Read},
        Rule{"topic", Permission::Write},
    });
    CHECK(p.can_read("topic"));
    CHECK_FALSE(p.can_write("topic"));
}

TEST_CASE("AccessPolicy: malformed pattern is skipped, not fatal",
          "[security][access][REQ-SECURITY-008]") {
    // "[bad" is an unterminated character class — Go's path.Match returns
    // ErrBadPattern for it; the policy must skip the rule rather than
    // treating it as a match or propagating an error.
    AccessPolicy p({
        Rule{"[bad", Permission::ReadWrite},
        Rule{"good", Permission::Read},
    });
    CHECK(p.can_read("good"));
    CHECK_FALSE(p.can_read("[bad"));
}

TEST_CASE("AccessPolicy: '?' matches exactly one non-'/' character",
          "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"topic?", Permission::Read}});
    CHECK(p.can_read("topic1"));
    CHECK(p.can_read("topicA"));
    CHECK_FALSE(p.can_read("topic"));   // '?' requires exactly one char
    CHECK_FALSE(p.can_read("topic12")); // and no more than one
}

TEST_CASE("AccessPolicy: character class matches a range",
          "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"zone[1-3]", Permission::Read}});
    CHECK(p.can_read("zone1"));
    CHECK(p.can_read("zone2"));
    CHECK(p.can_read("zone3"));
    CHECK_FALSE(p.can_read("zone4"));
}

TEST_CASE("AccessPolicy: negated character class excludes a range",
          "[security][access][REQ-SECURITY-008]") {
    AccessPolicy p({Rule{"zone[^1-3]", Permission::Read}});
    CHECK_FALSE(p.can_read("zone1"));
    CHECK(p.can_read("zone9"));
}

TEST_CASE("AccessPolicy: new_access_policy factory mirrors the constructor",
          "[security][access][REQ-SECURITY-008]") {
    auto p = new_access_policy({Rule{"a/*", Permission::Read}});
    CHECK(p.can_read("a/b"));
    CHECK_FALSE(p.can_write("a/b"));
}
