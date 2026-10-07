// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file discovery_steps_core.cpp
 * @brief The reactions to a completed Moonraker discovery that every build runs, the ESP32
 *        firmware included.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "discovery_steps.h"
#include "hardware_role_registry.h"
#include "light_button_config.h"
#include "printer_detector.h"
#include "printer_fan_state.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "temp_graph_controller.h"
#include "thermal_rate_model.h"
#include "z_offset_persistence.h"

#include <spdlog/spdlog.h>

namespace helix {

bool discovery_print_active(bool subject_active, const nlohmann::json& status) {
    return subject_active || PrinterPrintState::status_indicates_active_print(status);
}

namespace {

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

// Fetch job queue now that WebSocket is actually connected.
void job_queue_fetch_step(DiscoveryContext& ctx) {
    if (ctx.job_queue_state) {
        ctx.job_queue_state->fetch();
    }
}

void settle_light_buttons_step(DiscoveryContext&) {
    settle_light_buttons();
}

constexpr DiscoveryStep kCoreSteps[] = {
    {"set_hardware", false, set_hardware_step, nullptr},
    {"zoffset_persistence", false, zoffset_persistence_step, nullptr},
    {"temp_graph_seed", false, temp_graph_seed_step, nullptr},
    {"status_dispatch", false, status_dispatch_step, "post_status_dispatch"},
    {"software_versions", false, software_versions_step, nullptr},
    {"auto_detect_printer", true, auto_detect_printer_step, nullptr},
    {"heal_heater_roles", true, heal_heater_roles_step, nullptr},
    {"safety_limits", false, safety_limits_step, nullptr},
    {"helix_plugin_check", false, helix_plugin_check_step, nullptr},
    {"job_queue_fetch", false, job_queue_fetch_step, nullptr},
    {"settle_light_buttons", false, settle_light_buttons_step, nullptr},
};

} // namespace

DiscoveryStepRange discovery_core_steps() {
    return {std::begin(kCoreSteps), std::end(kCoreSteps)};
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

} // namespace helix
