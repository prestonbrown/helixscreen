// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MacrosPanel edit-mode model: the pure per-row decision logic
// (macro_edit_logic.h), the SettingsManager hidden-set round-trip, and the
// panel model methods (enter/exit edit mode, toggle_row) driven through a
// friend test accessor. Widget instantiation is intentionally NOT exercised
// here — the row widgets are owned by the XML <repeat> and the singleton panel
// has the known PrintStatusPanel-style lifetime hazard, so we validate the
// subject-driving logic (the panel's sole responsibility) instead.

#include "ui_panel_macros.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "macro_edit_logic.h"
#include "settings_manager.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::macros;

// ---------------------------------------------------------------------------
// Friend accessor — private model access without prod-header test methods (L088)
// ---------------------------------------------------------------------------
struct MacrosPanelTestAccess {
    static void prepare(MacrosPanel& p, std::vector<std::string> macros) {
        // HelixTestFixture::reset_all() does NOT clear Config data, and the
        // per-printer "macros/hidden" key is a process-singleton — reset it so
        // each case starts key-absent (first-run seed) regardless of order.
        helix::Config::get_instance()->reset_to_defaults();
        // The test build's get_moonraker_api() stub always returns nullptr
        // (ui_test_utils.cpp), so refresh_macros() no-ops and keeps the
        // injected list — no live API can clobber it.
        p.init_subjects();
        p.ui_alive_ = true;
        p.edit_mode_ = false;
        p.pending_hidden_.clear();
        p.all_macros_ = std::move(macros);
        std::sort(p.all_macros_.begin(), p.all_macros_.end());
    }
    static void teardown(MacrosPanel& p) {
        p.edit_mode_ = false;
        p.pending_hidden_.clear();
        p.all_macros_.clear();
        p.displayed_.clear();
        p.ui_alive_ = false;
    }
    static void enter(MacrosPanel& p) {
        p.enter_edit_mode();
    }
    static void exit(MacrosPanel& p, bool save) {
        p.exit_edit_mode(save);
    }
    static void toggle(MacrosPanel& p, size_t i) {
        p.toggle_row(i);
    }
    static const std::vector<std::string>& displayed(MacrosPanel& p) {
        return p.displayed_;
    }
    static const std::set<std::string>& pending_hidden(MacrosPanel& p) {
        return p.pending_hidden_;
    }
    static bool edit_mode(MacrosPanel& p) {
        return p.edit_mode_;
    }
    static int visible_int(MacrosPanel& p, size_t i) {
        return lv_subject_get_int(p.visible_pool_.at(i));
    }
    static int row_count(MacrosPanel& p) {
        return lv_subject_get_int(&p.macro_row_count_);
    }
    static int edit_mode_subject(MacrosPanel& p) {
        return lv_subject_get_int(&p.macro_edit_mode_);
    }
    static int save_hidden_subject(MacrosPanel& p) {
        return lv_subject_get_int(&p.macros_edit_save_hidden_);
    }
};

static size_t index_of(MacrosPanel& p, const std::string& name) {
    const auto& d = MacrosPanelTestAccess::displayed(p);
    auto it = std::find(d.begin(), d.end(), name);
    REQUIRE(it != d.end());
    return static_cast<size_t>(it - d.begin());
}

// ===========================================================================
// Pure decision logic (no LVGL, no singletons)
// ===========================================================================

TEST_CASE("seed hides exactly the _-prefixed macros when key absent", "[macros][editmode]") {
    std::vector<std::string> all = {"CLEAN_NOZZLE", "LOAD", "_HOME_Z", "_CALIBRATE", "PRINT_START"};
    auto hidden = compute_effective_hidden(all, /*key_exists=*/false, /*saved=*/{});
    REQUIRE(hidden.size() == 2);
    REQUIRE(hidden.count("_HOME_Z") == 1);
    REQUIRE(hidden.count("_CALIBRATE") == 1);
    REQUIRE(hidden.count("CLEAN_NOZZLE") == 0);
}

TEST_CASE("saved set is authoritative once the key exists (even if empty)", "[macros][editmode]") {
    std::vector<std::string> all = {"CLEAN_NOZZLE", "_HOME_Z"};
    // Key exists but the saved set is empty -> nothing hidden (user un-hid _*).
    auto none = compute_effective_hidden(all, /*key_exists=*/true, /*saved=*/{});
    REQUIRE(none.empty());
    // Key exists with an explicit set -> exactly that set, ignoring _* seed.
    auto some = compute_effective_hidden(all, /*key_exists=*/true, {"CLEAN_NOZZLE"});
    REQUIRE(some.size() == 1);
    REQUIRE(some.count("CLEAN_NOZZLE") == 1);
    REQUIRE(some.count("_HOME_Z") == 0);
}

