// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_input_shaper.h"

namespace helix {
namespace ui {

// Test-only reach into InputShaperPanel's private members, so tests can drive
// the real implementation without building the panel's XML UI or widening the
// production API. Follows the existing TestAccess pattern (tests/test_helpers/,
// [L088]).
struct InputShaperPanelTestAccess {
    // The expected-restart flow tests drive the real SAVE_CONFIG initiation.
    static void save_configuration(InputShaperPanel& p) {
        p.save_configuration();
    }

    // Current-config display: populates the is_current_* / is_shaper_configured
    // subjects the header card binds to.
    static void populate_current_config(InputShaperPanel& p, const ::InputShaperConfig& config) {
        p.populate_current_config(config);
    }

    // Pure lookup tables behind the results cards. Static on the panel, so the
    // boundary cases can be pinned without a panel instance at all.
    static const char* shaper_explanation(const std::string& type) {
        return InputShaperPanel::get_shaper_explanation(type);
    }

    static int vibration_quality(float vibrations) {
        return InputShaperPanel::get_vibration_quality(vibrations);
    }

    static const char* quality_description(float vibrations) {
        return InputShaperPanel::get_quality_description(vibrations);
    }
};

} // namespace ui
} // namespace helix
