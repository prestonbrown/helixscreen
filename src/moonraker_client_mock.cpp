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

#include "moonraker_client_mock.h"

#include "../tests/mocks/mock_printer_state.h"
#include "gcode_parser.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

// Directory paths for mock G-code files
static constexpr const char* TEST_GCODE_DIR = "assets/test_gcodes";
static constexpr const char* THUMBNAIL_CACHE_DIR = "build/thumbnail_cache";

/**
 * @brief Scan test directory for G-code files
 * @return Vector of filenames (not full paths)
 */
static std::vector<std::string> scan_mock_gcode_files() {
    std::vector<std::string> files;

    DIR* dir = opendir(TEST_GCODE_DIR);
    if (!dir) {
        spdlog::warn("[MoonrakerClientMock] Cannot open test G-code directory: {}", TEST_GCODE_DIR);
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Skip hidden files and non-gcode files
        if (name[0] == '.' || name.length() < 7) {
            continue;
        }

        // Check for .gcode extension (case insensitive)
        std::string ext = name.substr(name.length() - 6);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gcode") {
            continue;
        }

        files.push_back(name);
    }

    closedir(dir);
    std::sort(files.begin(), files.end());

    spdlog::debug("[MoonrakerClientMock] Found {} mock G-code files", files.size());
    return files;
}

/**
 * @brief Build mock JSON response for server.files.list
 * @param path Directory path relative to gcodes root (empty = root)
 * @return JSON response matching Moonraker format
 */
static json build_mock_file_list_response(const std::string& path = "") {
    json files_array = json::array();
    json dirs_array = json::array();

    if (path.empty()) {
        // Root directory - add mock subdirectories plus real files
        auto filenames = scan_mock_gcode_files();

        for (const auto& filename : filenames) {
            std::string full_path = std::string(TEST_GCODE_DIR) + "/" + filename;

            struct stat file_stat;
            uint64_t size = 0;
            double modified = 0.0;
            if (stat(full_path.c_str(), &file_stat) == 0) {
                size = static_cast<uint64_t>(file_stat.st_size);
                modified = static_cast<double>(file_stat.st_mtime);
            }

            json file_entry = {{"filename", filename},
                               {"path", filename},
                               {"size", size},
                               {"modified", modified},
                               {"permissions", "rw"}};
            files_array.push_back(file_entry);
        }

        // Add mock subdirectories at root
        dirs_array.push_back({{"dirname", "calibration"},
                              {"modified", 1700000000.0},
                              {"size", 4096},
                              {"permissions", "rw"}});
        dirs_array.push_back({{"dirname", "my_projects"},
                              {"modified", 1699900000.0},
                              {"size", 4096},
                              {"permissions", "rw"}});
    } else if (path == "calibration") {
        // Mock calibration subdirectory
        files_array.push_back({{"filename", "temp_tower.gcode"},
                               {"path", "calibration/temp_tower.gcode"},
                               {"size", 125000},
                               {"modified", 1698000000.0},
                               {"permissions", "rw"}});
        files_array.push_back({{"filename", "bed_level_test.gcode"},
                               {"path", "calibration/bed_level_test.gcode"},
                               {"size", 85000},
                               {"modified", 1697500000.0},
                               {"permissions", "rw"}});
    } else if (path == "my_projects") {
        // Mock my_projects subdirectory with nested folder
        files_array.push_back({{"filename", "custom_bracket.gcode"},
                               {"path", "my_projects/custom_bracket.gcode"},
                               {"size", 250000},
                               {"modified", 1696000000.0},
                               {"permissions", "rw"}});
        dirs_array.push_back({{"dirname", "keycaps"},
                              {"modified", 1695000000.0},
                              {"size", 4096},
                              {"permissions", "rw"}});
    } else if (path == "my_projects/keycaps") {
        // Nested subdirectory
        files_array.push_back({{"filename", "cherry_mx_cap.gcode"},
                               {"path", "my_projects/keycaps/cherry_mx_cap.gcode"},
                               {"size", 45000},
                               {"modified", 1694000000.0},
                               {"permissions", "rw"}});
    }
    // Unknown paths return empty lists

    json response = {{"result", {{"dirs", dirs_array}, {"files", files_array}}}};

    spdlog::debug("[MoonrakerClientMock] Built mock file list for path '{}': {} dirs, {} files",
                  path.empty() ? "/" : path, dirs_array.size(), files_array.size());
    return response;
}

/**
 * @brief Build mock JSON response for server.files.metadata
 * @param filename Filename to get metadata for
 * @return JSON response matching Moonraker format
 */
static json build_mock_file_metadata_response(const std::string& filename) {
    std::string full_path = std::string(TEST_GCODE_DIR) + "/" + filename;

    // Get file info from filesystem
    struct stat file_stat;
    uint64_t size = 0;
    double modified = 0.0;
    if (stat(full_path.c_str(), &file_stat) == 0) {
        size = static_cast<uint64_t>(file_stat.st_size);
        modified = static_cast<double>(file_stat.st_mtime);
    }

    // Extract metadata from G-code header
    auto header_meta = gcode::extract_header_metadata(full_path);

    // Get cached thumbnail path (creates cache if needed)
    std::string thumbnail_path = gcode::get_cached_thumbnail(full_path, THUMBNAIL_CACHE_DIR);

    json thumbnails = json::array();
    if (!thumbnail_path.empty()) {
        // Return relative path to cached thumbnail (no LVGL prefix - that's a UI concern)
        // Format must match Moonraker's response structure: array of objects with relative_path
        thumbnails.push_back({{"relative_path", thumbnail_path}});
    }

    json result = {{"filename", filename},
                   {"size", size},
                   {"modified", modified},
                   {"slicer", header_meta.slicer},
                   {"slicer_version", header_meta.slicer_version},
                   {"estimated_time", header_meta.estimated_time_seconds},
                   {"filament_total", header_meta.filament_used_mm},
                   {"filament_weight_total", header_meta.filament_used_g},
                   {"filament_type", header_meta.filament_type},
                   {"layer_count", header_meta.layer_count},
                   {"first_layer_bed_temp", header_meta.first_layer_bed_temp},
                   {"first_layer_extr_temp", header_meta.first_layer_nozzle_temp},
                   {"thumbnails", thumbnails}};

    json response = {{"result", result}};

    spdlog::debug("[MoonrakerClientMock] Built metadata for '{}': {}s, {}g filament", filename,
                  header_meta.estimated_time_seconds, header_meta.filament_used_g);
    return response;
}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type) : printer_type_(type) {
    spdlog::info("[MoonrakerClientMock] Created with printer type: {}", static_cast<int>(type));

    // Populate hardware immediately (available for wizard without calling discover_printer())
    populate_hardware();
    spdlog::debug(
        "[MoonrakerClientMock] Hardware populated: {} heaters, {} sensors, {} fans, {} LEDs",
        heaters_.size(), sensors_.size(), fans_.size(), leds_.size());

    // Generate synthetic bed mesh data
    generate_mock_bed_mesh();
}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type, double speedup_factor)
    : printer_type_(type) {
    // Set speedup factor (clamped)
    speedup_factor_.store(std::clamp(speedup_factor, 0.1, 10000.0));

    spdlog::info("[MoonrakerClientMock] Created with printer type: {}, speedup: {}x",
                 static_cast<int>(type), speedup_factor_.load());

    // Populate hardware immediately (available for wizard without calling discover_printer())
    populate_hardware();
    spdlog::debug(
        "[MoonrakerClientMock] Hardware populated: {} heaters, {} sensors, {} fans, {} LEDs",
        heaters_.size(), sensors_.size(), fans_.size(), leds_.size());

    // Generate synthetic bed mesh data
    generate_mock_bed_mesh();
}

void MoonrakerClientMock::set_simulation_speedup(double factor) {
    double clamped = std::clamp(factor, 0.1, 10000.0);
    speedup_factor_.store(clamped);
    spdlog::info("[MoonrakerClientMock] Simulation speedup set to {}x", clamped);
}

double MoonrakerClientMock::get_simulation_speedup() const {
    return speedup_factor_.load();
}

int MoonrakerClientMock::get_current_layer() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (print_metadata_.layer_count == 0) {
        return 0;
    }
    return static_cast<int>(print_progress_.load() * print_metadata_.layer_count);
}

int MoonrakerClientMock::get_total_layers() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return static_cast<int>(print_metadata_.layer_count);
}

std::set<std::string> MoonrakerClientMock::get_excluded_objects() const {
    // If shared state is set, use that for consistency with MoonrakerAPIMock
    if (mock_state_) {
        return mock_state_->get_excluded_objects();
    }
    // Fallback to local state for backward compatibility
    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
    return excluded_objects_;
}

void MoonrakerClientMock::set_mock_state(std::shared_ptr<MockPrinterState> state) {
    mock_state_ = state;
    if (state) {
        spdlog::debug("[MoonrakerClientMock] Shared mock state attached");
    } else {
        spdlog::debug("[MoonrakerClientMock] Shared mock state detached");
    }
}

MoonrakerClientMock::~MoonrakerClientMock() {
    // Signal restart thread to stop and wait for it
    restart_pending_.store(false);
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }

    stop_temperature_simulation();
}

