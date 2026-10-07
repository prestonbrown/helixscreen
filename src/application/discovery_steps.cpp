// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file discovery_steps.cpp
 * @brief The ordered reactions to a completed Moonraker discovery.
 */

#include "discovery_steps.h"

#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_filament.h"
#include "ui_settings_about.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "hardware_role_registry.h"
#include "light_button_config.h"
#include "power_device_state.h"
#include "printer_detector.h"
#include "printer_fan_state.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "sensor_state.h"
#include "settings_manager.h"
#include "spoolman_active_spool_sync.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "temp_graph_controller.h"
#include "thermal_rate_model.h"
#include "timelapse_state.h"
#include "wizard_step_logic.h"
#include "z_offset_persistence.h"

#include <spdlog/spdlog.h>

using namespace helix;

namespace helix {

bool discovery_print_active(bool subject_active, const nlohmann::json& status) {
    return subject_active || PrinterPrintState::status_indicates_active_print(status);
}

namespace {

// The app started successfully: clear the self-update sentinel and listen for Moonraker
// finishing an update of HelixScreen.
void update_checker_connected_step(DiscoveryContext& ctx) {
    UpdateChecker::instance().on_connected(ctx.client);
}

// Move the snapshot into PrinterState (by-value param) so no hash-table copy iterates
// against a live, potentially-mutated api.hardware_ (#799). After this the snapshot is
// empty: read ctx.hw.
void set_hardware_step(DiscoveryContext& ctx) {
    crash_handler::breadcrumb::note("disc", "pre_set_hw",
                                    static_cast<long>(ctx.snapshot.macros().size()));
    get_printer_state().set_hardware(std::move(ctx.snapshot));
    crash_handler::breadcrumb::note("disc", "post_set_hw", ctx.n);
    const auto& fans = ctx.hw.fans();
    get_printer_state().fan_state().init_fans(
        fans, FanRoleConfig::from_config(Config::get_instance(), fans), ctx.hw.fan_max_power());
    crash_handler::breadcrumb::note("disc", "post_init_fans", static_cast<long>(fans.size()));
}

// Turn on the firmware's own z-offset persistence, at most once per printer and only
// when idle. Some firmwares store the offset themselves and re-apply it at print start
// only when their own setting says to, so with that setting off an adjustment made here
// does not survive. Whether to send, what to send, and recording that it went out all
// live behind claim_persistence_enable() in include/z_offset_persistence.h.
void zoffset_persistence_step(DiscoveryContext& ctx) {
    const std::string enable_gcode = zoffset::persistence_enable_gcode(ctx.hw);
    if (enable_gcode.empty() || !zoffset::claim_persistence_enable(Config::get_instance(), ctx.hw,
                                                                   &ctx.status, ctx.print_active)) {
        return;
    }
    spdlog::info("[ZOffset] Enabling firmware z-offset persistence ({})",
                 zoffset::persistence_provider_name(ctx.hw));
    // Fire-and-forget: callbacks are LOG-ONLY and capture nothing that
    // can dangle, so the background response thread is lifetime-safe.
    ctx.api.execute_gcode(
        enable_gcode, []() { spdlog::info("[ZOffset] Firmware z-offset persistence enabled"); },
        [](const MoonrakerError& err) {
            spdlog::warn("[ZOffset] Failed to enable z-offset persistence: {}", err.message);
            // The claim was recorded before the send so a second
            // discovery could not inject the same gcode. It did not
            // land, so hand the one shot back or this printer is
            // never told for the life of the install. Marshalled:
            // this runs on the response thread and Config is not
            // synchronised.
            ui::queue_update("zoffset_release_claim",
                             []() { zoffset::release_persistence_enable(Config::get_instance()); });
        },
        0, /*silent=*/true, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

// Seed temperature graphs from Moonraker's cached history. Fired after init_fans so
// heater/sensor subjects exist.
void temp_graph_seed_step(DiscoveryContext& ctx) {
    TempGraphController::seed_from_moonraker(ctx.client);
}

// Dispatch initial subscription status AFTER init_fans so fan/sensor subjects exist when
// the status data is processed. The initial status is passed from the discovery sequence
// rather than dispatched separately to guarantee ordering. Flagged as a cached snapshot:
// it was captured on the background thread when the subscribe response landed and has
// been carried through the rest of discovery, so it can be seconds stale by the time it
// lands here. Live WebSocket frames have been updating the same state the whole time, so
// this replay must not walk a liveness signal (klippy state) backwards.
void status_dispatch_step(DiscoveryContext& ctx) {
    if (!ctx.status.empty()) {
        ctx.client.dispatch_status_update(ctx.status, /*from_cached_snapshot=*/true);
    }
}

void software_versions_step(DiscoveryContext& ctx) {
    get_printer_state().set_klipper_version(ctx.hw.software_version());
    get_printer_state().set_moonraker_version(ctx.hw.moonraker_version());
    if (!ctx.hw.os_version().empty()) {
        get_printer_state().set_os_version(ctx.hw.os_version());
    }
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

// Auto-detect printer type if not already set (e.g., fresh install with preset). MUST run
// BEFORE HardwareValidator::validate: otherwise the validator checks the scaffolded
// defaults (fans/part="fan", fans/hotend="heater_fan hotend_fan") against the discovered
// hardware and flags them as missing, even though the preset about to be applied would
// map those slots to the correct device-specific names. Runs even while the wizard is
// active so the preset lands BEFORE the wizard hits its connection / printer-identify
// steps: that is the only path for preset_mode to become true on a fresh install of a
// known printer. auto_detect_and_save self-guards on PRINTER_TYPE already being set, so a
// user's manual pick in the identify step won't be overwritten by a later discovery.
// Reads ctx.hw: the snapshot was moved away, and detection on it would see "0 sensors,
// 0 fans, hostname ''" (#802).
void auto_detect_printer_step(DiscoveryContext& ctx) {
    PrinterDetector::auto_detect_and_save(ctx.hw, Config::get_instance());
}

// Auto-heal + persist heater roles (batched single save, symmetry with fan roles above).
// Ensures the validator sees resolved heater names so it does not emit a toast every boot
// for a stale saved role that has a confident replacement.
void heal_heater_roles_step(DiscoveryContext& ctx) {
    heal_heater_roles(Config::get_instance(), ctx.hw.heaters());
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
    ctx.prompter.run_discovery_prompts(ctx.hw, ctx.hw_changed, ctx.print_active,
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

// Fetch safety limits and build volume from Klipper config (stepper ranges,
// min_extrude_temp, max_temp, etc.). Runs for ALL discovery completions (normal startup
// AND post-wizard) so callers need not duplicate it.
void safety_limits_step(DiscoveryContext& ctx) {
    IMoonrakerAPI* api_ptr = &ctx.api;
    api_ptr->update_safety_limits_from_printer(
        [api_ptr]() {
            // A copy: the panel reads it later, on the main thread.
            const SafetyLimits limits = api_ptr->get_safety_limits();

            ui::queue_update("discovery_steps::safety_limits_step", [limits]() {
                get_global_filament_panel().set_limits(limits);
                spdlog::debug("[Application] Safety limits propagated to panels");
            });

            // Apply archetype-based thermal rate defaults using build volume
            // Must marshal to main thread — runs in JSONRPC response callback
            float bed_x_max = api_ptr->hardware().build_volume().x_max;
            ui::queue_update("discovery_steps::safety_limits_step", [bed_x_max]() {
                ThermalRateManager::instance().apply_archetype_defaults(
                    bed_x_max, get_printer_state().profile_state().printer_type());
            });

            // Record hardware profile after build volume is populated
            TelemetryManager::instance().record_hardware_profile();
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Application] Failed to fetch safety limits: {}", err.message);
            // Record hardware profile anyway, just without build volume
            TelemetryManager::instance().record_hardware_profile();
        });
}

// Detect helix_print plugin during discovery (not UI-initiated) so plugin status is known
// early for UI gating.
void helix_plugin_check_step(DiscoveryContext& ctx) {
    ctx.api.job().check_helix_plugin(
        [](bool available) { get_printer_state().set_helix_plugin_installed(available); },
        [](const MoonrakerError&) {
            // Silently treat errors as "plugin not installed"
            get_printer_state().set_helix_plugin_installed(false);
        });
}

// Mirror Moonraker's active Spoolman spool onto the external slot and the active tool,
// now and whenever it changes.
void spoolman_sync_step(DiscoveryContext& ctx) {
    spoolman_sync::attach(ctx.client, ctx.api.spoolman());
}

// Fetch job queue now that WebSocket is actually connected.
void job_queue_fetch_step(DiscoveryContext& ctx) {
    if (ctx.job_queue_state) {
        ctx.job_queue_state->fetch();
    }
}

void settle_light_buttons_step(DiscoveryContext&) {
    settle_light_buttons();
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

constexpr DiscoveryStep kSteps[] = {
    {"update_checker_connected", false, update_checker_connected_step, nullptr},
    {"set_hardware", false, set_hardware_step, nullptr},
    {"zoffset_persistence", false, zoffset_persistence_step, nullptr},
    {"temp_graph_seed", false, temp_graph_seed_step, nullptr},
    {"status_dispatch", false, status_dispatch_step, "post_status_dispatch"},
    {"software_versions", false, software_versions_step, nullptr},
    {"about_print_hours", false, about_print_hours_step, nullptr},
    {"timelapse_events", false, timelapse_events_step, nullptr},
    {"power_sensor_subscribe", false, power_sensor_subscribe_step, "post_subscribe"},
    {"auto_detect_printer", true, auto_detect_printer_step, nullptr},
    {"heal_heater_roles", true, heal_heater_roles_step, nullptr},
    {"acknowledge_deferred_hardware", false, acknowledge_deferred_hardware_step, nullptr},
    {"validate_hardware", false, validate_hardware_step, nullptr},
    {"hardware_prompts", false, hardware_prompts_step, nullptr},
    {"save_validation_snapshot", false, save_validation_snapshot_step, "post_validate"},
    {"telemetry", false, telemetry_step, "post_telemetry"},
    {"safety_limits", false, safety_limits_step, nullptr},
    {"helix_plugin_check", false, helix_plugin_check_step, nullptr},
    {"spoolman_sync", false, spoolman_sync_step, nullptr},
    {"job_queue_fetch", false, job_queue_fetch_step, nullptr},
    {"settle_light_buttons", false, settle_light_buttons_step, nullptr},
    {"auto_update_check", false, auto_update_check_step, nullptr},
    {"moonraker_update_channel", false, moonraker_update_channel_step, nullptr},
    {"manual_probe_autoopen", false, manual_probe_autoopen_step, nullptr},
};

} // namespace

DiscoveryStepRange discovery_steps() {
    return {std::begin(kSteps), std::end(kSteps)};
}

void run_discovery_steps(DiscoveryStepRange steps, DiscoveryContext& ctx) {
    for (const DiscoveryStep& step : steps) {
        if (!step.only_when_hw_changed || ctx.hw_changed) {
            step.run(ctx);
        }
        if (step.breadcrumb) {
            crash_handler::breadcrumb::note("disc", step.breadcrumb, ctx.n);
        }
    }
}

void run_discovery_steps(DiscoveryContext& ctx) {
    run_discovery_steps(discovery_steps(), ctx);
}

} // namespace helix
