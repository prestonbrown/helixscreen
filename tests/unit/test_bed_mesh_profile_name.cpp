// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_profile_name.cpp
 * @brief #1360 - an empty profile-name field must not silently mean "default".
 *
 * The calibrate, save and rename modals each read their textarea back and
 * substituted the literal string "default" when it came back empty:
 *
 *   std::string profile_name = "default";
 *   if (input) { const char* t = lv_textarea_get_text(input);
 *                if (t && std::strlen(t) > 0) profile_name = t; }
 *
 * The field carries a placeholder, not default text, so an untouched field
 * reads back empty and the fallback won. A user who calibrated and tapped Save
 * without typing replaced their stored `default` mesh, with no confirmation and
 * nothing in the toast naming what had been written. Field evidence in the
 * issue: two byte-identical point grids staged 71 seconds apart, `default` then
 * `test`, from a reporter who believed he had saved one profile.
 *
 * `strlen > 0` also let a field holding only spaces through, so a profile could
 * be stored under the name "  ".
 *
 * These pin the decision helper the three call sites now share. The Empty
 * verdict is the regression: if the "default" substitution ever comes back,
 * the first two cases go red.
 */

#include "bed_mesh_profile_name.h"

#include "../catch_amalgamated.hpp"

using helix::ui::bed_mesh::check_profile_name;
using helix::ui::bed_mesh::ProfileNameVerdict;

namespace {
const std::vector<std::string> kStored = {"default", "cold", "PEI Sheet"};
}

TEST_CASE("An untouched field is rejected, not turned into 'default'",
          "[bed_mesh][profile_name][1360]") {
    const auto check = check_profile_name("", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::Empty);
    // The old code returned "default" here. Nothing may be saved under a name
    // the user did not type.
    REQUIRE(check.name.empty());
}

TEST_CASE("A field holding only whitespace is rejected", "[bed_mesh][profile_name][1360]") {
    for (const char* raw : {" ", "   ", "\t", "\n", " \t \n "}) {
        const auto check = check_profile_name(raw, kStored);
        INFO("raw = '" << raw << "'");
        REQUIRE(check.verdict == ProfileNameVerdict::Empty);
        REQUIRE(check.name.empty());
    }
}

TEST_CASE("An unused name is new and needs no confirmation", "[bed_mesh][profile_name][1360]") {
    const auto check = check_profile_name("test", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::New);
    REQUIRE(check.name == "test");
}

TEST_CASE("A stored name is an overwrite", "[bed_mesh][profile_name][1360]") {
    const auto check = check_profile_name("default", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::Overwrite);
    REQUIRE(check.name == "default");
}

TEST_CASE("Surrounding whitespace is trimmed before the name is used",
          "[bed_mesh][profile_name][1360]") {
    const auto check = check_profile_name("  test  ", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::New);
    // Trimmed, so BED_MESH_PROFILE SAVE= gets "test" and not "  test  ".
    REQUIRE(check.name == "test");
}

TEST_CASE("Trimming can reveal a clash with a stored profile", "[bed_mesh][profile_name][1360]") {
    // " default " would have been stored as a second, near-invisible profile.
    const auto check = check_profile_name(" default ", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::Overwrite);
    REQUIRE(check.name == "default");
}

TEST_CASE("Matching is case-sensitive, because Klipper's profile names are",
          "[bed_mesh][profile_name][1360]") {
    // SAVE=Default and SAVE=default are two profiles to Klipper. Calling this a
    // clash would refuse a save the printer would have accepted.
    const auto check = check_profile_name("Default", kStored);
    REQUIRE(check.verdict == ProfileNameVerdict::New);
    REQUIRE(check.name == "Default");
}

TEST_CASE("A name with interior spaces is kept whole", "[bed_mesh][profile_name][1360]") {
    const auto exists = check_profile_name("PEI Sheet", kStored);
    REQUIRE(exists.verdict == ProfileNameVerdict::Overwrite);
    REQUIRE(exists.name == "PEI Sheet");

    const auto fresh = check_profile_name("  Textured PEI  ", kStored);
    REQUIRE(fresh.verdict == ProfileNameVerdict::New);
    REQUIRE(fresh.name == "Textured PEI");
}

TEST_CASE("With no profiles stored, any name is new", "[bed_mesh][profile_name][1360]") {
    const std::vector<std::string> none;
    const auto check = check_profile_name("default", none);
    // Nothing to overwrite on a printer that has never saved a mesh, so the
    // save must not stop to ask.
    REQUIRE(check.verdict == ProfileNameVerdict::New);
    REQUIRE(check.name == "default");

    const auto empty = check_profile_name("", none);
    REQUIRE(empty.verdict == ProfileNameVerdict::Empty);
}
