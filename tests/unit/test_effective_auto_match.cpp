// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_effective_auto_match.cpp
 * @brief The auto-match rule both print surfaces resolve their lane colors with.
 *
 * "Effective auto match" is the toggle-aware answer the print-select detail view
 * and the print-status panel both hand to FilamentMapper — the detail view for
 * effective_mappings() (swatches + the pre-flight gate), the status panel for
 * effective_tool_colors() (the gcode viewer's per-tool colors). It is the same
 * question asked twice:
 *
 *   - A backend whose tool-mapping card is NOT editable (Snapmaker U1, ACE) has
 *     no UI anywhere that can flip the persisted auto-color preference, so it
 *     must always auto-match on color+type. Honoring the persisted default
 *     (FALSE) there falls back to positional matching and picks the wrong lane.
 *   - An editable backend has that UI, so the user's setting wins both ways.
 *
 * Neither call site had a test. The two implementations were byte-identical
 * bodies in different files, so a change to the rule in one silently left the
 * other on the old behaviour — the detail view's swatches would promise a lane
 * the status panel then colored from a different one.
 *
 * These drive the two production entry points directly (not a re-implementation
 * of the rule), so they keep pinning the behaviour after the bodies are shared.
 */

#include "ui_panel_print_status.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "settings_manager.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Installs an AMS backend with a chosen tool-mapping editability and restores
/// both process singletons it touches on the way out. AmsState's backend is NOT
/// cleared by HelixTestFixture::reset_all(), and the auto-color-map setting is
/// persisted, so a test that leaves either set leaks into every later test.
struct AutoMatchFixture : public LVGLTestFixture {
    AutoMatchFixture() {
        // get_auto_color_map() reads an lv_subject, and set_auto_color_map()
        // writes one — so WITHOUT this the setter silently does not stick and
        // every case below would read a constant 0 while appearing to drive the
        // setting. Idempotent (early-returns on subjects_initialized_), and the
        // same call test_afc_spool_reassert.cpp makes for the same reason.
        helix::SettingsManager::instance().init_subjects();
        prev_auto_color_ = helix::SettingsManager::instance().get_auto_color_map();

        // set_backend() publishes into AmsState's subjects; without this they
        // are uninitialized. register_xml=false keeps the names out of the
        // process-wide XML scope.
        AmsState::instance().init_subjects(true);
    }

    ~AutoMatchFixture() override {
        AmsState::instance().set_backend(nullptr);
        helix::SettingsManager::instance().set_auto_color_map(prev_auto_color_);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// @param editable  what get_tool_mapping_capabilities().editable must report.
    ///                  Snapmaker mode is the mock's non-editable shape ({true,
    ///                  false}) — the real U1/ACE report the same.
    static void install_backend(bool editable) {
        auto mock = std::make_unique<AmsBackendMock>(4);
        mock->set_snapmaker_mode(!editable);
        auto* raw = mock.get();
        AmsState::instance().set_backend(std::move(mock));
        // Load-bearing: if the mock ever stopped modelling the two shapes, every
        // assertion below would still pass while testing only one branch.
        REQUIRE(raw->get_tool_mapping_capabilities().editable == editable);
        REQUIRE(AmsState::instance().get_backend() == raw);
    }

    static void set_auto_color_map(bool on) {
        helix::SettingsManager::instance().set_auto_color_map(on);
        REQUIRE(helix::SettingsManager::instance().get_auto_color_map() == on);
    }

    bool prev_auto_color_ = false;
};

} // namespace

// ============================================================================
// PrintSelectDetailView — src/ui/ui_print_select_detail_view.cpp
// Consumed by effective_mappings() (swatches + pre-flight gate).
// ============================================================================

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: non-editable backend always "
                 "auto-matches",
                 "[auto_match][print_select][detail_view][ams]") {
    helix::ui::PrintSelectDetailView view;
    install_backend(/*editable=*/false);

    // The persisted default is FALSE, which is the case that picked the wrong
    // lane on a U1 before the rule existed.
    set_auto_color_map(false);
    CHECK(view.effective_auto_match());

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: editable backend honors the setting",
                 "[auto_match][print_select][detail_view][ams]") {
    helix::ui::PrintSelectDetailView view;
    install_backend(/*editable=*/true);

    set_auto_color_map(false);
    CHECK_FALSE(view.effective_auto_match());

    set_auto_color_map(true);
    CHECK(view.effective_auto_match());
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintSelectDetailView::effective_auto_match: no backend auto-matches",
                 "[auto_match][print_select][detail_view][ams]") {
    // No AMS at all is the third branch: card_editable stays false, so the
    // setting is not consulted. A single-extruder printer never reaches the
    // mapper anyway, but the predicate must not depend on that.
    AmsState::instance().set_backend(nullptr);
    helix::ui::PrintSelectDetailView view;

    set_auto_color_map(false);
    CHECK(view.effective_auto_match());
}

// ============================================================================
// PrintStatusPanel — src/ui/ui_panel_print_status.cpp
// Consumed by build_and_apply_tool_colors() (gcode viewer per-tool colors).
// ============================================================================

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintStatusPanel::effective_auto_match: non-editable backend always auto-matches",
                 "[auto_match][print_status][ams]") {
    PrintStatusPanel panel(get_printer_state(), nullptr);
    install_backend(/*editable=*/false);

    set_auto_color_map(false);
    CHECK(PrintStatusPanelTestAccess::effective_auto_match(panel));

    set_auto_color_map(true);
    CHECK(PrintStatusPanelTestAccess::effective_auto_match(panel));
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintStatusPanel::effective_auto_match: editable backend honors the setting",
                 "[auto_match][print_status][ams]") {
    PrintStatusPanel panel(get_printer_state(), nullptr);
    install_backend(/*editable=*/true);

    set_auto_color_map(false);
    CHECK_FALSE(PrintStatusPanelTestAccess::effective_auto_match(panel));

    set_auto_color_map(true);
    CHECK(PrintStatusPanelTestAccess::effective_auto_match(panel));
}

TEST_CASE_METHOD(AutoMatchFixture,
                 "PrintStatusPanel::effective_auto_match: no backend auto-matches",
                 "[auto_match][print_status][ams]") {
    AmsState::instance().set_backend(nullptr);
    PrintStatusPanel panel(get_printer_state(), nullptr);

    set_auto_color_map(false);
    CHECK(PrintStatusPanelTestAccess::effective_auto_match(panel));
}

// ============================================================================
// Agreement — the reason the rule is shared at all.
// ============================================================================

TEST_CASE_METHOD(AutoMatchFixture, "print-select and print-status resolve auto-match identically",
                 "[auto_match][print_select][print_status][ams]") {
    helix::ui::PrintSelectDetailView view;
    PrintStatusPanel panel(get_printer_state(), nullptr);

    for (bool editable : {false, true}) {
        install_backend(editable);
        for (bool setting : {false, true}) {
            set_auto_color_map(setting);
            INFO("editable=" << editable << " auto_color_map=" << setting);
            CHECK(view.effective_auto_match() ==
                  PrintStatusPanelTestAccess::effective_auto_match(panel));
        }
    }
}
