// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_api.h
 * @brief MoonrakerAPI - Domain Operations Layer
 *
 * ## Responsibilities
 *
 * - High-level printer operations (print, move, heat, home, etc.)
 * - Input validation and safety checks (temperature limits, movement bounds)
 * - HTTP file upload/download (G-code files, thumbnails, config)
 * - Response parsing and error handling
 * - Domain-specific callbacks (progress, completion, errors)
 * - Bed mesh operations (delegating to MoonrakerClient for storage)
 * - Print history and timelapse management
 * - Spoolman filament tracking integration
 *
 * ## NOT Responsible For
 *
 * - WebSocket connection management (done by MoonrakerClient)
 * - JSON-RPC protocol details (done by MoonrakerClient)
 * - Hardware discovery (done by MoonrakerClient)
 * - Raw subscription handling (done by MoonrakerClient)
 *
 * ## Architecture Notes
 *
 * MoonrakerAPI is the domain layer that provides a clean, high-level interface
 * for printer operations. It uses MoonrakerClient for transport (WebSocket
 * communication) and adds:
 *
 * - Safety validation (temperature limits, movement bounds)
 * - HTTP file transfers (multipart uploads, range downloads)
 * - Response transformation (JSON -> domain types)
 * - Error handling with domain-specific error types
 *
 * Application code should prefer MoonrakerAPI for all printer interactions.
 * Direct MoonrakerClient access should only be needed for low-level operations
 * like custom G-code execution or subscription management.
 *
 * @see MoonrakerClient for transport layer details
 * @see SafetyLimits for input validation configuration
 */

#pragma once

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "moonraker_advanced_api.h"
#include "moonraker_client.h"
#include "moonraker_error.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"
#include "moonraker_history_api.h"
#include "moonraker_job_api.h"
#include "moonraker_motion_api.h"
#include "moonraker_queue_api.h"
#include "moonraker_rest_api.h"
#include "moonraker_spoolman_api.h"
#include "moonraker_timelapse_api.h"
#include "moonraker_types.h"
#include "print_history_data.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace helix {
struct SensorInfo;      // Forward declaration for get_sensors()
struct PlrDetectResult; // plr_backend.h — check_continue_print_state() result
} // namespace helix

/**
 * @brief High-level Moonraker API facade
 *
 * Provides simplified, domain-specific operations on top of MoonrakerClient.
 * All methods are asynchronous with success/error callbacks.
 */
class MoonrakerAPI : public IMoonrakerAPI {
  public:
    // G-code execute_gcode timeout constants (HOMING_TIMEOUT_MS,
    // AMS_OPERATION_TIMEOUT_MS, EXTRUSION_TIMEOUT_MS, MACRO_TIMEOUT_MS) and
    // MOONRAKER_DEFAULT_PORT are inherited from IMoonrakerAPI.

