// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_client_mock.h"

#include <functional>
#include <string>
#include <unordered_map>

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

// Bed dimensions (mm)
constexpr double MOCK_BED_X_MIN = 0.0;
constexpr double MOCK_BED_X_MAX = 250.0;
constexpr double MOCK_BED_Y_MIN = 0.0;
constexpr double MOCK_BED_Y_MAX = 250.0;
constexpr double MOCK_BED_Z_MAX = 300.0;

// Probe margins - typical probes can't reach bed edges
constexpr double MOCK_PROBE_MARGIN = 15.0;

// Bed screw thread advertised in configfile.settings.screws_tilt_adjust.
// MockScrewsTiltState derives its adjustment strings from this same value, so
// the mock's SCREWS_TILT_CALCULATE output agrees with the config it reports.
constexpr const char* MOCK_SCREW_THREAD = "CW-M3";

// Derived mesh bounds (bed size minus probe margins)
constexpr double MOCK_MESH_X_MIN = MOCK_BED_X_MIN + MOCK_PROBE_MARGIN;
constexpr double MOCK_MESH_X_MAX = MOCK_BED_X_MAX - MOCK_PROBE_MARGIN;
constexpr double MOCK_MESH_Y_MIN = MOCK_BED_Y_MIN + MOCK_PROBE_MARGIN;
constexpr double MOCK_MESH_Y_MAX = MOCK_BED_Y_MAX - MOCK_PROBE_MARGIN;

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
using MethodHandler = std::function<bool(MoonrakerClientMock* self, const json& params,
                                         std::function<void(const json&)> success_cb,
                                         std::function<void(const MoonrakerError&)> error_cb)>;

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
 * Single source of truth for populate_capabilities(), discover_printer() and
 * the objects.query / objects.subscribe handlers, which previously carried
 * separate copies and disagreed: the query handler omitted it entirely.
 */
json get_mock_accel_config();

/**
 * @brief Get the mock probe's configfile.config section
 *
 * Keyed off HELIX_MOCK_PROBE_TYPE — the same variable that picks the probe
 * object in populate_capabilities() and the probe status in
 * dispatch_initial_state() — so all three stay in step. Returns an empty object
 * for "none".
 *
 * Values are STRINGS, matching Klipper: configfile.config is the verbatim
 * printer.cfg text, and ProbeSensorManager::discover_from_config() parses
 * z_offset with std::stof.
 *
 * The "loadcell" profile deliberately disagrees with the status payload, which
 * reports z_offset: null. That is the flashforge_loadcell shape the config
 * seeding exists for, and it is the only profile where the seeded value is
 * observably different from what a status update would have produced.
 */
json get_mock_probe_config();

/**
 * @brief Get mock Happy Hare "mmu" status (--real-ams)
 *
 * Minimal static 4-gate setup with a mix of loaded/empty gates. The only
 * reachable caller today is MoonrakerClientMock's post-discovery dispatch
 * that seeds AmsBackendHappyHare's initial state (moonraker_client_mock.cpp).
 * The objects.query and printer.objects.subscribe handlers in
 * moonraker_client_mock_objects.cpp also call this for a requested "mmu"
 * object, but neither is reached in practice: AmsBackendHappyHare's
 * printer.objects.query sites all request "configfile", never "mmu", and
 * printer.objects.subscribe is issued only by MoonrakerDiscoverySequence,
 * which MoonrakerClientMock::discover_printer() overrides and never calls.
 * Those branches are reserved for a future caller, not dead code to remove.
 */
json get_mock_mmu_status();

} // namespace mock_internal
