// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "i_moonraker_sub_apis.h" // sub-API interfaces returned by the accessors
#include "json_fwd.h"
#include "lvgl.h"             // lv_subject_t
#include "moonraker_client.h" // for helix::ConnectionState, helix::SubscriptionId
#include "moonraker_error.h"
#include "moonraker_types.h" // SafetyLimits, PowerDevice, GcodeStoreEntry

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix {
struct SensorInfo;      // Forward declaration for get_sensors()
struct PlrDetectResult; // plr_backend.h — check_continue_print_state() result
class IMoonrakerClient; // returned by get_client()
class PrinterState;     // returned by printer_state()
class PrinterDiscovery; // returned by hardware()
} // namespace helix

/**
 * @brief Abstract interface for the high-level Moonraker API façade.
 *
 * Production and test consumers depend on this interface rather than the
 * concrete MoonrakerAPI. It is the complete consumer contract: every method a
 * consumer calls on the API façade — temperature/fan/LED control, system
 * control, safety limits, HTTP base URL, connection/subscription proxies, the
 * database and Helix-plugin calls, and the ten sub-API accessors — is declared
 * here. The sub-API accessors return the sub-API interface references
 * (IAdvancedAPI&, IFilesAPI&, ...); the concrete MoonrakerAPI covariantly
 * overrides them to return the concrete sub-APIs, so callers reaching the API
 * through this interface see only the interface surface.
 *
 * MoonrakerAPIMock still inherits the concrete MoonrakerAPI, so it satisfies
 * this interface through the inheritance chain; drift protection
 * (test_interface_drift_moonraker_api.cpp) fails the build if the mock ever
 * stops implementing every pure virtual declared here.
 */
class IMoonrakerAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using BoolCallback = std::function<void(bool)>;
    using StringCallback = std::function<void(const std::string&)>;
    using JsonCallback = std::function<void(const json&)>;

    using PowerDevicesCallback = std::function<void(const std::vector<PowerDevice>&)>;
    using SensorsCallback =
        std::function<void(const std::vector<helix::SensorInfo>&, const nlohmann::json&)>;

    // ========== G-code execute_gcode timeout constants ==========
    // Default (timeout_ms = 0) is MoonrakerRequestTracker's 60s. These are for
    // long-running commands.
    static constexpr uint32_t HOMING_TIMEOUT_MS = 300000;        // 5 min — G28 on large printers
    static constexpr uint32_t AMS_OPERATION_TIMEOUT_MS = 300000; // 5 min — MMU/AFC/tool change ops
    static constexpr uint32_t EXTRUSION_TIMEOUT_MS =
        120000; // 2 min — filament purge/load at slow feedrate
    static constexpr uint32_t MACRO_TIMEOUT_MS =
        300000; // 5 min — user macros can do anything (homing, leveling, filament ops)
    /// 20 min — a pre-print macro chain that the print start must WAIT for.
    /// Creality's BED_MESH_CALIBRATE_START_PRINT homes, wipes, heats the bed to
    /// print temp and then runs a full adaptive mesh; measured at ~10 min on a
    /// warm K2 Plus, and a cold ASA soak to 105C pushes it past MACRO_TIMEOUT_MS.
    /// If even this ceiling expires while the printer still reports busy,
    /// PrintPreparationManager waits for the busy->idle edge instead of failing;
    /// only a printer that never goes idle aborts the start.
    static constexpr uint32_t PRE_START_MACRO_TIMEOUT_MS = 1200000;

    /// Moonraker's default API port. When the HTTP base points here we assume a
    /// "direct to Moonraker" connection, where the webcam is served by a separate
    /// reverse proxy (conventionally nginx on :80) rather than the API port.
    static constexpr int MOONRAKER_DEFAULT_PORT = 7125;

    virtual ~IMoonrakerAPI() = default;

    // ========================================================================
    // Power Device Control
    // ========================================================================

    /// @brief Get list of all configured power devices
    virtual void get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Set power device state ("on", "off", "toggle")
    virtual void set_device_power(const std::string& device, const std::string& action,
                                  SuccessCallback on_success, ErrorCallback on_error) = 0;

    // ========================================================================
    // Sensor Operations
    // ========================================================================

    /// @brief Get list of all configured Moonraker sensors
    virtual void get_sensors(SensorsCallback on_success, ErrorCallback on_error = nullptr) = 0;

    // ========================================================================
    // Connection State
    // ========================================================================

    /// @brief Check if the client is currently connected to Moonraker
    virtual bool is_connected() const = 0;

    /// @brief Get current connection state
    virtual helix::ConnectionState get_connection_state() const = 0;

    /// @brief Get the WebSocket URL used for the current connection
    virtual std::string get_websocket_url() const = 0;

    // ========================================================================
    // Subscriptions and Method Callbacks
    // ========================================================================

    /// @brief Subscribe to status update notifications
    virtual helix::SubscriptionId
    subscribe_notifications(std::function<void(const json&)> callback) = 0;

    /// @brief Unsubscribe from status update notifications
    virtual bool unsubscribe_notifications(helix::SubscriptionId id) = 0;

    /// @brief Register a persistent callback for a specific notification method
    virtual void register_method_callback(const std::string& method, const std::string& name,
                                          std::function<void(const json&)> callback) = 0;

    /// @brief Unregister a method-specific callback
    virtual bool unregister_method_callback(const std::string& method, const std::string& name) = 0;

    /// @brief Temporarily suppress disconnect modal notifications
    virtual void suppress_disconnect_modal(uint32_t duration_ms) = 0;

    /// @brief Retrieve recent G-code commands/responses from Moonraker's store
    virtual void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    std::function<void(const MoonrakerError&)> on_error) = 0;

    // ========================================================================
    // Helix Plugin
    // ========================================================================

    /// @brief Get phase tracking plugin status
    virtual void get_phase_tracking_status(std::function<void(bool enabled)> on_success,
                                           ErrorCallback on_error = nullptr) = 0;

    /// @brief Enable or disable phase tracking plugin
    virtual void set_phase_tracking_enabled(bool enabled,
                                            std::function<void(bool success)> on_success,
                                            ErrorCallback on_error = nullptr) = 0;

    // ========================================================================
    // Moonraker Database
    // ========================================================================

    /// @brief Get a value from Moonraker's database
    virtual void database_get_item(const std::string& namespace_name, const std::string& key,
                                   std::function<void(const json&)> on_success,
                                   ErrorCallback on_error = nullptr) = 0;

    /// @brief Store a value in Moonraker's database
    virtual void database_post_item(const std::string& namespace_name, const std::string& key,
                                    const json& value, std::function<void()> on_success = nullptr,
                                    ErrorCallback on_error = nullptr) = 0;

    /// @brief Get all keys in a namespace. Moonraker returns a JSON object
    ///        mapping key -> value. Empty object if namespace is empty/missing.
    virtual void database_get_namespace(const std::string& namespace_name,
                                        std::function<void(const json&)> on_success,
                                        ErrorCallback on_error = nullptr) = 0;

    /// @brief Delete a key from Moonraker's database. Implementations normalize
    ///        the "missing key" error (Moonraker returns 404 for absent entries)
    ///        into on_success so callers can treat delete as idempotent. on_error
    ///        only fires for genuine failures (network, permission, protocol).
    virtual void database_delete_item(const std::string& namespace_name, const std::string& key,
                                      std::function<void()> on_success = nullptr,
                                      ErrorCallback on_error = nullptr) = 0;

    // ========================================================================
    // Temperature / Fan / LED Control
    // ========================================================================

    /// @brief Set target temperature for a heater
    /// @param caller_surfaces_errors Whether @p on_error actually shows the user
    ///        something. Forwarded to execute_gcode() — see its contract and
    ///        include/rpc_error_policy.h. Pass false when the callback only logs.
    virtual void set_temperature(const std::string& heater, double temperature,
                                 SuccessCallback on_success, ErrorCallback on_error,
                                 bool caller_surfaces_errors = true) = 0;

    /// @brief Set fan speed (0-100)
    virtual void set_fan_speed(const std::string& fan, double speed, SuccessCallback on_success,
                               ErrorCallback on_error) = 0;

    /// @brief Set LED color/brightness
    /// @param on_queued Optional "queued behind a blocking op" disposition — see
    ///        execute_gcode() on MoonrakerAPI for the full contract.
    /// @param caller_surfaces_errors Whether @p on_error actually shows the user
    ///        something. Forwarded to execute_gcode() — see its contract and
    ///        include/rpc_error_policy.h. Pass false when the callback only logs.
    virtual void set_led(const std::string& led, double red, double green, double blue,
                         double white, SuccessCallback on_success, ErrorCallback on_error,
                         SuccessCallback on_queued = nullptr,
                         bool caller_surfaces_errors = true) = 0;

    // ========================================================================
    // System Control
    // ========================================================================

    /// @brief Execute custom G-code command
    /// @param on_queued Optional third disposition, fired when a discretionary
    ///        command was accepted to run behind a blocking op and its RPC
    ///        response was dropped. Runs SYNCHRONOUSLY on the calling thread.
    /// @param caller_surfaces_errors Whether @p on_error actually shows the user
    ///        something. Pass false when it only logs — a spdlog line is not a
    ///        report, and claiming otherwise silences Klipper's `!!` broadcast,
    ///        the surface that would have explained the failure.
    virtual void execute_gcode(const std::string& gcode, SuccessCallback on_success,
                               ErrorCallback on_error, uint32_t timeout_ms = 0, bool silent = false,
                               SuccessCallback on_queued = nullptr,
                               bool caller_surfaces_errors = true) = 0;

    /// @brief Check if a string is safe to use as a G-code parameter
    static bool is_safe_gcode_param(const std::string& str);

    /// @brief Check if a filament material name is safe as a G-code parameter value.
    ///
    /// Deliberately wider than is_safe_gcode_param(): material names legitimately carry
    /// `+`, `-`, `.` and spaces (`PLA+`, `PA6-CF`, `Silk PLA`), and rejecting those made
    /// spool saves silently drop the material. Pair with gcode_param_value().
    static bool is_safe_material_param(const std::string& str);

    /// @brief Render a validated value for interpolation into a G-code command,
    ///        quoting it when it contains whitespace so Klipper's argument tokenizer
    ///        keeps it as one parameter. Values without whitespace pass through unchanged.
    static std::string gcode_param_value(const std::string& value);

    /// @brief Exclude an object from the current print
    virtual void exclude_object(const std::string& object_name, SuccessCallback on_success,
                                ErrorCallback on_error) = 0;

    /// @brief Probe Creality's power-loss recovery snapshot. SIDE-EFFECTFUL —
    ///        call at most once per connection, only in standby, never poll.
    ///        See MoonrakerAPI for the full contract.
    virtual void
    check_continue_print_state(std::function<void(const helix::PlrDetectResult&)> on_result,
                               ErrorCallback on_error) = 0;

    /// @brief Discard the Creality power-loss recovery snapshot.
    virtual void cancel_continue_print(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Emergency stop
    virtual void emergency_stop(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Restart Klipper firmware
    virtual void restart_firmware(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Restart Klipper host process
    virtual void restart_klipper(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Restart a system service via Moonraker's machine.services.restart
    virtual void restart_service(const std::string& service_name, SuccessCallback on_success,
                                 ErrorCallback on_error) = 0;

    /// @brief Restart the Moonraker service
    virtual void restart_moonraker(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Shut down the host machine
    virtual void machine_shutdown(SuccessCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Reboot the host machine
    virtual void machine_reboot(SuccessCallback on_success, ErrorCallback on_error) = 0;

    // ========================================================================
    // Query Operations
    // ========================================================================

    /// @brief Query if printer is ready for commands
    virtual void is_printer_ready(BoolCallback on_result, ErrorCallback on_error) = 0;

    /// @brief Get current print state
    virtual void get_print_state(StringCallback on_result, ErrorCallback on_error) = 0;

    // ========================================================================
    // Safety Limits Configuration
    // ========================================================================

    /// @brief Set safety limits explicitly (overrides auto-detection)
    virtual void set_safety_limits(const SafetyLimits& limits) = 0;

    /// @brief Get current safety limits
    virtual const SafetyLimits& get_safety_limits() const = 0;

    /// @brief Update safety limits from printer configuration via Moonraker API
    virtual void update_safety_limits_from_printer(SuccessCallback on_success,
                                                   ErrorCallback on_error) = 0;

    /// @brief Query the printer's configfile object
    virtual void query_configfile(JsonCallback on_success, ErrorCallback on_error) = 0;

    // ========================================================================
    // HTTP Base URL Configuration
    // ========================================================================

    /// @brief Set the HTTP base URL for file transfers and REST operations
    virtual void set_http_base_url(const std::string& base_url) = 0;

    /// @brief Get the current HTTP base URL
    virtual const std::string& get_http_base_url() const = 0;

    /// @brief Resolve a relative webcam URL against the web frontend base
    virtual void resolve_webcam_url(std::string& url) = 0;

    /// @brief Ensure HTTP base URL is set, auto-deriving from WebSocket if needed
    virtual bool ensure_http_base_url() = 0;

    // ========================================================================
    // Connection Proxies (client lifetime)
    // ========================================================================

    /// @brief Get client lifetime guard (for SubscriptionGuard safety)
    virtual std::weak_ptr<bool> client_lifetime_weak() const = 0;

    // ========================================================================
    // Internal Access
    // ========================================================================

    /// @brief Get reference to underlying Moonraker client (transport layer)
    virtual helix::IMoonrakerClient& get_client() = 0;

    /// @brief Get const reference to discovered hardware
    virtual const helix::PrinterDiscovery& hardware() const = 0;

    /// @brief Get non-const reference to hardware for internal updates
    virtual helix::PrinterDiscovery& hardware() = 0;

    /// @brief Get build volume version subject for change notifications
    virtual lv_subject_t* get_build_volume_version_subject() = 0;

    /// @brief Notify that build_volume has changed
    virtual void notify_build_volume_changed() = 0;

    /// @brief Access the PrinterState this API reads/writes
    virtual helix::PrinterState& printer_state() = 0;

    // ========================================================================
    // Sub-API Accessors (Delegated) — return interface references; concrete
    // MoonrakerAPI covariantly overrides these to return the concrete sub-APIs.
    // ========================================================================

    virtual IAdvancedAPI& advanced() = 0;
    virtual ITransfersAPI& transfers() = 0;
    virtual IHistoryAPI& history() = 0;
    virtual IJobAPI& job() = 0;
    virtual ITimelapseAPI& timelapse() = 0;
    virtual IMotionAPI& motion() = 0;
    virtual IRestAPI& rest() = 0;
    virtual ISpoolmanAPI& spoolman() = 0;
    virtual IFilesAPI& files() = 0;
    virtual IQueueAPI& queue() = 0;
};
