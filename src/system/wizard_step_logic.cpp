// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_logic.h"

#include "config.h"
#include "wizard_step.h" // helix::wizard::StepId

#include <spdlog/spdlog.h>

#include <atomic>
#include <string>

namespace helix {

namespace {
/// Set once the printer-identify step applies a preset; never cleared in
/// production. Process-local on purpose — it is exactly the "this wizard run
/// is still going" signal that a persisted flag cannot express.
std::atomic<bool> g_preset_applied_this_session{false};
} // namespace

bool wizard_preset_is_authoritative(bool preset_marker, bool provisional, bool wizard_completed,
                                    bool applied_this_session) {
    if (!preset_marker) {
        return false;
    }
    if (wizard_completed) {
        // The run that wrote it finished; the marker is settled.
        return true;
    }
    if (!provisional) {
        // Seeded before the wizard ran (install-time detection) — the fast path.
        return true;
    }
    // Written by an unfinished wizard: honor it only while that run is live.
    return applied_this_session;
}

void wizard_mark_preset_applied_this_session() {
    g_preset_applied_this_session.store(true, std::memory_order_relaxed);
}

bool wizard_preset_applied_this_session() {
    return g_preset_applied_this_session.load(std::memory_order_relaxed);
}

void wizard_reset_preset_session_state() {
    g_preset_applied_this_session.store(false, std::memory_order_relaxed);
}

bool wizard_hardware_snapshot_is_deferred(bool discovery_succeeded, bool snapshot_has_entries) {
    if (discovery_succeeded) {
        // The pickers had real lists; whatever the user chose (including
        // nothing) is a deliberate answer and the snapshot is final.
        return false;
    }
    // Klipper never answered. A preset still supplies real names, so only an
    // empty result is a debt to settle on a later boot.
    return !snapshot_has_entries;
}

bool wizard_apply_hardware_snapshot_decision(Config* config, bool discovery_succeeded,
                                             bool snapshot_has_entries) {
    if (!config) {
        return false;
    }
    // df() — per-printer, alongside `preset` and WIZARD_PRESET_PROVISIONAL. A
    // root-level flag would let a second printer inherit or clear the first
    // one's debt, which is exactly what #1162 fixed for the preset marker.
    const std::string key = config->df() + WIZARD_HARDWARE_SETUP_DEFERRED;

    if (wizard_hardware_snapshot_is_deferred(discovery_succeeded, snapshot_has_entries)) {
        config->set<bool>(key, true);
        spdlog::warn("[Wizard] Finished without Klipper — deferring the expected-hardware "
                     "snapshot until discovery succeeds");
        return true;
    }
    if (config->get<bool>(key, false)) {
        config->set<bool>(key, false);
        spdlog::info("[Wizard] Hardware recorded from a run that reached Klipper — deferred "
                     "setup settled");
    }
    return false;
}

bool wizard_hardware_setup_deferred(Config* config) {
    if (!config) {
        return false;
    }
    return config->get<bool>(config->df() + WIZARD_HARDWARE_SETUP_DEFERRED, false);
}

bool wizard_clear_hardware_setup_deferred(Config* config) {
    if (!wizard_hardware_setup_deferred(config)) {
        return false;
    }
    config->set<bool>(config->df() + WIZARD_HARDWARE_SETUP_DEFERRED, false);
    return true;
}

WizardPresetPlan wizard_preset_plan(bool has_preset, int printer_count) {
    WizardPresetPlan plan;
    // A complete preset configures the hardware for any printer it applies to,
    // so the hardware-pick steps are redundant regardless of printer order.
    plan.skip_hardware = has_preset;
    // The first-run fast path additionally skips the summary and shows the
    // one-time telemetry opt-in — gated to the initial printer so telemetry
    // never re-prompts when adding subsequent printers.
    plan.first_run = has_preset && printer_count <= 1;
    return plan;
}

// ============================================================================
// Id-based pure navigation over the step registry.
// ============================================================================

namespace {

// Index of `current` in the vector (matched by id), or -1 if absent — treated
// as "before the first entry" by the navigation helpers.
int index_of(wizard::StepId current, const std::vector<StepSkip>& steps) {
    for (size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].id == current) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

int wizard_visible_count(const std::vector<StepSkip>& steps) {
    int count = 0;
    for (const auto& s : steps) {
        if (!s.skipped) {
            count++;
        }
    }
    return count;
}

int wizard_display_number(wizard::StepId current, const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps);
    int upper = (idx < 0) ? 0 : idx;
    int display = 1; // 1-based
    for (int i = 0; i < upper; ++i) {
        if (!steps[i].skipped) {
            display++;
        }
    }
    return display;
}

std::optional<wizard::StepId> wizard_next(wizard::StepId current,
                                          const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps); // -1 => before first, start scan at 0
    for (int i = idx + 1; i < static_cast<int>(steps.size()); ++i) {
        if (!steps[i].skipped) {
            return steps[i].id;
        }
    }
    return std::nullopt;
}

std::optional<wizard::StepId> wizard_prev(wizard::StepId current,
                                          const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps);
    if (idx < 0) {
        idx = static_cast<int>(steps.size()); // not found => scan whole list backward
    }
    for (int i = idx - 1; i >= 0; --i) {
        if (!steps[i].skipped) {
            return steps[i].id;
        }
    }
    return std::nullopt;
}

bool wizard_is_last(wizard::StepId current, const std::vector<StepSkip>& steps) {
    return wizard_next(current, steps) == std::nullopt;
}

std::vector<wizard::StepId> wizard_deferred_hardware_steps(const std::vector<StepSkip>& steps) {
    // Wizard order. PrinterIdentify leads: the model pick drives the preset that
    // the pickers behind it are collapsed by, so re-running the hardware steps
    // without it would ask about hardware while leaving the printer unidentified.
    static constexpr wizard::StepId CANDIDATES[] = {
        wizard::StepId::PrinterIdentify, wizard::StepId::HeaterSelect,
        wizard::StepId::FanSelect,       wizard::StepId::AmsIdentify,
        wizard::StepId::LedSelect,       wizard::StepId::FilamentSensor,
        wizard::StepId::InputShaper,
    };

    std::vector<wizard::StepId> out;
    for (wizard::StepId id : CANDIDATES) {
        for (const auto& s : steps) {
            if (s.id == id) {
                if (!s.skipped) {
                    out.push_back(id);
                }
                break;
            }
        }
    }
    return out;
}

} // namespace helix
