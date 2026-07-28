// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <dds/security/access.hpp>

// fusa:req REQ-SECURITY-008

namespace dds::security {

namespace {

// ── glob_match — a byte-oriented port of Go's path.Match ────────────────────
//
// Faithful transcription of go/src/path/match.go's Match/scanChunk/
// matchChunk/getEsc, treating each byte as one "rune" (topic names in
// practice are ASCII, so this is behaviorally identical to Go's full
// UTF-8-aware version for the domain AccessPolicy patterns operate over).
// See include/dds/security/access.hpp's file comment for the supported
// syntax.

struct ScanResult {
    bool        star;
    std::string chunk;
    std::string rest;
};

// scan_chunk gets the next segment of pattern: a non-'*' string possibly
// preceded by one or more '*'.
ScanResult scan_chunk(std::string pattern) {
    bool        star = false;
    std::size_t p    = 0;
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
        star = true;
    }
    pattern = pattern.substr(p);

    bool inrange = false;
    for (std::size_t i = 0; i < pattern.size(); i++) {
        switch (pattern[i]) {
        case '\\':
            if (i + 1 < pattern.size()) i++;
            break;
        case '[':
            inrange = true;
            break;
        case ']':
            inrange = false;
            break;
        case '*':
            if (!inrange) {
                return {star, pattern.substr(0, i), pattern.substr(i)};
            }
            break;
        default:
            break;
        }
    }
    return {star, pattern, ""};
}

// get_esc extracts a possibly-'\'-escaped character from the front of
// chunk. Returns false (malformed pattern) if chunk starts with an
// unescapable character or would become empty after consuming this one
// (every character inside a class must be followed by more chunk, since
// the class must still be terminated by ']').
bool get_esc(std::string& chunk, char& out) {
    if (chunk.empty() || chunk[0] == '-' || chunk[0] == ']') return false;
    if (chunk[0] == '\\') {
        chunk = chunk.substr(1);
        if (chunk.empty()) return false;
    }
    out   = chunk[0];
    chunk = chunk.substr(1);
    return !chunk.empty();
}

struct ChunkResult {
    std::string rest;
    bool        ok;
    bool        malformed;
};

// match_chunk checks whether chunk (all single-character operators:
// literals, character classes, '?') matches the beginning of s. If so, it
// returns the remainder of s after the match.
ChunkResult match_chunk(std::string chunk, std::string s) {
    bool failed = false;
    while (!chunk.empty()) {
        failed = failed || s.empty();
        char c0 = chunk[0];

        if (c0 == '[') {
            char r = 0;
            if (!failed) {
                r = s[0];
                s = s.substr(1);
            }
            chunk = chunk.substr(1);

            bool negated = false;
            if (!chunk.empty() && chunk[0] == '^') {
                negated = true;
                chunk   = chunk.substr(1);
            }

            bool        match  = false;
            int         nrange = 0;
            for (;;) {
                if (!chunk.empty() && chunk[0] == ']' && nrange > 0) {
                    chunk = chunk.substr(1);
                    break;
                }
                char lo = 0, hi = 0;
                if (!get_esc(chunk, lo)) return {"", false, true};
                hi = lo;
                if (!chunk.empty() && chunk[0] == '-') {
                    chunk = chunk.substr(1);
                    if (!get_esc(chunk, hi)) return {"", false, true};
                }
                if (lo <= r && r <= hi) match = true;
                nrange++;
            }
            failed = failed || (match == negated);

        } else if (c0 == '?') {
            if (!failed) {
                failed = s[0] == '/';
                s      = s.substr(1);
            }
            chunk = chunk.substr(1);

        } else if (c0 == '\\') {
            chunk = chunk.substr(1);
            if (chunk.empty()) return {"", false, true};
            if (!failed) {
                failed = chunk[0] != s[0];
                s      = s.substr(1);
            }
            chunk = chunk.substr(1);

        } else {
            if (!failed) {
                failed = c0 != s[0];
                s      = s.substr(1);
            }
            chunk = chunk.substr(1);
        }
    }
    if (failed) return {"", false, false};
    return {s, true, false};
}

struct MatchOutcome {
    bool matched;
    bool malformed;
};

// glob_match reports whether name matches pattern, per Go's path.Match
// semantics. malformed=true signals a syntax error in pattern (Go's
// ErrBadPattern) — the caller (AccessPolicy) treats such rules as
// non-matching and skips them.
MatchOutcome glob_match(std::string pattern, std::string name) {
    while (!pattern.empty()) {
        auto sc = scan_chunk(pattern);
        bool star = sc.star;
        std::string chunk = std::move(sc.chunk);
        pattern = std::move(sc.rest);

        if (star && chunk.empty()) {
            // Trailing '*' matches the rest of name unless it has a '/'.
            return {name.find('/') == std::string::npos, false};
        }

        auto mc = match_chunk(chunk, name);
        if (mc.malformed) return {false, true};
        if (mc.ok && (mc.rest.empty() || !pattern.empty())) {
            name = std::move(mc.rest);
            continue;
        }

        bool restarted = false;
        if (star) {
            for (std::size_t i = 0; i < name.size() && name[i] != '/'; i++) {
                auto mc2 = match_chunk(chunk, name.substr(i + 1));
                if (mc2.malformed) return {false, true};
                if (mc2.ok) {
                    if (pattern.empty() && !mc2.rest.empty()) {
                        continue;
                    }
                    name      = std::move(mc2.rest);
                    restarted = true;
                    break;
                }
            }
        }
        if (restarted) continue;

        // Before returning false with no error, check that the remainder
        // of the pattern is syntactically valid.
        std::string p = pattern;
        while (!p.empty()) {
            auto sc2 = scan_chunk(p);
            p        = std::move(sc2.rest);
            auto mc3 = match_chunk(sc2.chunk, "");
            if (mc3.malformed) return {false, true};
        }
        return {false, false};
    }
    return {name.empty(), false};
}

} // namespace

bool AccessPolicy::allows(const std::string& topic, Permission perm) const {
    for (const auto& r : rules_) {
        auto res = glob_match(r.pattern, topic);
        if (res.malformed) continue; // malformed pattern — skip, per go-DDS
        if (res.matched) {
            return static_cast<uint8_t>(r.allow & perm) != 0;
        }
    }
    return false;
}

} // namespace dds::security
