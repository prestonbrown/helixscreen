// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_mapper.h"
#include "preflight_validator.h"

#include "../catch_amalgamated.hpp"

using helix::AvailableSlot;
using helix::GcodeToolInfo;
using helix::PreflightValidator;
using helix::ToolMapping;
using Severity = helix::ToolCheck::Severity;

TEST_CASE("material mismatch is advisory, not block", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xED1C24, "PLA"}};
    std::vector<AvailableSlot> slots = {{0, 0, 0xED1C24, "PETG", false, -1, 0, 0, ""}};
    std::vector<ToolMapping> mapping = {{0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}};
    auto r = PreflightValidator::validate(tools, slots, mapping, false);
    CHECK(r.checks[0].severity == Severity::MaterialMismatch);
    CHECK_FALSE(r.checks[0].material_ok);
    CHECK(r.has_advisory());
    CHECK_FALSE(r.has_block());
}

TEST_CASE("color mismatch is display-only (no block, no advisory)", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {{0, 0x00C1AE, "PLA"}}; // green intended
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x2233FF, "PLA", false, -1, 0, 0, ""}}; // blue loaded
    std::vector<ToolMapping> mapping = {{0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}};
    auto r = PreflightValidator::validate(tools, slots, mapping, false);
    CHECK(r.checks[0].severity == Severity::ColorMismatch);
    CHECK_FALSE(r.checks[0].color_ok);
    CHECK_FALSE(r.has_advisory()); // color is NOT advisory — display-only
    CHECK_FALSE(r.has_block());
}

TEST_CASE("exact match is Ok", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xED1C24, "PLA"}};
    std::vector<AvailableSlot> slots = {{0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""}};
    std::vector<ToolMapping> mapping = {{0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}};
    auto r = PreflightValidator::validate(tools, slots, mapping, false);
    CHECK(r.checks[0].severity == Severity::Ok);
}

TEST_CASE("empty required slot is flagged and blocks", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xED1C24, "PLA"},
        {2, 0x00C1AE, "PLA"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""},
        {1, 0, 0x000000, "", true, -1, 0, 0, ""},
        {2, 0, 0x00C1AE, "PLA", false, -1, 0, 0, ""},
    };
    std::vector<ToolMapping> mapping = {
        {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO},
        {2, 1, 0, false, false, ToolMapping::MatchReason::AUTO}, // green mapped to EMPTY slot 1
    };
    auto result = PreflightValidator::validate(tools, slots, mapping, false);
    REQUIRE(result.checks.size() == 2);
    CHECK(result.checks[0].severity == Severity::Ok);
    CHECK(result.checks[1].severity == Severity::EmptySlot);
    CHECK(result.checks[1].tool_index == 2);
    CHECK(result.checks[1].mapped_slot == 1);
    CHECK_FALSE(result.checks[1].slot_present);
    CHECK(result.has_block());
    CHECK_FALSE(result.has_advisory());
}

TEST_CASE("tool with no mapping is treated as empty/block", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {{3, 0xFFFFFF, "PLA"}};
    std::vector<AvailableSlot> slots = {{0, 0, 0xFFFFFF, "PLA", false, -1, 0, 0, ""}};
    std::vector<ToolMapping> mapping = {}; // tool 3 unmapped
    auto r = PreflightValidator::validate(tools, slots, mapping, false);
    CHECK(r.checks[0].severity == Severity::EmptySlot);
    CHECK(r.checks[0].mapped_slot == -1);
    CHECK(r.has_block());
}

TEST_CASE("no tools yields empty, non-blocking result", "[preflight_validator]") {
    auto r = PreflightValidator::validate({}, {}, {}, false);
    CHECK(r.checks.empty());
    CHECK_FALSE(r.has_block());
    CHECK_FALSE(r.has_advisory());
}

// Regression: single-extruder printer with NO multi-material/AMS system (e.g. Ender 3 V3 SE).
// AmsState::collect_available_slots() emits one AvailableSlot per physical AMS slot regardless
// of its empty status, so an EMPTY slots vector uniquely means "no AMS hardware at all" — never
// "AMS present with empty slots." In that case there is no slot to map T0 to, filament presence
// is the physical runout sensor's job (surfaced separately), and pre-flight must NOT block.
// Without this guard the default mapping yields mapped_slot == -1 → EmptySlot → a false
// "T0 has no filament loaded" block even though filament is loaded and detected.
TEST_CASE("no AMS system (empty slots) does not block", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {{0, 0x2233FF, "PLA"}}; // slicer emitted a T0 color
    std::vector<AvailableSlot> slots = {};                     // no AMS backend → zero slots
    auto mapping = helix::FilamentMapper::compute_defaults(tools, slots); // yields slot -1 for T0
    auto r = PreflightValidator::validate(tools, slots, mapping, false);
    CHECK_FALSE(r.has_block());
    CHECK_FALSE(r.has_advisory());
}