int MoonrakerClientMock::connect(const char* url, std::function<void()> on_connected,
                                 [[maybe_unused]] std::function<void()> on_disconnected) {
    spdlog::info("[MoonrakerClientMock] Simulating connection to: {}", url ? url : "(null)");

    // Simulate connection state change (same as real client)
    set_connection_state(ConnectionState::CONNECTING);

    // Small delay to simulate realistic connection (250ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    set_connection_state(ConnectionState::CONNECTED);

    // Start temperature simulation
    start_temperature_simulation();

    // Dispatch initial state BEFORE calling on_connected (matches real Moonraker behavior)
    // Real client sends initial state from subscription response - mock does it here
    dispatch_initial_state();

    // Immediately invoke connection callback
    if (on_connected) {
        spdlog::info("[MoonrakerClientMock] Simulated connection successful");
        on_connected();
    }

    // Store disconnect callback (never invoked in mock, but stored for consistency)
    // Note: Not needed for this simple mock implementation

    return 0; // Success
}

void MoonrakerClientMock::discover_printer(std::function<void()> on_complete) {
    spdlog::info("[MoonrakerClientMock] Simulating hardware discovery");

    // Populate hardware based on printer type
    populate_hardware();

    // Generate synthetic bed mesh data
    generate_mock_bed_mesh();

    // Set mock printer info (mimics server.info and printer.info responses)
    hostname_ = "mock-printer";
    software_version_ = "v0.12.0-mock";
    moonraker_version_ = "v0.8.0-mock";

    // Populate capabilities by creating mock Klipper object list
    json mock_objects = json::array();

    // Add common objects
    mock_objects.push_back("heater_bed");
    mock_objects.push_back("extruder");
    mock_objects.push_back("bed_mesh");
    mock_objects.push_back("probe"); // Most printers have a probe for bed mesh/leveling

    // Add LED objects from populated hardware
    for (const auto& led : leds_) {
        mock_objects.push_back(led);
    }

    // Add printer-specific objects
    switch (printer_type_) {
    case PrinterType::VORON_24:
        mock_objects.push_back("quad_gantry_level");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    case PrinterType::VORON_TRIDENT:
        mock_objects.push_back("z_tilt");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    default:
        // Other printers may not have these features
        break;
    }

    capabilities_.parse_objects(mock_objects);

    // Log discovered hardware
    spdlog::debug("[MoonrakerClientMock] Discovered: {} heaters, {} sensors, {} fans, {} LEDs",
                  heaters_.size(), sensors_.size(), fans_.size(), leds_.size());

    // Invoke discovery complete callback with capabilities (for PrinterState binding)
    if (on_discovery_complete_) {
        on_discovery_complete_(capabilities_);
    }

    // Invoke completion callback immediately (no async delay in mock)
    if (on_complete) {
        on_complete();
    }
}

void MoonrakerClientMock::populate_hardware() {
    // Clear existing data (inherited from MoonrakerClient)
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();

    // Populate based on printer type
    switch (printer_type_) {
    case PrinterType::VORON_24:
        // Voron 2.4 configuration
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor (Klipper naming: bare heater name)
                    "temperature_sensor chamber", "temperature_sensor raspberry_pi",
                    "temperature_sensor mcu_temp"};
        fans_ = {"heater_fan hotend_fan",
                 "fan", // Part cooling fan
                 "fan_generic nevermore", "controller_fan controller_fan"};
        leds_ = {"neopixel chamber_light", "neopixel status_led"};
        break;

    case PrinterType::VORON_TRIDENT:
        // Voron Trident configuration
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor (Klipper naming: bare heater name)
                    "temperature_sensor chamber",
                    "temperature_sensor raspberry_pi",
                    "temperature_sensor mcu_temp",
                    "temperature_sensor z_thermal_adjust"};
        fans_ = {"heater_fan hotend_fan", "fan", "fan_generic exhaust_fan",
                 "controller_fan electronics_fan"};
        leds_ = {"neopixel sb_leds", "neopixel chamber_leds"};
        break;

    case PrinterType::CREALITY_K1:
        // Creality K1/K1 Max configuration
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor (Klipper naming: bare heater name)
                    "temperature_sensor mcu_temp", "temperature_sensor host_temp"};
        fans_ = {"heater_fan hotend_fan", "fan", "fan_generic auxiliary_fan"};
        leds_ = {"neopixel logo_led"};
        break;

    case PrinterType::FLASHFORGE_AD5M:
        // FlashForge Adventurer 5M configuration
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor (Klipper naming: bare heater name)
                    "temperature_sensor chamber", "temperature_sensor mcu_temp"};
        fans_ = {"heater_fan hotend_fan", "fan", "fan_generic chamber_fan"};
        leds_ = {"led chamber_light"};
        break;

    case PrinterType::GENERIC_COREXY:
        // Generic CoreXY printer
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor (Klipper naming: bare heater name)
                    "temperature_sensor raspberry_pi"};
        fans_ = {"heater_fan hotend_fan", "fan"};
        leds_ = {};
        break;

    case PrinterType::GENERIC_BEDSLINGER:
        // Generic i3-style bedslinger
        heaters_ = {"heater_bed", "extruder"};
        sensors_ = {
            "heater_bed", // Bed thermistor (Klipper naming: bare heater name)
            "extruder"    // Hotend thermistor (Klipper naming: bare heater name)
        };
        fans_ = {"heater_fan hotend_fan", "fan"};
        leds_ = {};
        break;

    case PrinterType::MULTI_EXTRUDER:
        // Multi-extruder test case
        heaters_ = {"heater_bed", "extruder", "extruder1"};
        sensors_ = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                    "extruder",   // Hotend thermistor primary (Klipper naming: bare heater name)
                    "extruder1",  // Hotend thermistor secondary (Klipper naming: bare heater name)
                    "temperature_sensor chamber", "temperature_sensor mcu_temp"};
        fans_ = {"heater_fan hotend_fan", "heater_fan hotend_fan1", "fan",
                 "fan_generic exhaust_fan"};
        leds_ = {"neopixel chamber_light"};
        break;
    }

    // Initialize LED states (all off by default)
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        led_states_.clear();
        for (const auto& led : leds_) {
            led_states_[led] = LedColor{0.0, 0.0, 0.0, 0.0};
        }
    }

    spdlog::debug("[MoonrakerClientMock] Populated hardware:");
    for (const auto& h : heaters_)
        spdlog::debug("  Heater: {}", h);
    for (const auto& s : sensors_)
        spdlog::debug("  Sensor: {}", s);
    for (const auto& f : fans_)
        spdlog::debug("  Fan: {}", f);
    for (const auto& l : leds_)
        spdlog::debug("  LED: {}", l);
}

void MoonrakerClientMock::generate_mock_bed_mesh() {
    // Configure mesh profile
    active_bed_mesh_.name = "default";
    active_bed_mesh_.mesh_min[0] = 0.0f;
    active_bed_mesh_.mesh_min[1] = 0.0f;
    active_bed_mesh_.mesh_max[0] = 200.0f;
    active_bed_mesh_.mesh_max[1] = 200.0f;
    active_bed_mesh_.x_count = 7;
    active_bed_mesh_.y_count = 7;
    active_bed_mesh_.algo = "lagrange";

    // Generate dome-shaped mesh (matches Phase 3 test mesh for consistency)
    active_bed_mesh_.probed_matrix.clear();
    float center_x = active_bed_mesh_.x_count / 2.0f;
    float center_y = active_bed_mesh_.y_count / 2.0f;
    float max_radius = std::min(center_x, center_y);

    for (int row = 0; row < active_bed_mesh_.y_count; row++) {
        std::vector<float> row_vec;
        for (int col = 0; col < active_bed_mesh_.x_count; col++) {
            // Distance from center
            float dx = col - center_x;
            float dy = row - center_y;
            float dist = std::sqrt(dx * dx + dy * dy);

            // Dome shape: height decreases with distance from center
            // Z values from 0.0 to 0.3mm (realistic bed mesh range)
            float normalized_dist = dist / max_radius;
            float height = 0.3f * (1.0f - normalized_dist * normalized_dist);

            row_vec.push_back(height);
        }
        active_bed_mesh_.probed_matrix.push_back(row_vec);
    }

    // Add profile names
    bed_mesh_profiles_ = {"default", "adaptive"};

    spdlog::info("[MoonrakerClientMock] Generated synthetic bed mesh: profile='{}', size={}x{}, "
                 "profiles={}",
                 active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count,
                 bed_mesh_profiles_.size());
}

