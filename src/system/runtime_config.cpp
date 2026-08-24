// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime_config.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
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
        if (std::getenv("HELIX_DEBUG_TOUCH") != nullptr ||
            std::getenv("HELIX_DEBUG_TOUCHES") != nullptr) {
            g_debug_touches = true;
        }
    }
    return g_debug_touches;
}

void RuntimeConfig::set_debug_touches(bool value) {
    g_debug_touches = value;
}

bool RuntimeConfig::touch_cal_debounce() {
    // ON by default (issue #943): capture-on-press / commit-on-release with a
    // time refractory. Opt OUT only when HELIX_TOUCH_CAL_DEBOUNCE is exactly "0"
    // (legacy sample-on-press behavior, for A/B testing on real hardware).
    static bool checked = false;
    static bool enabled = true;
    if (!checked) {
        checked = true;
        const char* val = std::getenv("HELIX_TOUCH_CAL_DEBOUNCE");
        enabled = !(val != nullptr && std::strcmp(val, "0") == 0);
    }
    return enabled;
}

bool RuntimeConfig::hot_reload_enabled() {
    static bool checked = false;
    static bool enabled = false;
    if (!checked) {
        checked = true;
        // Native (non-cross-compiled) builds default to hot-reload ON for
        // faster UI iteration — edit XML, save, see changes without restart.
        // Cross-compiled release builds (Pi, AD5M, K1, etc.) define
        // HELIX_RELEASE_BUILD and default to OFF: polling ~300 XML files every
        // 500ms is wasteful on low-power devices and the files don't change
        // post-install anyway.
        //
        // Env var always wins:
        //   HELIX_HOT_RELOAD=1  force ON  (e.g., enable on a device for live debugging)
        //   HELIX_HOT_RELOAD=0  force OFF (e.g., disable in a native dev build)
        bool default_on;
#ifdef HELIX_RELEASE_BUILD
        default_on = false;
#else
        default_on = true;
#endif
        const char* val = std::getenv("HELIX_HOT_RELOAD");
        if (val != nullptr && std::strcmp(val, "0") == 0) {
            enabled = false;
        } else if (val != nullptr && std::strcmp(val, "1") == 0) {
            enabled = true;
        } else {
            enabled = default_on;
        }
    }
    return enabled;
}

namespace {

/// Whether this backend surfaces its own runout fault, and therefore owns the
/// runout surface for the printer it is on.
///
/// The rule, and it is one rule for both channels of the error architecture
/// (docs/devel/FILAMENT_MANAGEMENT.md § "Two error channels"): **exactly one
/// surface per printer**. A backend that raises its own event — AFC and Happy
/// Hare through classify_error(), AD5X IFS through current_error(), CFS through
/// classify_error() as of #1250 — already tells the user what happened, with
/// recovery buttons derived from live hardware state. The generic
/// sensor-driven modal must stand down for those, or a runout on a config where
/// an AMS lane sensor does carry a FilamentSensorRole produces two dialogs.
///
/// A backend that raises nothing owns nothing. ACE and QIDI Box implement none
/// of the three hooks, so suppressing the generic modal for them (which the old
/// blanket "is it hub topology" test did) left those users with no runout
/// notification at all. That is the case this function narrows.
///
/// Keyed on AmsType rather than a virtual because the answer is "does this class
/// override an error hook", which the class cannot usefully answer about itself
/// without a second flag to keep in sync. Adding a backend that overrides
/// classify_error() or current_error() means adding it here — the switch has no
/// `default:` precisely so `-Wswitch` names this function when AmsType grows a
/// value. Should one slip through anyway, the fallthrough is false, i.e. keep
/// the generic modal: a redundant dialog beats silence about a real runout.
bool backend_owns_runout_surface(AmsType type) {
    switch (type) {
    case AmsType::HAPPY_HARE: // classify_error()
    case AmsType::AFC:        // classify_error() + current_error()
    case AmsType::AD5X_IFS:   // current_error() (unattended-runout detector)
    case AmsType::CFS:        // classify_error() (auto-refill give-up messages)
        return true;
    case AmsType::ACE:
    case AmsType::QIDI_BOX:
    case AmsType::TOOL_CHANGER:
    case AmsType::SNAPMAKER:
    case AmsType::NONE:
        return false;
    }
    return false;
}

} // namespace

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
        // Tool changers (Snapmaker U1, generic TOOL_CHANGER) have one extruder
        // per slot and no shared hub — runout on a tool cannot be auto-resolved
        // by swapping spools. Treat them like "no AMS" for runout guidance so
        // the user gets the same Load/Resume/Cancel modal they would on a
        // single-extruder printer. Bypass doesn't apply to tool changers.
        if (auto* backend = ams.get_backend(0);
            backend && backend->supports_per_tool_spool_assignment()) {
            spdlog::debug("[RuntimeConfig] Tool-changer AMS - showing runout modal");
            return true;
        }

        // Hub-topology AMS (Happy Hare, AFC, ACE, AD5X IFS, CFS, QIDI Box):
        // bypass_active=1: external spool (show modal - toolhead sensor matters)
        // bypass_active=0: AMS managing filament, so the toolhead sensor going
        //   empty mid-swap is normal and the generic modal would be false
        //   guidance. Suppressed — but ONLY for a backend that raises its own
        //   runout fault instead, so the user still hears about a real one.
        //   A backend with no error hook of its own would otherwise be silenced
        //   with nothing put in its place (#1250).
        int bypass_active = lv_subject_get_int(ams.get_bypass_active_subject());
        if (bypass_active == 0) {
            auto* backend = ams.get_backend(0);
            const AmsType type = backend ? backend->get_type() : AmsType::NONE;
            if (backend_owns_runout_surface(type)) {
                spdlog::debug("[RuntimeConfig] Suppressing runout modal - {} raises its own "
                              "runout fault",
                              ams_type_to_string(type));
                return false;
            }
            spdlog::debug("[RuntimeConfig] {} has no runout fault of its own - showing modal",
                          ams_type_to_string(type));
            return true;
        }
        spdlog::debug("[RuntimeConfig] AMS bypass active - showing runout modal");
    }

    // No AMS or AMS with bypass active - show modal
    return true;
}
