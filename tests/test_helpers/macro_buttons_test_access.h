// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_settings_macro_buttons.h"

/**
 * Reaches MacroButtonsOverlay::quick_button_index_to_slot_name(), which is a
 * private static. Declared a friend in ui_settings_macro_buttons.h, following
 * the existing TestAccess pattern (tests/test_helpers/) rather than widening
 * the production API for a test.
 *
 * Worth reaching directly: the function is pure and depends only on
 * StandardMacros::instance().all(), whose table is filled by that singleton's
 * constructor. So it needs neither init() nor LVGL, and testing it through the
 * dropdown would drag in a whole overlay to observe one index mapping.
 */
class MacroButtonsOverlayTestAccess {
  public:
    static std::string quick_button_index_to_slot_name(int index) {
        return helix::settings::MacroButtonsOverlay::quick_button_index_to_slot_name(index);
    }
};
