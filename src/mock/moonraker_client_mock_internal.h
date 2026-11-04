// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_shared_callback.h"
#include "moonraker_client_mock.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file moonraker_client_mock_internal.h
 * @brief Internal types and handler registry for MoonrakerClientMock
 *
 * This header defines the method handler function type and registration
 * functions for domain-specific mock handlers. It's used internally by
 * the mock implementation modules and should not be included by external code.
 */

namespace mock_internal {

// ============================================================================
// Mock Printer Configuration Constants
// ============================================================================

// Probe margins - typical probes can't reach bed edges
constexpr double kMockProbeMargin = 15.0;

// Bed screw thread advertised in configfile.settings.screws_tilt_adjust.
// MockScrewsTiltState derives its adjustment strings from this same value, so
// the mock's SCREWS_TILT_CALCULATE output agrees with the config it reports.
constexpr const char* kMockScrewThread = "CW-M3";

// The K2 chamber thermostat's configured resting target (its configfile
// target_temp, captured live in docs/devel/CHAMBER_HEATER.md's verification
// log). The fan reports this target whenever no M141 set is live, and the
// configfile answer names it so the discovery-side resting read resolves the
// same value the frames carry.
constexpr double kMockK2ChamberFanRestingC = 35.0;

/// Area a probe can reach: the persona's bed inset by kMockProbeMargin.
struct MeshBounds {
    double x_min;
    double x_max;
    double y_min;
    double y_max;
};

[[nodiscard]] inline MeshBounds mesh_bounds(helix::mock::PrinterType type) {
    const auto d = helix::mock::descriptor(type);
    return {d.axis_min.x + kMockProbeMargin, d.axis_max.x - kMockProbeMargin,
            d.axis_min.y + kMockProbeMargin, d.axis_max.y - kMockProbeMargin};
}

/// toolhead axis_minimum/maximum as Klipper reports them: one value per axis
/// plus a zero for the extruder slot. Every toolhead block the mock serves
/// builds them here, so the copies cannot drift apart again.
[[nodiscard]] inline std::array<double, 4>
toolhead_axis_minimum(const helix::mock::PersonaDescriptor& persona) {
    return {persona.axis_min.x, persona.axis_min.y, persona.axis_min.z, 0.0};
}

[[nodiscard]] inline std::array<double, 4>
toolhead_axis_maximum(const helix::mock::PersonaDescriptor& persona) {
    return {persona.axis_max.x, persona.axis_max.y, persona.axis_max.z, 0.0};
}

/**
 * @brief Type for method handler functions
 *
 * Handlers process a specific JSON-RPC method call and invoke either
 * the success or error callback.
 *
 * @param self Pointer to MoonrakerClientMock instance
 * @param params JSON parameters from the RPC call
 * @param success_cb Success callback to invoke with result
 * @param error_cb Error callback to invoke on failure
 * @return true if the handler recognized and processed the method, false otherwise
 */
using MethodHandler =
    std::function<bool(MoonrakerClientMock* self, const json& params,
                       helix::SharedCallback<void(const json&)> success_cb,
                       helix::SharedCallback<void(const MoonrakerError&)> error_cb)>;

/**
 * @brief Register file-related method handlers
 *
 * Registers handlers for:
 * - server.files.list
 * - server.files.get_directory
 * - server.files.metadata
 * - server.files.metascan
 * - server.files.get_file
 * - server.files.delete
 * - server.files.delete_file
 * - server.files.move
 * - server.files.copy
 * - server.files.post_directory
 * - server.files.delete_directory
 *
 * @param registry Map to register handlers into
 */
void register_file_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register print control method handlers
 *
 * Registers handlers for:
 * - printer.print.start
 * - printer.print.pause
 * - printer.print.resume
 * - printer.print.cancel
 * - printer.gcode.script
 *
 * @param registry Map to register handlers into
 */
void register_print_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register object query method handlers
 *
 * Registers handlers for:
 * - printer.objects.list
 * - printer.objects.query
 *
 * @param registry Map to register handlers into
 */
void register_object_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register history method handlers
 *
 * Registers handlers for:
 * - server.history.list
 * - server.history.totals
 * - server.history.delete_job
 *
 * @param registry Map to register handlers into
 */
void register_history_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register server method handlers
 *
 * Registers handlers for:
 * - server.connection.identify
 * - server.info
 * - printer.info
 * - server.config (job_queue.automatic_transition; env-overridable)
 * - server.database.get_item / post_item / delete_item (the client's own store)
 *
 * @param registry Map to register handlers into
 */
void register_server_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register job queue method handlers
 *
 * Registers handlers for:
 * - server.job_queue.status
 * - server.job_queue.start
 * - server.job_queue.pause
 * - server.job_queue.post_job
 * - server.job_queue.delete_job
 *
 * @param registry Map to register handlers into
 */
void register_queue_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Register the HelixPrint plugin's power-loss handlers
 *
 * server.helix.plr.status / plan / prepare / commit / abort / discard, answered
 * by MoonrakerClientMock::mock_plr_request() while HELIX_MOCK_PLR is set.
 *
 * @param registry Map to register handlers into
 */
// NAMESPACE_OK: mock_internal handler registry, alongside its sibling registrars
void register_plr_handlers(std::unordered_map<std::string, MethodHandler>& registry);

/**
 * @brief Get mock gcode_macro configfile entries
 *
 * Returns a JSON object with gcode_macro entries (lowercase keys) for the
 * mock configfile.config response. Single source of truth used by both
 * populate_capabilities() and the objects.query/subscribe handlers.
 */
json get_mock_gcode_macro_config();

/**
 * @brief Get mock accelerometer configfile entries
 *
 * Accelerometer modules have no get_status(), so Klipper never lists them in
 * printer.objects.list — configfile.config is the only place they appear.
 * Single source of truth for populate_capabilities() and the objects.query /
 * objects.subscribe handlers.
 */
json get_mock_accel_config();

/**
 * @brief The probe profile the mock reports for a persona
 *
 * HELIX_MOCK_PROBE_TYPE when set. Otherwise "cartographer" for a persona that
 * inherits the default probe, else the persona's own probe
 * (PersonaDescriptor::probe), else "none".
 */
std::string mock_probe_type(helix::mock::PrinterType type);

/**
 * @brief Get the mock probe's configfile.config section
 *
 * Keyed off mock_probe_type() — the same profile that picks the probe objects
 * in populate_capabilities() and the probe status in dispatch_initial_state()
 * — so all three stay in step. Returns an empty object for "none".
 *
 * Values are STRINGS, matching Klipper: configfile.config is the verbatim
 * printer.cfg text, and ProbeSensorManager seeds its Z offset from the section,
 * parsing z_offset as a string.
 *
 * The "loadcell" profile deliberately disagrees with the status payload, which
 * reports z_offset: null. That is the flashforge_loadcell shape the config
 * seeding exists for, and it is the only profile where the seeded value is
 * observably different from what a status update would have produced.
 */
json get_mock_probe_config(helix::mock::PrinterType type);

/**
 * @brief Get mock Happy Hare "mmu" status (--real-ams)
 *
 * Minimal static 4-gate setup with a mix of loaded/empty gates, answered for a
 * requested "mmu" object by the objects.query and printer.objects.subscribe
 * handlers. The discovery sequence's subscription delivers it to
 * AmsBackendHappyHare as the initial state.
 */
json get_mock_mmu_status();

} // namespace mock_internal

namespace helix::sim {
/**
 * @brief A filament sensor's status fields as HELIX_MOCK_FILAMENT_STATE sets them
 *
 * The variable is comma-separated `name:state` pairs keyed on the sensor's short
 * name; `state` is `detected` (the default), `empty`, or `disabled` (stood down in
 * Klipper, `enabled: false`). Both the initial status frame and printer.objects.subscribe
 * answer from this, so the two cannot disagree.
 */
struct MockFilamentSensorEnvState {
    bool filament_detected = true;
    bool enabled = true;
};
MockFilamentSensorEnvState mock_filament_sensor_env_state(const std::string& short_name);

/**
 * @brief Extra macro names from HELIX_MOCK_MACRO_COUNT=N (MOCK_MACRO_000 ...)
 *
 * Empty when the variable is unset. Lets list-scale macro UI run against a
 * long list; get_mock_gcode_macro_config() describes every third one.
 */
std::vector<std::string> mock_padded_macro_names();

/**
 * @brief Get the mock probe's printer objects and their status, keyed by object
 *
 * Keyed off mock_internal::mock_probe_type(type). The keys are the objects the probe registers
 * (a module that also claims the generic "probe" object lists both), and each
 * value is the full get_status() payload that module returns, per the per-type
 * table in docs/devel/SENSOR_MANAGEMENT.md. Empty for "none".
 */
nlohmann::json mock_probe_status(helix::mock::PrinterType type);

} // namespace helix::sim
