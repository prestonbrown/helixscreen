// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <cstdint>

namespace helix::ui {

/**
 * @brief What a slot's material label should read.
 *
 * The rule picks the *source*, not the string: "Empty" is UI copy and gets
 * translated at the call site, while a material name (PLA, PETG) never is.
 */
enum class SlotLabel {
    /// Draw the lane's material name, or "--" when it reports none.
    Material,
    /// Draw a translated "Empty" — the lane is unused, not merely unnamed.
    Empty,
};

/// Opacity the spool visual and its material label render at, as an LVGL
/// opacity value. Spelled out here rather than pulled from LVGL so the rule
/// stays testable without a display; each consumer static_asserts the match
/// against LV_OPA_COVER / LV_OPA_20, so the two cannot drift silently.
inline constexpr uint8_t SPOOL_OPA_FULL = 255;
inline constexpr uint8_t SPOOL_OPA_GHOST = 51;

/**
 * @brief How one AMS lane presents itself.
 *
 * @p show_spool and @p show_placeholder are complementary today; both are named
 * because they drive different widgets (the spool visual vs. the dashed empty
 * circle) and a consumer that conflated them would be reading the wrong one.
 */
struct SlotPresentation {
    /// Applied to the spool visual AND the material label, so the two read as
    /// one lane rather than a dimmed spool beside full-strength text.
    uint8_t spool_opa = SPOOL_OPA_FULL;
    bool show_spool = true;
    bool show_placeholder = false;
    SlotLabel label = SlotLabel::Material;
};

/**
 * @brief Does this lane still carry an identity after being ejected?
 *
 * Spoolman link, material, brand or spool name — the override is deliberately
 * NOT cleared on eject (#1071), so a lane that has one is "assigned, not
 * present" rather than genuinely unused.
 */
[[nodiscard]] inline bool slot_has_retained_identity(const SlotInfo& slot) {
    return slot.spoolman_id > 0 || !slot.material.empty() || !slot.brand.empty() ||
           !slot.spool_name.empty();
}

/**
 * @brief THE empty-lane presentation rule, for every surface that draws a lane.
 *
 * Three outcomes, keyed on whether the lane is empty and whether it kept an
 * identity when it was ejected:
 *
 *   present              full-strength spool, material text
 *   ejected + assigned   spool KEPT but ghosted, label ghosted with it
 *                        ("assigned, not present", #1065)
 *   ejected + unassigned spool hidden, dashed placeholder, "Empty"
 *
 * Consumers: the ams_slot widget (ui_ams_slot.cpp) and the mini status strip
 * the filament panel embeds (ui_ams_mini_status.cpp). Both hand-rolled this
 * rule and agreed only by convention; see the UNKNOWN note below for where they
 * had already drifted.
 *
 * @warning Only SlotStatus::EMPTY triggers the empty presentation. UNKNOWN
 * means "this backend publishes no presence signal for the lane", NOT "the lane
 * is empty" — the same distinction slot_presence() in filament_op_slot_resolver.h
 * exists to preserve. SlotInfo::is_present() collapses the two, so a consumer
 * that keys on !is_present() labels an unanswered lane "Empty". QIDI, Snapmaker,
 * AFC, Happy Hare and ACE all publish UNKNOWN, and AmsState inits every per-slot
 * status subject to it, so that is a live misread and not a theoretical one.
 *
 * Pure so the rule is testable without an LVGL display: the outcome depends on
 * nothing but these two inputs.
 *
 * @param status                 The lane's reported status.
 * @param has_retained_identity  slot_has_retained_identity() for this lane.
 */
[[nodiscard]] constexpr SlotPresentation resolve_slot_presentation(SlotStatus status,
                                                                   bool has_retained_identity) {
    if (status != SlotStatus::EMPTY) {
        return {SPOOL_OPA_FULL, /*show_spool=*/true, /*show_placeholder=*/false,
                SlotLabel::Material};
    }
    if (has_retained_identity) {
        // Ghosted, not hidden: the retained material still names what belongs
        // in this lane, it just is not there right now.
        return {SPOOL_OPA_GHOST, /*show_spool=*/true, /*show_placeholder=*/false,
                SlotLabel::Material};
    }
    // Nothing to dim — the spool is hidden outright, so the placeholder and its
    // "Empty" label render at full strength.
    return {SPOOL_OPA_FULL, /*show_spool=*/false, /*show_placeholder=*/true, SlotLabel::Empty};
}

} // namespace helix::ui
