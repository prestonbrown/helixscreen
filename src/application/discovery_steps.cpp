// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file discovery_steps.cpp
 * @brief The desktop-only reactions to a completed Moonraker discovery, run after the core
 *        steps in discovery_steps_core.cpp.
 */

#include "discovery_steps.h"

#include "ui_panel_calibration_zoffset.h"
#include "ui_settings_about.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "hardware_setup_prompter.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "sensor_state.h"
#include "settings_manager.h"
#include "spoolman_active_spool_sync.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "timelapse_state.h"
#include "wizard_step_logic.h"

#include <spdlog/spdlog.h>

using namespace helix;

namespace helix {

namespace {

// The app started successfully: clear the self-update sentinel and listen for Moonraker
// finishing an update of HelixScreen.
void update_checker_connected_step(DiscoveryContext& ctx) {
    UpdateChecker::instance().on_connected(ctx.client);
}

// Fetch print hours now that connection is live, and refresh on job changes.
void about_print_hours_step(DiscoveryContext& ctx) {
    settings::get_about_settings_overlay().attach_print_hours(ctx.client);
}

void timelapse_events_step(DiscoveryContext& ctx) {
    TimelapseState::instance().attach(ctx.client);
}

void power_sensor_subscribe_step(DiscoveryContext& ctx) {
    PowerDeviceState::instance().subscribe(ctx.api);
    SensorState::instance().subscribe(ctx.api);
}

// Pay off a hardware snapshot the wizard deferred because Klipper was down during setup
// (#1160). Everything discovered now was present all along, so accepting it BEFORE
// validate() keeps this first successful discovery clean instead of reporting every fan,
// filament sensor and LED as newly appeared. The marker itself is cleared only once the
// user answers the offer in the prompt step, so a session killed before answering still
// gets asked next boot.
void acknowledge_deferred_hardware_step(DiscoveryContext& ctx) {
    ctx.hardware_setup_deferred = wizard_hardware_setup_deferred(Config::get_instance());
    if (ctx.hardware_setup_deferred && !Config::get_instance()->is_wizard_required() &&
        !is_wizard_active()) {
        size_t accepted =
            HardwareValidator::acknowledge_discovered_hardware(Config::get_instance(), ctx.hw);
        spdlog::info("[Application] Deferred wizard hardware snapshot written "
                     "({} object(s) accepted)",
                     accepted);
    }
}

// Hardware validation: check config expectations vs discovered hardware. Uses the
// post-preset config so preset-mapped fan/heater names are checked against discovery, not
// the pre-preset scaffolded defaults. validate() always runs (cheap, caches result,
// updates the validation_result subject for the UI); the user-facing notify is gated on a
// changed shape, or every reconnect would re-fire "expected fan not found". The
// validator's session snapshot, taken after the prompts, tracks state across runs
// regardless.
void validate_hardware_step(DiscoveryContext& ctx) {
    ctx.validator.emplace();
    auto validation_result = ctx.validator->validate(Config::get_instance(), ctx.hw);
    get_printer_state().hardware_validation_state().set_hardware_validation_result(
        validation_result);
    if (validation_result.has_issues() && ctx.hw_changed &&
        !Config::get_instance()->is_wizard_required() && !is_wizard_active()) {
        ctx.validator->notify_user(validation_result);
    }
}

void hardware_prompts_step(DiscoveryContext& ctx) {
    ctx.prompter->run_discovery_prompts(ctx.hw, ctx.hw_changed, ctx.print_active,
                                        ctx.hardware_setup_deferred);
}

// Save session snapshot for next comparison (even if no issues).
void save_validation_snapshot_step(DiscoveryContext& ctx) {
    ctx.validator->save_session_snapshot(Config::get_instance(), ctx.hw);
}

// record_session always runs (it counts sessions, including reconnects). The settings and
// memory snapshots capture a fingerprint of the current config + heap state, and would
// just re-record identical data on a reconnect with unchanged hardware.
// hardware_profile is deferred until the build volume is fetched.
void telemetry_step(DiscoveryContext& ctx) {
    TelemetryManager::instance().record_session();
    if (ctx.hw_changed) {
        TelemetryManager::instance().record_settings_snapshot();
        TelemetryManager::instance().record_memory_snapshot("session_start");
    }
}

// Mirror Moonraker's active Spoolman spool onto the external slot and the active tool,
// now and whenever it changes.
void spoolman_sync_step(DiscoveryContext& ctx) {
    spoolman_sync::attach(ctx.client, ctx.api.spoolman());
}

// Start automatic update checks (15s initial delay, then every 24h).
void auto_update_check_step(DiscoveryContext&) {
    UpdateChecker::instance().start_auto_check();
}

// Bring moonraker.conf's update channel back in line with the app's. Every connect
// runs it, so an install whose stanza drifted heals without a channel change.
void moonraker_update_channel_step(DiscoveryContext&) {
    UpdateChecker::instance().sync_moonraker_channel();
}

// Auto-navigate to Z-Offset Calibration if manual probe is already active (e.g.,
// PROBE_CALIBRATE started from Mainsail or console before HelixScreen launched). Deferred
// one tick: status updates from the subscription response are queued via ui_queue_update
// and may not have landed yet at this point.
void manual_probe_autoopen_step(DiscoveryContext& ctx) {
    IMoonrakerAPI* api = &ctx.api;
    lv_obj_t* screen = ctx.screen;
    ui::queue_update("discovery_steps::manual_probe_autoopen_step", [api, screen]() {
        auto& ps = get_printer_state();
        int probe_active =
            lv_subject_get_int(ps.calibration_state().get_manual_probe_active_subject());
        spdlog::info("[Application] Checking manual_probe at startup: is_active={}", probe_active);
        if (probe_active == 1) {
            spdlog::info("[Application] Manual probe active at startup, auto-opening "
                         "Z-Offset Calibration");
            auto& overlay = get_global_zoffset_cal_panel();
            overlay.set_api(api);
            overlay.show(screen);
        }
    });
}

constexpr DiscoveryStep kTailSteps[] = {
    {"update_checker_connected", false, update_checker_connected_step, nullptr},
    {"about_print_hours", false, about_print_hours_step, nullptr},
    {"timelapse_events", false, timelapse_events_step, nullptr},
    {"power_sensor_subscribe", false, power_sensor_subscribe_step, "post_subscribe"},
    {"acknowledge_deferred_hardware", false, acknowledge_deferred_hardware_step, nullptr},
    {"validate_hardware", false, validate_hardware_step, nullptr},
    {"hardware_prompts", false, hardware_prompts_step, nullptr},
    {"save_validation_snapshot", false, save_validation_snapshot_step, "post_validate"},
    {"telemetry", false, telemetry_step, "post_telemetry"},
    {"spoolman_sync", false, spoolman_sync_step, nullptr},
    {"auto_update_check", false, auto_update_check_step, nullptr},
    {"moonraker_update_channel", false, moonraker_update_channel_step, nullptr},
    {"manual_probe_autoopen", false, manual_probe_autoopen_step, nullptr},
};

} // namespace

DiscoveryStepRange discovery_tail_steps() {
    return {std::begin(kTailSteps), std::end(kTailSteps)};
}

} // namespace helix