void MoonrakerClientMock::generate_mock_bed_mesh_with_variation() {
    // Generate a new mesh with the same structure but slightly different values
    // This simulates re-probing the bed and getting slightly different results

    // Keep existing configuration
    active_bed_mesh_.mesh_min[0] = 0.0f;
    active_bed_mesh_.mesh_min[1] = 0.0f;
    active_bed_mesh_.mesh_max[0] = 200.0f;
    active_bed_mesh_.mesh_max[1] = 200.0f;
    active_bed_mesh_.x_count = 7;
    active_bed_mesh_.y_count = 7;
    active_bed_mesh_.algo = "lagrange";

    // Generate dome-shaped mesh with slight random variation
    active_bed_mesh_.probed_matrix.clear();
    float center_x = active_bed_mesh_.x_count / 2.0f;
    float center_y = active_bed_mesh_.y_count / 2.0f;
    float max_radius = std::min(center_x, center_y);

    // Use a simple pseudo-random offset based on profile name
    // This ensures different profiles get different (but deterministic) meshes
    float offset = 0.0f;
    for (char c : active_bed_mesh_.name) {
        offset += static_cast<float>(c) * 0.001f;
    }
    offset = std::fmod(offset, 0.05f); // Keep variation small (0-0.05mm)

    for (int row = 0; row < active_bed_mesh_.y_count; row++) {
        std::vector<float> row_vec;
        for (int col = 0; col < active_bed_mesh_.x_count; col++) {
            // Distance from center
            float dx = col - center_x;
            float dy = row - center_y;
            float dist = std::sqrt(dx * dx + dy * dy);

            // Dome shape with variation: height decreases with distance from center
            float normalized_dist = dist / max_radius;
            float height = 0.3f * (1.0f - normalized_dist * normalized_dist);

            // Add small variation based on position and profile
            float variation = std::sin(col * 0.5f + offset) * 0.02f + std::cos(row * 0.5f) * 0.02f;
            height += variation + offset;

            row_vec.push_back(height);
        }
        active_bed_mesh_.probed_matrix.push_back(row_vec);
    }

    spdlog::debug("[MoonrakerClientMock] Regenerated bed mesh with variation for profile '{}'",
                  active_bed_mesh_.name);
}

void MoonrakerClientMock::dispatch_bed_mesh_update() {
    // Build bed mesh JSON in Moonraker format
    json probed_matrix_json = json::array();
    for (const auto& row : active_bed_mesh_.probed_matrix) {
        json row_json = json::array();
        for (float val : row) {
            row_json.push_back(val);
        }
        probed_matrix_json.push_back(row_json);
    }

    json profiles_json = json::object();
    for (const auto& profile : bed_mesh_profiles_) {
        profiles_json[profile] = json::object();
    }

    json bed_mesh_status = {
        {"bed_mesh",
         {{"profile_name", active_bed_mesh_.name},
          {"probed_matrix", probed_matrix_json},
          {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
          {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
          {"profiles", profiles_json},
          {"mesh_params", {{"algo", active_bed_mesh_.algo}}}}}};

    // Dispatch via base class method
    dispatch_status_update(bed_mesh_status);
}

void MoonrakerClientMock::disconnect() {
    spdlog::info("[MoonrakerClientMock] Simulating disconnection");
    stop_temperature_simulation();
    set_connection_state(ConnectionState::DISCONNECTED);
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {}", method);
    return 0; // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method,
                                      [[maybe_unused]] const json& params) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with params)", method);
    return 0; // Success
}

RequestId MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                            std::function<void(json)> cb) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with callback)", method);

    // Handle file listing API
    if (method == "server.files.list" && cb) {
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        json response = build_mock_file_list_response(path);
        spdlog::info("[MoonrakerClientMock] Returning mock file list for path: '{}'",
                     path.empty() ? "/" : path);
        cb(response);
        return next_mock_request_id();
    }

    // Handle file metadata API
    if (method == "server.files.metadata" && cb) {
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            json response = build_mock_file_metadata_response(filename);
            spdlog::info("[MoonrakerClientMock] Returning mock metadata for: {}", filename);
            cb(response);
            return next_mock_request_id();
        }
    }

    // Unimplemented methods - see docs/MOCK_CLIENT_IMPLEMENTATION_PLAN.md
    spdlog::warn("[MoonrakerClientMock] Method '{}' not implemented - callback not invoked",
                 method);
    return next_mock_request_id();
}

