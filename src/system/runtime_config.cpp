// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime_config.h"

#include "ams_state.h"
#include "app_globals.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>

// Global runtime configuration instance
static RuntimeConfig g_runtime_config;

// Debug subjects flag (separate from RuntimeConfig instance for static access)
static bool g_debug_subjects = false;
static bool g_debug_touches = false;

RuntimeConfig* get_runtime_config() {
    return &g_runtime_config;
}

bool RuntimeConfig::debug_subjects() {
    // Check environment variable as fallback if not explicitly set
    static bool env_checked = false;
    if (!env_checked) {
        env_checked = true;
        if (std::getenv("HELIX_DEBUG_SUBJECTS") != nullptr) {
            g_debug_subjects = true;
        }
    }
    return g_debug_subjects;
}

void RuntimeConfig::set_debug_subjects(bool value) {
    g_debug_subjects = value;
}

bool RuntimeConfig::debug_touches() {
    static bool env_checked = false;
    if (!env_checked) {
        env_checked = true;
        if (std::getenv("HELIX_DEBUG_TOUCHES") != nullptr) {
            g_debug_touches = true;
        }
    }
    return g_debug_touches;
}

void RuntimeConfig::set_debug_touches(bool value) {
    g_debug_touches = value;
}

bool RuntimeConfig::hot_reload_enabled() {
    static bool checked = false;
    static bool enabled = false;
    if (!checked) {
        checked = true;
        const char* val = std::getenv("HELIX_HOT_RELOAD");
        enabled = (val != nullptr && std::strcmp(val, "1") == 0);
    }
    return enabled;
}

bool RuntimeConfig::should_show_runout_modal() const {
    // If explicitly forced via env var, always show
    if (std::getenv("HELIX_FORCE_RUNOUT_MODAL") != nullptr) {
        return true;
    }

    // Suppress during wizard setup
    if (is_wizard_active()) {
        spdlog::debug("[RuntimeConfig] Suppressing runout modal - wizard active");
        return false;
    }

    // Check AMS state
    auto& ams = AmsState::instance();
    if (ams.is_available()) {
        // AMS present - check bypass state
        // bypass_active=1: external spool (show modal - toolhead sensor matters)
        // bypass_active=0: AMS managing filament (suppress - runout during swaps normal)
        int bypass_active = lv_subject_get_int(ams.get_bypass_active_subject());
        if (bypass_active == 0) {
            spdlog::debug("[RuntimeConfig] Suppressing runout modal - AMS managing filament");
            return false;
        }
        spdlog::debug("[RuntimeConfig] AMS bypass active - showing runout modal");
    }

    // No AMS or AMS with bypass active - show modal
    return true;
}
