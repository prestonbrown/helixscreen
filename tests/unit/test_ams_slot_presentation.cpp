// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_presentation.cpp
 * @brief The AMS empty-lane presentation rule (pure).
 *
 * A lane presents one of three ways, keyed on whether it is empty and whether
 * it kept an identity when it was ejected:
 *
 *   present              full-strength spool, material text
 *   ejected + assigned   spool KEPT but ghosted, label ghosted with it
 *   ejected + unassigned spool hidden, dashed placeholder, "Empty"
 *
 * This rule was hand-written twice — ui_ams_slot.cpp and ui_ams_mini_status.cpp
 * — and the two agreed only by convention. These tests cover the single pure
 * rule they now share. Each consumer's wiring to it is tested separately in
 * test_ams_slot_presentation_wiring.cpp.
 */

#include "ams_slot_presentation.h"

#include "../catch_amalgamated.hpp"

using helix::ui::resolve_slot_presentation;
using helix::ui::slot_has_retained_identity;
using helix::ui::SlotLabel;
using helix::ui::SlotPresentation;
using helix::ui::SPOOL_OPA_FULL;
using helix::ui::SPOOL_OPA_GHOST;

namespace {

/// Every value SlotStatus can take, so "exhaustive" below is literally that.
/// name_of()'s switch has no default arm, so adding a status to the enum makes
/// the compiler point here rather than letting the new value go untested.
constexpr SlotStatus ALL_STATUSES[] = {
    SlotStatus::UNKNOWN, SlotStatus::EMPTY,   SlotStatus::AVAILABLE,
    SlotStatus::LOADED,  SlotStatus::BLOCKED, SlotStatus::FROM_BUFFER,
};

const char* name_of(SlotStatus s) {
    switch (s) {
    case SlotStatus::UNKNOWN:
        return "UNKNOWN";
    case SlotStatus::EMPTY:
        return "EMPTY";
    case SlotStatus::AVAILABLE:
        return "AVAILABLE";
    case SlotStatus::LOADED:
        return "LOADED";
    case SlotStatus::BLOCKED:
        return "BLOCKED";
    case SlotStatus::FROM_BUFFER:
        return "FROM_BUFFER";
    }
    return "?";
}

} // namespace

// ============================================================================
// The rule is pure — provable at compile time
// ============================================================================

// If any of these stop compiling, the function grew a runtime dependency and is
// no longer the testable-without-a-display rule this file is about.
static_assert(resolve_slot_presentation(SlotStatus::LOADED, false).show_spool);
static_assert(resolve_slot_presentation(SlotStatus::EMPTY, true).spool_opa == SPOOL_OPA_GHOST);
static_assert(resolve_slot_presentation(SlotStatus::EMPTY, false).label == SlotLabel::Empty);

// ============================================================================
// Exhaustive table: 6 statuses x 2 identity states
// ============================================================================

TEST_CASE("Presentation rule is exhaustively pinned", "[ams][slot][presentation]") {
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            INFO("status=" << name_of(status) << " retained_identity=" << identity);
            const SlotPresentation p = resolve_slot_presentation(status, identity);

            if (status != SlotStatus::EMPTY) {
                // A lane that is not empty renders normally whatever its
                // identity says — the identity only matters once it is ejected.
                CHECK(p.spool_opa == SPOOL_OPA_FULL);
                CHECK(p.show_spool);
                CHECK_FALSE(p.show_placeholder);
                CHECK(p.label == SlotLabel::Material);
            } else if (identity) {
                // Ejected but still assigned: keep the spool, dim it.
                CHECK(p.spool_opa == SPOOL_OPA_GHOST);
                CHECK(p.show_spool);
                CHECK_FALSE(p.show_placeholder);
                CHECK(p.label == SlotLabel::Material);
            } else {
                // Ejected and unassigned: no spool at all, name the lane's
                // purpose. Nothing is dimmed — the spool is gone, not faded.
                CHECK(p.spool_opa == SPOOL_OPA_FULL);
                CHECK_FALSE(p.show_spool);
                CHECK(p.show_placeholder);
                CHECK(p.label == SlotLabel::Empty);
            }
        }
    }
}

TEST_CASE("Spool and placeholder are never both shown or both hidden",
          "[ams][slot][presentation]") {
    // They occupy the same cell. Showing both draws a spool on top of a dashed
    // circle; showing neither leaves a hole where the lane should be.
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            INFO("status=" << name_of(status) << " retained_identity=" << identity);
            const SlotPresentation p = resolve_slot_presentation(status, identity);
            CHECK(p.show_spool != p.show_placeholder);
        }
    }
}

TEST_CASE("Ghosting happens on exactly one input pair", "[ams][slot][presentation]") {
    // The ghost strength is the whole "assigned, not present" signal (#1065).
    // If any other pair starts producing it, a loaded lane is reading as
    // ejected somewhere.
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            INFO("status=" << name_of(status) << " retained_identity=" << identity);
            const bool expect_ghost = (status == SlotStatus::EMPTY && identity);
            CHECK((resolve_slot_presentation(status, identity).spool_opa == SPOOL_OPA_GHOST) ==
                  expect_ghost);
        }
    }
}