RequestId MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                            std::function<void(json)> success_cb,
                                            std::function<void(const MoonrakerError&)> error_cb,
                                            [[maybe_unused]] uint32_t timeout_ms) {
    spdlog::debug("[MoonrakerClientMock] Mock send_jsonrpc: {} (with success/error callbacks)",
                  method);

    // Handle file listing API
    if (method == "server.files.list" && success_cb) {
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        json response = build_mock_file_list_response(path);
        spdlog::info("[MoonrakerClientMock] Returning mock file list for path: '{}'",
                     path.empty() ? "/" : path);
        success_cb(response);
        return next_mock_request_id();
    }

    // Handle file metadata API
    if (method == "server.files.metadata" && success_cb) {
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            json response = build_mock_file_metadata_response(filename);
            spdlog::info("[MoonrakerClientMock] Returning mock metadata for: {}", filename);
            success_cb(response);
            return next_mock_request_id();
        } else if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Missing filename parameter";
            err.method = method;
            error_cb(err);
            return next_mock_request_id();
        }
    }

    // Handle G-code script execution (routes to gcode_script for state updates)
    if (method == "printer.gcode.script") {
        std::string script;
        if (params.contains("script")) {
            script = params["script"].get<std::string>();
        }
        gcode_script(script); // Process G-code (updates LED state, etc.)
        if (success_cb) {
            success_cb(json::object()); // Return empty success response
        }
        return next_mock_request_id();
    }

    // Handle print control API methods (delegate to unified internal handlers)
    if (method == "printer.print.start") {
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            if (start_print_internal(filename)) {
                if (success_cb) {
                    success_cb(json::object());
                }
            } else if (error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::VALIDATION_ERROR;
                err.message = "Failed to start print";
                err.method = method;
                error_cb(err);
            }
        } else if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Missing filename parameter";
            err.method = method;
            error_cb(err);
        }
        return next_mock_request_id();
    }

    if (method == "printer.print.pause") {
        if (pause_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Cannot pause - not currently printing";
            err.method = method;
            error_cb(err);
        }
        return next_mock_request_id();
    }

    if (method == "printer.print.resume") {
        if (resume_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Cannot resume - not currently paused";
            err.method = method;
            error_cb(err);
        }
        return next_mock_request_id();
    }

    if (method == "printer.print.cancel") {
        if (cancel_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Cannot cancel - no active print";
            err.method = method;
            error_cb(err);
        }
        return next_mock_request_id();
    }

    // ========================================================================
    // File Mutation Operations (JSON-RPC based)
    // ========================================================================

    // server.files.delete - Delete a file
    if (method == "server.files.delete") {
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        spdlog::info("[MoonrakerClientMock] Mock delete_file: {}", path);
        if (success_cb) {
            // Return success response matching Moonraker format
            json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // server.files.move - Move/rename a file
    if (method == "server.files.move") {
        std::string source, dest;
        if (params.contains("source")) {
            source = params["source"].get<std::string>();
        }
        if (params.contains("dest")) {
            dest = params["dest"].get<std::string>();
        }
        spdlog::info("[MoonrakerClientMock] Mock move_file: {} -> {}", source, dest);
        if (success_cb) {
            json response = {{"result", {{"item", {{"path", dest}, {"root", "gcodes"}}}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // server.files.copy - Copy a file
    if (method == "server.files.copy") {
        std::string source, dest;
        if (params.contains("source")) {
            source = params["source"].get<std::string>();
        }
        if (params.contains("dest")) {
            dest = params["dest"].get<std::string>();
        }
        spdlog::info("[MoonrakerClientMock] Mock copy_file: {} -> {}", source, dest);
        if (success_cb) {
            json response = {{"result", {{"item", {{"path", dest}, {"root", "gcodes"}}}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // server.files.post_directory - Create a directory
    if (method == "server.files.post_directory") {
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        spdlog::info("[MoonrakerClientMock] Mock create_directory: {}", path);
        if (success_cb) {
            json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // server.files.delete_directory - Delete a directory
    if (method == "server.files.delete_directory") {
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        spdlog::info("[MoonrakerClientMock] Mock delete_directory: {}", path);
        if (success_cb) {
            json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // ========================================================================
    // Query Operations
    // ========================================================================

    // printer.objects.query - Query printer state (used by is_printer_ready, get_print_state)
    if (method == "printer.objects.query") {
        json status_obj = json::object();

        // Check what objects are being queried
        if (params.contains("objects")) {
            auto& objects = params["objects"];

            // webhooks state (for is_printer_ready)
            if (objects.contains("webhooks")) {
                KlippyState klippy = klippy_state_.load();
                std::string state_str = "ready";
                switch (klippy) {
                case KlippyState::STARTUP:
                    state_str = "startup";
                    break;
                case KlippyState::SHUTDOWN:
                    state_str = "shutdown";
                    break;
                case KlippyState::ERROR:
                    state_str = "error";
                    break;
                default:
                    break;
                }
                status_obj["webhooks"] = {{"state", state_str}};
            }

            // print_stats (for get_print_state)
            if (objects.contains("print_stats")) {
                status_obj["print_stats"] = {{"state", get_print_state_string()}};
            }

            // configfile.settings (for update_safety_limits_from_printer)
            if (objects.contains("configfile")) {
                status_obj["configfile"] = {
                    {"settings",
                     {{"printer", {{"max_velocity", 500.0}, {"max_accel", 10000.0}}},
                      {"stepper_x", {{"position_min", 0.0}, {"position_max", 250.0}}},
                      {"stepper_y", {{"position_min", 0.0}, {"position_max", 250.0}}},
                      {"stepper_z", {{"position_min", 0.0}, {"position_max", 300.0}}},
                      {"extruder", {{"min_temp", 0.0}, {"max_temp", 300.0}}},
                      {"heater_bed", {{"min_temp", 0.0}, {"max_temp", 120.0}}}}}};
            }
        }

        if (success_cb) {
            json response = {{"result", {{"status", status_obj}}}};
            success_cb(response);
        }
        return next_mock_request_id();
    }

    // Unimplemented methods - log warning
    spdlog::warn("[MoonrakerClientMock] Method '{}' not implemented - callbacks not invoked",
                 method);
    return next_mock_request_id();
}

int MoonrakerClientMock::gcode_script(const std::string& gcode) {
    spdlog::debug("[MoonrakerClientMock] Mock gcode_script: {}", gcode);

    // Parse temperature commands to update simulation targets
    // M104 Sxxx - Set extruder temp (no wait)
    // M109 Sxxx - Set extruder temp (wait)
    // M140 Sxxx - Set bed temp (no wait)
    // M190 Sxxx - Set bed temp (wait)
    // SET_HEATER_TEMPERATURE HEATER=extruder TARGET=xxx
    // SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=xxx

    // Check for Klipper-style SET_HEATER_TEMPERATURE commands
    if (gcode.find("SET_HEATER_TEMPERATURE") != std::string::npos) {
        double target = 0.0;
        size_t target_pos = gcode.find("TARGET=");
        if (target_pos != std::string::npos) {
            target = std::stod(gcode.substr(target_pos + 7));
        }

        if (gcode.find("HEATER=extruder") != std::string::npos) {
            set_extruder_target(target);
            spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C", target);
        } else if (gcode.find("HEATER=heater_bed") != std::string::npos) {
            set_bed_target(target);
            spdlog::info("[MoonrakerClientMock] Bed target set to {}°C", target);
        }
    }
    // Check for M-code style temperature commands
    else if (gcode.find("M104") != std::string::npos || gcode.find("M109") != std::string::npos) {
        size_t s_pos = gcode.find('S');
        if (s_pos != std::string::npos) {
            double target = std::stod(gcode.substr(s_pos + 1));
            set_extruder_target(target);
            spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C (M-code)", target);
        }
    } else if (gcode.find("M140") != std::string::npos || gcode.find("M190") != std::string::npos) {
        size_t s_pos = gcode.find('S');
        if (s_pos != std::string::npos) {
            double target = std::stod(gcode.substr(s_pos + 1));
            set_bed_target(target);
            spdlog::info("[MoonrakerClientMock] Bed target set to {}°C (M-code)", target);
        }
    }

    // Parse motion mode commands (G90/G91)
    // G90 - Absolute positioning mode
    // G91 - Relative positioning mode
    if (gcode.find("G90") != std::string::npos) {
        relative_mode_.store(false);
        spdlog::info("[MoonrakerClientMock] Set absolute positioning mode (G90)");
    } else if (gcode.find("G91") != std::string::npos) {
        relative_mode_.store(true);
        spdlog::info("[MoonrakerClientMock] Set relative positioning mode (G91)");
    }

    // Parse homing command (G28)
    // G28 - Home all axes
    // G28 X - Home X axis only
    // G28 Y - Home Y axis only
    // G28 Z - Home Z axis only
    // G28 X Y - Home X and Y axes
    if (gcode.find("G28") != std::string::npos) {
        // Check if specific axes are mentioned after G28
        // Need to look after the G28 to avoid false matches
        size_t g28_pos = gcode.find("G28");
        std::string after_g28 = gcode.substr(g28_pos + 3);

        // Check for specific axis letters (case insensitive search)
        bool has_x =
            after_g28.find('X') != std::string::npos || after_g28.find('x') != std::string::npos;
        bool has_y =
            after_g28.find('Y') != std::string::npos || after_g28.find('y') != std::string::npos;
        bool has_z =
            after_g28.find('Z') != std::string::npos || after_g28.find('z') != std::string::npos;

        // If no specific axis mentioned, home all
        bool home_all = !has_x && !has_y && !has_z;

        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);

            if (home_all) {
                // Home all axes
                homed_axes_ = "xyz";
                pos_x_.store(0.0);
                pos_y_.store(0.0);
                pos_z_.store(0.0);
                spdlog::info("[MoonrakerClientMock] Homed all axes (G28), homed_axes='xyz'");
            } else {
                // Home specific axes and update position
                if (has_x) {
                    if (homed_axes_.find('x') == std::string::npos) {
                        homed_axes_ += 'x';
                    }
                    pos_x_.store(0.0);
                }
                if (has_y) {
                    if (homed_axes_.find('y') == std::string::npos) {
                        homed_axes_ += 'y';
                    }
                    pos_y_.store(0.0);
                }
                if (has_z) {
                    if (homed_axes_.find('z') == std::string::npos) {
                        homed_axes_ += 'z';
                    }
                    pos_z_.store(0.0);
                }
                spdlog::info("[MoonrakerClientMock] Homed axes: X={} Y={} Z={}, homed_axes='{}'",
                             has_x, has_y, has_z, homed_axes_);
            }
        }
    }

    // Parse movement commands (G0/G1)
    // G0 X100 Y50 Z10 - Rapid move
    // G1 X100 Y50 Z10 E5 F3000 - Linear move (E and F ignored for now)
    if (gcode.find("G0") != std::string::npos || gcode.find("G1") != std::string::npos) {
        bool is_relative = relative_mode_.load();

        // Helper lambda to parse axis value from gcode string
        auto parse_axis = [&gcode](char axis) -> std::pair<bool, double> {
            // Look for the axis letter followed by a number
            size_t pos = gcode.find(axis);
            if (pos == std::string::npos) {
                // Try lowercase
                pos = gcode.find(static_cast<char>(axis + 32));
            }
            if (pos != std::string::npos && pos + 1 < gcode.length()) {
                // Skip any spaces after the axis letter
                size_t value_start = pos + 1;
                while (value_start < gcode.length() && gcode[value_start] == ' ') {
                    value_start++;
                }
                if (value_start < gcode.length()) {
                    try {
                        double value = std::stod(gcode.substr(value_start));
                        return {true, value};
                    } catch (...) {
                        // Parse error, ignore this axis
                    }
                }
            }
            return {false, 0.0};
        };

        auto [has_x, x_val] = parse_axis('X');
        auto [has_y, y_val] = parse_axis('Y');
        auto [has_z, z_val] = parse_axis('Z');

        if (has_x) {
            if (is_relative) {
                pos_x_.store(pos_x_.load() + x_val);
            } else {
                pos_x_.store(x_val);
            }
        }
        if (has_y) {
            if (is_relative) {
                pos_y_.store(pos_y_.load() + y_val);
            } else {
                pos_y_.store(y_val);
            }
        }
        if (has_z) {
            if (is_relative) {
                pos_z_.store(pos_z_.load() + z_val);
            } else {
                pos_z_.store(z_val);
            }
        }

        if (has_x || has_y || has_z) {
            spdlog::debug("[MoonrakerClientMock] Move {} X={} Y={} Z={} (mode={})",
                          gcode.find("G0") != std::string::npos ? "G0" : "G1", pos_x_.load(),
                          pos_y_.load(), pos_z_.load(), is_relative ? "relative" : "absolute");
        }
    }

    // Parse print job commands (delegate to unified internal handlers)
    // SDCARD_PRINT_FILE FILENAME=xxx - Start printing a file
    if (gcode.find("SDCARD_PRINT_FILE") != std::string::npos) {
        size_t filename_pos = gcode.find("FILENAME=");
        if (filename_pos != std::string::npos) {
            // Extract filename (ends at space or end of string)
            size_t start = filename_pos + 9;
            size_t end = gcode.find(' ', start);
            std::string filename =
                (end != std::string::npos) ? gcode.substr(start, end - start) : gcode.substr(start);

            // Use unified internal handler
            start_print_internal(filename);
        }
    }
    // PAUSE - Pause current print
    else if (gcode == "PAUSE" || gcode.find("PAUSE ") == 0) {
        pause_print_internal();
    }
    // RESUME - Resume paused print
    else if (gcode == "RESUME" || gcode.find("RESUME ") == 0) {
        resume_print_internal();
    }
    // CANCEL_PRINT - Cancel current print
    else if (gcode == "CANCEL_PRINT" || gcode.find("CANCEL_PRINT ") == 0) {
        cancel_print_internal();
    }
    // M112 - Emergency stop
    else if (gcode.find("M112") != std::string::npos) {
        print_phase_.store(MockPrintPhase::ERROR);
        print_state_.store(5); // error
        extruder_target_.store(0.0);
        bed_target_.store(0.0);
        spdlog::warn("[MoonrakerClientMock] Emergency stop (M112)!");
        dispatch_print_state_notification("error");
    }

    // ========================================================================
    // UNIMPLEMENTED G-CODE STUBS - Log warnings for missing features
    // ========================================================================

    // Fan control - M106/M107/SET_FAN_SPEED
    // M106 P0 S128 - Set fan index 0 to 50% (S is 0-255, P is fan index)
    if (gcode.find("M106") != std::string::npos) {
        int fan_index = 0;
        int speed_value = 0;

        // Parse P parameter (fan index)
        auto p_pos = gcode.find('P');
        if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
            try {
                fan_index = std::stoi(gcode.substr(p_pos + 1));
            } catch (...) {
            }
        }

        // Parse S parameter (speed 0-255)
        auto s_pos = gcode.find('S');
        if (s_pos != std::string::npos && s_pos + 1 < gcode.length()) {
            try {
                speed_value = std::stoi(gcode.substr(s_pos + 1));
                speed_value = std::clamp(speed_value, 0, 255);
            } catch (...) {
            }
        }

        // Convert to normalized speed (0.0-1.0)
        double normalized_speed = speed_value / 255.0;

        // Fan index 0 = "fan", index 1+ = "fan1", "fan2", etc.
        std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
        set_fan_speed_internal(fan_name, normalized_speed);

        spdlog::info("[MoonrakerClientMock] M106 P{} S{} -> {} speed={:.2f}", fan_index,
                     speed_value, fan_name, normalized_speed);
    }
    // M107 - Turn off fan
    else if (gcode.find("M107") != std::string::npos) {
        int fan_index = 0;

        auto p_pos = gcode.find('P');
        if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
            try {
                fan_index = std::stoi(gcode.substr(p_pos + 1));
            } catch (...) {
            }
        }

        std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
        set_fan_speed_internal(fan_name, 0.0);

        spdlog::info("[MoonrakerClientMock] M107 P{} -> {} off", fan_index, fan_name);
    }
    // SET_FAN_SPEED - Klipper extended fan control
    // SET_FAN_SPEED FAN=nevermore SPEED=0.5
    else if (gcode.find("SET_FAN_SPEED") != std::string::npos) {
        std::string fan_name;
        double speed = 0.0;

        // Parse FAN parameter
        auto fan_pos = gcode.find("FAN=");
        if (fan_pos != std::string::npos) {
            size_t start = fan_pos + 4;
            size_t end = gcode.find_first_of(" \t\n", start);
            fan_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }

        // Parse SPEED parameter (0.0-1.0)
        auto speed_pos = gcode.find("SPEED=");
        if (speed_pos != std::string::npos) {
            try {
                speed = std::stod(gcode.substr(speed_pos + 6));
                speed = std::clamp(speed, 0.0, 1.0);
            } catch (...) {
            }
        }

        if (!fan_name.empty()) {
            // Try to find matching fan in discovered fans_ list
            std::string full_fan_name = find_fan_by_suffix(fan_name);
            if (!full_fan_name.empty()) {
                set_fan_speed_internal(full_fan_name, speed);
                spdlog::info("[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f}",
                             full_fan_name, speed);
            } else {
                // Use short name if no match found
                set_fan_speed_internal(fan_name, speed);
                spdlog::info(
                    "[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f} (unmatched fan)",
                    fan_name, speed);
            }
        }
    }

    // Extrusion control (NOT IMPLEMENTED)
    if (gcode.find("G92") != std::string::npos && gcode.find('E') != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: G92 E (set extruder position) NOT IMPLEMENTED");
    }
    if ((gcode.find("G0") != std::string::npos || gcode.find("G1") != std::string::npos) &&
        gcode.find('E') != std::string::npos) {
        spdlog::debug("[MoonrakerClientMock] Note: Extrusion (E parameter) ignored in G0/G1");
    }

    // Bed mesh commands
    if (gcode.find("BED_MESH_CALIBRATE") != std::string::npos) {
        // Parse optional PROFILE= parameter
        std::string profile_name = "default";
        auto profile_pos = gcode.find("PROFILE=");
        if (profile_pos != std::string::npos) {
            size_t start = profile_pos + 8; // Length of "PROFILE="
            size_t end = gcode.find_first_of(" \t\n", start);
            profile_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }

        // Regenerate mesh with slight random variation
        active_bed_mesh_.name = profile_name;
        generate_mock_bed_mesh_with_variation();

        // Add new profile to list if not already present
        if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
            bed_mesh_profiles_.end()) {
            bed_mesh_profiles_.push_back(profile_name);
        }

        spdlog::info(
            "[MoonrakerClientMock] BED_MESH_CALIBRATE: generated new mesh for profile '{}'",
            profile_name);

        // Dispatch bed mesh update notification
        dispatch_bed_mesh_update();

    } else if (gcode.find("BED_MESH_PROFILE") != std::string::npos) {
        // Parse LOAD= or SAVE= or REMOVE= parameter
        if (gcode.find("LOAD=") != std::string::npos) {
            auto load_pos = gcode.find("LOAD=");
            size_t start = load_pos + 5; // Length of "LOAD="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Check if profile exists
            if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) !=
                bed_mesh_profiles_.end()) {
                active_bed_mesh_.name = profile_name;
                // In real Moonraker, this would load actual saved mesh data
                // For mock, we just change the profile name
                generate_mock_bed_mesh_with_variation();
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: loaded profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: profile '{}' not found",
                             profile_name);
            }
        } else if (gcode.find("SAVE=") != std::string::npos) {
            auto save_pos = gcode.find("SAVE=");
            size_t start = save_pos + 5; // Length of "SAVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Add new profile to list if not already present
            if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
                bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.push_back(profile_name);
            }
            active_bed_mesh_.name = profile_name;
            spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE SAVE: saved profile '{}'",
                         profile_name);
            dispatch_bed_mesh_update();
        } else if (gcode.find("REMOVE=") != std::string::npos) {
            auto remove_pos = gcode.find("REMOVE=");
            size_t start = remove_pos + 7; // Length of "REMOVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Remove profile from list
            auto it = std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name);
            if (it != bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.erase(it);
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: removed profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn(
                    "[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: profile '{}' not found",
                    profile_name);
            }
        }
    } else if (gcode.find("BED_MESH_CLEAR") != std::string::npos) {
        // Clear the active bed mesh
        active_bed_mesh_.name = "";
        active_bed_mesh_.probed_matrix.clear();
        active_bed_mesh_.x_count = 0;
        active_bed_mesh_.y_count = 0;
        spdlog::info("[MoonrakerClientMock] BED_MESH_CLEAR: cleared active mesh");
        dispatch_bed_mesh_update();
    }

    // Z offset - SET_GCODE_OFFSET Z=0.2 or SET_GCODE_OFFSET Z_ADJUST=-0.05
    if (gcode.find("SET_GCODE_OFFSET") != std::string::npos) {
        // Parse Z parameter (absolute offset)
        auto z_pos = gcode.find(" Z=");
        if (z_pos != std::string::npos) {
            try {
                double z_offset = std::stod(gcode.substr(z_pos + 3));
                gcode_offset_z_.store(z_offset);
                spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z={:.3f}", z_offset);
                dispatch_gcode_move_update();
            } catch (...) {
            }
        }

        // Parse Z_ADJUST parameter (relative adjustment)
        auto z_adj_pos = gcode.find("Z_ADJUST=");
        if (z_adj_pos != std::string::npos) {
            try {
                double adjustment = std::stod(gcode.substr(z_adj_pos + 9));
                double new_offset = gcode_offset_z_.load() + adjustment;
                gcode_offset_z_.store(new_offset);
                spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z_ADJUST={:.3f} -> Z={:.3f}",
                             adjustment, new_offset);
                dispatch_gcode_move_update();
            } catch (...) {
            }
        }
    }

    // Input shaping (NOT IMPLEMENTED)
    if (gcode.find("SET_INPUT_SHAPER") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: SET_INPUT_SHAPER NOT IMPLEMENTED");
    }

    // Pressure advance (NOT IMPLEMENTED)
    if (gcode.find("SET_PRESSURE_ADVANCE") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: SET_PRESSURE_ADVANCE NOT IMPLEMENTED");
    }

    // LED control - SET_LED LED=<name> RED=<0-1> GREEN=<0-1> BLUE=<0-1> [WHITE=<0-1>]
    if (gcode.find("SET_LED") != std::string::npos) {
        // Parse LED name
        std::string led_name;
        auto led_pos = gcode.find("LED=");
        if (led_pos != std::string::npos) {
            size_t start = led_pos + 4;
            size_t end = gcode.find_first_of(" \t\n", start);
            led_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }

        // Parse color values (default to 0)
        auto parse_color = [&gcode](const std::string& param) -> double {
            auto pos = gcode.find(param + "=");
            if (pos != std::string::npos) {
                size_t start = pos + param.length() + 1;
                try {
                    return std::clamp(std::stod(gcode.substr(start)), 0.0, 1.0);
                } catch (...) {
                    return 0.0;
                }
            }
            return 0.0;
        };

        double red = parse_color("RED");
        double green = parse_color("GREEN");
        double blue = parse_color("BLUE");
        double white = parse_color("WHITE");

        // Find matching LED in our list (need to match by suffix since command uses short name)
        std::string full_led_name;
        for (const auto& led : leds_) {
            // Match if LED name ends with the command's led_name
            // e.g., "neopixel chamber_light" matches "chamber_light"
            if (led.length() >= led_name.length()) {
                size_t suffix_start = led.length() - led_name.length();
                if (led.substr(suffix_start) == led_name) {
                    full_led_name = led;
                    break;
                }
            }
        }

        if (!full_led_name.empty()) {
            // Update LED state
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                led_states_[full_led_name] = LedColor{red, green, blue, white};
            }

            spdlog::info("[MoonrakerClientMock] SET_LED: {} R={:.2f} G={:.2f} B={:.2f} W={:.2f}",
                         full_led_name, red, green, blue, white);

            // Dispatch LED state update notification (like real Moonraker would)
            json led_status;
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                for (const auto& [name, color] : led_states_) {
                    led_status[name] = {
                        {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
                }
            }
            dispatch_status_update(led_status);
        } else {
            spdlog::warn("[MoonrakerClientMock] SET_LED: unknown LED '{}'", led_name);
        }
    }

    // Firmware/Klipper restart - simulates klippy_state transition
    // FIRMWARE_RESTART: Full firmware reset (~3s delay)
    // RESTART: Klipper service restart (~2s delay)
    if (gcode.find("FIRMWARE_RESTART") != std::string::npos) {
        trigger_restart(/*is_firmware=*/true);
    } else if (gcode.find("RESTART") != std::string::npos &&
               gcode.find("FIRMWARE") == std::string::npos) {
        trigger_restart(/*is_firmware=*/false);
    }

    // EXCLUDE_OBJECT - Track excluded objects during print
    // EXCLUDE_OBJECT NAME=Part_1
    // EXCLUDE_OBJECT NAME="Part With Spaces"
    if (gcode.find("EXCLUDE_OBJECT") != std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_DEFINE") == std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_START") == std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_END") == std::string::npos) {
        // Parse NAME parameter
        size_t name_pos = gcode.find("NAME=");
        if (name_pos != std::string::npos) {
            size_t start = name_pos + 5;
            std::string object_name;

            // Handle quoted names (NAME="Part With Spaces")
            if (start < gcode.length() && gcode[start] == '"') {
                size_t end_quote = gcode.find('"', start + 1);
                if (end_quote != std::string::npos) {
                    object_name = gcode.substr(start + 1, end_quote - start - 1);
                }
            } else {
                // Unquoted name (ends at space or end of string)
                size_t end = gcode.find_first_of(" \t\n", start);
                object_name = (end != std::string::npos) ? gcode.substr(start, end - start)
                                                         : gcode.substr(start);
            }

            if (!object_name.empty()) {
                // Update shared state if available
                if (mock_state_) {
                    mock_state_->add_excluded_object(object_name);
                }
                // Also update local state for backward compatibility
                {
                    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                    excluded_objects_.insert(object_name);
                }
                spdlog::info("[MoonrakerClientMock] EXCLUDE_OBJECT: '{}' added to exclusion list",
                             object_name);
            }
        } else {
            spdlog::warn("[MoonrakerClientMock] EXCLUDE_OBJECT without NAME parameter ignored");
        }
    }

    // QGL / Z-tilt (NOT IMPLEMENTED)
    if (gcode.find("QUAD_GANTRY_LEVEL") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: QUAD_GANTRY_LEVEL NOT IMPLEMENTED");
    } else if (gcode.find("Z_TILT_ADJUST") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: Z_TILT_ADJUST NOT IMPLEMENTED");
    }

    // Probe (NOT IMPLEMENTED)
    if (gcode.find("PROBE") != std::string::npos && gcode.find("BED_MESH") == std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: PROBE NOT IMPLEMENTED");
    }

    return 0; // Success
}

std::string MoonrakerClientMock::get_print_state_string() const {
    switch (print_state_.load()) {
    case 0:
        return "standby";
    case 1:
        return "printing";
    case 2:
        return "paused";
    case 3:
        return "complete";
    case 4:
        return "cancelled";
    case 5:
        return "error";
    default:
        return "standby";
    }
}

// ============================================================================
// Unified Print Control (internal implementation)
// ============================================================================

bool MoonrakerClientMock::start_print_internal(const std::string& filename) {
    // Build path to test G-code file
    std::string full_path = std::string(TEST_GCODE_DIR) + "/" + filename;

    // Extract metadata from G-code file
    auto meta = gcode::extract_header_metadata(full_path);

    // Populate simulation metadata
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        print_metadata_.estimated_time_seconds =
            (meta.estimated_time_seconds > 0) ? meta.estimated_time_seconds : 300.0;
        print_metadata_.layer_count = (meta.layer_count > 0) ? meta.layer_count : 100;
        print_metadata_.target_bed_temp =
            (meta.first_layer_bed_temp > 0) ? meta.first_layer_bed_temp : 60.0;
        print_metadata_.target_nozzle_temp =
            (meta.first_layer_nozzle_temp > 0) ? meta.first_layer_nozzle_temp : 210.0;
        print_metadata_.filament_mm = meta.filament_used_mm;
    }

    // Set temperature targets for preheat
    double nozzle_target, bed_target;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        nozzle_target = print_metadata_.target_nozzle_temp;
        bed_target = print_metadata_.target_bed_temp;
    }
    extruder_target_.store(nozzle_target);
    bed_target_.store(bed_target);

    // Set print filename
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        print_filename_ = filename;
    }

    // Reset progress and timing
    print_progress_.store(0.0);
    total_pause_duration_sim_ = 0.0;
    preheat_start_time_ = std::chrono::steady_clock::now();
    printing_start_time_.reset();

    // Clear excluded objects from any previous print
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
    }

    // Transition to PREHEAT phase
    print_phase_.store(MockPrintPhase::PREHEAT);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Starting print '{}': est_time={:.0f}s, layers={}, "
                 "nozzle={:.0f}°C, bed={:.0f}°C",
                 filename, meta.estimated_time_seconds, meta.layer_count, nozzle_target,
                 bed_target);

    dispatch_print_state_notification("printing");
    return true;
}