    // Callback typedefs (SuccessCallback, ErrorCallback, BoolCallback,
    // StringCallback, JsonCallback, PowerDevicesCallback, SensorsCallback)
    // are inherited from IMoonrakerAPI.

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param state PrinterState instance (must remain valid during API lifetime)
     */
    MoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state);
    virtual ~MoonrakerAPI();

    // ========================================================================
    // Temperature Control Operations
    // ========================================================================

    /**
     * @brief Set target temperature for a heater
     *
     * @param heater Heater name ("extruder", "heater_bed", etc.)
     * @param temperature Target temperature in Celsius
     * @param on_success Success callback
     * @param on_error Error callback
     * @param caller_surfaces_errors Whether @p on_error actually SHOWS the user
     *        something. Forwarded verbatim to execute_gcode(); false when the
     *        callback only logs. Default must match IMoonrakerAPI's — default
     *        arguments on virtuals resolve statically.
     */
    void set_temperature(const std::string& heater, double temperature, SuccessCallback on_success,
                         ErrorCallback on_error, bool caller_surfaces_errors = true) override;

    /**
     * @brief Set fan speed
     *
     * @param fan Fan name ("fan", "fan_generic cooling_fan", etc.)
     * @param speed Speed percentage (0-100)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_fan_speed(const std::string& fan, double speed, SuccessCallback on_success,
                       ErrorCallback on_error) override;

    /**
     * @brief Set LED color/brightness
     *
     * Controls LED output by name. For simple on/off control, use brightness 1.0 or 0.0.
     * Supports neopixel, dotstar, led, and pca9632 LED types.
     *
     * @param led LED name (e.g., "neopixel chamber_light", "led status_led")
     * @param red Red component (0.0-1.0)
     * @param green Green component (0.0-1.0)
     * @param blue Blue component (0.0-1.0)
     * @param white Optional white component for RGBW LEDs (0.0-1.0, default 0.0)
     * @param on_success Success callback
     * @param on_error Error callback
     * @param on_queued Optional "queued behind a blocking op" disposition — see
     *        execute_gcode() for the full contract. SET_LED is discretionary, so
     *        this is the LED path's only settle signal while Klipper's gcode lock
     *        is held.
     * @param caller_surfaces_errors Whether @p on_error actually shows the user
     *        something. Forwarded to execute_gcode() — see its contract and
     *        include/rpc_error_policy.h. Pass false when the callback only logs.
     *        Default must stay in sync with IMoonrakerAPI's declaration: default
     *        arguments on virtuals resolve from the static type.
     */
    void set_led(const std::string& led, double red, double green, double blue, double white,
                 SuccessCallback on_success, ErrorCallback on_error,
                 SuccessCallback on_queued = nullptr, bool caller_surfaces_errors = true) override;

    // ========================================================================
    // Power Device Control Operations
    // ========================================================================

    // PowerDevice is defined in moonraker_types.h.
    // PowerDevicesCallback is inherited from IMoonrakerAPI.

    /**
     * @brief Get list of all configured power devices
     *
     * Queries Moonraker's /machine/device_power/devices endpoint
     *
     * @param on_success Callback with list of power devices
     * @param on_error Error callback
     */
    void get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Set power device state
     *
     * @param device Device name
     * @param action Action to perform ("on", "off", "toggle")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_device_power(const std::string& device, const std::string& action,
                          SuccessCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Sensor Operations
    // ========================================================================

    // SensorsCallback is inherited from IMoonrakerAPI.

    /**
     * @brief Get list of all configured Moonraker sensors
     *
     * Queries server.sensors.list endpoint for sensor metadata and initial values.
     *
     * @param on_success Callback with sensor list and initial values JSON
     * @param on_error Error callback
     */
    void get_sensors(SensorsCallback on_success, ErrorCallback on_error = nullptr) override;

    // ========================================================================
    // System Control Operations
    // ========================================================================

    /**
     * @brief Execute custom G-code command
     *
     * The command is sent VERBATIM — HelixScreen appends nothing to it. See
     * src/api/moonraker_gcode_guards.h and tests/unit/test_gcode_verbatim.cpp.
     *
     * @param gcode G-code command string
     * @param on_success Success callback
     * @param on_error Error callback
     * @param timeout_ms Request timeout (0 = default)
     * @param silent Suppress warning logs / error toasts on refusal
     * @param on_queued Optional THIRD disposition, distinct from success and error.
     *        Fires when a benign discretionary command (fan/temp/LED) was handed to
     *        Klipper to run behind a blocking op and its RPC response was
     *        deliberately dropped, so neither @p on_success nor @p on_error will
     *        ever fire. It means "accepted for later execution", NOT "done" —
     *        opt in only to release a caller-side in-flight counter, never to
     *        report completion. Callers that omit it get nothing on that path.
     *
     *        THREADING: unlike @p on_success / @p on_error, which arrive on the
     *        libhv response thread, @p on_queued runs SYNCHRONOUSLY on the calling
     *        thread — typically the main thread inside an LVGL LV_EVENT_CLICKED
     *        frame. A handler here must therefore obey the input-handler rules:
     *        no synchronous widget deletion (lv_obj_delete / lv_obj_clean /
     *        safe_delete); use the deferred forms. See CLAUDE.md § Threading.
     * @param caller_surfaces_errors Whether @p on_error actually SHOWS the user
     *        something. Pass false when it only logs: a spdlog line is not a
     *        user-visible report, and letting it claim ownership of the report
     *        suppresses Klipper's `!!` broadcast for the same failure — the
     *        surface that would have explained it. See rpc_error_policy.h.
     */
    void execute_gcode(const std::string& gcode, SuccessCallback on_success, ErrorCallback on_error,
                       uint32_t timeout_ms = 0, bool silent = false,
                       SuccessCallback on_queued = nullptr,
                       bool caller_surfaces_errors = true) override;

    // is_safe_gcode_param() is a static utility declared on IMoonrakerAPI
    // (inherited here) so consumers can validate without the concrete type.

    // ========================================================================
    // Object Exclusion Operations
    // ========================================================================

    /**
     * @brief Exclude an object from the current print
     *
     * Sends EXCLUDE_OBJECT command to Klipper to skip printing a specific object.
     * Object must be defined in the G-code file metadata (EXCLUDE_OBJECT_DEFINE).
     * Requires [exclude_object] section in printer.cfg.
     *
     * @param object_name Object name from G-code metadata (e.g., "Part_1")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void exclude_object(const std::string& object_name, SuccessCallback on_success,
                        ErrorCallback on_error) override;

    /**
     * @brief Emergency stop
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void emergency_stop(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Restart Klipper firmware
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void restart_firmware(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Restart Klipper host process
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void restart_klipper(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Restart a system service via Moonraker's `machine.services.restart`.
     *
     * Unlike `restart_klipper()` (which sends Klipper's `RESTART` gcode and
     * therefore requires Klippy alive to receive it), this calls into
     * Moonraker's host-side service controller. Required when Klipper is
     * fully shut down — e.g. K2 `key298` rpi MCU bridge shutdown, where
     * `FIRMWARE_RESTART` only resets the gd32 MCUs and leaves the host
     * `klipper_mcu` process stuck. A full `/etc/init.d/klipper restart`
     * (which this RPC triggers) is the only thing that recovers.
     *
     * The service name must be in Moonraker's `[machine] allowed_services`
     * allowlist (default includes klipper, moonraker).
     *
     * @param service_name Service to restart (e.g. "klipper")
     * @param on_success   Success callback
     * @param on_error     Error callback
     */
    void restart_service(const std::string& service_name, SuccessCallback on_success,
                         ErrorCallback on_error) override;

    /**
     * @brief Restart the Moonraker service
     *
     * POST /server/restart - Restarts the Moonraker service itself.
     * This will cause a temporary WebSocket disconnect.
     *
     * @param on_success Success callback (called before disconnect)
     * @param on_error Error callback
     */
    void restart_moonraker(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Shut down the host machine
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void machine_shutdown(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Reboot the host machine
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void machine_reboot(SuccessCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Query Operations
    // ========================================================================

    /**
     * @brief Query if printer is ready for commands
     *
     * @param on_result Callback with ready state
     * @param on_error Error callback
     */
    void is_printer_ready(BoolCallback on_result, ErrorCallback on_error) override;

    /**
     * @brief Get current print state
     *
     * @param on_result Callback with state ("standby", "printing", "paused", "complete", "error")
     * @param on_error Error callback
     */
    void get_print_state(StringCallback on_result, ErrorCallback on_error) override;

    // ========================================================================
    // Power-Loss Recovery — Creality Klipper fork
    // ========================================================================

    /**
     * @brief One-shot Creality power-loss-recovery probe.
     *
     * Calls the Klipper webhook endpoint
     * `pause_resume/check_continue_print_state`, which Moonraker auto-registers
     * as a JSON-RPC method.
     *
     * @warning SIDE-EFFECTFUL. On a JSON parse failure the Klipper handler
     * DELETES the recovery sidecar; it clears exclude_object_info when the state
     * is false; and it sets `print_stats.power_loss = 1` when both states are
     * true and the printer is in standby. Call AT MOST ONCE per connection and
     * only while `print_stats.state == "standby"`. NEVER poll it.
     *
     * That last effect is also why the call is mandatory before resuming: the
     * stock sensorless-homing macro gates its pre-homing Z clearance lift on
     * `power_loss == 1`. See docs/devel/POWER_LOSS_RECOVERY.md.
     *
     * @param on_result Receives the parsed result. `completed` is false when the
     *                  response was malformed, which forbids resume.
     * @param on_error  Transport/RPC error callback
     */
    void check_continue_print_state(std::function<void(const helix::PlrDetectResult&)> on_result,
                                    ErrorCallback on_error) override;

    /**
     * @brief Discard the Creality power-loss recovery snapshot.
     *
     * JSON-RPC `printer.pause_resume.cancel_continue_print`. Touches no motion,
     * so it is safe to expose regardless of the probe outcome.
     */
    void cancel_continue_print(SuccessCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Safety Limits Configuration
    // ========================================================================

    /**
     * @brief Set safety limits explicitly (overrides auto-detection)
     *
     * When called, prevents update_safety_limits_from_printer() from modifying limits.
     * Use this to enforce project-specific constraints regardless of printer configuration.
     *
     * @param limits Safety limits to apply
     */
    void set_safety_limits(const SafetyLimits& limits) override {
        safety_limits_ = limits;
        limits_explicitly_set_ = true;
    }

    /**
     * @brief Get current safety limits
     *
     * @return Current safety limits (explicit, auto-detected, or defaults)
     */
    const SafetyLimits& get_safety_limits() const override {
        return safety_limits_;
    }

    /**
     * @brief Update safety limits from printer configuration via Moonraker API
     *
     * Queries printer.objects.query for configfile.settings and extracts:
     * - max_velocity → max_feedrate_mm_min
     * - stepper_* position_min/max → absolute position limits
     * - extruder/heater_* min_temp/max_temp → temperature limits
     *
     * Only updates limits if set_safety_limits() has NOT been called (explicit config takes
     * priority). Fallback to defaults if Moonraker query fails or values unavailable.
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void update_safety_limits_from_printer(SuccessCallback on_success,
                                           ErrorCallback on_error) override;

    /**
     * @brief Query the printer's configfile object
     *
     * Fetches the raw configuration from Klipper's configfile object.
     * This includes all sections and their raw string values, which is useful
     * for parsing macro definitions (gcode_macro sections contain the raw gcode).
     *
     * The response is the "config" portion of configfile, not "settings".
     * - "config": Raw strings as written in config files
     * - "settings": Parsed/typed values (not useful for macro text)
     *
     * @param on_success Callback with parsed JSON config object
     * @param on_error Error callback
     */
    void query_configfile(JsonCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // HTTP Base URL Configuration
    // ========================================================================

    /**
     * @brief Set the HTTP base URL for file transfers and REST operations
     *
     * Must be called before using transfer/REST methods.
     * Typically derived from WebSocket URL: ws://host:port -> http://host:port
     *
     * @param base_url HTTP base URL (e.g., "http://192.168.1.100:7125")
     */
    void set_http_base_url(const std::string& base_url) override {
        http_base_url_ = base_url;
    }

    /**
     * @brief Get the current HTTP base URL
     */
    const std::string& get_http_base_url() const override {
        return http_base_url_;
    }

    /**
     * @brief Resolve a relative webcam URL against the web frontend base.
     *
     * Moonraker webcam URLs are often relative paths (e.g. "/webcam/?action=stream").
     * The base URL is always of the form "http://HOST" or "http://HOST:PORT".
     *
     * Port handling depends on which port the HTTP base points at:
     *   - Base port == 7125 (Moonraker's default API port): we're talking to
     *     Moonraker directly. The webcam is NOT served on the API port — it lives
     *     on the reverse proxy, conventionally nginx on :80. So we STRIP the port
     *     and resolve against the bare host (port 80). This is the common, working
     *     case (e.g. a printer on :7125 with an mjpegstreamer-adaptive webcam).
     *   - Base port == anything else (user connected via a reverse-proxy port such
     *     as :4408, e.g. a Creality K2): the frontend that serves the webcam is on
     *     that same port. KEEP the port and resolve against host:port.
     *   - Base has no port: resolve against the base as-is.
     *
     * @param url The URL to resolve (modified in place). Absolute URLs are unchanged.
     */
    void resolve_webcam_url(std::string& url) override {
        if (url.empty() || url[0] != '/')
            return;
        ensure_http_base_url();
        const auto& base = get_http_base_url();
        if (base.empty())
            return;
        auto scheme_end = base.find("://");
        if (scheme_end == std::string::npos) {
            url = base + url;
            return;
        }
        auto port_pos = base.find(':', scheme_end + 3);
        if (port_pos == std::string::npos) {
            // No port in base — resolve as-is.
            url = base + url;
            return;
        }
        // Parse the port that follows the ':'.
        int base_port = 0;
        for (size_t i = port_pos + 1; i < base.size(); ++i) {
            char c = base[i];
            if (c < '0' || c > '9')
                break;
            base_port = base_port * 10 + (c - '0');
        }
        if (base_port == MOONRAKER_DEFAULT_PORT) {
            // Direct-to-Moonraker: webcam lives on the reverse proxy (port 80).
            url = base.substr(0, port_pos) + url;
        } else {
            // Reverse-proxy port (e.g. :4408): the frontend serving the webcam is
            // on this same port — keep it.
            url = base + url;
        }
    }

    /**
     * @brief Ensure HTTP base URL is set, auto-deriving from WebSocket if needed
     *
     * If http_base_url_ is empty, attempts to derive it from the client's
     * WebSocket URL: ws://host:port/websocket -> http://host:port
     *
     * @return true if HTTP base URL is available, false if derivation failed
     */
    bool ensure_http_base_url() override;

    // ========================================================================
    // Connection and Subscription Proxies
    // ========================================================================

    /// Check if the client is currently connected to Moonraker
    bool is_connected() const override;

    /// Get current connection state
    helix::ConnectionState get_connection_state() const override;

    /// Get the WebSocket URL used for the current connection
    std::string get_websocket_url() const override;

    /// Subscribe to status update notifications (mirrors MoonrakerClient::register_notify_update)
    helix::SubscriptionId
    subscribe_notifications(std::function<void(const json&)> callback) override;

    /// Unsubscribe from status update notifications
    bool unsubscribe_notifications(helix::SubscriptionId id) override;

    /// Get client lifetime guard (for SubscriptionGuard safety)
    std::weak_ptr<bool> client_lifetime_weak() const override;

    /// Register a persistent callback for a specific notification method
    void register_method_callback(const std::string& method, const std::string& name,
                                  std::function<void(const json&)> callback) override;

    /// Unregister a method-specific callback
    bool unregister_method_callback(const std::string& method, const std::string& name) override;

    /// Temporarily suppress disconnect modal notifications
    void suppress_disconnect_modal(uint32_t duration_ms) override;

    /// Retrieve recent G-code commands/responses from Moonraker's store
    void get_gcode_store(int count,
                         std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                         std::function<void(const MoonrakerError&)> on_error) override;

    // ========================================================================
    // Helix Plugin Operations
    // ========================================================================

    /// Get phase tracking plugin status
    void get_phase_tracking_status(std::function<void(bool enabled)> on_success,
                                   ErrorCallback on_error = nullptr) override;

    /// Enable or disable phase tracking plugin
    void set_phase_tracking_enabled(bool enabled, std::function<void(bool success)> on_success,
                                    ErrorCallback on_error = nullptr) override;

    // ========================================================================
    // Database Operations
    // ========================================================================

    /// Get a value from Moonraker's database
    void database_get_item(const std::string& namespace_name, const std::string& key,
                           std::function<void(const json&)> on_success,
                           ErrorCallback on_error = nullptr) override;

    /// Store a value in Moonraker's database
    void database_post_item(const std::string& namespace_name, const std::string& key,
                            const json& value, std::function<void()> on_success = nullptr,
                            ErrorCallback on_error = nullptr) override;

    /// Get all keys in a namespace
    void database_get_namespace(const std::string& namespace_name,
                                std::function<void(const json&)> on_success,
                                ErrorCallback on_error = nullptr) override;

    /// Delete a key from Moonraker's database. Absent key is not an error in
    /// Moonraker's semantics; on_success fires either way.
    void database_delete_item(const std::string& namespace_name, const std::string& key,
                              std::function<void()> on_success = nullptr,
                              ErrorCallback on_error = nullptr) override;

    // ========================================================================
    // Internal Access
    // ========================================================================

    /**
     * @brief Get reference to underlying client transport
     *
     * Provides direct access to the transport client for advanced operations
     * requiring direct G-code execution or state observation.
     *
     * @return Reference to IMoonrakerClient
     */
    helix::IMoonrakerClient& get_client() override {
        return client_;
    }

    /**
     * @brief Get const reference to discovered hardware
     *
     * Provides read-only access to the printer hardware discovery data,
     * including heaters, fans, sensors, LEDs, and capability flags.
     * This data is populated during printer discovery via MoonrakerClient.
     *
     * @return Const reference to PrinterDiscovery
     */
    [[nodiscard]] const helix::PrinterDiscovery& hardware() const override {
        return hardware_;
    }

    /**
     * @brief Get non-const reference to hardware for internal updates
     *
     * Used internally by discovery callbacks to populate hardware data.
     * Application code should use the const accessor instead.
     *
     * @return Reference to PrinterDiscovery
     */
    helix::PrinterDiscovery& hardware() override {
        return hardware_;
    }

    /**
     * @brief Get build volume version subject for change notifications
     *
     * This integer subject is incremented whenever build_volume is updated
     * (e.g., when stepper config loads). Observers can watch this to refresh
     * UI that depends on build_volume dimensions.
     *
     * @return Pointer to the version subject
     */
    lv_subject_t* get_build_volume_version_subject() override {
        return &build_volume_version_;
    }

    /**
     * @brief Notify that build_volume has changed
     *
     * Call this after updating hardware().set_build_volume() to notify
     * observers that they should refresh any cached build volume data.
     * Increments the build_volume_version_ subject.
     */
    void notify_build_volume_changed() override;

    // ========================================================================
    // Sub-API Accessors (Delegated)
    // ========================================================================

    /**
     * @brief Get Advanced API for calibration, bed mesh, macros, etc.
     *
     * All advanced panel methods (bed mesh, input shaper, PID calibration,
     * machine limits, macros, save_config) are available through this accessor.
     *
     * @return Reference to MoonrakerAdvancedAPI
     */
    MoonrakerAdvancedAPI& advanced() override {
        return *advanced_api_;
    }

    /**
     * @brief Observe live bed-mesh presence from the status stream
     *
     * Invoked (WebSocket thread) whenever a bed_mesh status update carries a
     * verdict on probed_matrix — true when a mesh is loaded, false when it
     * has been cleared. Updates that omit the key say nothing about presence
     * and do not invoke the observer. Concrete-only: wiring detail for
     * MoonrakerManager (print-start collector), not part of the consumer
     * contract.
     *
     * Set from the main thread, invoked from the WebSocket thread: the
     * mutex is load-bearing. Without it a weakly-ordered target (MIPS) can
     * keep reading a stale null observer long after the store.
     */
    void set_bed_mesh_presence_observer(std::function<void(bool)> observer) {
        std::lock_guard<std::mutex> lock(bed_mesh_presence_mutex_);
        bed_mesh_presence_observer_ = std::move(observer);
    }

    /**
     * @brief Get File Transfer API for HTTP download/upload operations
     *
     * All file transfer methods (download_file, download_thumbnail, upload_file, etc.)
     * are available through this accessor.
     *
     * @return Reference to MoonrakerFileTransferAPI
     */
    MoonrakerFileTransferAPI& transfers() override {
        return *file_transfer_api_;
    }

    /**
     * @brief Get History API for print history operations
     *
     * All history methods (get_history_list, get_history_totals, delete_history_job)
     * are available through this accessor.
     *
     * @return Reference to MoonrakerHistoryAPI
     */
    MoonrakerHistoryAPI& history() override {
        return *history_api_;
    }

    /**
     * @brief Get Job API for print job control operations
     *
     * All job methods (start_print, pause_print, resume_print, cancel_print,
     * start_modified_print, check_helix_plugin) are available through this accessor.
     *
     * @return Reference to MoonrakerJobAPI
     */
    MoonrakerJobAPI& job() override {
        return *job_api_;
    }

    /**
     * @brief Get Timelapse API for timelapse and webcam operations
     *
     * All timelapse methods (get/set settings, render, frames) and
     * webcam queries are available through this accessor.
     *
     * @return Reference to MoonrakerTimelapseAPI
     */
    MoonrakerTimelapseAPI& timelapse() override {
        return *timelapse_api_;
    }

    /**
     * @brief Get Motion API for axis control operations
     *
     * All motion methods (home_axes, move_axis, move_to_position)
     * are available through this accessor.
     *
     * @return Reference to MoonrakerMotionAPI
     */
    MoonrakerMotionAPI& motion() override {
        return *motion_api_;
    }

    /**
     * @brief Access the PrinterState this API reads/writes.
     *
     * Exposed so collaborators that hold a MoonrakerAPI* (e.g. AMS backends) can
     * consult live printer state — used by the homing guard to refuse toolhead-
     * motion filament ops while a print is active. The reference is valid for the
     * API's lifetime.
     */
    helix::PrinterState& printer_state() override {
        return state_;
    }

    /**
     * @brief Get REST API for generic REST endpoint and WLED operations
     *
     * All REST methods (call_rest_get, call_rest_post, wled_get_strips,
     * wled_set_strip, wled_get_status, get_server_config) are available
     * through this accessor.
     *
     * @return Reference to MoonrakerRestAPI
     */
    MoonrakerRestAPI& rest() override {
        return *rest_api_;
    }

    /**
     * @brief Get Spoolman API for filament tracking operations
     *
     * All Spoolman methods (get_spoolman_spools, set_active_spool, etc.)
     * are available through this accessor.
     *
     * @return Reference to MoonrakerSpoolmanAPI
     */
    MoonrakerSpoolmanAPI& spoolman() override {
        return *spoolman_api_;
    }

    /**
     * @brief Get File API for file management operations
     *
     * All file management methods (list_files, get_directory, get_file_metadata,
     * metascan_file, delete_file, move_file, copy_file, create_directory,
     * delete_directory) are available through this accessor.
     *
     * @return Reference to MoonrakerFileAPI
     */
    MoonrakerFileAPI& files() override {
        return *file_api_;
    }

    /**
     * @brief Get Queue API for job queue operations
     *
     * All queue methods (get_queue_status, start_queue, pause_queue,
     * add_job, remove_jobs) are available through this accessor.
     *
     * @return Reference to MoonrakerQueueAPI
     */
    MoonrakerQueueAPI& queue() override {
        return *queue_api_;
    }

  private:
    // Data members MUST be declared BEFORE sub-API unique_ptrs.
    // C++ destroys members in reverse declaration order, so data members
    // (declared first) are destroyed LAST, after sub-APIs that reference them.
    std::string http_base_url_; ///< HTTP base URL for file transfers
    helix::IMoonrakerClient& client_;
    helix::PrinterState& state_;

    /// Discovered printer hardware (heaters, fans, sensors, LEDs, capabilities)
    helix::PrinterDiscovery hardware_;

    /// Subject for notifying when build_volume changes (version counter)
    lv_subject_t build_volume_version_;
    std::atomic<int> build_volume_version_counter_{0};

    /// Generation guard for the subject writes this class defers to the main
    /// thread. Invalidated by the destructor *before* `build_volume_version_` is
    /// deinited, so a callback still sitting in the UpdateQueue is dropped
    /// instead of notifying a freed observer list — the exact failure that took
    /// down the unsharded suite (#1165, #1146). Unlike the subject-owning state
    /// classes there is no `deinit_subjects()` here: the subject is created in
    /// the constructor and torn down only at destruction.
    helix::AsyncLifetimeGuard async_lifetime_;

    SafetyLimits safety_limits_;
    bool limits_explicitly_set_ = false;

  protected:
    // Sub-API unique_ptrs declared AFTER data members they reference
    // (http_base_url_, safety_limits_) so they are destroyed FIRST.
    std::unique_ptr<MoonrakerAdvancedAPI> advanced_api_; ///< Advanced panel operations API
    std::mutex bed_mesh_presence_mutex_;                 ///< Guards the observer across threads
    std::function<void(bool)>
        bed_mesh_presence_observer_; ///< Optional presence sink (manager wiring)
    std::unique_ptr<MoonrakerFileTransferAPI> file_transfer_api_; ///< HTTP file transfer API
    std::unique_ptr<MoonrakerFileAPI> file_api_;                  ///< File management API
    std::unique_ptr<MoonrakerHistoryAPI> history_api_;            ///< Print history API
    std::unique_ptr<MoonrakerJobAPI> job_api_;                    ///< Job control API
    std::unique_ptr<MoonrakerMotionAPI> motion_api_;              ///< Motion control API
    std::unique_ptr<MoonrakerRestAPI> rest_api_;                  ///< REST endpoint & WLED API
    std::unique_ptr<MoonrakerSpoolmanAPI> spoolman_api_;   ///< Spoolman filament tracking API
    std::unique_ptr<MoonrakerQueueAPI> queue_api_;         ///< Job queue API
    std::unique_ptr<MoonrakerTimelapseAPI> timelapse_api_; ///< Timelapse & webcam API
};