// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file input_settings_test_helpers.h
 * @brief Deterministic InputSettingsManager setup for tests.
 *
 * Every value the manager exposes is loaded from Config once, inside
 * init_subjects(). That function early-returns when the manager is already
 * initialized (src/system/input_settings_manager.cpp:27), and
 * SettingsManager::init_subjects() cascades into it — so in a shared test binary
 * a bare init_subjects() is frequently a no-op that leaves some co-tenant test's
 * values in force. Anything asserting a default, a persisted round-trip, or a
 * load-time clamp has to tear the manager down first or it is testing whatever
 * ran before it.
 */

#pragma once

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "input_settings_manager.h"

namespace helix_test {

/// Drop the persisted /input node so the next load sees a fresh install.
inline void forget_input_settings() {
    helix::Config* config = helix::Config::get_instance();
    if (config == nullptr) {
        return;
    }
    json& data = helix::ConfigTestAccess::data(*config);
    if (data.is_object()) {
        data.erase("input");
    }
}

/// Force InputSettingsManager to re-read Config, and leave it INITIALIZED.
///
/// Leaving it torn down is the trap this avoids: deinit_subjects() withdraws the
/// settings_* names from the XML scope while SettingsManager::subjects_initialized_
/// stays true, so a later SettingsManager::init_subjects() short-circuits and never
/// re-registers them — a test that then builds settings_touch_overlay.xml gets
/// "No subject was found" and silently unbound toggles.
inline void reload_input_settings() {
    helix::Config::get_instance();
    helix::InputSettingsManager& input = helix::InputSettingsManager::instance();
    input.deinit_subjects();
    input.init_subjects();
    input.clear_restart_pending();
}

/// Wipe the persisted node and reload: the manager comes up on compiled-in defaults.
inline void reset_input_settings_to_defaults() {
    forget_input_settings();
    reload_input_settings();
}

} // namespace helix_test
