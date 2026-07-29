// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// REQ-SAFETY-004 ("every source/test file shall carry fusa:req/fusa:test
// annotations linking to requirement identifiers; the CI fusa-asil-b job
// shall validate that all requirements are both implemented and tested")
// is self-referential: its real verification IS the fusa-asil-b CI gate
// (`cpfusa trace --req-coverage 100 --sec-tested 96`, .github/workflows/
// ci.yml), not something a single Catch2 TEST_CASE can meaningfully
// re-verify (see SoundMatt/cpp-DDS#52). What a unit test *can* usefully
// assert is the one precondition that CI gate silently depends on: that
// requirements/requirements.json — the registry `cpfusa trace` is loaded
// from (see the "Load real requirements registry" CI step) — itself stays
// well-formed and non-empty, so a corrupted or accidentally-emptied
// registry fails loudly here rather than letting the CI gate silently
// compute coverage over nothing (exactly the failure mode #43 fixed).
//
// fusa:test REQ-SAFETY-004

#include "json_lite.hpp" // cli::json — the CLI's minimal JSON parser (cli/json_lite.hpp)

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <set>
#include <sstream>

#ifndef CPPDDS_REQUIREMENTS_JSON
#error "CPPDDS_REQUIREMENTS_JSON must be defined by tests/CMakeLists.txt"
#endif

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

} // namespace

TEST_CASE("requirements registry is well-formed and non-empty",
          "[requirements][REQ-SAFETY-004]") {
    std::string text = read_file(CPPDDS_REQUIREMENTS_JSON);
    REQUIRE_FALSE(text.empty());

    cli::json::Value doc = cli::json::parse(text);
    REQUIRE(doc.is_array());
    const auto& reqs = doc.as_array();
    REQUIRE(reqs.size() > 0);

    std::set<std::string> seen_ids;
    for (const auto& entry : reqs) {
        REQUIRE(entry.is_object());

        const auto* id = entry.find("id");
        REQUIRE(id != nullptr);
        REQUIRE(id->is_string());
        REQUIRE_FALSE(id->as_string().empty());
        // Every requirement ID must be unique — a duplicate would make
        // cpfusa trace's per-ID annotation/test coverage counters silently
        // undercount (two source annotations for one nominal ID, one of
        // which is actually orphaned).
        auto [it, inserted] = seen_ids.insert(id->as_string());
        (void)it;
        REQUIRE(inserted);

        const auto* title = entry.find("title");
        REQUIRE(title != nullptr);
        REQUIRE(title->is_string());
        REQUIRE_FALSE(title->as_string().empty());

        const auto* text_field = entry.find("text");
        REQUIRE(text_field != nullptr);
        REQUIRE(text_field->is_string());
        REQUIRE_FALSE(text_field->as_string().empty());
    }
}

TEST_CASE("requirements registry includes REQ-SAFETY-004 itself",
          "[requirements][REQ-SAFETY-004]") {
    // A minimal sanity check that the registry hasn't drifted out from
    // under this very test: REQ-SAFETY-004 must still be a real entry.
    std::string text = read_file(CPPDDS_REQUIREMENTS_JSON);
    cli::json::Value doc = cli::json::parse(text);
    bool found = false;
    for (const auto& entry : doc.as_array()) {
        const auto* id = entry.find("id");
        if (id && id->is_string() && id->as_string() == "REQ-SAFETY-004") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}
