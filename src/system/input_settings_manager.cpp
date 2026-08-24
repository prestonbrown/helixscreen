// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_settings_manager.h"

#include "app_constants.h"
#include "config.h"
#include "display_manager.h"
#include "runtime_config.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>

using namespace helix;

InputSettingsManager& InputSettingsManager::instance() {
    static InputSettingsManager instance;
    return instance;
}

InputSettingsManager::InputSettingsManager() {
    spdlog::trace("[InputSettingsManager] Constructor");
}

void InputSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[InputSettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[InputSettingsManager] Initializing subjects");

    Config* config = Config::get_instance();

    // Scroll throw (default: 25, range 5-50)
    int scroll_throw = config->get<int>("/input/scroll_throw", 25);
    scroll_throw = std::max(5, std::min(50, scroll_throw));
    UI_MANAGED_SUBJECT_INT(scroll_throw_subject_, scroll_throw, "settings_scroll_throw", subjects_);

    // Scroll limit (default: 10, range 1-20)
    int scroll_limit = config->get<int>("/input/scroll_limit", 10);
    scroll_limit = std::max(1, std::min(20, scroll_limit));
    UI_MANAGED_SUBJECT_INT(scroll_limit_subject_, scroll_limit, "settings_scroll_limit", subjects_);

    // Long-press time (default: 500ms, range 300-1500). Global — every long-press
    // in the app flips at this threshold, not just home edit mode (#1245).
    int long_press_time = config->get<int>("/input/long_press_time",
                                           static_cast<int>(AppConstants::Input::LONG_PRESS_MS));
    long_press_time = std::max(300, std::min(1500, long_press_time));
    UI_MANAGED_SUBJECT_INT(long_press_time_subject_, long_press_time, "settings_long_press_time",
                           subjects_);

    // Jitter threshold (default: 5, range 0-30; 0 disables)
    int jitter_threshold = config->get<int>("/input/jitter_threshold", 5);
    jitter_threshold = std::max(0, std::min(30, jitter_threshold));
    UI_MANAGED_SUBJECT_INT(jitter_threshold_subject_, jitter_threshold, "settings_jitter_threshold",
                           subjects_);

    // Scroll guard (default: false; AD5M/AD5X presets enable it via hardware preset)
    bool scroll_guard = config->get<bool>("/input/scroll_guard", false);
    UI_MANAGED_SUBJECT_INT(scroll_guard_subject_, scroll_guard ? 1 : 0, "settings_scroll_guard",
                           subjects_);

    // Debug touch visualization (default: false). Apply live at init so the
    // persisted setting matches RuntimeConfig before the ripple timer first fires.
    bool debug_touches = config->get<bool>("/input/debug_touches", false);
    RuntimeConfig::set_debug_touches(debug_touches);
    UI_MANAGED_SUBJECT_INT(debug_touches_subject_, debug_touches ? 1 : 0, "settings_debug_touches",
                           subjects_);

    // Home-screen edit mode enabled (default: true). When false the long-press
    // that enters grid edit mode is suppressed entirely (#1245).
    bool home_edit_mode = config->get<bool>("/input/home_edit_mode_enabled", true);
    UI_MANAGED_SUBJECT_INT(home_edit_mode_enabled_subject_, home_edit_mode ? 1 : 0,
                           "settings_home_edit_mode_enabled", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "InputSettingsManager", []() { InputSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[InputSettingsManager] Subjects initialized: scroll_throw={}, scroll_limit={}, "
                  "long_press_time={}, jitter={}, scroll_guard={}, debug_touches={}",
                  scroll_throw, scroll_limit, long_press_time, jitter_threshold, scroll_guard,
                  debug_touches);
}

void InputSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[InputSettingsManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[InputSettingsManager] Subjects deinitialized");
}

// =============================================================================
// GETTERS / SETTERS
// =============================================================================

int InputSettingsManager::get_scroll_throw() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_throw_subject_));
}

