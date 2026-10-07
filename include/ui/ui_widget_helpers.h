// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file ui_widget_helpers.h
 * @brief Named-widget lookups that report a breached XML contract, and small widget helpers
 */

#pragma once

#include <spdlog/spdlog.h>

// Include LVGL for lv_obj_t and lv_obj_find_by_name
// This ensures we get the correct type declarations
#include "lvgl/lvgl.h"

namespace helix::ui {

/**
 * @brief Make a breached UI contract fatal
 *
 * On in unit tests and under --test: a breach prints to stderr and aborts, so
 * the run fails where the XML and the C++ disagree. Off (the default) it only
 * logs. Release builds never abort.
 */
void set_strict_ui_checks(bool enabled) noexcept;

/// Whether set_strict_ui_checks() turned breaches fatal.
[[nodiscard]] bool strict_ui_checks() noexcept;

/// Log @p message at error, then abort when strict UI checks are on.
void report_ui_contract_breach(const char* message);

/**
 * @brief A widget the component's XML must contain
 *
 * A missing name logs once per (owner, name) at error and returns nullptr; with
 * strict UI checks on it aborts. A null @p root returns nullptr silently, so a
 * lookup nested under a failed one does not report twice.
 * scripts/check_required_names.py checks each literal @p name against every
 * layout variant of the component the calling file creates.
 */
lv_obj_t* find_required(lv_obj_t* root, const char* name, const char* owner);

/// A widget that may legitimately be absent (inside <if>, omitted by a layout
/// variant, or in plugin-supplied XML). Never reports.
inline lv_obj_t* find_optional(lv_obj_t* root, const char* name) {
    return root ? lv_obj_find_by_name(root, name) : nullptr;
}

} // namespace helix::ui

/**
 * @brief Toggle a widget's enabled state with visual feedback
 *
 * Adds/removes LV_STATE_DISABLED and sets main-part opacity to LV_OPA_COVER
 * when enabled or LV_OPA_50 when disabled — the conventional "button greyed
 * out" appearance used across the UI.
 *
 * Safe to call with a null widget (no-op).
 */
inline void ui_set_button_enabled(lv_obj_t* btn, bool enabled) {
    if (!btn)
        return;
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_50, LV_PART_MAIN);
    }
}
