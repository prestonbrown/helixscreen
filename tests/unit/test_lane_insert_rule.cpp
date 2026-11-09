// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether the spool just inserted into a lane is the one that was there.

#include "lane_binding.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// classify_insert (prestonbrown/helixscreen#1710)
// ============================================================================

namespace {

using helix::ams::classify_insert;
using helix::ams::InsertVerdict;
using helix::ams::SpoolEvidence;

SpoolEvidence tag(std::string uid, std::string material = "", std::optional<uint32_t> rgb = {}) {
    return SpoolEvidence{std::move(uid), std::move(material), rgb};
}

SpoolEvidence read(std::string material, std::optional<uint32_t> rgb) {
    return SpoolEvidence{"", std::move(material), rgb};
}

} // namespace

TEST_CASE("an insert with no reading of the spool before it has no evidence",
          "[lane][insert_rule]") {
    CHECK(classify_insert(std::nullopt, tag("04A1")) == InsertVerdict::NoEvidence);
    CHECK(classify_insert(std::nullopt, read("PLA", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("an untagged insert that reads nothing has no evidence", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), SpoolEvidence{}) == InsertVerdict::NoEvidence);
    CHECK(classify_insert(SpoolEvidence{}, SpoolEvidence{}) == InsertVerdict::NoEvidence);
}

TEST_CASE("a tag UID on both sides decides alone", "[lane][insert_rule]") {
    SECTION("the same tag is the same spool even with its contents rewritten") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), tag("04A1", "PETG", 0x0000FF)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("a new tag is a new spool even with identical contents") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), tag("04B2", "PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

TEST_CASE("a tag on one side only falls through to material and colour", "[lane][insert_rule]") {
    CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    CHECK(classify_insert(read("PLA", 0xFF0000), tag("04A1", "PETG", 0xFF0000)) ==
          InsertVerdict::DifferentSpool);
    CHECK(classify_insert(tag("04A1"), read("PLA", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("identical material and colour are the same spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    SECTION("material compares without case") {
        CHECK(classify_insert(read("petg", 0x00FF00), read("PETG", 0x00FF00)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("colour compares on RGB, ignoring any alpha byte") {
        CHECK(classify_insert(read("PLA", 0xFF00FF00u), read("PLA", 0x0000FF00u)) ==
              InsertVerdict::SameSpool);
    }
}

TEST_CASE("any field read on both sides that differs is a different spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PETG", 0xFF0000)) ==
          InsertVerdict::DifferentSpool);
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA", 0xFF0001)) ==
          InsertVerdict::DifferentSpool);
    SECTION("one differing field is enough when the other went unread") {
        CHECK(classify_insert(read("PLA", std::nullopt), read("PETG", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
        CHECK(classify_insert(read("", 0xFF0000), read("PLA", 0x00FF00)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("a similar material family is still a different spool") {
        CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA-CF", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

namespace {

SpoolEvidence untagged_read(std::string material, std::optional<uint32_t> rgb) {
    SpoolEvidence e{"", std::move(material), rgb};
    e.tag_read_complete = true;
    return e;
}

} // namespace

TEST_CASE("a finished tag read decides when a tag appears or disappears", "[lane][insert_rule]") {
    SECTION("a tagged spool replacing an untagged one is a new spool") {
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), tag("04A1", "PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("an untagged spool replacing a tagged one is a new spool") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), untagged_read("PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("a read not yet finished falls through to material and colour") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
        CHECK(classify_insert(read("PLA", 0xFF0000), tag("04A1", "PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("two finished reads with no tag fall through to material and colour") {
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), untagged_read("PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), untagged_read("ABS", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

TEST_CASE("material ignores surrounding whitespace", "[lane][insert_rule]") {
    CHECK(classify_insert(read(" PLA ", 0xFF0000), read("pla", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    CHECK(classify_insert(read("  ", 0xFF0000), read("", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("one matching field alone is not the same spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", std::nullopt), read("PLA", 0xFF0000)) ==
          InsertVerdict::NoEvidence);
    CHECK(classify_insert(read("", 0xFF0000), read("", 0xFF0000)) == InsertVerdict::NoEvidence);
}