void InputSettingsManager::set_scroll_throw(int value) {
    // Clamp to valid range (5-50)
    int clamped = std::max(5, std::min(50, value));
    spdlog::info("[InputSettingsManager] set_scroll_throw({})", clamped);

    // 1. Update subject
    lv_subject_set_int(&scroll_throw_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/input/scroll_throw", clamped);
    config->save();

    // 3. Mark restart needed (this setting only takes effect on startup)
    restart_pending_ = true;
    spdlog::debug("[InputSettingsManager] Scroll throw set to {} (restart required)", clamped);
}

int InputSettingsManager::get_scroll_limit() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_limit_subject_));
}

void InputSettingsManager::set_scroll_limit(int value) {
    // Clamp to valid range (1-20)
    int clamped = std::max(1, std::min(20, value));
    spdlog::info("[InputSettingsManager] set_scroll_limit({})", clamped);

    // 1. Update subject
    lv_subject_set_int(&scroll_limit_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/input/scroll_limit", clamped);
    config->save();

    // 3. Mark restart needed (this setting only takes effect on startup)
    restart_pending_ = true;
    spdlog::debug("[InputSettingsManager] Scroll limit set to {} (restart required)", clamped);
}

int InputSettingsManager::get_long_press_time() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&long_press_time_subject_));
}

void InputSettingsManager::set_long_press_time(int value) {
    // Clamp to valid range (300-1500 ms)
    int clamped = std::max(300, std::min(1500, value));
    spdlog::info("[InputSettingsManager] set_long_press_time({}) [live]", clamped);

    // 1. Update subject
    lv_subject_set_int(&long_press_time_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/input/long_press_time", clamped);
    config->save();

    // 3. Live-apply: lv_indev_set_long_press_time is a live indev property, so
    //    the new threshold takes effect on the very next press without a restart.
    //    This is global — every long-press in the app (home edit mode, file-card
    //    delete, macro edit, gcode object select, LED, timelapse) follows it.
    if (auto* dm = DisplayManager::instance()) {
        if (auto* pointer = dm->pointer_input()) {
            lv_indev_set_long_press_time(pointer, clamped);
        }
    }
    // No restart_pending_ — applies immediately.
}

int InputSettingsManager::get_jitter_threshold() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&jitter_threshold_subject_));
}

void InputSettingsManager::set_jitter_threshold(int value) {
    int clamped = std::max(0, std::min(30, value));
    spdlog::info("[InputSettingsManager] set_jitter_threshold({})", clamped);

    lv_subject_set_int(&jitter_threshold_subject_, clamped);

    Config* config = Config::get_instance();
    config->set<int>("/input/jitter_threshold", clamped);
    config->save();

    restart_pending_ = true;
}

bool InputSettingsManager::get_scroll_guard() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&scroll_guard_subject_)) != 0;
}

void InputSettingsManager::set_scroll_guard(bool enabled) {
    spdlog::info("[InputSettingsManager] set_scroll_guard({})", enabled);

    lv_subject_set_int(&scroll_guard_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/input/scroll_guard", enabled);
    config->save();

    restart_pending_ = true;
}

bool InputSettingsManager::get_debug_touches() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&debug_touches_subject_)) != 0;
}

void InputSettingsManager::set_debug_touches(bool enabled) {
    spdlog::info("[InputSettingsManager] set_debug_touches({}) [live]", enabled);

    // Live-apply: the ripple timer checks RuntimeConfig::debug_touches() on every tick.
    RuntimeConfig::set_debug_touches(enabled);

    lv_subject_set_int(&debug_touches_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/input/debug_touches", enabled);
    config->save();
    // No restart_pending_ — change takes effect immediately.
}

bool InputSettingsManager::get_home_edit_mode_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&home_edit_mode_enabled_subject_)) != 0;
}

void InputSettingsManager::set_home_edit_mode_enabled(bool enabled) {
    spdlog::info("[InputSettingsManager] set_home_edit_mode_enabled({}) [live]", enabled);

    lv_subject_set_int(&home_edit_mode_enabled_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/input/home_edit_mode_enabled", enabled);
    config->save();
    // No restart_pending_ — should_suppress_edit_mode checks this live.
}