TEST_CASE("Only EMPTY can produce the Empty label", "[ams][slot][presentation]") {
    for (SlotStatus status : ALL_STATUSES) {
        for (bool identity : {false, true}) {
            INFO("status=" << name_of(status) << " retained_identity=" << identity);
            const bool expect_empty_label = (status == SlotStatus::EMPTY && !identity);
            CHECK((resolve_slot_presentation(status, identity).label == SlotLabel::Empty) ==
                  expect_empty_label);
        }
    }
}

// ============================================================================
// UNKNOWN is not EMPTY
// ============================================================================

TEST_CASE("UNKNOWN renders as a normal lane, not an empty one",
          "[ams][slot][presentation][unknown]") {
    // SlotInfo::is_present() is false for BOTH EMPTY and UNKNOWN. A consumer
    // that keys the empty presentation on !is_present() therefore labels an
    // unanswered lane "Empty" — which is what ui_ams_mini_status.cpp did before
    // this rule was extracted. UNKNOWN means "this backend publishes no
    // presence signal", the same distinction slot_presence() in
    // filament_op_slot_resolver.h exists to preserve.
    //
    // This is live, not theoretical: QIDI, Snapmaker, AFC, Happy Hare and ACE
    // all publish UNKNOWN, and AmsState inits every per-slot status subject to
    // it, so every lane is UNKNOWN before the first sync lands.
    for (bool identity : {false, true}) {
        INFO("retained_identity=" << identity);
        const SlotPresentation p = resolve_slot_presentation(SlotStatus::UNKNOWN, identity);
        CHECK(p.label == SlotLabel::Material);
        CHECK(p.show_spool);
        CHECK_FALSE(p.show_placeholder);
        CHECK(p.spool_opa == SPOOL_OPA_FULL);
    }

    // Stated as the direct contrast, because collapsing these two is the bug.
    const SlotPresentation unknown = resolve_slot_presentation(SlotStatus::UNKNOWN, false);
    const SlotPresentation empty = resolve_slot_presentation(SlotStatus::EMPTY, false);
    CHECK(unknown.label != empty.label);
    CHECK(unknown.show_spool != empty.show_spool);
}

// ============================================================================
// The retained-identity predicate
// ============================================================================

TEST_CASE("Any one identity field retains the lane", "[ams][slot][presentation][identity]") {
    // Four independent handles, any of which means "assigned". Brand and
    // spool_name carry IFS-style backends where a user override exists with no
    // Spoolman ID, so dropping one silently un-ghosts those lanes.
    SECTION("nothing set") {
        SlotInfo slot;
        CHECK_FALSE(slot_has_retained_identity(slot));
    }
    SECTION("spoolman_id alone") {
        SlotInfo slot;
        slot.spoolman_id = 7;
        CHECK(slot_has_retained_identity(slot));
    }
    SECTION("material alone") {
        SlotInfo slot;
        slot.material = "PLA";
        CHECK(slot_has_retained_identity(slot));
    }
    SECTION("brand alone") {
        SlotInfo slot;
        slot.brand = "Polymaker";
        CHECK(slot_has_retained_identity(slot));
    }
    SECTION("spool_name alone") {
        SlotInfo slot;
        slot.spool_name = "Galaxy Black #3";
        CHECK(slot_has_retained_identity(slot));
    }
}

TEST_CASE("Identity fields are not truthy when empty or zero",
          "[ams][slot][presentation][identity]") {
    SlotInfo slot;
    slot.spoolman_id = 0; // 0 = not tracked, not "spool zero"
    slot.material = "";
    slot.brand = "";
    slot.spool_name = "";
    CHECK_FALSE(slot_has_retained_identity(slot));

    // A negative id is not a handle either — some backends use -1 for unset.
    slot.spoolman_id = -1;
    CHECK_FALSE(slot_has_retained_identity(slot));
}

TEST_CASE("An ejected lane keeps its identity through the rule",
          "[ams][slot][presentation][identity]") {
    // The end-to-end shape of #1071: eject does NOT clear the override, so the
    // lane must ghost rather than blank.
    SlotInfo slot;
    slot.material = "PETG";
    slot.brand = "eSUN";
    slot.status = SlotStatus::LOADED;

    const SlotPresentation loaded =
        resolve_slot_presentation(slot.status, slot_has_retained_identity(slot));
    CHECK(loaded.spool_opa == SPOOL_OPA_FULL);
    CHECK(loaded.label == SlotLabel::Material);

    slot.status = SlotStatus::EMPTY; // ejected; material/brand deliberately kept
    const SlotPresentation ejected =
        resolve_slot_presentation(slot.status, slot_has_retained_identity(slot));
    CHECK(ejected.spool_opa == SPOOL_OPA_GHOST);
    CHECK(ejected.show_spool);
    CHECK(ejected.label == SlotLabel::Material);

    // Unlinking Spoolman clears the handles and spool_name but deliberately
    // KEEPS material/brand, so the lane is still assigned and still ghosts.
    slot.clear_spoolman_link();
    CHECK(slot_has_retained_identity(slot));
    CHECK(resolve_slot_presentation(slot.status, slot_has_retained_identity(slot)).spool_opa ==
          SPOOL_OPA_GHOST);
}