TEST_CASE("normal-mode visible filter excludes hidden, keeps order", "[macros][editmode]") {
    std::vector<std::string> all = {"A", "B", "C", "D"};
    std::set<std::string> hidden = {"B", "D"};
    auto vis = filter_visible(all, hidden);
    REQUIRE(vis == std::vector<std::string>{"A", "C"});
}

TEST_CASE("per-row values: normal mode shows desc/chevron per macro facts", "[macros][editmode]") {
    // Normal mode, has description, takes params -> desc + chevron shown.
    auto a = compute_row_values(/*edit=*/false, /*hidden=*/false, /*has_desc=*/true,
                                /*no_params=*/false);
    REQUIRE(a.visible == 1);
    REQUIRE(a.desc_hidden == 0);
    REQUIRE(a.chevron_hidden == 0);

    // Normal mode, no description, no params -> both hidden.
    auto b = compute_row_values(false, false, /*has_desc=*/false, /*no_params=*/true);
    REQUIRE(b.desc_hidden == 1);
    REQUIRE(b.chevron_hidden == 1);
}

TEST_CASE("per-row values: edit mode hides chevron but keeps desc, visible tracks hidden",
          "[macros][editmode]") {
    // Edit mode always hides the chevron regardless of macro facts, but the
    // description stays visible (depends only on has_desc) so the row doesn't
    // jump vertically when entering/exiting edit mode.
    auto shown = compute_row_values(/*edit=*/true, /*hidden=*/false, /*has_desc=*/true,
                                    /*no_params=*/false);
    REQUIRE(shown.visible == 1); // not hidden -> checked
    REQUIRE(shown.desc_hidden == 0);
    REQUIRE(shown.chevron_hidden == 1);

    // Edit mode, no description -> desc stays hidden regardless of edit mode.
    auto no_desc = compute_row_values(true, /*hidden=*/false, /*has_desc=*/false,
                                      /*no_params=*/false);
    REQUIRE(no_desc.desc_hidden == 1);

    auto hidden = compute_row_values(true, /*hidden=*/true, true, false);
    REQUIRE(hidden.visible == 0); // hidden -> unchecked
}

// ===========================================================================
// SettingsManager round-trip
// ===========================================================================

TEST_CASE_METHOD(HelixTestFixture, "hidden-macro save round-trips through settings",
                 "[macros][editmode]") {
    helix::Config::get_instance()->reset_to_defaults(); // key-absent start (see prepare())
    auto& sm = helix::SettingsManager::instance();
    REQUIRE_FALSE(sm.hidden_macros_key_exists());

    sm.set_hidden_macros({"CLEAN_NOZZLE", "_HOME_Z"});
    REQUIRE(sm.hidden_macros_key_exists());

    auto got = sm.get_hidden_macros();
    REQUIRE(got.size() == 2);
    REQUIRE(std::find(got.begin(), got.end(), "CLEAN_NOZZLE") != got.end());
    REQUIRE(std::find(got.begin(), got.end(), "_HOME_Z") != got.end());
}

// ===========================================================================
// Panel model methods (friend-driven; subjects only, no row widgets)
// ===========================================================================

TEST_CASE_METHOD(LVGLTestFixture, "enter edit mode shows all macros incl. _-prefixed",
                 "[macros][editmode]") {
    auto& p = get_global_macros_panel();
    MacrosPanelTestAccess::prepare(p, {"CLEAN_NOZZLE", "LOAD", "_HOME_Z"});

    MacrosPanelTestAccess::enter(p);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(MacrosPanelTestAccess::edit_mode(p));
    REQUIRE(MacrosPanelTestAccess::edit_mode_subject(p) == 1);
    REQUIRE(MacrosPanelTestAccess::save_hidden_subject(p) == 0); // Save shown
    // Edit mode shows every macro, including the _-prefixed one.
    REQUIRE(MacrosPanelTestAccess::displayed(p).size() == 3);
    REQUIRE(MacrosPanelTestAccess::row_count(p) == 3);
    // First-run seed hides the _-prefixed macro -> its checkbox is unchecked.
    size_t sys = index_of(p, "_HOME_Z");
    REQUIRE(MacrosPanelTestAccess::visible_int(p, sys) == 0);
    size_t clean = index_of(p, "CLEAN_NOZZLE");
    REQUIRE(MacrosPanelTestAccess::visible_int(p, clean) == 1);

    MacrosPanelTestAccess::teardown(p);
}

