// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file home_edit_mode_test_helpers.h
 * @brief Scoped guards shared by the home-grid edit-mode tests (#1245).
 *
 * Both entry conditions for grid edit mode — the lock screen and the Touch &
 * Input kill switch — are checked inside should_suppress_edit_mode(), a static
 * function in ui_panel_home.cpp. The only seam that reaches it is the
 * LV_EVENT_LONG_PRESSED handler the XML wires onto the carousel host, so both
 * test files drive that handler and share the setup below rather than keeping
 * two copies of it.
 *
 * Header-only and used from more than one translation unit, so everything here
 * is either `inline` or a class definition.
 */

#pragma once

#include "ui_panel_home.h"

#include "../test_helpers/home_panel_test_access.h"
#include "config.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"
#include "lock_manager.h"

#include "../catch_amalgamated.hpp"

namespace helix_test {

/**
 * @brief Puts InputSettingsManager into a known, asserted state for the test.
 *
 * The precondition is checked, not assumed: the default-flavoured constructor
 * fails the test outright if home_edit_mode_enabled does not come up true, so a
 * change to the documented default can never quietly turn a "kill switch off"
 * case into a no-op that still passes.
 *
 * The destructor leaves the manager INITIALIZED at its defaults rather than torn
 * down. deinit_subjects() withdraws the settings_* names from the XML scope
 * while SettingsManager::subjects_initialized_ stays true, so a later
 * SettingsManager::init_subjects() short-circuits and never re-registers them —
 * a test that then builds settings_touch_overlay.xml would get "No subject was
 * found" and silently unbound toggles.
 */
class ScopedInputSettings {
  public:
    /// No persisted value: assert the documented default (enabled) is in force.
    ScopedInputSettings() {
        helix::Config* config = helix::Config::get_instance();
        REQUIRE(config != nullptr);
        forget_input_settings();
        REQUIRE_FALSE(config->exists("/input/home_edit_mode_enabled"));
        reload_input_settings();
        REQUIRE(helix::InputSettingsManager::instance().get_home_edit_mode_enabled());
    }

    /// Persist an explicit value and assert it survives the load.
    explicit ScopedInputSettings(bool home_edit_mode_enabled) {
        helix::Config* config = helix::Config::get_instance();
        REQUIRE(config != nullptr);
        forget_input_settings();
        config->set<bool>("/input/home_edit_mode_enabled", home_edit_mode_enabled);
        reload_input_settings();
        REQUIRE(helix::InputSettingsManager::instance().get_home_edit_mode_enabled() ==
                home_edit_mode_enabled);
    }

    ~ScopedInputSettings() {
        forget_input_settings();
        reload_input_settings();
    }

    ScopedInputSettings(const ScopedInputSettings&) = delete;
    ScopedInputSettings& operator=(const ScopedInputSettings&) = delete;
};

/**
 * @brief Restores LockManager to "no PIN, unlocked" however the test exits.
 *
 * It is a process-wide singleton that persists its PIN to Config, so a leaked
 * lock would follow every later test in the binary.
 */
class ScopedLockState {
  public:
    ScopedLockState() {
        helix::LockManager::instance().remove_pin(); // known-clean starting point
    }
    ~ScopedLockState() {
        helix::LockManager::instance().remove_pin();
    }

    ScopedLockState(const ScopedLockState&) = delete;
    ScopedLockState& operator=(const ScopedLockState&) = delete;
};

/**
 * @brief Seeds the panel's page-container list, standing in for build_carousel().
 *
 * on_home_grid_long_press() only does anything once the panel owns a page
 * container; seeding it directly is what lets a test drive the real handler
 * without standing up the whole carousel. Detaches on scope exit so the global
 * HomePanel does not outlive the test holding a pointer to a fixture-owned
 * widget.
 */
class ScopedHomePanelPage {
  public:
    ScopedHomePanelPage(HomePanel& panel, lv_obj_t* container) : panel_(panel) {
        HomePanelTestAccess::set_single_page_container(panel_, container);
    }
    ~ScopedHomePanelPage() {
        panel_.exit_grid_edit_mode();
        HomePanelTestAccess::clear_page_containers(panel_);
    }

    ScopedHomePanelPage(const ScopedHomePanelPage&) = delete;
    ScopedHomePanelPage& operator=(const ScopedHomePanelPage&) = delete;

  private:
    HomePanel& panel_;
};

/**
 * @brief Build the stand-in carousel page container and wire the real handler.
 *
 * The XML wires HomePanel::on_home_grid_long_press onto carousel_host_ for
 * LV_EVENT_LONG_PRESSED; this attaches that exact callback.
 */
inline lv_obj_t* make_long_press_container(lv_obj_t* parent) {
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(container, 400, 300);
    lv_obj_update_layout(container);
    lv_obj_add_event_cb(container, HomePanelTestAccess::long_press_cb(), LV_EVENT_LONG_PRESSED,
                        nullptr);
    return container;
}

} // namespace helix_test
