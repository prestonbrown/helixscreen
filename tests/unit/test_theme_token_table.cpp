// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parity gate: the generated table must be indistinguishable from the live
// runtime scan over the real ui_xml/. A failure here means ui_xml tokens
// changed without regenerating — run: make regen-tokens
#include "theme_manager.h"
#include "theme_token_table.h"

#include "../catch_amalgamated.hpp"

static const char* TYPES[] = {"color", "px", "string"};
static const char* SUFFIXES[] = {"_light",  "_dark",  "_micro",  "_tiny",   "_small",
                                 "_medium", "_large", "_xlarge", "_xxlarge"};

TEST_CASE("token table matches runtime scan (full element maps)", "[theme][tokens]") {
    // Fail loudly if HELIX_TOKEN_TABLE leaked into the test environment: with
    // the table enabled, the "scanned" side below would hit the fast path too,
    // making the parity check vacuous (table compared against itself).
    REQUIRE_FALSE(helix::theme_tokens::enabled());
    for (const char* type : TYPES) {
        INFO("type=" << type << " — if this fails, run: make regen-tokens");
        auto scanned = theme_manager_parse_all_xml_for_element("ui_xml", type);
        auto table = helix::theme_tokens::for_element(type);
        REQUIRE(table == scanned);
    }
}

TEST_CASE("token table matches runtime scan (suffix maps)", "[theme][tokens]") {
    // Fail loudly if HELIX_TOKEN_TABLE leaked into the test environment: with
    // the table enabled, the "scanned" side below would hit the fast path too,
    // making the parity check vacuous (table compared against itself).
    REQUIRE_FALSE(helix::theme_tokens::enabled());
    for (const char* type : TYPES) {
        for (const char* suffix : SUFFIXES) {
            INFO("type=" << type << " suffix=" << suffix
                         << " — if this fails, run: make regen-tokens");
            auto scanned = theme_manager_parse_all_xml_for_suffix("ui_xml", type, suffix);
            auto table = helix::theme_tokens::for_suffix(type, suffix);
            REQUIRE(table == scanned);
        }
    }
}

TEST_CASE("uncovered token types still reach the live scanner", "[theme][tokens]") {
    // ui_xml defines <str>, <int> and <percentage> tokens that the generator
    // does not emit, so the table holds nothing for them and a scan is the only
    // source of an answer.
    for (const char* type : {"str", "int", "percentage"}) {
        INFO("type=" << type);
        REQUIRE_FALSE(helix::theme_tokens::covers(type));
        REQUIRE_FALSE(theme_manager_parse_all_xml_for_element("ui_xml", type).empty());
    }

    // The three the table does carry, so the fast path is reachable at all.
    for (const char* type : TYPES) {
        INFO("type=" << type);
        REQUIRE(helix::theme_tokens::covers(type));
    }
}

TEST_CASE("the table answers only covered types in the canonical dir", "[theme][tokens]") {
    // enabled() is false in every test build, so the aggregation guard's own
    // behaviour is only reachable through the pure predicate. Each term below
    // is a way the fast path could wrongly answer from the table.
    using helix::theme_tokens::answers_from_table;
    const char* canonical = "ui_xml";

    for (const char* type : TYPES) {
        INFO("type=" << type);
        REQUIRE(answers_from_table(true, type, canonical, canonical));
    }

    // Answering these from the table returns an empty map, not the ~300 tokens
    // ui_xml actually defines for them.
    for (const char* type : {"str", "int", "percentage"}) {
        INFO("type=" << type);
        REQUIRE_FALSE(answers_from_table(true, type, canonical, canonical));
    }

    // A caller naming another directory wants that directory scanned.
    REQUIRE_FALSE(answers_from_table(true, "color", "/tmp/some-other-dir", canonical));

    // Disabled scans regardless.
    REQUIRE_FALSE(answers_from_table(false, "color", canonical, canonical));

    // No input is trusted to be non-null.
    REQUIRE_FALSE(answers_from_table(true, nullptr, canonical, canonical));
    REQUIRE_FALSE(answers_from_table(true, "color", nullptr, canonical));
    REQUIRE_FALSE(answers_from_table(true, "color", canonical, nullptr));
}