TEST_CASE_METHOD(LVGLTestFixture, "toggle_row flips pending_hidden_ and the visible pool int",
                 "[macros][editmode]") {
    auto& p = get_global_macros_panel();
    MacrosPanelTestAccess::prepare(p, {"CLEAN_NOZZLE", "LOAD", "_HOME_Z"});
    MacrosPanelTestAccess::enter(p);
    helix::ui::UpdateQueue::instance().drain();

    size_t clean = index_of(p, "CLEAN_NOZZLE");
    REQUIRE(MacrosPanelTestAccess::visible_int(p, clean) == 1);
    REQUIRE(MacrosPanelTestAccess::pending_hidden(p).count("CLEAN_NOZZLE") == 0);

    MacrosPanelTestAccess::toggle(p, clean); // hide it
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(MacrosPanelTestAccess::pending_hidden(p).count("CLEAN_NOZZLE") == 1);
    REQUIRE(MacrosPanelTestAccess::visible_int(p, clean) == 0);

    MacrosPanelTestAccess::toggle(p, clean); // un-hide it
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(MacrosPanelTestAccess::pending_hidden(p).count("CLEAN_NOZZLE") == 0);
    REQUIRE(MacrosPanelTestAccess::visible_int(p, clean) == 1);

    MacrosPanelTestAccess::teardown(p);
}

TEST_CASE_METHOD(LVGLTestFixture, "exit(save=false) discards; exit(save=true) persists",
                 "[macros][editmode]") {
    auto& sm = helix::SettingsManager::instance();
    auto& p = get_global_macros_panel();

    // --- discard path ---
    MacrosPanelTestAccess::prepare(p, {"CLEAN_NOZZLE", "LOAD", "_HOME_Z"});
    MacrosPanelTestAccess::enter(p);
    MacrosPanelTestAccess::toggle(p, index_of(p, "CLEAN_NOZZLE")); // hide CLEAN_NOZZLE
    MacrosPanelTestAccess::exit(p, /*save=*/false);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(MacrosPanelTestAccess::edit_mode(p));
    REQUIRE_FALSE(sm.hidden_macros_key_exists()); // nothing written

    // --- save path (seed hides _HOME_Z; we additionally hide CLEAN_NOZZLE) ---
    MacrosPanelTestAccess::prepare(p, {"CLEAN_NOZZLE", "LOAD", "_HOME_Z"});
    MacrosPanelTestAccess::enter(p);
    MacrosPanelTestAccess::toggle(p, index_of(p, "CLEAN_NOZZLE"));
    MacrosPanelTestAccess::exit(p, /*save=*/true);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(MacrosPanelTestAccess::edit_mode_subject(p) == 0);
    REQUIRE(MacrosPanelTestAccess::save_hidden_subject(p) == 1); // Save hidden again
    REQUIRE(sm.hidden_macros_key_exists());
    auto hidden = sm.get_hidden_macros();
    REQUIRE(std::find(hidden.begin(), hidden.end(), "CLEAN_NOZZLE") != hidden.end());
    REQUIRE(std::find(hidden.begin(), hidden.end(), "_HOME_Z") != hidden.end()); // seed kept

    MacrosPanelTestAccess::teardown(p);
}

TEST_CASE_METHOD(LVGLTestFixture, "normal mode after save excludes the hidden macros",
                 "[macros][editmode]") {
    auto& p = get_global_macros_panel();
    MacrosPanelTestAccess::prepare(p, {"CLEAN_NOZZLE", "LOAD", "_HOME_Z"});

    // Save a hidden set through edit mode, then rebuild in normal mode.
    MacrosPanelTestAccess::enter(p);
    MacrosPanelTestAccess::toggle(p, index_of(p, "CLEAN_NOZZLE"));
    MacrosPanelTestAccess::exit(p, /*save=*/true);
    helix::ui::UpdateQueue::instance().drain();

    // Normal mode now filters out both the seeded _HOME_Z and CLEAN_NOZZLE.
    const auto& shown = MacrosPanelTestAccess::displayed(p);
    REQUIRE(std::find(shown.begin(), shown.end(), "LOAD") != shown.end());
    REQUIRE(std::find(shown.begin(), shown.end(), "CLEAN_NOZZLE") == shown.end());
    REQUIRE(std::find(shown.begin(), shown.end(), "_HOME_Z") == shown.end());

    MacrosPanelTestAccess::teardown(p);
}
