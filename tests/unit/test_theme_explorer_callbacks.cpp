// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_explorer_callbacks.cpp
 * @brief The theme explorer's preset dropdown keeps its own event callback
 *
 * XML event callbacks are one global name -> function table, and a dropdown
 * binds whatever the name resolves to when it is created. The explorer is
 * rebuilt every time it opens, so any later registration of its callback name
 * silently rewires the next explorer.
 */

#include "ui_settings_display_sound.h"
#include "ui_theme_editor_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ThemeExplorer: opening the editor does not rewire the preset dropdown",
                 "[theme][ui]") {
    helix::settings::get_display_sound_settings_overlay().register_callbacks();
    lv_event_cb_t explorer_cb = lv_xml_get_event_cb(nullptr, "on_theme_preset_changed");
    REQUIRE(explorer_cb != nullptr);

    // The editor registers its callbacks the first time Edit is pressed.
    get_theme_editor_overlay().register_callbacks();

    // A different handler here previews nothing and never enables Apply, so a
    // reopened explorer can no longer change theme.
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_theme_preset_changed") == explorer_cb);
}
