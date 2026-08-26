// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_input_shaper.h"

#include <string>
#include <utility>
#include <vector>

// Friend access to InputShaperPanel internals. `ui_panel_input_shaper.h`
// declares `friend class InputShaperPanelTestAccess;` and InputShaperPanel
// lives in the GLOBAL namespace, so this definition must too - and in ONE
// place, since two test translation units each defining their own version
// would be an ODR violation.
//
// save_configuration() is the panel's only path that rewrites the user's
// printer config, and everything it needs comes from private state that
// normally only a full calibration run produces:
//
//  - save_restart_timeout_ms_ is the budget safe_multi_edit()'s health monitor
//    polls for after FIRMWARE_RESTART. At the production 30s a test would sit
//    there for half a minute and then call back into a destroyed fixture.
//  - x_result_ / y_result_ hold the console-side recommendation and the full
//    fitted-shaper list.
//  - x_chart_/y_chart_'s shaper_curves + selected_shaper are the CSV side and
//    the chip the user tapped. Selection resolution joins the two by name, so
//    seeding only one half cannot exercise it.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API - those are lint-gated
// out of include/ and src/ by tests/shell/test_code_lint.bats.
class InputShaperPanelTestAccess {
  public:
    /// Shorten the post-FIRMWARE_RESTART health-monitor window so a test
    /// reaches the terminal callback in about a second instead of 30.
    static void set_save_restart_timeout_ms(InputShaperPanel& p, uint32_t ms) {
        p.save_restart_timeout_ms_ = ms;
    }

    /// Seed one axis exactly as a finished calibration would leave it: the
    /// console result, the CSV curves the chips are built from, and which chip
    /// is active (-1 = none, so resolution falls back to the recommendation).
    static void seed_axis(InputShaperPanel& p, char axis, const InputShaperResult& result,
                          std::vector<ShaperResponseCurve> curves, int selected_shaper) {
        InputShaperPanel::AxisChartData& chart = (axis == 'X') ? p.x_chart_ : p.y_chart_;
        InputShaperResult& stored = (axis == 'X') ? p.x_result_ : p.y_result_;

        stored = result;
        chart.shaper_curves = std::move(curves);
        chart.selected_shaper = selected_shaper;
    }

    /// Drop both axes back to "nothing calibrated". The panel is a process-wide
    /// singleton, so a case that does not reset leaks its seed into the next.
    static void clear_results(InputShaperPanel& p) {
        p.x_result_ = InputShaperResult{};
        p.y_result_ = InputShaperResult{};
        p.x_chart_.shaper_curves.clear();
        p.x_chart_.selected_shaper = -1;
        p.y_chart_.shaper_curves.clear();
        p.y_chart_.selected_shaper = -1;
    }

    /// The private entry point under test. handle_save_clicked() reaches it
    /// through a widget event and then immediately tears the panel down, which
    /// would hide everything the async chain does afterwards.
    static void save(InputShaperPanel& p) {
        p.save_configuration();
    }
};

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
