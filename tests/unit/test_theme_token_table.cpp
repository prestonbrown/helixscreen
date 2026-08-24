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