bool MoonrakerClientMock::pause_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can only pause from PRINTING or PREHEAT
    if (current_phase != MockPrintPhase::PRINTING && current_phase != MockPrintPhase::PREHEAT) {
        spdlog::warn("[MoonrakerClientMock] Cannot pause - not currently printing (phase={})",
                     static_cast<int>(current_phase));
        return false;
    }

    // Record pause start time
    pause_start_time_ = std::chrono::steady_clock::now();

    // Transition to PAUSED
    print_phase_.store(MockPrintPhase::PAUSED);
    print_state_.store(2); // "paused" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print paused at {:.1f}% progress",
                 print_progress_.load() * 100.0);

    dispatch_print_state_notification("paused");
    return true;
}

bool MoonrakerClientMock::resume_print_internal() {
    if (print_phase_.load() != MockPrintPhase::PAUSED) {
        spdlog::warn("[MoonrakerClientMock] Cannot resume - not currently paused");
        return false;
    }

    // Calculate pause duration and add to total
    auto pause_real = std::chrono::steady_clock::now() - pause_start_time_;
    double pause_sim = std::chrono::duration<double>(pause_real).count() * speedup_factor_.load();
    total_pause_duration_sim_ += pause_sim;

    // Resume to PRINTING phase (skip PREHEAT since temps should still be maintained)
    print_phase_.store(MockPrintPhase::PRINTING);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print resumed (pause duration: {:.1f}s simulated)",
                 pause_sim);

    dispatch_print_state_notification("printing");
    return true;
}

