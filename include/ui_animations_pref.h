// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_animations_pref.h
 * @brief The user's "Animations" preference, for the UI code that has to stop moving
 *
 * Anything driven by an LV_ANIM_REPEAT_INFINITE — a scrolling label, a heater
 * icon pulse — invalidates its area on every step, so it repaints at the
 * display refresh rate for as long as it is on screen, on a printer host that
 * shares its CPU with Klipper. Turning animations off has to reach all of them,
 * so every consumer resolves the subject through here instead of spelling its
 * name out again.
 *
 * @threading Main thread only — the subject drives LVGL widget APIs from its
 *            observers.
 */

#pragma once

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

namespace helix::ui {

/// Name of the global subject carrying the user's "Animations" preference.
inline constexpr const char* ANIMATIONS_SUBJECT_NAME = "settings_animations_enabled";

/**
 * @brief The preference subject, or nullptr when it has not been registered yet.
 *
 * @param scope XML component scope to search. nullptr searches the global
 *              scope, where DisplaySettingsManager registers the subject; the
 *              XML parsers pass their component's scope instead.
 */
inline lv_subject_t* animations_pref_subject(lv_xml_component_scope_t* scope = nullptr) {
    return lv_xml_get_subject(scope, ANIMATIONS_SUBJECT_NAME);
}

/**
 * @brief Whether motion is allowed, given the preference subject.
 *
 * A null subject means the settings subjects are not built yet — widgets
 * created in that window animate, matching the platform default the manager
 * will publish a moment later.
 */
inline bool animations_enabled(lv_subject_t* subject) {
    return subject == nullptr || lv_subject_get_int(subject) != 0;
}

} // namespace helix::ui