// Regression: Snapmaker U1 toolchanger — 2-color print uses heads 0 and 2; head 1 is unloaded.
// Firmware auto-feeds all configured heads, but head 1 is not required by this print.
// Pre-flight must NOT block when an unrequired head is empty, and MUST block when a required
// head is empty. Identity mapping: tool_index N -> slot N (slot_index == physical head).
TEST_CASE("U1 toolchanger: empty non-required head does not block",
          "[preflight_validator][snapmaker]") {
    // Colors chosen so each present slot matches its tool exactly → only EmptySlot in play.
    // tool 0 = red, tool 2 = cyan; slots for unused heads (1, 3) are also present/loaded.
    std::vector<GcodeToolInfo> tools = {
        {0, 0xED1C24, "PLA"}, // physical head 0, red
        {2, 0x00C1AE, "PLA"}, // physical head 2, cyan
    };

    SECTION("head 1 empty but not required → no block") {
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""}, // head 0: loaded, red
            {1, 0, 0x000000, "", true, -1, 0, 0, ""},     // head 1: EMPTY (not required)
            {2, 0, 0x00C1AE, "PLA", false, -1, 0, 0, ""}, // head 2: loaded, cyan
            {3, 0, 0xFFFFFF, "PLA", false, -1, 0, 0, ""}, // head 3: loaded (not required)
        };
        std::vector<ToolMapping> mapping = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}, // tool 0 → slot 0
            {2, 2, 0, false, false, ToolMapping::MatchReason::AUTO}, // tool 2 → slot 2
        };
        auto result = PreflightValidator::validate(tools, slots, mapping, false);
        CHECK_FALSE(result.has_block());
        CHECK_FALSE(result.has_advisory());
        REQUIRE(result.checks.size() == 2);
        CHECK(result.checks[0].severity == Severity::Ok);
        CHECK(result.checks[1].severity == Severity::Ok);
    }

    SECTION("required head empty → block") {
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""}, // head 0: loaded, red
            {1, 0, 0xFFFFFF, "PLA", false, -1, 0, 0, ""}, // head 1: loaded (not required)
            {2, 0, 0x000000, "", true, -1, 0, 0, ""},     // head 2: EMPTY (required!)
            {3, 0, 0xFFFFFF, "PLA", false, -1, 0, 0, ""}, // head 3: loaded (not required)
        };
        std::vector<ToolMapping> mapping = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}, // tool 0 → slot 0
            {2, 2, 0, false, false, ToolMapping::MatchReason::AUTO}, // tool 2 → slot 2 (empty)
        };
        auto result = PreflightValidator::validate(tools, slots, mapping, false);
        CHECK(result.has_block());
        REQUIRE(result.checks.size() == 2);
        CHECK(result.checks[0].severity == Severity::Ok);
        CHECK(result.checks[1].severity == Severity::EmptySlot);
        CHECK(result.checks[1].tool_index == 2);
        CHECK(result.checks[1].mapped_slot == 2);
    }
}

// Regression: printing from the AFC hardware bypass. AFC's own rule is that a loaded
// bypass deactivates toolchanges entirely ("Filament loaded in bypass, toolchanges
// deactivated", AFC_prep.py) — the print is fed from the bypass spool, not from a lane.
// The gcode still says T0, so the mapping resolves T0 to a lane that is empty (or to
// nothing at all), and without this guard every bypass print is blocked by a
// "T0 has no filament loaded — this print will run out." modal that the user cannot
// clear by any configuration, because bypass is never a slot in
// AmsState::collect_available_slots().
TEST_CASE("bypass engaged: slot-based checks do not apply", "[preflight_validator][bypass]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xED1C24, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x000000, "", true, -1, 0, 0, ""},     // lane 0: EMPTY
        {1, 0, 0x00C1AE, "PETG", false, -1, 0, 0, ""} // lane 1: loaded, wrong material
    };

    SECTION("T0 mapped to an empty lane blocks when bypass is OFF") {
        std::vector<ToolMapping> mapping = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}}; // T0 -> empty lane 0
        auto r = PreflightValidator::validate(tools, slots, mapping, false);
        REQUIRE(r.checks.size() == 1);
        CHECK(r.checks[0].severity == Severity::EmptySlot);
        CHECK(r.has_block());
    }

    SECTION("same mapping does NOT block when bypass is ON") {
        std::vector<ToolMapping> mapping = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO}};
        auto r = PreflightValidator::validate(tools, slots, mapping, true);
        CHECK(r.checks.empty());
        CHECK_FALSE(r.has_block());
        CHECK_FALSE(r.has_advisory());
    }

    SECTION("unmapped T0 does not block when bypass is ON") {
        // The reporter's case: nothing maps T0, so mapped_slot == -1 and the modal takes
        // its "T%d has no filament loaded" branch.
        std::vector<ToolMapping> mapping = {};
        auto blocked = PreflightValidator::validate(tools, slots, mapping, false);
        REQUIRE(blocked.checks.size() == 1);
        CHECK(blocked.checks[0].mapped_slot == -1);
        CHECK(blocked.has_block()); // pins the bug being fixed

        auto r = PreflightValidator::validate(tools, slots, mapping, true);
        CHECK(r.checks.empty());
        CHECK_FALSE(r.has_block());
    }

    SECTION("material mismatch is also suppressed under bypass") {
        // The seated lane material is meaningless when the filament comes from the
        // bypass spool, so the advisory must go too, not just the block.
        std::vector<ToolMapping> mapping = {
            {0, 1, 0, false, false, ToolMapping::MatchReason::AUTO}}; // T0 -> PETG lane 1
        auto advisory = PreflightValidator::validate(tools, slots, mapping, false);
        CHECK(advisory.has_advisory());

        auto r = PreflightValidator::validate(tools, slots, mapping, true);
        CHECK(r.checks.empty());
        CHECK_FALSE(r.has_advisory());
    }
}

// Bypass must not suppress checks on a machine that simply has no AMS — that path is
// already covered by the slots.empty() early-out and must stay independent of bypass.
TEST_CASE("bypass off with no AMS still does not block", "[preflight_validator][bypass]") {
    std::vector<GcodeToolInfo> tools = {{0, 0x2233FF, "PLA"}};
    auto r = PreflightValidator::validate(tools, {}, {}, false);
    CHECK_FALSE(r.has_block());
}
