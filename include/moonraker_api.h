// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MOONRAKER_API_H
#define MOONRAKER_API_H

#include "moonraker_client.h"
#include "moonraker_domain_service.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

/**
 * @brief Safety limits for G-code generation and validation
 *
 * These limits protect against dangerous operations:
 * - Temperature limits prevent heater damage or fire hazards
 * - Position/distance limits prevent mechanical collisions
 * - Feedrate limits prevent motor stalling or mechanical stress
 *
 * Priority order:
 * 1. Explicitly configured values (via set_safety_limits())
 * 2. Auto-detected from printer.cfg (via update_safety_limits_from_printer())
 * 3. Conservative fallback defaults
 */
struct SafetyLimits {
    double max_temperature_celsius = 400.0;
    double min_temperature_celsius = 0.0;
    double max_fan_speed_percent = 100.0;
    double min_fan_speed_percent = 0.0;
    double max_feedrate_mm_min = 50000.0;
    double min_feedrate_mm_min = 0.0;
    double max_relative_distance_mm = 1000.0;
    double min_relative_distance_mm = -1000.0;
    double max_absolute_position_mm = 1000.0;
    double min_absolute_position_mm = 0.0;
};

/**
 * @brief File information structure
 */
struct FileInfo {
    std::string filename;
    std::string path; // Relative to root
    uint64_t size = 0;
    double modified = 0.0;
    std::string permissions;
    bool is_dir = false;
};

/**
 * @brief File metadata structure (detailed file info)
 */
struct FileMetadata {
    std::string filename;
    uint64_t size = 0;
    double modified = 0.0;
    std::string slicer;
    std::string slicer_version;
    double print_start_time = 0.0;
    double job_id = 0.0;
    uint32_t layer_count = 0;
    double object_height = 0.0;         // mm
    double estimated_time = 0.0;        // seconds
    double filament_total = 0.0;        // mm
    double filament_weight_total = 0.0; // grams
    std::string filament_type;          // e.g., "PLA", "PETG", "ABS", "TPU", "ASA"
    double first_layer_bed_temp = 0.0;
    double first_layer_extr_temp = 0.0;
    uint64_t gcode_start_byte = 0;
    uint64_t gcode_end_byte = 0;
    std::vector<std::string> thumbnails; // Base64 or paths
};

/**
 * @brief High-level Moonraker API facade
 *
 * Provides simplified, domain-specific operations on top of MoonrakerClient.
 * All methods are asynchronous with success/error callbacks.
 */
class MoonrakerAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using FileListCallback = std::function<void(const std::vector<FileInfo>&)>;
    using FileMetadataCallback = std::function<void(const FileMetadata&)>;
    using BoolCallback = std::function<void(bool)>;
    using StringCallback = std::function<void(const std::string&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param state PrinterState instance (must remain valid during API lifetime)
     */
    MoonrakerAPI(MoonrakerClient& client, PrinterState& state);
    virtual ~MoonrakerAPI() = default;

    // ========================================================================
    // File Management Operations
    // ========================================================================

    /**
     * @brief List files in a directory
     *
     * @param root Root directory ("gcodes", "config", "timelapse")
     * @param path Subdirectory path (empty for root)
     * @param recursive Include subdirectories
     * @param on_success Callback with file list
     * @param on_error Error callback
     */
    void list_files(const std::string& root, const std::string& path, bool recursive,
                    FileListCallback on_success, ErrorCallback on_error);

    /**
     * @brief Get detailed metadata for a file
     *
     * @param filename Full path to file (relative to gcodes root)
     * @param on_success Callback with metadata
     * @param on_error Error callback
     */
    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error);

    /**
     * @brief Delete a file
     *
     * @param filename Full path to file (relative to gcodes root)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void delete_file(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error);

    /**
     * @brief Move or rename a file
     *
     * @param source Source path (e.g., "gcodes/old_dir/file.gcode")
     * @param dest Destination path (e.g., "gcodes/new_dir/file.gcode")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_file(const std::string& source, const std::string& dest, SuccessCallback on_success,
                   ErrorCallback on_error);

    /**
     * @brief Copy a file
     *
     * @param source Source path (e.g., "gcodes/original.gcode")
     * @param dest Destination path (e.g., "gcodes/copy.gcode")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void copy_file(const std::string& source, const std::string& dest, SuccessCallback on_success,
                   ErrorCallback on_error);

    /**
     * @brief Create a directory
     *
     * @param path Directory path (e.g., "gcodes/my_folder")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void create_directory(const std::string& path, SuccessCallback on_success,
                          ErrorCallback on_error);

    /**
     * @brief Delete a directory
     *
     * @param path Directory path (e.g., "gcodes/old_folder")
     * @param force Force deletion even if not empty
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void delete_directory(const std::string& path, bool force, SuccessCallback on_success,
                          ErrorCallback on_error);

    // ========================================================================
    // Job Control Operations
    // ========================================================================

    /**
     * @brief Start printing a file
     *
     * @param filename Full path to G-code file
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void start_print(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error);

    /**
     * @brief Pause the current print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void pause_print(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Resume a paused print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void resume_print(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Cancel the current print
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void cancel_print(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // Motion Control Operations
    // ========================================================================

    /**
     * @brief Home one or more axes
     *
     * @param axes Axes to home (e.g., "XY", "Z", "XYZ", empty for all)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void home_axes(const std::string& axes, SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Move an axis by a relative amount
     *
     * @param axis Axis name ('X', 'Y', 'Z', 'E')
     * @param distance Distance to move in mm
     * @param feedrate Movement speed in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_axis(char axis, double distance, double feedrate, SuccessCallback on_success,
                   ErrorCallback on_error);

    /**
     * @brief Set absolute position for an axis
     *
     * @param axis Axis name ('X', 'Y', 'Z')
     * @param position Absolute position in mm
     * @param feedrate Movement speed in mm/min (0 for default)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_to_position(char axis, double position, double feedrate, SuccessCallback on_success,
                          ErrorCallback on_error);

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
     */
    void set_temperature(const std::string& heater, double temperature, SuccessCallback on_success,
                         ErrorCallback on_error);

    /**
     * @brief Set fan speed
     *
     * @param fan Fan name ("fan", "fan_generic cooling_fan", etc.)
     * @param speed Speed percentage (0-100)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_fan_speed(const std::string& fan, double speed, SuccessCallback on_success,
                       ErrorCallback on_error);

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
     */
    void set_led(const std::string& led, double red, double green, double blue, double white,
                 SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Turn LED on (full white)
     *
     * Convenience method to turn LED on at full brightness.
     *
     * @param led LED name
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_led_on(const std::string& led, SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Turn LED off
     *
     * Convenience method to turn LED off.
     *
     * @param led LED name
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_led_off(const std::string& led, SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // System Control Operations
    // ========================================================================

    /**
     * @brief Execute custom G-code command
     *
     * @param gcode G-code command string
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void execute_gcode(const std::string& gcode, SuccessCallback on_success,
                       ErrorCallback on_error);

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
                        ErrorCallback on_error);

    /**
     * @brief Emergency stop
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void emergency_stop(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Restart Klipper firmware
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void restart_firmware(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Restart Klipper host process
     *
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void restart_klipper(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // Query Operations
    // ========================================================================

    /**
     * @brief Query if printer is ready for commands
     *
     * @param on_result Callback with ready state
     * @param on_error Error callback
     */
    void is_printer_ready(BoolCallback on_result, ErrorCallback on_error);

    /**
     * @brief Get current print state
     *
     * @param on_result Callback with state ("standby", "printing", "paused", "complete", "error")
     * @param on_error Error callback
     */
    void get_print_state(StringCallback on_result, ErrorCallback on_error);

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
    void set_safety_limits(const SafetyLimits& limits) {
        safety_limits_ = limits;
        limits_explicitly_set_ = true;
    }

    /**
     * @brief Get current safety limits
     *
     * @return Current safety limits (explicit, auto-detected, or defaults)
     */
    const SafetyLimits& get_safety_limits() const {
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
    void update_safety_limits_from_printer(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // HTTP File Transfer Operations
    // ========================================================================

    /**
     * @brief Download a file's content from the printer via HTTP
     *
     * Uses GET request to /server/files/{root}/{path} endpoint.
     * The file content is returned as a string in the callback.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock reads local files).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path File path relative to root
     * @param on_success Callback with file content as string
     * @param on_error Error callback
     */
    virtual void download_file(const std::string& root, const std::string& path,
                               StringCallback on_success, ErrorCallback on_error);

    /**
     * @brief Upload file content to the printer via HTTP multipart form
     *
     * Uses POST request to /server/files/upload endpoint with multipart form data.
     * Suitable for G-code files, config files, and macro files.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock logs but doesn't write).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path Destination path relative to root
     * @param content File content to upload
     * @param on_success Success callback
     * @param on_error Error callback
     */
    virtual void upload_file(const std::string& root, const std::string& path,
                             const std::string& content, SuccessCallback on_success,
                             ErrorCallback on_error);

    /**
     * @brief Upload file content with custom filename
     *
     * Like upload_file() but allows specifying a different filename for the
     * multipart form than the path. Useful when uploading to a subdirectory.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock logs but doesn't write).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path Destination path relative to root (e.g., ".helix_temp/foo.gcode")
     * @param filename Filename for form (e.g., ".helix_temp/foo.gcode")
     * @param content File content to upload
     * @param on_success Success callback
     * @param on_error Error callback
     */
    virtual void upload_file_with_name(const std::string& root, const std::string& path,
                                       const std::string& filename, const std::string& content,
                                       SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Set the HTTP base URL for file transfers
     *
     * Must be called before using download_file/upload_file.
     * Typically derived from WebSocket URL: ws://host:port -> http://host:port
     *
     * @param base_url HTTP base URL (e.g., "http://192.168.1.100:7125")
     */
    void set_http_base_url(const std::string& base_url) { http_base_url_ = base_url; }

    /**
     * @brief Get the current HTTP base URL
     */
    const std::string& get_http_base_url() const { return http_base_url_; }

    // ========================================================================
    // Domain Service Operations (Hardware Discovery, Bed Mesh, Object Exclusion)
    // ========================================================================

    /**
     * @brief Guess the most likely bed heater from discovered hardware
     *
     * Searches heaters for names containing "bed", "heated_bed", "heater_bed".
     * Returns the first match found using priority-based search:
     * 1. Exact match: "heater_bed"
     * 2. Exact match: "heated_bed"
     * 3. Substring match: any heater containing "bed"
     *
     * Delegates to MoonrakerClient's discovered hardware list.
     *
     * @return Bed heater name or empty string if none found
     */
    std::string guess_bed_heater() const;

    /**
     * @brief Guess the most likely hotend heater from discovered hardware
     *
     * Searches heaters for names containing "extruder", "hotend", "e0".
     * Prioritizes "extruder" (base extruder) over numbered variants.
     * Priority order:
     * 1. Exact match: "extruder"
     * 2. Exact match: "extruder0"
     * 3. Substring match: any heater containing "extruder"
     * 4. Substring match: any heater containing "hotend"
     * 5. Substring match: any heater containing "e0"
     *
     * Delegates to MoonrakerClient's discovered hardware list.
     *
     * @return Hotend heater name or empty string if none found
     */
    std::string guess_hotend_heater() const;

    /**
     * @brief Guess the most likely bed temperature sensor from discovered hardware
     *
     * First checks heaters for bed heater (heaters have built-in sensors).
     * If no bed heater found, searches sensors for names containing "bed".
     *
     * Delegates to MoonrakerClient's discovered hardware list.
     *
     * @return Bed sensor name or empty string if none found
     */
    std::string guess_bed_sensor() const;

    /**
     * @brief Guess the most likely hotend temperature sensor from discovered hardware
     *
     * First checks heaters for extruder heater (heaters have built-in sensors).
     * If no extruder heater found, searches sensors for names containing
     * "extruder", "hotend", "e0".
     *
     * Delegates to MoonrakerClient's discovered hardware list.
     *
     * @return Hotend sensor name or empty string if none found
     */
    std::string guess_hotend_sensor() const;

    /**
     * @brief Get currently active bed mesh profile
     *
     * Returns pointer to the active mesh profile loaded from Moonraker's
     * bed_mesh object. The probed_matrix field contains the 2D Z-height
     * array ready for rendering.
     *
     * @return Pointer to active mesh profile, or nullptr if none loaded
     */
    const BedMeshProfile* get_active_bed_mesh() const;

    /**
     * @brief Get list of available mesh profile names
     *
     * Returns profile names from bed_mesh.profiles (e.g., "default",
     * "adaptive", "calibration"). Empty vector if no profiles available
     * or discovery hasn't completed.
     *
     * @return Vector of profile names
     */
    std::vector<std::string> get_bed_mesh_profiles() const;

    /**
     * @brief Check if bed mesh data is available
     *
     * @return true if a mesh profile with valid probed_matrix is loaded
     */
    bool has_bed_mesh() const;

    /**
     * @brief Get set of currently excluded object names (async)
     *
     * Queries Klipper's exclude_object module for the list of objects
     * that have been excluded from the current print.
     *
     * @param on_success Callback with set of excluded object names
     * @param on_error Error callback
     */
    void get_excluded_objects(std::function<void(const std::set<std::string>&)> on_success,
                              ErrorCallback on_error);

    /**
     * @brief Get list of available objects in current print (async)
     *
     * Queries Klipper's exclude_object module for the list of objects
     * defined in the current G-code file (from EXCLUDE_OBJECT_DEFINE).
     *
     * @param on_success Callback with vector of available object names
     * @param on_error Error callback
     */
    void get_available_objects(std::function<void(const std::vector<std::string>&)> on_success,
                               ErrorCallback on_error);

    // ========================================================================
    // Internal Access (for CommandSequencer integration)
    // ========================================================================

    /**
     * @brief Get reference to underlying MoonrakerClient
     *
     * Required by CommandSequencer which needs direct client access for
     * G-code execution and state observation.
     *
     * @return Reference to MoonrakerClient
     */
    MoonrakerClient& get_client() { return client_; }

  private:
    std::string http_base_url_;  ///< HTTP base URL for file transfers
    MoonrakerClient& client_;

    SafetyLimits safety_limits_;
    bool limits_explicitly_set_ = false;

    /**
     * @brief Parse file list response from server.files.list
     */
    std::vector<FileInfo> parse_file_list(const json& response);

    /**
     * @brief Parse metadata response from server.files.metadata
     */
    FileMetadata parse_file_metadata(const json& response);

    /**
     * @brief Generate G-code for homing axes
     */
    std::string generate_home_gcode(const std::string& axes);

    /**
     * @brief Generate G-code for relative movement
     */
    std::string generate_move_gcode(char axis, double distance, double feedrate);

    /**
     * @brief Generate G-code for absolute movement
     */
    std::string generate_absolute_move_gcode(char axis, double position, double feedrate);
};

#endif // MOONRAKER_API_H