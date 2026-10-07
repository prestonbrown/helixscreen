// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_settings_manager.h"

#include "app_constants.h"
#include "display_manager.h"
#include "runtime_config.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

using namespace helix;

using settings::Scope;
// Row order is InputSettingsManager::Key.
static constexpr settings::PersistedSetting INPUT_SETTINGS[] = {
    {"settings_scroll_throw", "/input/scroll_throw", Scope::Global, false,
     InputSettingsManager::DEFAULT_SCROLL_THROW, 5, 50, nullptr},
    {"settings_scroll_limit", "/input/scroll_limit", Scope::Global, false, 10, 1, 20, nullptr},
    {"settings_long_press_time", "/input/long_press_time", Scope::Global, false,
     static_cast<int>(AppConstants::Input::LONG_PRESS_MS), 300, 1500, nullptr},
    // AD5M/AD5X presets enable the scroll guard via the hardware preset.
    {"settings_scroll_guard", "/input/scroll_guard", Scope::Global, true, 0, 0, 1, nullptr},
    {"settings_debug_touches", "/input/debug_touches", Scope::Global, true, 0, 0, 1, nullptr},
    {"settings_home_edit_mode_enabled", "/input/home_edit_mode_enabled", Scope::Global, true, 1, 0,
     1, nullptr},
    {"settings_keypad_layout", "/input/keypad_layout", Scope::Global, false,
     static_cast<int>(KeypadLayout::PHONE), 0, 1, nullptr},
};

InputSettingsManager& InputSettingsManager::instance() {
    static InputSettingsManager instance;
    return instance;
}

InputSettingsManager::InputSettingsManager() : settings_(INPUT_SETTINGS) {
    spdlog::trace("[InputSettingsManager] Constructor");
}

void InputSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[InputSettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[InputSettingsManager] Initializing subjects");

    settings_.init(subjects_);
    // Apply live at init so RuntimeConfig matches before the ripple timer first fires.
    RuntimeConfig::set_debug_touches(get_debug_touches());
    apply_to_pointers();

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "InputSettingsManager", []() { InputSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[InputSettingsManager] Subjects initialized");
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

// Scroll throw, scroll limit and long-press time are live indev properties:
// the next gesture uses the new values. Every pointer indev gets them, so this
// works the same whichever platform layer created the indev.
void InputSettingsManager::apply_to_pointers() const {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        lv_indev_set_scroll_throw(indev, static_cast<uint8_t>(get_scroll_throw()));
        lv_indev_set_scroll_limit(indev, static_cast<uint8_t>(get_scroll_limit()));
        lv_indev_set_long_press_time(indev, static_cast<uint16_t>(get_long_press_time()));
    }
    // The post-scroll click guard decides "was this a scroll" by the same limit.
    if (auto* dm = DisplayManager::instance()) {
        dm->set_scroll_guard_limit(get_scroll_limit());
    }
}

void InputSettingsManager::set_scroll_throw(int value) {
    settings_.set(Key::ScrollThrow, value);
    apply_to_pointers();
}

void InputSettingsManager::set_scroll_limit(int value) {
    settings_.set(Key::ScrollLimit, value);
    apply_to_pointers();
}

void InputSettingsManager::set_long_press_time(int value) {
    settings_.set(Key::LongPressTime, value);
    apply_to_pointers();
}

void InputSettingsManager::set_scroll_guard(bool enabled) {
    settings_.set(Key::ScrollGuard, enabled);
    restart_pending_ = true;
}

void InputSettingsManager::set_debug_touches(bool enabled) {
    // The ripple timer checks RuntimeConfig::debug_touches() on every tick.
    RuntimeConfig::set_debug_touches(enabled);
    settings_.set(Key::DebugTouches, enabled);
}
