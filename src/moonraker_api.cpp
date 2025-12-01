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

#include "moonraker_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "hv/requests.h"  // libhv HTTP client for file transfers

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

// ============================================================================
// Input Validation Helpers
// ============================================================================

namespace {

/**
 * @brief Validate that a string contains only safe identifier characters
 *
 * Allows alphanumeric, underscore, and space (for names like "heater_generic chamber").
 * Rejects newlines, semicolons, and other G-code control characters.
 *
 * @param str String to validate
 * @return true if safe, false otherwise
 */
bool is_safe_identifier(const std::string& str) {
    if (str.empty()) {
        return false;
    }

    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ';
    });
}

/**
 * @brief Validate that a file path is safe from directory traversal attacks
 *
 * Rejects paths containing:
 * - Parent directory references (..)
 * - Absolute paths (starting with /)
 * - Null bytes (path truncation attack)
 * - Windows-style absolute paths (C:, D:, etc)
 * - Suspicious characters (<>|*?)
 *
 * @param path File path to validate
 * @return true if safe relative path, false otherwise
 */
bool is_safe_path(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    if (path.find("..") != std::string::npos) {
        return false;
    }

    if (path[0] == '/') {
        return false;
    }

    if (path.find('\0') != std::string::npos) {
        return false;
    }

    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }

    const std::string dangerous_chars = "<>|*?";
    if (path.find_first_of(dangerous_chars) != std::string::npos) {
        return false;
    }

    for (char c : path) {
        if (std::iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Validate that an axis character is valid
 *
 * @param axis Axis character to validate
 * @return true if valid axis (X, Y, Z, E), false otherwise
 */
bool is_valid_axis(char axis) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
    return upper == 'X' || upper == 'Y' || upper == 'Z' || upper == 'E';
}

/**
 * @brief Validate temperature is in safe range
 *
 * @param temp Temperature in Celsius
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
bool is_safe_temperature(double temp, const SafetyLimits& limits) {
    return temp >= limits.min_temperature_celsius && temp <= limits.max_temperature_celsius;
}

/**
 * @brief Validate fan speed is in valid percentage range
 *
 * @param speed Speed percentage
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
bool is_safe_fan_speed(double speed, const SafetyLimits& limits) {
    return speed >= limits.min_fan_speed_percent && speed <= limits.max_fan_speed_percent;
}

/**
 * @brief Validate feedrate is within safe limits
 *
 * @param feedrate Feedrate in mm/min
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
bool is_safe_feedrate(double feedrate, const SafetyLimits& limits) {
    return feedrate >= limits.min_feedrate_mm_min && feedrate <= limits.max_feedrate_mm_min;
}

/**
 * @brief Validate distance is reasonable for axis movement
 *
 * @param distance Distance in mm
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
bool is_safe_distance(double distance, const SafetyLimits& limits) {
    return distance >= limits.min_relative_distance_mm &&
           distance <= limits.max_relative_distance_mm;
}

/**
 * @brief Validate position is reasonable for axis positioning
 *
 * @param position Position in mm
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
bool is_safe_position(double position, const SafetyLimits& limits) {
    return position >= limits.min_absolute_position_mm &&
           position <= limits.max_absolute_position_mm;
}

} // anonymous namespace

// ============================================================================
// MoonrakerAPI Implementation
// ============================================================================

MoonrakerAPI::MoonrakerAPI(MoonrakerClient& client, PrinterState& state) : client_(client) {
    // state parameter reserved for future use
    (void)state;
}

// ============================================================================
// File Management Operations
// ============================================================================

void MoonrakerAPI::list_files(const std::string& root, const std::string& path, bool recursive,
                              FileListCallback on_success, ErrorCallback on_error) {
    // Validate root parameter
    if (!is_safe_identifier(root)) {
        NOTIFY_ERROR("File path error: '{}' is not a valid location. Please check the root name.",
                     root);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid root name contains illegal characters";
            err.method = "list_files";
            on_error(err);
        }
        return;
    }

    // Validate path if provided
    if (!path.empty() && !is_safe_path(path)) {
        NOTIFY_ERROR("Invalid file path '{}'. Path contains unsafe characters or references.",
                     path);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid path contains directory traversal or illegal characters";
            err.method = "list_files";
            on_error(err);
        }
        return;
    }

    json params = {{"root", root}};

    if (!path.empty()) {
        params["path"] = path;
    }

    if (recursive) {
        params["extended"] = true;
    }

    spdlog::debug("[Moonraker API] Listing files in {}/{}", root, path);

    client_.send_jsonrpc(
        "server.files.list", params,
        [this, on_success](json response) {
            try {
                std::vector<FileInfo> files = parse_file_list(response);
                spdlog::debug("[Moonraker API] Found {} files", files.size());
                on_success(files);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse file list: {}", e.what());
                on_success(std::vector<FileInfo>{}); // Return empty list on parse error
            }
        },
        on_error);
}

void MoonrakerAPI::get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                                     ErrorCallback on_error) {
    // Validate filename path
    if (!is_safe_path(filename)) {
        NOTIFY_ERROR("Invalid filename '{}'. Check the file path format.", filename);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid filename contains directory traversal or illegal characters";
            err.method = "get_file_metadata";
            on_error(err);
        }
        return;
    }

    json params = {{"filename", filename}};

    spdlog::debug("[Moonraker API] Getting metadata for file: {}", filename);

    client_.send_jsonrpc(
        "server.files.metadata", params,
        [this, on_success](json response) {
            try {
                FileMetadata metadata = parse_file_metadata(response);
                on_success(metadata);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse file metadata: {}", e.what());
                FileMetadata empty;
                on_success(empty);
            }
        },
        on_error);
}

void MoonrakerAPI::delete_file(const std::string& filename, SuccessCallback on_success,
                               ErrorCallback on_error) {
    // Validate filename path
    if (!is_safe_path(filename)) {
        NOTIFY_ERROR("Cannot delete '{}'. Invalid file path.", filename);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid filename contains directory traversal or illegal characters";
            err.method = "delete_file";
            on_error(err);
        }
        return;
    }

    json params = {{"path", filename}};

    spdlog::info("[Moonraker API] Deleting file: {}", filename);

    client_.send_jsonrpc(
        "server.files.delete_file", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] File deleted successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::move_file(const std::string& source, const std::string& dest,
                             SuccessCallback on_success, ErrorCallback on_error) {
    // Validate source path
    if (!is_safe_path(source)) {
        NOTIFY_ERROR("Cannot move file. Source path '{}' is invalid.", source);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid source path contains directory traversal or illegal characters";
            err.method = "move_file";
            on_error(err);
        }
        return;
    }

    // Validate destination path
    if (!is_safe_path(dest)) {
        NOTIFY_ERROR("Cannot move file. Destination path '{}' is invalid.", dest);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Invalid destination path contains directory traversal or illegal characters";
            err.method = "move_file";
            on_error(err);
        }
        return;
    }

    spdlog::info("[Moonraker API] Moving file from {} to {}", source, dest);

    json params = {{"source", source}, {"dest", dest}};

    client_.send_jsonrpc(
        "server.files.move", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] File moved successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::copy_file(const std::string& source, const std::string& dest,
                             SuccessCallback on_success, ErrorCallback on_error) {
    // Validate source path
    if (!is_safe_path(source)) {
        NOTIFY_ERROR("Cannot copy file. Source path '{}' is invalid.", source);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid source path contains directory traversal or illegal characters";
            err.method = "copy_file";
            on_error(err);
        }
        return;
    }

    // Validate destination path
    if (!is_safe_path(dest)) {
        NOTIFY_ERROR("Cannot copy file. Destination path '{}' is invalid.", dest);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Invalid destination path contains directory traversal or illegal characters";
            err.method = "copy_file";
            on_error(err);
        }
        return;
    }

    spdlog::info("[Moonraker API] Copying file from {} to {}", source, dest);

    json params = {{"source", source}, {"dest", dest}};

    client_.send_jsonrpc(
        "server.files.copy", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] File copied successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::create_directory(const std::string& path, SuccessCallback on_success,
                                    ErrorCallback on_error) {
    // Validate path
    if (!is_safe_path(path)) {
        NOTIFY_ERROR("Cannot create directory '{}'. Invalid path.", path);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Invalid directory path contains directory traversal or illegal characters";
            err.method = "create_directory";
            on_error(err);
        }
        return;
    }

    spdlog::info("[Moonraker API] Creating directory: {}", path);

    json params = {{"path", path}};

    client_.send_jsonrpc(
        "server.files.post_directory", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] Directory created successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::delete_directory(const std::string& path, bool force, SuccessCallback on_success,
                                    ErrorCallback on_error) {
    // Validate path
    if (!is_safe_path(path)) {
        NOTIFY_ERROR("Cannot delete directory '{}'. Invalid path.", path);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Invalid directory path contains directory traversal or illegal characters";
            err.method = "delete_directory";
            on_error(err);
        }
        return;
    }

    spdlog::info("[Moonraker API] Deleting directory: {} (force: {})", path, force);

    json params = {{"path", path}, {"force", force}};

    client_.send_jsonrpc(
        "server.files.delete_directory", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] Directory deleted successfully");
            on_success();
        },
        on_error);
}

// ============================================================================
// Job Control Operations
// ============================================================================

void MoonrakerAPI::start_print(const std::string& filename, SuccessCallback on_success,
                               ErrorCallback on_error) {
    // Validate filename path
    if (!is_safe_path(filename)) {
        NOTIFY_ERROR("Cannot start print. File '{}' has invalid path.", filename);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid filename contains directory traversal or illegal characters";
            err.method = "start_print";
            on_error(err);
        }
        return;
    }

    json params = {{"filename", filename}};

    spdlog::info("[Moonraker API] Starting print: {}", filename);

    client_.send_jsonrpc(
        "printer.print.start", params,
        [on_success](json) {
            spdlog::info("[Moonraker API] Print started successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::pause_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Pausing print");

    client_.send_jsonrpc(
        "printer.print.pause", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print paused successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::resume_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Resuming print");

    client_.send_jsonrpc(
        "printer.print.resume", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print resumed successfully");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::cancel_print(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Canceling print");

    client_.send_jsonrpc(
        "printer.print.cancel", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Print canceled successfully");
            on_success();
        },
        on_error);
}

// ============================================================================
// Motion Control Operations
// ============================================================================

void MoonrakerAPI::home_axes(const std::string& axes, SuccessCallback on_success,
                             ErrorCallback on_error) {
    // Validate axes string (empty means all, or contains only XYZE)
    if (!axes.empty()) {
        for (char axis : axes) {
            if (!is_valid_axis(axis)) {
                NOTIFY_ERROR("Invalid axis '{}' in homing command. Must be X, Y, Z, or E.", axis);
                if (on_error) {
                    MoonrakerError err;
                    err.type = MoonrakerErrorType::VALIDATION_ERROR;
                    err.message = "Invalid axis character (must be X, Y, Z, or E)";
                    err.method = "home_axes";
                    on_error(err);
                }
                return;
            }
        }
    }

    std::string gcode = generate_home_gcode(axes);
    spdlog::info("[Moonraker API] Homing axes: {} (G-code: {})", axes.empty() ? "all" : axes,
                 gcode);

    execute_gcode(gcode, on_success, on_error);
}

void MoonrakerAPI::move_axis(char axis, double distance, double feedrate,
                             SuccessCallback on_success, ErrorCallback on_error) {
    // Validate axis
    if (!is_valid_axis(axis)) {
        NOTIFY_ERROR("Invalid axis '{}'. Must be X, Y, Z, or E.", axis);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid axis: " + std::string(1, axis) + " (must be X, Y, Z, or E)";
            err.method = "move_axis";
            on_error(err);
        }
        return;
    }

    // Validate distance is within safety limits
    if (!is_safe_distance(distance, safety_limits_)) {
        NOTIFY_ERROR("Move distance {:.1f}mm is too large. Maximum: {:.1f}mm.", std::abs(distance),
                     safety_limits_.max_relative_distance_mm);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Distance " + std::to_string(distance) + "mm exceeds safety limits (" +
                          std::to_string(safety_limits_.min_relative_distance_mm) + "-" +
                          std::to_string(safety_limits_.max_relative_distance_mm) + "mm)";
            err.method = "move_axis";
            on_error(err);
        }
        return;
    }

    // Validate feedrate if specified (0 means use default, negative is invalid)
    if (feedrate != 0 && !is_safe_feedrate(feedrate, safety_limits_)) {
        NOTIFY_ERROR("Speed {:.0f}mm/min is too fast. Maximum: {:.0f}mm/min.", feedrate,
                     safety_limits_.max_feedrate_mm_min);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Feedrate " + std::to_string(feedrate) +
                          "mm/min exceeds safety limits (" +
                          std::to_string(safety_limits_.min_feedrate_mm_min) + "-" +
                          std::to_string(safety_limits_.max_feedrate_mm_min) + "mm/min)";
            err.method = "move_axis";
            on_error(err);
        }
        return;
    }

    std::string gcode = generate_move_gcode(axis, distance, feedrate);
    spdlog::info("[Moonraker API] Moving axis {} by {}mm (G-code: {})", axis, distance, gcode);

    execute_gcode(gcode, on_success, on_error);
}

void MoonrakerAPI::move_to_position(char axis, double position, double feedrate,
                                    SuccessCallback on_success, ErrorCallback on_error) {
    // Validate axis
    if (!is_valid_axis(axis)) {
        NOTIFY_ERROR("Invalid axis '{}'. Must be X, Y, Z, or E.", axis);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid axis character (must be X, Y, Z, or E)";
            err.method = "move_to_position";
            on_error(err);
        }
        return;
    }

    // Validate position is within safety limits
    if (!is_safe_position(position, safety_limits_)) {
        NOTIFY_ERROR("Position {:.1f}mm is out of range. Valid: {:.1f}mm to {:.1f}mm.", position,
                     safety_limits_.min_absolute_position_mm,
                     safety_limits_.max_absolute_position_mm);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Position " + std::to_string(position) + "mm exceeds safety limits (" +
                          std::to_string(safety_limits_.min_absolute_position_mm) + "-" +
                          std::to_string(safety_limits_.max_absolute_position_mm) + "mm)";
            err.method = "move_to_position";
            on_error(err);
        }
        return;
    }

    // Validate feedrate if specified (0 means use default, negative is invalid)
    if (feedrate != 0 && !is_safe_feedrate(feedrate, safety_limits_)) {
        NOTIFY_ERROR("Speed {:.0f}mm/min is too fast. Maximum: {:.0f}mm/min.", feedrate,
                     safety_limits_.max_feedrate_mm_min);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Feedrate " + std::to_string(feedrate) +
                          "mm/min exceeds safety limits (" +
                          std::to_string(safety_limits_.min_feedrate_mm_min) + "-" +
                          std::to_string(safety_limits_.max_feedrate_mm_min) + "mm/min)";
            err.method = "move_to_position";
            on_error(err);
        }
        return;
    }

    std::string gcode = generate_absolute_move_gcode(axis, position, feedrate);
    spdlog::info("[Moonraker API] Moving axis {} to {}mm (G-code: {})", axis, position, gcode);

    execute_gcode(gcode, on_success, on_error);
}

// ============================================================================
// Temperature Control Operations
// ============================================================================

void MoonrakerAPI::set_temperature(const std::string& heater, double temperature,
                                   SuccessCallback on_success, ErrorCallback on_error) {
    // Validate heater name
    if (!is_safe_identifier(heater)) {
        NOTIFY_ERROR("Invalid heater name '{}'. Contains unsafe characters.", heater);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid heater name contains illegal characters";
            err.method = "set_temperature";
            on_error(err);
        }
        return;
    }

    // Validate temperature range
    if (!is_safe_temperature(temperature, safety_limits_)) {
        NOTIFY_ERROR("Temperature {:.0f}°C is out of range. Valid: {:.0f}°C to {:.0f}°C.",
                     temperature, safety_limits_.min_temperature_celsius,
                     safety_limits_.max_temperature_celsius);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Temperature " + std::to_string(static_cast<int>(temperature)) +
                "°C exceeds safety limits (" +
                std::to_string(static_cast<int>(safety_limits_.min_temperature_celsius)) + "-" +
                std::to_string(static_cast<int>(safety_limits_.max_temperature_celsius)) + "°C)";
            err.method = "set_temperature";
            on_error(err);
        }
        return;
    }

    std::ostringstream gcode;
    gcode << "SET_HEATER_TEMPERATURE HEATER=" << heater << " TARGET=" << temperature;

    spdlog::info("[Moonraker API] Setting {} temperature to {}°C", heater, temperature);

    execute_gcode(gcode.str(), on_success, on_error);
}

void MoonrakerAPI::set_fan_speed(const std::string& fan, double speed, SuccessCallback on_success,
                                 ErrorCallback on_error) {
    // Validate fan name
    if (!is_safe_identifier(fan)) {
        NOTIFY_ERROR("Invalid fan name '{}'. Contains unsafe characters.", fan);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid fan name contains illegal characters";
            err.method = "set_fan_speed";
            on_error(err);
        }
        return;
    }

    // Validate speed percentage
    if (!is_safe_fan_speed(speed, safety_limits_)) {
        NOTIFY_ERROR("Fan speed {:.0f}% is out of range. Valid: {:.0f}% to {:.0f}%.", speed,
                     safety_limits_.min_fan_speed_percent, safety_limits_.max_fan_speed_percent);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message =
                "Fan speed " + std::to_string(static_cast<int>(speed)) +
                "% exceeds safety limits (" +
                std::to_string(static_cast<int>(safety_limits_.min_fan_speed_percent)) + "-" +
                std::to_string(static_cast<int>(safety_limits_.max_fan_speed_percent)) + "%)";
            err.method = "set_fan_speed";
            on_error(err);
        }
        return;
    }

    // Convert percentage to 0-255 range for M106 command
    int fan_value = static_cast<int>(speed * 255.0 / 100.0);

    std::ostringstream gcode;
    if (fan == "fan") {
        // Part cooling fan uses M106
        gcode << "M106 S" << fan_value;
    } else {
        // Generic fans use SET_FAN_SPEED
        gcode << "SET_FAN_SPEED FAN=" << fan << " SPEED=" << (speed / 100.0);
    }

    spdlog::info("[Moonraker API] Setting {} speed to {}%", fan, speed);

    execute_gcode(gcode.str(), on_success, on_error);
}

void MoonrakerAPI::set_led(const std::string& led, double red, double green, double blue,
                           double white, SuccessCallback on_success, ErrorCallback on_error) {
    // Validate LED name
    if (!is_safe_identifier(led)) {
        NOTIFY_ERROR("Invalid LED name '{}'. Contains unsafe characters.", led);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid LED name contains illegal characters";
            err.method = "set_led";
            on_error(err);
        }
        return;
    }

    // Clamp color values to 0.0-1.0 range
    red = std::clamp(red, 0.0, 1.0);
    green = std::clamp(green, 0.0, 1.0);
    blue = std::clamp(blue, 0.0, 1.0);
    white = std::clamp(white, 0.0, 1.0);

    // Extract just the LED name without the type prefix (e.g., "neopixel " or "led ")
    std::string led_name = led;
    size_t space_pos = led.find(' ');
    if (space_pos != std::string::npos) {
        led_name = led.substr(space_pos + 1);
    }

    // Build SET_LED G-code command
    std::ostringstream gcode;
    gcode << "SET_LED LED=" << led_name << " RED=" << red << " GREEN=" << green << " BLUE=" << blue;

    // Only add WHITE parameter if non-zero (for RGBW LEDs)
    if (white > 0.0) {
        gcode << " WHITE=" << white;
    }

    spdlog::info("[Moonraker API] Setting LED {}: R={:.2f} G={:.2f} B={:.2f} W={:.2f}", led_name,
                 red, green, blue, white);

    execute_gcode(gcode.str(), on_success, on_error);
}

void MoonrakerAPI::set_led_on(const std::string& led, SuccessCallback on_success,
                              ErrorCallback on_error) {
    set_led(led, 1.0, 1.0, 1.0, 1.0, on_success, on_error);
}

void MoonrakerAPI::set_led_off(const std::string& led, SuccessCallback on_success,
                               ErrorCallback on_error) {
    set_led(led, 0.0, 0.0, 0.0, 0.0, on_success, on_error);
}

// ============================================================================
// System Control Operations
// ============================================================================

void MoonrakerAPI::execute_gcode(const std::string& gcode, SuccessCallback on_success,
                                 ErrorCallback on_error) {
    json params = {{"script", gcode}};

    spdlog::debug("[Moonraker API] Executing G-code: {}", gcode);

    client_.send_jsonrpc(
        "printer.gcode.script", params, [on_success](json) { on_success(); }, on_error);
}

// ============================================================================
// Object Exclusion Operations
// ============================================================================

void MoonrakerAPI::exclude_object(const std::string& object_name, SuccessCallback on_success,
                                  ErrorCallback on_error) {
    // Validate object name to prevent G-code injection
    if (!is_safe_identifier(object_name)) {
        NOTIFY_ERROR("Invalid object name '{}'. Contains unsafe characters.", object_name);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid object name contains illegal characters";
            err.method = "exclude_object";
            on_error(err);
        }
        return;
    }

    std::ostringstream gcode;
    gcode << "EXCLUDE_OBJECT NAME=" << object_name;

    spdlog::info("[Moonraker API] Excluding object: {}", object_name);

    execute_gcode(gcode.str(), on_success, on_error);
}

void MoonrakerAPI::emergency_stop(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] Emergency stop requested!");

    client_.send_jsonrpc(
        "printer.emergency_stop", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Emergency stop executed");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::restart_firmware(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Restarting firmware");

    client_.send_jsonrpc(
        "printer.firmware_restart", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Firmware restart initiated");
            on_success();
        },
        on_error);
}

void MoonrakerAPI::restart_klipper(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Restarting Klipper");

    client_.send_jsonrpc(
        "printer.restart", json::object(),
        [on_success](json) {
            spdlog::info("[Moonraker API] Klipper restart initiated");
            on_success();
        },
        on_error);
}

// ============================================================================
// Query Operations
// ============================================================================

void MoonrakerAPI::is_printer_ready(BoolCallback on_result, ErrorCallback on_error) {
    client_.send_jsonrpc(
        "printer.info", json::object(),
        [on_result](json response) {
            bool ready = false;
            if (response.contains("result") && response["result"].contains("state")) {
                std::string state = response["result"]["state"].get<std::string>();
                ready = (state == "ready");
            }
            on_result(ready);
        },
        on_error);
}

void MoonrakerAPI::get_print_state(StringCallback on_result, ErrorCallback on_error) {
    json params = {{"objects", json::object({{"print_stats", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_result](json response) {
            std::string state = "unknown";
            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].contains("print_stats") &&
                response["result"]["status"]["print_stats"].contains("state")) {
                state = response["result"]["status"]["print_stats"]["state"].get<std::string>();
            }
            on_result(state);
        },
        on_error);
}

void MoonrakerAPI::update_safety_limits_from_printer(SuccessCallback on_success,
                                                     ErrorCallback on_error) {
    // Only update if limits haven't been explicitly set
    if (limits_explicitly_set_) {
        spdlog::debug("[Moonraker API] Safety limits explicitly configured, skipping Moonraker "
                      "auto-detection");
        if (on_success) {
            on_success();
        }
        return;
    }

    // Query printer configuration for safety limits
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [this, on_success](json response) {
            try {
                if (!response.contains("result") || !response["result"].contains("status") ||
                    !response["result"]["status"].contains("configfile") ||
                    !response["result"]["status"]["configfile"].contains("settings")) {
                    spdlog::warn("[Moonraker API] Printer configuration not available, using "
                                 "default safety limits");
                    if (on_success) {
                        on_success();
                    }
                    return;
                }

                const json& settings = response["result"]["status"]["configfile"]["settings"];
                bool updated = false;

                // Extract max_velocity from printer settings
                if (settings.contains("printer") && settings["printer"].contains("max_velocity")) {
                    double max_velocity_mm_s = settings["printer"]["max_velocity"].get<double>();
                    safety_limits_.max_feedrate_mm_min = max_velocity_mm_s * 60.0;
                    updated = true;
                    spdlog::info(
                        "[Moonraker API] Updated max_feedrate from printer config: {} mm/min",
                        safety_limits_.max_feedrate_mm_min);
                }

                // Extract axis limits from stepper configurations
                for (const std::string& stepper : {"stepper_x", "stepper_y", "stepper_z"}) {
                    if (settings.contains(stepper)) {
                        if (settings[stepper].contains("position_max")) {
                            double pos_max = settings[stepper]["position_max"].get<double>();
                            // Use the largest axis max as absolute position limit
                            if (pos_max > safety_limits_.max_absolute_position_mm) {
                                safety_limits_.max_absolute_position_mm = pos_max;
                                updated = true;
                            }
                        }
                        if (settings[stepper].contains("position_min")) {
                            double pos_min = settings[stepper]["position_min"].get<double>();
                            // Use the smallest (most negative) axis min as absolute position limit
                            if (pos_min < safety_limits_.min_absolute_position_mm) {
                                safety_limits_.min_absolute_position_mm = pos_min;
                                updated = true;
                            }
                        }
                    }
                }

                // Extract temperature limits from heater configurations
                for (const auto& [key, value] : settings.items()) {
                    if ((key.find("extruder") != std::string::npos ||
                         key.find("heater_") != std::string::npos) &&
                        value.is_object()) {
                        if (value.contains("max_temp")) {
                            double max_temp = value["max_temp"].get<double>();
                            // Use the highest heater max_temp as temperature limit
                            if (max_temp > safety_limits_.max_temperature_celsius) {
                                safety_limits_.max_temperature_celsius = max_temp;
                                updated = true;
                            }
                        }
                        if (value.contains("min_temp")) {
                            double min_temp = value["min_temp"].get<double>();
                            // Use the lowest heater min_temp as temperature limit
                            if (min_temp < safety_limits_.min_temperature_celsius) {
                                safety_limits_.min_temperature_celsius = min_temp;
                                updated = true;
                            }
                        }
                    }
                }

                if (updated) {
                    spdlog::info(
                        "[Moonraker API] Updated safety limits from printer configuration:");
                    spdlog::info("[Moonraker API]   Temperature: {} to {}°C",
                                 safety_limits_.min_temperature_celsius,
                                 safety_limits_.max_temperature_celsius);
                    spdlog::info("[Moonraker API]   Position: {} to {}mm",
                                 safety_limits_.min_absolute_position_mm,
                                 safety_limits_.max_absolute_position_mm);
                    spdlog::info("[Moonraker API]   Feedrate: {} to {} mm/min",
                                 safety_limits_.min_feedrate_mm_min,
                                 safety_limits_.max_feedrate_mm_min);
                } else {
                    spdlog::debug("[Moonraker API] No safety limit overrides found in printer "
                                  "config, using defaults");
                }

                if (on_success) {
                    on_success();
                }
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("Failed to parse printer configuration for safety limits: {}",
                                   e.what());
                if (on_success) {
                    on_success(); // Continue with defaults on parse error
                }
            }
        },
        on_error);
}

// ============================================================================
// HTTP File Transfer Operations
// ============================================================================

void MoonrakerAPI::download_file(const std::string& root, const std::string& path,
                                  StringCallback on_success, ErrorCallback on_error) {
    // Validate inputs
    if (!is_safe_path(path)) {
        spdlog::error("[Moonraker API] Invalid file path for download: {}", path);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid file path contains unsafe characters";
            err.method = "download_file";
            on_error(err);
        }
        return;
    }

    if (http_base_url_.empty()) {
        spdlog::error("[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::CONNECTION_LOST;
            err.message = "HTTP base URL not configured";
            err.method = "download_file";
            on_error(err);
        }
        return;
    }

    // Build URL: http://host:port/server/files/{root}/{path}
    std::string url = http_base_url_ + "/server/files/" + root + "/" + path;

    spdlog::debug("[Moonraker API] Downloading file: {}", url);

    // Run HTTP request in a separate thread to avoid blocking
    std::thread([url, path, on_success, on_error]() {
        auto resp = requests::get(url.c_str());

        if (!resp) {
            spdlog::error("[Moonraker API] HTTP request failed for: {}", url);
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::CONNECTION_LOST;
                err.message = "HTTP request failed";
                err.method = "download_file";
                on_error(err);
            }
            return;
        }

        if (resp->status_code == 404) {
            spdlog::error("[Moonraker API] File not found: {}", path);
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::FILE_NOT_FOUND;
                err.code = resp->status_code;
                err.message = "File not found: " + path;
                err.method = "download_file";
                on_error(err);
            }
            return;
        }

        if (resp->status_code != 200) {
            spdlog::error("[Moonraker API] HTTP {} downloading {}: {}",
                          static_cast<int>(resp->status_code), path, resp->status_message());
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::UNKNOWN;
                err.code = static_cast<int>(resp->status_code);
                err.message = "HTTP " + std::to_string(static_cast<int>(resp->status_code)) +
                              ": " + resp->status_message();
                err.method = "download_file";
                on_error(err);
            }
            return;
        }

        spdlog::debug("[Moonraker API] Downloaded {} bytes from {}",
                      resp->body.size(), path);

        if (on_success) {
            on_success(resp->body);
        }
    }).detach();
}

void MoonrakerAPI::upload_file(const std::string& root, const std::string& path,
                                const std::string& content, SuccessCallback on_success,
                                ErrorCallback on_error) {
    upload_file_with_name(root, path, path, content, on_success, on_error);
}

void MoonrakerAPI::upload_file_with_name(const std::string& root, const std::string& path,
                                          const std::string& filename, const std::string& content,
                                          SuccessCallback on_success, ErrorCallback on_error) {
    // Validate inputs
    if (!is_safe_path(path)) {
        spdlog::error("[Moonraker API] Invalid file path for upload: {}", path);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid file path contains unsafe characters";
            err.method = "upload_file";
            on_error(err);
        }
        return;
    }

    if (http_base_url_.empty()) {
        spdlog::error("[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::CONNECTION_LOST;
            err.message = "HTTP base URL not configured";
            err.method = "upload_file";
            on_error(err);
        }
        return;
    }

    // Build URL: http://host:port/server/files/upload
    std::string url = http_base_url_ + "/server/files/upload";

    spdlog::debug("[Moonraker API] Uploading {} bytes to {}/{}", content.size(), root, path);

    // Run HTTP request in a separate thread to avoid blocking
    std::thread([url, root, path, filename, content, on_success, on_error]() {
        // Create multipart form request
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = url;
        req->timeout = 120;  // 2 minute timeout for uploads
        req->content_type = MULTIPART_FORM_DATA;

        // Add root parameter (e.g., "gcodes" or "config")
        req->SetFormData("root", root);

        // Add path parameter if uploading to subdirectory
        if (path.find('/') != std::string::npos) {
            // Extract directory from path
            size_t last_slash = path.rfind('/');
            if (last_slash != std::string::npos) {
                std::string directory = path.substr(0, last_slash);
                req->SetFormData("path", directory);
            }
        }

        // Add file content with filename
        // Use hv::FormData for multipart file upload
        hv::FormData file_data;
        file_data.content = content;
        file_data.filename = filename;
        req->form["file"] = file_data;

        // Send request
        auto resp = requests::request(req);

        if (!resp) {
            spdlog::error("[Moonraker API] HTTP upload request failed to: {}", url);
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::CONNECTION_LOST;
                err.message = "HTTP upload request failed";
                err.method = "upload_file";
                on_error(err);
            }
            return;
        }

        if (resp->status_code != 201 && resp->status_code != 200) {
            spdlog::error("[Moonraker API] HTTP {} uploading {}: {} - {}",
                          static_cast<int>(resp->status_code), path, resp->status_message(),
                          resp->body);
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::UNKNOWN;
                err.code = static_cast<int>(resp->status_code);
                err.message = "HTTP " + std::to_string(static_cast<int>(resp->status_code)) +
                              ": " + resp->status_message();
                err.method = "upload_file";
                on_error(err);
            }
            return;
        }

        spdlog::info("[Moonraker API] Successfully uploaded {} ({} bytes)",
                     path, content.size());

        if (on_success) {
            on_success();
        }
    }).detach();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::vector<FileInfo> MoonrakerAPI::parse_file_list(const json& response) {
    std::vector<FileInfo> files;

    if (!response.contains("result")) {
        return files;
    }

    const json& result = response["result"];

    // Parse directory items
    if (result.contains("dirs")) {
        for (const auto& dir : result["dirs"]) {
            FileInfo info;
            if (dir.contains("dirname")) {
                info.filename = dir["dirname"].get<std::string>();
                info.is_dir = true;
            }
            if (dir.contains("modified")) {
                info.modified = dir["modified"].get<double>();
            }
            if (dir.contains("permissions")) {
                info.permissions = dir["permissions"].get<std::string>();
            }
            files.push_back(info);
        }
    }

    // Parse file items
    if (result.contains("files")) {
        for (const auto& file : result["files"]) {
            FileInfo info;
            if (file.contains("filename")) {
                info.filename = file["filename"].get<std::string>();
            }
            if (file.contains("path")) {
                info.path = file["path"].get<std::string>();
            }
            if (file.contains("size")) {
                info.size = file["size"].get<uint64_t>();
            }
            if (file.contains("modified")) {
                info.modified = file["modified"].get<double>();
            }
            if (file.contains("permissions")) {
                info.permissions = file["permissions"].get<std::string>();
            }
            info.is_dir = false;
            files.push_back(info);
        }
    }

    return files;
}

FileMetadata MoonrakerAPI::parse_file_metadata(const json& response) {
    FileMetadata metadata;

    if (!response.contains("result")) {
        return metadata;
    }

    const json& result = response["result"];

    // Basic file info
    if (result.contains("filename")) {
        metadata.filename = result["filename"].get<std::string>();
    }
    if (result.contains("size")) {
        metadata.size = result["size"].get<uint64_t>();
    }
    if (result.contains("modified")) {
        metadata.modified = result["modified"].get<double>();
    }

    // Slicer info
    if (result.contains("slicer")) {
        metadata.slicer = result["slicer"].get<std::string>();
    }
    if (result.contains("slicer_version")) {
        metadata.slicer_version = result["slicer_version"].get<std::string>();
    }

    // Print info
    if (result.contains("print_start_time")) {
        metadata.print_start_time = result["print_start_time"].get<double>();
    }
    if (result.contains("job_id")) {
        metadata.job_id = result["job_id"].get<double>();
    }
    if (result.contains("layer_count")) {
        metadata.layer_count = result["layer_count"].get<uint32_t>();
    }
    if (result.contains("object_height")) {
        metadata.object_height = result["object_height"].get<double>();
    }
    if (result.contains("estimated_time")) {
        metadata.estimated_time = result["estimated_time"].get<double>();
    }

    // Filament info
    if (result.contains("filament_total")) {
        metadata.filament_total = result["filament_total"].get<double>();
    }
    if (result.contains("filament_weight_total")) {
        metadata.filament_weight_total = result["filament_weight_total"].get<double>();
    }
    if (result.contains("filament_type")) {
        // Moonraker returns "PLA;PLA;PLA;PLA" for multi-extruder - take first value
        std::string raw_type = result["filament_type"].get<std::string>();
        size_t semicolon = raw_type.find(';');
        metadata.filament_type =
            (semicolon != std::string::npos) ? raw_type.substr(0, semicolon) : raw_type;
    }

    // Temperature info
    if (result.contains("first_layer_bed_temp")) {
        metadata.first_layer_bed_temp = result["first_layer_bed_temp"].get<double>();
    }
    if (result.contains("first_layer_extr_temp")) {
        metadata.first_layer_extr_temp = result["first_layer_extr_temp"].get<double>();
    }

    // G-code info
    if (result.contains("gcode_start_byte")) {
        metadata.gcode_start_byte = result["gcode_start_byte"].get<uint64_t>();
    }
    if (result.contains("gcode_end_byte")) {
        metadata.gcode_end_byte = result["gcode_end_byte"].get<uint64_t>();
    }

    // Thumbnails
    if (result.contains("thumbnails")) {
        for (const auto& thumb : result["thumbnails"]) {
            if (thumb.contains("relative_path")) {
                metadata.thumbnails.push_back(thumb["relative_path"].get<std::string>());
            }
        }
    }

    return metadata;
}

std::string MoonrakerAPI::generate_home_gcode(const std::string& axes) {
    if (axes.empty()) {
        return "G28"; // Home all axes
    } else {
        std::ostringstream gcode;
        gcode << "G28";
        for (char axis : axes) {
            gcode << " " << static_cast<char>(std::toupper(axis));
        }
        return gcode.str();
    }
}

std::string MoonrakerAPI::generate_move_gcode(char axis, double distance, double feedrate) {
    std::ostringstream gcode;
    gcode << "G91\n"; // Relative positioning
    gcode << "G0 " << static_cast<char>(std::toupper(axis)) << distance;
    if (feedrate > 0) {
        gcode << " F" << feedrate;
    }
    gcode << "\nG90"; // Back to absolute positioning
    return gcode.str();
}

std::string MoonrakerAPI::generate_absolute_move_gcode(char axis, double position,
                                                       double feedrate) {
    std::ostringstream gcode;
    gcode << "G90\n"; // Absolute positioning
    gcode << "G0 " << static_cast<char>(std::toupper(axis)) << position;
    if (feedrate > 0) {
        gcode << " F" << feedrate;
    }
    return gcode.str();
}

// ============================================================================
// Domain Service Operations (Hardware Discovery, Bed Mesh, Object Exclusion)
// ============================================================================

std::string MoonrakerAPI::guess_bed_heater() const {
    const auto& heaters = client_.get_heaters();

    // Priority 1: Exact match "heater_bed" (most common)
    if (std::find(heaters.begin(), heaters.end(), "heater_bed") != heaters.end()) {
        spdlog::debug("[Moonraker API] guess_bed_heater() -> 'heater_bed'");
        return "heater_bed";
    }

    // Priority 2: Exact match "heated_bed"
    if (std::find(heaters.begin(), heaters.end(), "heated_bed") != heaters.end()) {
        spdlog::debug("[Moonraker API] guess_bed_heater() -> 'heated_bed'");
        return "heated_bed";
    }

    // Priority 3: Substring match - any heater containing "bed"
    for (const auto& heater : heaters) {
        if (heater.find("bed") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_bed_heater() -> '{}'", heater);
            return heater;
        }
    }

    spdlog::debug("[Moonraker API] guess_bed_heater() -> no match found");
    return "";
}

std::string MoonrakerAPI::guess_hotend_heater() const {
    const auto& heaters = client_.get_heaters();

    // Priority 1: Exact match "extruder" (base extruder, most common)
    if (std::find(heaters.begin(), heaters.end(), "extruder") != heaters.end()) {
        spdlog::debug("[Moonraker API] guess_hotend_heater() -> 'extruder'");
        return "extruder";
    }

    // Priority 2: Exact match "extruder0"
    if (std::find(heaters.begin(), heaters.end(), "extruder0") != heaters.end()) {
        spdlog::debug("[Moonraker API] guess_hotend_heater() -> 'extruder0'");
        return "extruder0";
    }

    // Priority 3: Substring match - any heater containing "extruder"
    for (const auto& heater : heaters) {
        if (heater.find("extruder") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    // Priority 4: Substring match - any heater containing "hotend"
    for (const auto& heater : heaters) {
        if (heater.find("hotend") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    // Priority 5: Substring match - any heater containing "e0"
    for (const auto& heater : heaters) {
        if (heater.find("e0") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    spdlog::debug("[Moonraker API] guess_hotend_heater() -> no match found");
    return "";
}

std::string MoonrakerAPI::guess_bed_sensor() const {
    // Heaters have built-in thermistors, so check heaters first
    std::string bed_heater = guess_bed_heater();
    if (!bed_heater.empty()) {
        spdlog::debug("[Moonraker API] guess_bed_sensor() -> '{}' (from heater)", bed_heater);
        return bed_heater;
    }

    // No bed heater found, search sensors for names containing "bed"
    const auto& sensors = client_.get_sensors();
    for (const auto& sensor : sensors) {
        if (sensor.find("bed") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_bed_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    spdlog::debug("[Moonraker API] guess_bed_sensor() -> no match found");
    return "";
}

std::string MoonrakerAPI::guess_hotend_sensor() const {
    // Heaters have built-in thermistors, so check heaters first
    std::string hotend_heater = guess_hotend_heater();
    if (!hotend_heater.empty()) {
        spdlog::debug("[Moonraker API] guess_hotend_sensor() -> '{}' (from heater)", hotend_heater);
        return hotend_heater;
    }

    // No hotend heater found, search sensors for names containing relevant patterns
    // Priority: extruder > hotend > e0
    const auto& sensors = client_.get_sensors();

    for (const auto& sensor : sensors) {
        if (sensor.find("extruder") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    for (const auto& sensor : sensors) {
        if (sensor.find("hotend") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    for (const auto& sensor : sensors) {
        if (sensor.find("e0") != std::string::npos) {
            spdlog::debug("[Moonraker API] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    spdlog::debug("[Moonraker API] guess_hotend_sensor() -> no match found");
    return "";
}

const BedMeshProfile* MoonrakerAPI::get_active_bed_mesh() const {
    // Suppress deprecation warning - we're the migration target
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const BedMeshProfile& mesh = client_.get_active_bed_mesh();
#pragma GCC diagnostic pop

    if (mesh.probed_matrix.empty()) {
        return nullptr;
    }
    return &mesh;
}

std::vector<std::string> MoonrakerAPI::get_bed_mesh_profiles() const {
    // Suppress deprecation warning - we're the migration target
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return client_.get_bed_mesh_profiles();
#pragma GCC diagnostic pop
}

bool MoonrakerAPI::has_bed_mesh() const {
    // Suppress deprecation warning - we're the migration target
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return client_.has_bed_mesh();
#pragma GCC diagnostic pop
}

void MoonrakerAPI::get_excluded_objects(std::function<void(const std::set<std::string>&)> on_success,
                                        ErrorCallback on_error) {
    // Query exclude_object state from Klipper
    json params = {{"objects", json::object({{"exclude_object", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success](json response) {
            std::set<std::string> excluded;

            try {
                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains("exclude_object")) {
                    const json& exclude_obj = response["result"]["status"]["exclude_object"];

                    // excluded_objects is an array of object names
                    if (exclude_obj.contains("excluded_objects") &&
                        exclude_obj["excluded_objects"].is_array()) {
                        for (const auto& obj : exclude_obj["excluded_objects"]) {
                            if (obj.is_string()) {
                                excluded.insert(obj.get<std::string>());
                            }
                        }
                    }
                }

                spdlog::debug("[Moonraker API] get_excluded_objects() -> {} objects",
                              excluded.size());
                if (on_success) {
                    on_success(excluded);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker API] Failed to parse excluded objects: {}", e.what());
                if (on_success) {
                    on_success(std::set<std::string>{}); // Return empty set on error
                }
            }
        },
        on_error);
}

void MoonrakerAPI::get_available_objects(
    std::function<void(const std::vector<std::string>&)> on_success, ErrorCallback on_error) {
    // Query exclude_object state from Klipper
    json params = {{"objects", json::object({{"exclude_object", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success](json response) {
            std::vector<std::string> objects;

            try {
                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains("exclude_object")) {
                    const json& exclude_obj = response["result"]["status"]["exclude_object"];

                    // objects is an array of {name, center, polygon} objects
                    if (exclude_obj.contains("objects") && exclude_obj["objects"].is_array()) {
                        for (const auto& obj : exclude_obj["objects"]) {
                            if (obj.is_object() && obj.contains("name") && obj["name"].is_string()) {
                                objects.push_back(obj["name"].get<std::string>());
                            }
                        }
                    }
                }

                spdlog::debug("[Moonraker API] get_available_objects() -> {} objects",
                              objects.size());
                if (on_success) {
                    on_success(objects);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker API] Failed to parse available objects: {}", e.what());
                if (on_success) {
                    on_success(std::vector<std::string>{}); // Return empty vector on error
                }
            }
        },
        on_error);
}