bool MoonrakerClientMock::cancel_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can cancel from any non-idle phase
    if (current_phase == MockPrintPhase::IDLE) {
        spdlog::warn("[MoonrakerClientMock] Cannot cancel - no active print");
        return false;
    }

    // Set targets to 0 (begin cooldown)
    extruder_target_.store(0.0);
    bed_target_.store(0.0);

    // Transition to CANCELLED
    print_phase_.store(MockPrintPhase::CANCELLED);
    print_state_.store(4); // "cancelled" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print cancelled at {:.1f}% progress",
                 print_progress_.load() * 100.0);

    dispatch_print_state_notification("cancelled");
    return true;
}

// ============================================================================
// Simulation Helpers
// ============================================================================

bool MoonrakerClientMock::is_temp_stable(double current, double target, double tolerance) const {
    return std::abs(current - target) <= tolerance;
}

void MoonrakerClientMock::advance_print_progress(double dt_simulated) {
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_time = print_metadata_.estimated_time_seconds;
    }

    if (total_time <= 0) {
        return;
    }

    double rate = 1.0 / total_time; // Progress per simulated second
    double current = print_progress_.load();
    print_progress_.store(std::min(1.0, current + rate * dt_simulated));
}

void MoonrakerClientMock::dispatch_print_state_notification(const std::string& state) {
    json notification_status = {{"print_stats", {{"state", state}}}};
    dispatch_status_update(notification_status);
}

void MoonrakerClientMock::dispatch_enhanced_print_status() {
    double progress = print_progress_.load();
    int current_layer = get_current_layer();
    int total_layers;
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_layers = static_cast<int>(print_metadata_.layer_count);
        total_time = print_metadata_.estimated_time_seconds;
    }

    double elapsed = progress * total_time;

    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }

    MockPrintPhase phase = print_phase_.load();
    bool is_active = (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT);

    json status = {{"print_stats",
                    {{"state", get_print_state_string()},
                     {"filename", filename},
                     {"print_duration", elapsed},
                     {"total_duration", elapsed},
                     {"filament_used", 0.0},
                     {"message", ""},
                     {"info", {{"current_layer", current_layer}, {"total_layer", total_layers}}}}},
                   {"virtual_sdcard",
                    {{"file_path", filename}, {"progress", progress}, {"is_active", is_active}}}};

    // Note: dispatch_status_update called separately in the simulation loop
    // This function builds the enhanced status object
    dispatch_status_update(status);
}

// ============================================================================
// Temperature Simulation
// ============================================================================

void MoonrakerClientMock::dispatch_initial_state() {
    // Build initial state JSON matching real Moonraker subscription response format
    // Uses current simulated values (room temp by default, or preset values if set)
    double ext_temp = extruder_temp_.load();
    double ext_target = extruder_target_.load();
    double bed_temp_val = bed_temp_.load();
    double bed_target_val = bed_target_.load();
    double x = pos_x_.load();
    double y = pos_y_.load();
    double z = pos_z_.load();
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();
    int fan = fan_speed_.load();

    // Get homed_axes with thread safety
    std::string homed;
    {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        homed = homed_axes_;
    }

    // Get print state with thread safety
    std::string print_state_str = get_print_state_string();
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    double progress = print_progress_.load();

    // Convert probed_matrix to JSON 2D array
    json probed_matrix_json = json::array();
    for (const auto& row : active_bed_mesh_.probed_matrix) {
        json row_json = json::array();
        for (float val : row) {
            row_json.push_back(val);
        }
        probed_matrix_json.push_back(row_json);
    }

    // Build profiles object (Moonraker format: {"profile_name": {...}, ...})
    json profiles_json = json::object();
    for (const auto& profile : bed_mesh_profiles_) {
        profiles_json[profile] = json::object(); // Empty profile data (real has full mesh)
    }

    // Build LED state JSON
    json led_json = json::object();
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        for (const auto& [name, color] : led_states_) {
            led_json[name] = {{"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
        }
    }

    // Get Z offset and klippy state
    double z_offset = gcode_offset_z_.load();
    KlippyState klippy = klippy_state_.load();
    std::string klippy_str = "ready";
    switch (klippy) {
    case KlippyState::STARTUP:
        klippy_str = "startup";
        break;
    case KlippyState::SHUTDOWN:
        klippy_str = "shutdown";
        break;
    case KlippyState::ERROR:
        klippy_str = "error";
        break;
    default:
        break;
    }

    json initial_status = {
        {"extruder", {{"temperature", ext_temp}, {"target", ext_target}}},
        {"heater_bed", {{"temperature", bed_temp_val}, {"target", bed_target_val}}},
        {"toolhead", {{"position", {x, y, z, 0.0}}, {"homed_axes", homed}}},
        {"gcode_move",
         {{"speed_factor", speed / 100.0},
          {"extrude_factor", flow / 100.0},
          {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
        {"fan", {{"speed", fan / 255.0}}},
        {"webhooks", {{"state", klippy_str}, {"state_message", "Printer is ready"}}},
        {"print_stats", {{"state", print_state_str}, {"filename", filename}}},
        {"virtual_sdcard", {{"progress", progress}}},
        {"bed_mesh",
         {{"profile_name", active_bed_mesh_.name},
          {"probed_matrix", probed_matrix_json},
          {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
          {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
          {"profiles", profiles_json},
          {"mesh_params", {{"algo", active_bed_mesh_.algo}}}}}};

    // Merge LED states into initial_status (each LED is a top-level key)
    for (auto& [key, value] : led_json.items()) {
        initial_status[key] = value;
    }

    // Override fan speeds with explicitly-set values from fan_speeds_ map
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        for (const auto& [name, spd] : fan_speeds_) {
            if (name == "fan") {
                initial_status["fan"] = {{"speed", spd}};
            } else {
                initial_status[name] = {{"speed", spd}};
            }
        }
    }

    spdlog::info("[MoonrakerClientMock] Dispatching initial state: extruder={}/{}°C, bed={}/{}°C, "
                 "homed_axes='{}', leds={}",
                 ext_temp, ext_target, bed_temp_val, bed_target_val, homed, led_json.size());

    // Use the base class dispatch method (same as real client)
    dispatch_status_update(initial_status);
}

void MoonrakerClientMock::set_extruder_target(double target) {
    extruder_target_.store(target);
}

void MoonrakerClientMock::set_bed_target(double target) {
    bed_target_.store(target);
}

void MoonrakerClientMock::start_temperature_simulation() {
    if (simulation_running_.load()) {
        return; // Already running
    }

    simulation_running_.store(true);
    simulation_thread_ = std::thread(&MoonrakerClientMock::temperature_simulation_loop, this);
    spdlog::info("[MoonrakerClientMock] Temperature simulation started");
}

void MoonrakerClientMock::stop_temperature_simulation() {
    if (!simulation_running_.load()) {
        return; // Not running
    }

    simulation_running_.store(false);
    if (simulation_thread_.joinable()) {
        simulation_thread_.join();
    }
    spdlog::info("[MoonrakerClientMock] Temperature simulation stopped");
}

void MoonrakerClientMock::temperature_simulation_loop() {
    const double base_dt = SIMULATION_INTERVAL_MS / 1000.0; // Base time step (0.5s)

    while (simulation_running_.load()) {
        uint32_t tick = tick_count_.fetch_add(1);

        // Get speedup factor and calculate effective time step
        double speedup = speedup_factor_.load();
        double effective_dt = base_dt * speedup; // Simulated time step

        // Get current temperature state
        double ext_temp = extruder_temp_.load();
        double ext_target = extruder_target_.load();
        double bed_temp_val = bed_temp_.load();
        double bed_target_val = bed_target_.load();

        // Simulate extruder temperature change (scaled by speedup)
        if (ext_target > 0) {
            if (ext_temp < ext_target) {
                ext_temp += EXTRUDER_HEAT_RATE * effective_dt;
                if (ext_temp > ext_target)
                    ext_temp = ext_target;
            } else if (ext_temp > ext_target) {
                ext_temp -= EXTRUDER_COOL_RATE * effective_dt;
                if (ext_temp < ext_target)
                    ext_temp = ext_target;
            }
        } else {
            if (ext_temp > ROOM_TEMP) {
                ext_temp -= EXTRUDER_COOL_RATE * effective_dt;
                if (ext_temp < ROOM_TEMP)
                    ext_temp = ROOM_TEMP;
            }
        }
        extruder_temp_.store(ext_temp);

        // Simulate bed temperature change (scaled by speedup)
        if (bed_target_val > 0) {
            if (bed_temp_val < bed_target_val) {
                bed_temp_val += BED_HEAT_RATE * effective_dt;
                if (bed_temp_val > bed_target_val)
                    bed_temp_val = bed_target_val;
            } else if (bed_temp_val > bed_target_val) {
                bed_temp_val -= BED_COOL_RATE * effective_dt;
                if (bed_temp_val < bed_target_val)
                    bed_temp_val = bed_target_val;
            }
        } else {
            if (bed_temp_val > ROOM_TEMP) {
                bed_temp_val -= BED_COOL_RATE * effective_dt;
                if (bed_temp_val < ROOM_TEMP)
                    bed_temp_val = ROOM_TEMP;
            }
        }
        bed_temp_.store(bed_temp_val);

        // ========== Phase-Based Print Simulation ==========
        MockPrintPhase phase = print_phase_.load();

        switch (phase) {
        case MockPrintPhase::IDLE:
            // Nothing special - temps cool to room temp (handled above)
            break;

        case MockPrintPhase::PREHEAT:
            // Check if both extruder and bed have reached target temps
            if (is_temp_stable(ext_temp, ext_target) &&
                is_temp_stable(bed_temp_val, bed_target_val)) {
                // Transition to PRINTING phase
                print_phase_.store(MockPrintPhase::PRINTING);
                printing_start_time_ = std::chrono::steady_clock::now();
                spdlog::info("[MoonrakerClientMock] Preheat complete - starting print");
            }
            break;

        case MockPrintPhase::PRINTING:
            // Advance print progress based on file-estimated duration
            advance_print_progress(effective_dt);

            // Check for completion
            if (print_progress_.load() >= 1.0) {
                print_phase_.store(MockPrintPhase::COMPLETE);
                print_state_.store(3); // "complete" for backward compatibility
                extruder_target_.store(0.0);
                bed_target_.store(0.0);
                spdlog::info("[MoonrakerClientMock] Print complete!");
                dispatch_print_state_notification("complete");
            }
            break;

        case MockPrintPhase::PAUSED:
            // Temps maintained (targets unchanged), no progress advance
            break;

        case MockPrintPhase::COMPLETE:
        case MockPrintPhase::CANCELLED:
            // Cooling down - transition to IDLE when cool enough
            if (ext_temp < 50.0 && bed_temp_val < 35.0) {
                print_phase_.store(MockPrintPhase::IDLE);
                print_state_.store(0); // "standby" for backward compatibility
                {
                    std::lock_guard<std::mutex> lock(print_mutex_);
                    print_filename_.clear();
                }
                print_progress_.store(0.0);
                {
                    std::lock_guard<std::mutex> lock(metadata_mutex_);
                    print_metadata_.reset();
                }
                spdlog::info("[MoonrakerClientMock] Cooldown complete - returning to idle");
                dispatch_print_state_notification("standby");
            }
            break;

        case MockPrintPhase::ERROR:
            // Stay in error state until explicitly cleared (via new print start)
            break;
        }

        // ========== Position and Motion State ==========
        double x = pos_x_.load();
        double y = pos_y_.load();
        double z = pos_z_.load();

        std::string homed;
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            homed = homed_axes_;
        }

        // Simulate speed/flow oscillation (90-110%) - only during printing
        int speed = 100;
        int flow = 100;
        if (phase == MockPrintPhase::PRINTING) {
            speed = 100 + static_cast<int>(10.0 * std::sin(tick / 20.0));
            flow = 100 + static_cast<int>(5.0 * std::cos(tick / 30.0));
        }
        speed_factor_.store(speed);
        flow_factor_.store(flow);

        // Simulate fan ramping up during print (0-255 over 30 simulated seconds)
        int fan = 0;
        if (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT) {
            fan = std::min(255, static_cast<int>(print_progress_.load() * 255.0));
        }
        fan_speed_.store(fan);

        // ========== Build and Dispatch Status Notification ==========
        std::string print_state_str = get_print_state_string();
        std::string filename;
        {
            std::lock_guard<std::mutex> lock(print_mutex_);
            filename = print_filename_;
        }

        // Get layer info for enhanced status
        int current_layer = get_current_layer();
        int total_layers = get_total_layers();
        double total_time;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            total_time = print_metadata_.estimated_time_seconds;
        }
        double progress = print_progress_.load();
        double elapsed = progress * total_time;

        // Get Z offset for gcode_move
        double z_offset = gcode_offset_z_.load();

        // Build notification JSON (enhanced Moonraker format with layer info)
        json status_obj = {
            {"extruder", {{"temperature", ext_temp}, {"target", ext_target}}},
            {"heater_bed", {{"temperature", bed_temp_val}, {"target", bed_target_val}}},
            {"toolhead", {{"position", {x, y, z, 0.0}}, {"homed_axes", homed}}},
            {"gcode_move",
             {{"speed_factor", speed / 100.0},
              {"extrude_factor", flow / 100.0},
              {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
            {"fan", {{"speed", fan / 255.0}}},
            {"print_stats",
             {{"state", print_state_str},
              {"filename", filename},
              {"print_duration", elapsed},
              {"total_duration", elapsed},
              {"filament_used", 0.0},
              {"message", ""},
              {"info", {{"current_layer", current_layer}, {"total_layer", total_layers}}}}},
            {"virtual_sdcard",
             {{"file_path", filename},
              {"progress", progress},
              {"is_active",
               phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT}}}};

        // Add klippy state if not ready (only send when abnormal)
        KlippyState klippy = klippy_state_.load();
        if (klippy != KlippyState::READY) {
            std::string state_str;
            switch (klippy) {
            case KlippyState::STARTUP:
                state_str = "startup";
                break;
            case KlippyState::SHUTDOWN:
                state_str = "shutdown";
                break;
            case KlippyState::ERROR:
                state_str = "error";
                break;
            default:
                state_str = "ready";
                break;
            }
            status_obj["webhooks"] = {{"state", state_str}};
        }

        // Override fan speeds with explicitly-set values from fan_speeds_ map
        {
            std::lock_guard<std::mutex> lock(fan_mutex_);
            for (const auto& [name, spd] : fan_speeds_) {
                if (name == "fan") {
                    status_obj["fan"] = {{"speed", spd}};
                } else {
                    status_obj[name] = {{"speed", spd}};
                }
            }
        }

        json notification = {{"method", "notify_status_update"},
                             {"params", json::array({status_obj, tick * base_dt})}};

        // Push notification through all registered callbacks
        // Two-phase: copy under lock, invoke outside to avoid deadlock
        std::vector<std::function<void(json)>> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            callbacks_copy.reserve(notify_callbacks_.size());
            for (const auto& [id, cb] : notify_callbacks_) {
                callbacks_copy.push_back(cb);
            }
        }
        for (const auto& cb : callbacks_copy) {
            if (cb) {
                cb(notification);
            }
        }

        // Sleep wall-clock interval (unchanged by speedup factor)
        std::this_thread::sleep_for(std::chrono::milliseconds(SIMULATION_INTERVAL_MS));
    }
}

// ============================================================================
// Fan Control Helper Methods
// ============================================================================

void MoonrakerClientMock::set_fan_speed_internal(const std::string& fan_name, double speed) {
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        fan_speeds_[fan_name] = speed;
    }

    // Also update the legacy fan_speed_ atomic for backward compatibility
    // (only for part cooling fan "fan")
    if (fan_name == "fan") {
        fan_speed_.store(static_cast<int>(speed * 255.0));
    }

    // Dispatch fan status update
    json fan_status;
    if (fan_name == "fan") {
        // Part cooling fan uses simple format
        fan_status["fan"] = {{"speed", speed}};
    } else {
        // Generic/heater fans use full name as key
        fan_status[fan_name] = {{"speed", speed}};
    }
    dispatch_status_update(fan_status);
}

std::string MoonrakerClientMock::find_fan_by_suffix(const std::string& suffix) const {
    for (const auto& fan : fans_) {
        // Match if fan name ends with the suffix (e.g., "nevermore" matches "fan_generic
        // nevermore")
        if (fan.length() >= suffix.length()) {
            size_t suffix_start = fan.length() - suffix.length();
            if (fan.substr(suffix_start) == suffix) {
                return fan;
            }
        }
    }
    return "";
}

// ============================================================================
// G-code Offset Helper Methods
// ============================================================================

void MoonrakerClientMock::dispatch_gcode_move_update() {
    double z_offset = gcode_offset_z_.load();
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();

    json gcode_move = {{"gcode_move",
                        {{"speed_factor", speed / 100.0},
                         {"extrude_factor", flow / 100.0},
                         {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}}};
    dispatch_status_update(gcode_move);
}

// ============================================================================
// Restart Simulation Helper Methods
// ============================================================================

void MoonrakerClientMock::trigger_restart(bool is_firmware) {
    // Set klippy_state to "startup"
    klippy_state_.store(KlippyState::STARTUP);

    // Clear any active print state
    if (print_phase_.load() != MockPrintPhase::IDLE) {
        print_phase_.store(MockPrintPhase::IDLE);
        print_state_.store(0); // standby
        {
            std::lock_guard<std::mutex> lock(print_mutex_);
            print_filename_.clear();
        }
        print_progress_.store(0.0);
    }

    // Set temperature targets to 0 (heaters off) - temps will naturally cool
    extruder_target_.store(0.0);
    bed_target_.store(0.0);

    // Clear excluded objects list (restart clears Klipper state)
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
    }

    // Dispatch klippy state change notification
    json status = {{"webhooks",
                    {{"state", "startup"},
                     {"state_message", is_firmware ? "Firmware restart in progress"
                                                   : "Klipper restart in progress"}}}};
    dispatch_status_update(status);

    spdlog::info("[MoonrakerClientMock] {} triggered - klippy_state='startup'",
                 is_firmware ? "FIRMWARE_RESTART" : "RESTART");

    // Schedule return to ready state using tracked thread
    // IMPORTANT: Must track and join - detached threads cause use-after-free during destruction
    double delay_sec = is_firmware ? 3.0 : 2.0;

    // Apply speedup factor to delay
    double effective_delay = delay_sec / speedup_factor_.load();

    // Cancel and wait for any existing restart thread
    restart_pending_.store(false);
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }

    // Launch new restart thread
    restart_pending_.store(true);
    restart_thread_ = std::thread([this, effective_delay, is_firmware]() {
        // Sleep in small increments to allow early exit on destruction
        int total_ms = static_cast<int>(effective_delay * 1000);
        int elapsed_ms = 0;
        constexpr int SLEEP_INTERVAL_MS = 100;

        while (elapsed_ms < total_ms && restart_pending_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_INTERVAL_MS));
            elapsed_ms += SLEEP_INTERVAL_MS;
        }

        // Check if we were cancelled
        if (!restart_pending_.load()) {
            return;
        }

        // Return to ready state
        klippy_state_.store(KlippyState::READY);

        // Dispatch ready notification
        json ready_status = {
            {"webhooks", {{"state", "ready"}, {"state_message", "Printer is ready"}}}};
        dispatch_status_update(ready_status);

        spdlog::info("[MoonrakerClientMock] {} complete - klippy_state='ready'",
                     is_firmware ? "FIRMWARE_RESTART" : "RESTART");

        restart_pending_.store(false);
    });
}
