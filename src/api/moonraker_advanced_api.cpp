// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_advanced_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "accel_sensor_manager.h"
#include "bed_mesh_probe_parser.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "screws_tilt_parser.h"
#include "shaper_csv_parser.h"
#include "spdlog/spdlog.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
#include <regex>
#include <set>
#include <sstream>

using namespace helix;

// ============================================================================
// MoonrakerAdvancedAPI Implementation
// ============================================================================

MoonrakerAdvancedAPI::MoonrakerAdvancedAPI(MoonrakerClient& client, MoonrakerAPI& api)
    : client_(client), api_(api) {}

// ============================================================================
// Domain Service Operations - Bed Mesh
// ============================================================================

void MoonrakerAdvancedAPI::update_bed_mesh(const json& bed_mesh) {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    spdlog::debug("[MoonrakerAPI] update_bed_mesh called with keys: {}", [&]() {
        std::string keys;
        for (auto it = bed_mesh.begin(); it != bed_mesh.end(); ++it) {
            if (!keys.empty())
                keys += ", ";
            keys += it.key();
        }
        return keys;
    }());

    // Parse active profile name
    if (bed_mesh.contains("profile_name") && !bed_mesh["profile_name"].is_null()) {
        active_bed_mesh_.name = bed_mesh["profile_name"].template get<std::string>();
    }

    // Parse probed_matrix (2D array of Z heights)
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        for (const auto& row : bed_mesh["probed_matrix"]) {
            if (row.is_array()) {
                std::vector<float> row_vec;
                for (const auto& val : row) {
                    if (val.is_number()) {
                        row_vec.push_back(val.template get<float>());
                    }
                }
                if (!row_vec.empty()) {
                    active_bed_mesh_.probed_matrix.push_back(row_vec);
                }
            }
        }

        // Update dimensions
        active_bed_mesh_.y_count = static_cast<int>(active_bed_mesh_.probed_matrix.size());
        active_bed_mesh_.x_count = active_bed_mesh_.probed_matrix.empty()
                                       ? 0
                                       : static_cast<int>(active_bed_mesh_.probed_matrix[0].size());
    }

    // Parse mesh bounds (check that elements are numbers, not null)
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array() &&
        bed_mesh["mesh_min"].size() >= 2 && bed_mesh["mesh_min"][0].is_number() &&
        bed_mesh["mesh_min"][1].is_number()) {
        active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].template get<float>();
        active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].template get<float>();
    }

    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array() &&
        bed_mesh["mesh_max"].size() >= 2 && bed_mesh["mesh_max"][0].is_number() &&
        bed_mesh["mesh_max"][1].is_number()) {
        active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].template get<float>();
        active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].template get<float>();
    }

    // Parse available profiles and their mesh data
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        stored_bed_mesh_profiles_.clear();

        spdlog::debug("[MoonrakerAPI] Parsing {} bed mesh profiles", bed_mesh["profiles"].size());

        for (auto& [profile_name, profile_data] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(profile_name);

            // Parse and store mesh data for this profile (if available)
            if (profile_data.is_object()) {
                BedMeshProfile profile;
                profile.name = profile_name;

                // Parse points array (Moonraker calls it "points", not "probed_matrix")
                if (profile_data.contains("points") && profile_data["points"].is_array()) {
                    for (const auto& row : profile_data["points"]) {
                        if (row.is_array()) {
                            std::vector<float> row_vec;
                            for (const auto& val : row) {
                                if (val.is_number()) {
                                    row_vec.push_back(val.template get<float>());
                                }
                            }
                            if (!row_vec.empty()) {
                                profile.probed_matrix.push_back(row_vec);
                            }
                        }
                    }
                }

                // Parse mesh bounds
                if (profile_data.contains("mesh_params") &&
                    profile_data["mesh_params"].is_object()) {
                    const auto& params = profile_data["mesh_params"];
                    if (params.contains("min_x"))
                        profile.mesh_min[0] = params["min_x"].template get<float>();
                    if (params.contains("min_y"))
                        profile.mesh_min[1] = params["min_y"].template get<float>();
                    if (params.contains("max_x"))
                        profile.mesh_max[0] = params["max_x"].template get<float>();
                    if (params.contains("max_y"))
                        profile.mesh_max[1] = params["max_y"].template get<float>();
                    if (params.contains("x_count"))
                        profile.x_count = params["x_count"].template get<int>();
                    if (params.contains("y_count"))
                        profile.y_count = params["y_count"].template get<int>();
                }

                if (!profile.probed_matrix.empty()) {
                    stored_bed_mesh_profiles_[profile_name] = std::move(profile);
                }
            }
        }
    }

    // Parse algorithm from mesh_params (if available)
    if (bed_mesh.contains("mesh_params") && bed_mesh["mesh_params"].is_object()) {
        const json& params = bed_mesh["mesh_params"];
        if (params.contains("algo") && params["algo"].is_string()) {
            active_bed_mesh_.algo = params["algo"].template get<std::string>();
        }
    }

    if (active_bed_mesh_.probed_matrix.empty()) {
        spdlog::debug("[MoonrakerAPI] Bed mesh data cleared (no probed_matrix)");
    } else {
        spdlog::debug("[MoonrakerAPI] Bed mesh updated: profile='{}', size={}x{}, "
                      "profiles={}, algo='{}'",
                      active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count,
                      bed_mesh_profiles_.size(), active_bed_mesh_.algo);
    }
}

const BedMeshProfile* MoonrakerAdvancedAPI::get_active_bed_mesh() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    if (active_bed_mesh_.probed_matrix.empty()) {
        return nullptr;
    }
    return &active_bed_mesh_;
}

std::vector<std::string> MoonrakerAdvancedAPI::get_bed_mesh_profiles() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);
    return bed_mesh_profiles_;
}

bool MoonrakerAdvancedAPI::has_bed_mesh() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);
    return !active_bed_mesh_.probed_matrix.empty();
}

const BedMeshProfile*
MoonrakerAdvancedAPI::get_bed_mesh_profile(const std::string& profile_name) const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    // Check stored profiles first
    auto it = stored_bed_mesh_profiles_.find(profile_name);
    if (it != stored_bed_mesh_profiles_.end()) {
        return &it->second;
    }

    // Fall back to active mesh if name matches
    if (active_bed_mesh_.name == profile_name && !active_bed_mesh_.probed_matrix.empty()) {
        return &active_bed_mesh_;
    }

    return nullptr;
}

void MoonrakerAdvancedAPI::get_excluded_objects(
    std::function<void(const std::set<std::string>&)> on_success, ErrorCallback on_error) {
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

void MoonrakerAdvancedAPI::get_available_objects(
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
                            if (obj.is_object() && obj.contains("name") &&
                                obj["name"].is_string()) {
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

// ============================================================================
// ADVANCED PANEL STUB IMPLEMENTATIONS
// ============================================================================
// These methods are placeholders for future implementation.
// NOTE: start_bed_mesh_calibrate is implemented after BedMeshProgressCollector class below.

/**
 * @brief Collector for PID_CALIBRATE gcode responses
 *
 * Klipper sends PID calibration results as console output via notify_gcode_response.
 * This class monitors for the result line containing pid_Kp, pid_Ki, pid_Kd values.
 *
 * Expected output format:
 *   PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178
 *
 * Error handling:
 *   - "Unknown command" with "PID_CALIBRATE" - command not recognized
 *   - "Error"/"error"/"!! " - Klipper error messages
 *
 * Note: No timeout is implemented. Caller should implement UI-level timeout if needed.
 */
class PIDCalibrateCollector : public std::enable_shared_from_this<PIDCalibrateCollector> {
  public:
    using PIDCallback = std::function<void(float kp, float ki, float kd)>;
    using PIDProgressCallback = std::function<void(int sample, float tolerance)>;

    PIDCalibrateCollector(MoonrakerClient& client, PIDCallback on_success,
                          MoonrakerAdvancedAPI::ErrorCallback on_error,
                          PIDProgressCallback on_progress = nullptr)
        : client_(client), on_success_(std::move(on_success)), on_error_(std::move(on_error)),
          on_progress_(std::move(on_progress)) {}

    ~PIDCalibrateCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "pid_calibrate_collector_" + std::to_string(++s_collector_id);
        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
        registered_.store(true);
        spdlog::debug("[PIDCalibrateCollector] Started (handler: {})", handler_name_);
    }

    void unregister() {
        bool was = registered_.exchange(false);
        if (was) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[PIDCalibrateCollector] Unregistered");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load())
            return;
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty())
            return;

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[PIDCalibrateCollector] Received: {}", line);

        // Check for progress: "sample:1 pwm:0.5 asymmetry:0.2 tolerance:n/a"
        static const std::regex sample_regex(
            R"(sample:(\d+)\s+pwm:[\d.]+\s+asymmetry:[\d.]+\s+tolerance:(\S+))");
        std::smatch progress_match;
        if (std::regex_search(line, progress_match, sample_regex)) {
            int sample_num = std::stoi(progress_match[1].str());
            float tolerance_val = -1.0f;
            std::string tol_str = progress_match[2].str();
            if (tol_str != "n/a") {
                try {
                    tolerance_val = std::stof(tol_str);
                } catch (...) {
                }
            }
            spdlog::debug("[PIDCalibrateCollector] Progress: sample={} tolerance={}", sample_num,
                          tolerance_val);
            if (on_progress_)
                on_progress_(sample_num, tolerance_val);
            return;
        }

        // Check for PID result: "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178"
        static const std::regex pid_regex(R"(pid_Kp=([\d.]+)\s+pid_Ki=([\d.]+)\s+pid_Kd=([\d.]+))");
        std::smatch match;
        if (std::regex_search(line, match, pid_regex) && match.size() == 4) {
            float kp = std::stof(match[1].str());
            float ki = std::stof(match[2].str());
            float kd = std::stof(match[3].str());
            complete_success(kp, ki, kd);
            return;
        }

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("PID_CALIBRATE") != std::string::npos) {
            complete_error("PID_CALIBRATE command not recognized. Check Klipper configuration.");
            return;
        }

        // Broader error detection
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos ||
            line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

  private:
    void complete_success(float kp, float ki, float kd) {
        if (completed_.exchange(true))
            return;
        spdlog::info("[PIDCalibrateCollector] PID result: Kp={:.3f} Ki={:.3f} Kd={:.3f}", kp, ki,
                     kd);
        unregister();
        if (on_success_)
            on_success_(kp, ki, kd);
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true))
            return;
        spdlog::error("[PIDCalibrateCollector] Error: {}", message);
        unregister();
        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("PID_CALIBRATE", message);
            on_error_(err);
        }
    }

    MoonrakerClient& client_;
    PIDCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    PIDProgressCallback on_progress_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};
};

/**
 * @brief State machine for collecting MPC_CALIBRATE gcode responses
 *
 * Kalico/Danger Klipper sends MPC calibration output as multiple gcode_response lines.
 * The collector tracks calibration phases for progress reporting and accumulates
 * result values from multiple lines after "Finished MPC calibration".
 *
 * Result lines arrive separately:
 *   block_heat_capacity=18.5432 [J/K]
 *   sensor_responsiveness=0.123456 [K/s/K]
 *   ambient_transfer=0.078901 [W/K]
 *   fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]
 *
 * The collector fires the success callback once the minimum required parameters
 * (block_heat_capacity, sensor_responsiveness, ambient_transfer) have been parsed,
 * with a short accumulation window to capture fan_ambient_transfer if present.
 */
class MPCCalibrateCollector : public std::enable_shared_from_this<MPCCalibrateCollector> {
  public:
    using MPCCallback = MoonrakerAdvancedAPI::MPCCalibrateCallback;
    using MPCProgressCB = MoonrakerAdvancedAPI::MPCProgressCallback;
    using MPCResult = MoonrakerAdvancedAPI::MPCResult;

    MPCCalibrateCollector(MoonrakerClient& client, MPCCallback on_success,
                          MoonrakerAdvancedAPI::ErrorCallback on_error,
                          MPCProgressCB on_progress = nullptr, bool expect_fan_data = false)
        : client_(client), on_success_(std::move(on_success)), on_error_(std::move(on_error)),
          on_progress_(std::move(on_progress)) {
        expect_fan_data_ = expect_fan_data;
    }

    ~MPCCalibrateCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "mpc_calibrate_collector_" + std::to_string(++s_collector_id);
        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
        registered_.store(true);
        spdlog::debug("[MPCCalibrateCollector] Started (handler: {})", handler_name_);
    }

    void unregister() {
        bool was = registered_.exchange(false);
        if (was) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[MPCCalibrateCollector] Unregistered");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load())
            return;
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty())
            return;

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[MPCCalibrateCollector] Received: {}", line);

        // If we're accumulating result lines after "Finished MPC calibration"
        if (accumulating_results_) {
            parse_result_line(line);
            return;
        }

        // Check for "Finished MPC calibration" — begin result accumulation
        if (line.find("Finished MPC calibration") != std::string::npos) {
            spdlog::debug("[MPCCalibrateCollector] Calibration finished, accumulating results");
            accumulating_results_ = true;
            return;
        }

        // Progress: phase 1 — ambient settling
        if (line.find("Waiting for heater to settle") != std::string::npos) {
            report_progress(1, line);
            return;
        }

        // Progress: phase 2 — heatup test
        if (line.find("Performing heatup test") != std::string::npos) {
            report_progress(2, line);
            return;
        }

        // Progress: phase 3 — fan breakpoint measurements
        if (line.find("measuring power usage with") != std::string::npos) {
            report_progress(3, line);
            return;
        }

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("MPC_CALIBRATE") != std::string::npos) {
            complete_error(
                "MPC_CALIBRATE command not recognized. Requires Kalico or Danger Klipper.");
            return;
        }

        // Broader error detection
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos ||
            line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

  private:
    void report_progress(int phase, const std::string& description) {
        // Total phases: 1=settle, 2=heatup, 3=fan measurements
        static constexpr int TOTAL_PHASES = 3;
        spdlog::debug("[MPCCalibrateCollector] Progress: phase={} desc={}", phase, description);
        if (on_progress_)
            on_progress_(phase, TOTAL_PHASES, description);
    }

    void parse_result_line(const std::string& line) {
        // Parse: fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]
        // Must check BEFORE ambient_transfer since both contain "ambient_transfer"
        static const std::regex fat_regex(R"(fan_ambient_transfer=([\d., ]+)\s*\[W/K\])");
        // Parse: block_heat_capacity=18.5432 [J/K]
        static const std::regex bhc_regex(R"(block_heat_capacity=([\d.]+))");
        // Parse: sensor_responsiveness=0.123456 [K/s/K]
        static const std::regex sr_regex(R"(sensor_responsiveness=([\d.]+))");
        // Parse: ambient_transfer=0.078901 [W/K]
        static const std::regex at_regex(R"(ambient_transfer=([\d.]+)\s+\[W/K\])");

        std::smatch match;

        if (std::regex_search(line, match, fat_regex)) {
            result_.fan_ambient_transfer = match[1].str();
            // Trim trailing whitespace
            auto end = result_.fan_ambient_transfer.find_last_not_of(' ');
            if (end != std::string::npos)
                result_.fan_ambient_transfer.resize(end + 1);
            spdlog::debug("[MPCCalibrateCollector] fan_ambient_transfer={}",
                          result_.fan_ambient_transfer);
            parsed_fan_ambient_ = true;
            // fan_ambient_transfer is always the last result line — complete now
            if (has_required_params())
                complete_success();
            return;
        }

        if (std::regex_search(line, match, bhc_regex)) {
            result_.block_heat_capacity = std::stof(match[1].str());
            spdlog::debug("[MPCCalibrateCollector] block_heat_capacity={}",
                          result_.block_heat_capacity);
            parsed_bhc_ = true;
            return;
        }

        if (std::regex_search(line, match, sr_regex)) {
            result_.sensor_responsiveness = std::stof(match[1].str());
            spdlog::debug("[MPCCalibrateCollector] sensor_responsiveness={}",
                          result_.sensor_responsiveness);
            parsed_sr_ = true;
            return;
        }

        if (std::regex_search(line, match, at_regex)) {
            result_.ambient_transfer = std::stof(match[1].str());
            spdlog::debug("[MPCCalibrateCollector] ambient_transfer={}", result_.ambient_transfer);
            parsed_at_ = true;
            // ambient_transfer is the last required param. Complete now unless we're
            // expecting fan_ambient_transfer data to follow.
            if (has_required_params() && !expect_fan_data_)
                complete_success();
            return;
        }

        // Unrecognized line during accumulation — if we have all required params,
        // complete (fan_ambient_transfer was not sent). Otherwise check for errors.
        if (has_required_params()) {
            complete_success();
            return;
        }

        if (line.find("Error") != std::string::npos || line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

    bool has_required_params() const {
        return parsed_bhc_ && parsed_sr_ && parsed_at_;
    }

    void complete_success() {
        if (completed_.exchange(true))
            return;
        spdlog::info("[MPCCalibrateCollector] MPC result: bhc={:.4f} sr={:.6f} at={:.6f} fat={}",
                     result_.block_heat_capacity, result_.sensor_responsiveness,
                     result_.ambient_transfer, result_.fan_ambient_transfer);
        unregister();
        if (on_success_)
            on_success_(result_);
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true))
            return;
        spdlog::error("[MPCCalibrateCollector] Error: {}", message);
        unregister();
        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("MPC_CALIBRATE", message);
            on_error_(err);
        }
    }

    MoonrakerClient& client_;
    MPCCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    MPCProgressCB on_progress_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};

    // Result accumulation state
    bool expect_fan_data_ = false;
    bool accumulating_results_ = false;
    MPCResult result_;
    bool parsed_bhc_ = false;
    bool parsed_sr_ = false;
    bool parsed_at_ = false;
    bool parsed_fan_ambient_ = false;
};

/**
 * @brief State machine for collecting SCREWS_TILT_CALCULATE responses
 *
 * Klipper sends screw tilt results as console output lines via notify_gcode_response.
 * This class collects and parses those lines until the sequence completes.
 *
 * Expected output format:
 *   // front_left (base) : x=-5.0, y=30.0, z=2.48750
 *   // front_right : x=155.0, y=30.0, z=2.36000 : adjust CW 01:15
 *   // rear_right : x=155.0, y=180.0, z=2.42500 : adjust CCW 00:30
 *   // rear_left : x=155.0, y=180.0, z=2.42500 : adjust CW 00:18
 *
 * Error handling:
 *   - "Unknown command" - screws_tilt_adjust not configured
 *   - "!! " prefix - Klipper emergency/critical errors
 *
 * Completion is signaled by the execute_gcode success callback (JSON-RPC response),
 * NOT by an "ok" line in notify_gcode_response. Klipper may send intermediate "ok"
 * lines (e.g., from sub-commands during probing) before the actual screw results
 * arrive via "//" prefixed lines. The execute_gcode call to printer.gcode.script
 * only returns after the entire command finishes, so its success callback is the
 * reliable completion signal.
 */
class ScrewsTiltCollector : public std::enable_shared_from_this<ScrewsTiltCollector> {
  public:
    ScrewsTiltCollector(MoonrakerClient& client, ScrewTiltCallback on_success,
                        MoonrakerAdvancedAPI::ErrorCallback on_error)
        : client_(client), on_success_(std::move(on_success)), on_error_(std::move(on_error)) {}

    ~ScrewsTiltCollector() {
        // Ensure we always unregister callback
        unregister();
    }

    void start() {
        // Register for gcode_response notifications
        // Use atomic counter for unique handler names (safer than pointer address reuse)
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "screws_tilt_collector_" + std::to_string(++s_collector_id);

        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });

        registered_.store(true);
        spdlog::debug("[ScrewsTiltCollector] Started collecting responses (handler: {})",
                      handler_name_);
    }

    void unregister() {
        bool was_registered = registered_.exchange(false);
        if (was_registered) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[ScrewsTiltCollector] Unregistered callback");
        }
    }

    /**
     * @brief Mark as completed without invoking callbacks
     *
     * Used when the execute_gcode error path handles the error callback directly.
     */
    void mark_completed() {
        completed_.store(true);
    }

    /**
     * @brief Signal that execute_gcode completed successfully (JSON-RPC returned)
     *
     * This is the reliable completion signal. By the time the JSON-RPC call for
     * printer.gcode.script returns, Klipper has finished executing the command
     * and all notify_gcode_response lines (including screw results) have been sent.
     */
    void on_command_finished() {
        if (completed_.load()) {
            return;
        }

        if (!results_.empty()) {
            spdlog::info("[ScrewsTiltCollector] Command finished, {} results collected",
                         results_.size());
            complete_success();
        } else {
            spdlog::warn("[ScrewsTiltCollector] Command finished but no screw data received");
            complete_error("SCREWS_TILT_CALCULATE completed but no screw data received");
        }
    }

    void on_gcode_response(const json& msg) {
        // Check if already completed (prevent double-invocation)
        if (completed_.load()) {
            return;
        }

        // notify_gcode_response format: {"method": "notify_gcode_response", "params": ["line"]}
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[ScrewsTiltCollector] Received: {}", line);

        // Check for unknown command error (screws_tilt_adjust not configured)
        if (line.find("Unknown command") != std::string::npos &&
            line.find("SCREWS_TILT_CALCULATE") != std::string::npos) {
            complete_error("SCREWS_TILT_CALCULATE requires [screws_tilt_adjust] in printer.cfg");
            return;
        }

        // Parse screw result lines that start with "//"
        if (line.rfind("//", 0) == 0) {
            parse_screw_line(line);
        }

        // Klipper emergency/critical errors start with "!! "
        if (line.rfind("!! ", 0) == 0) {
            complete_error(line);
        }
    }

  private:
    void parse_screw_line(const std::string& line) {
        // Shared with the mock printer and the unit tests — see
        // screws_tilt_parser.h. Keep the parsing itself out of this collector so
        // there is exactly one implementation of Klipper's sign convention.
        ScrewTiltResult result;
        if (parse_screws_tilt_line(line, result)) {
            results_.push_back(std::move(result));
        }
    }

    void complete_success() {
        if (completed_) {
            return;
        }
        completed_ = true;

        spdlog::info("[ScrewsTiltCollector] Complete with {} screws", results_.size());
        unregister();

        if (on_success_) {
            on_success_(results_);
        }
    }

    void complete_error(const std::string& message) {
        if (completed_) {
            return;
        }
        completed_ = true;

        spdlog::error("[ScrewsTiltCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("SCREWS_TILT_CALCULATE", message);
            on_error_(err);
        }
    }

    MoonrakerClient& client_;
    ScrewTiltCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::string handler_name_;
    std::atomic<bool> registered_{false}; // Thread-safe: accessed from callback and destructor
    std::atomic<bool> completed_{false};  // Thread-safe: prevents double-callback invocation
    std::vector<ScrewTiltResult> results_;
};

/**
 * @brief State machine for collecting SHAPER_CALIBRATE responses
 *
 * Klipper sends input shaper results as console output lines via notify_gcode_response.
 * This class collects and parses those lines until the sequence completes.
 *
 * Expected output format:
 *   Testing frequency 5.00 Hz
 *   ...
 *   Testing frequency 100.00 Hz
 *   Wait for calculations..
 *   Fitted shaper 'zv' frequency = 35.8 Hz (vibrations = 22.7%, smoothing ~= 0.100)
 *   suggested max_accel <= 4000 mm/sec^2
 *   Fitted shaper 'mzv' frequency = 36.7 Hz (vibrations = 7.2%, smoothing ~= 0.140)
 *   suggested max_accel <= 5400 mm/sec^2
 *   ...
 *   Recommended shaper_type_x = mzv, shaper_freq_x = 36.7 Hz
 *   calibration data written to /tmp/calibration_data_x_*.csv
 */
class InputShaperCollector : public std::enable_shared_from_this<InputShaperCollector> {
  public:
    InputShaperCollector(MoonrakerClient& client, char axis, AdvancedProgressCallback on_progress,
                         InputShaperCallback on_success,
                         MoonrakerAdvancedAPI::ErrorCallback on_error)
        : client_(client), axis_(axis), on_progress_(std::move(on_progress)),
          on_success_(std::move(on_success)), on_error_(std::move(on_error)),
          last_activity_(std::chrono::steady_clock::now()) {}

    ~InputShaperCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "input_shaper_collector_" + std::to_string(++s_collector_id);

        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });

        registered_.store(true);
        spdlog::debug(
            "[InputShaperCollector] Started collecting responses for axis {} (handler: {})", axis_,
            handler_name_);
    }

    void unregister() {
        bool was_registered = registered_.exchange(false);
        if (was_registered) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[InputShaperCollector] Unregistered callback");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load()) {
            return;
        }

        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[InputShaperCollector] Received: {}", line);

        // Reset activity watchdog
        last_activity_ = std::chrono::steady_clock::now();

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("SHAPER_CALIBRATE") != std::string::npos) {
            complete_error(
                "SHAPER_CALIBRATE requires [resonance_tester] and ADXL345 in printer.cfg");
            return;
        }

        // Parse frequency sweep lines: "Testing frequency 62.00 Hz"
        if (line.find("Testing frequency") != std::string::npos) {
            parse_sweep_line(line);
            return;
        }

        // Parse "Wait for calculations.." — transition to CALCULATING
        if (line.find("Wait for calculations") != std::string::npos) {
            if (collector_state_ != CollectorState::CALCULATING) {
                collector_state_ = CollectorState::CALCULATING;
                emit_progress(55, "Calculating results...");
            }
            return;
        }

        // Parse shaper/smoother fit lines (Kalico uses "Fitted smoother" for smooth shapers)
        if (line.find("Fitted shaper") != std::string::npos ||
            line.find("Fitted smoother") != std::string::npos) {
            parse_shaper_line(line);
            return;
        }

        // Parse max_accel lines: "suggested max_accel <= 4000 mm/sec^2"
        if (line.find("suggested max_accel") != std::string::npos) {
            parse_max_accel_line(line);
            return;
        }

        // Parse recommendation line (try new format first, then old)
        // Don't complete yet — CSV path line follows immediately after
        if (line.find("Recommended shaper") != std::string::npos ||
            line.find("Recommended smoother") != std::string::npos) {
            parse_recommendation(line);
            collector_state_ = CollectorState::COMPLETE;
            return;
        }

        // Parse CSV path: "calibration data written to /tmp/calibration_data_x_*.csv"
        if (line.find("calibration data written to") != std::string::npos) {
            parse_csv_path(line);
            complete_success();
            return;
        }

        // If we have the recommendation, keep waiting for the CSV path line.
        // Don't complete early on unrelated G-code responses (e.g., temperature
        // reports) — that would discard the CSV data needed for frequency charts.
        if (collector_state_ == CollectorState::COMPLETE) {
            return;
        }

        // Error detection
        if (line.rfind("!! ", 0) == 0 || line.rfind("Error: ", 0) == 0 ||
            line.find("error:") != std::string::npos) {
            complete_error(line);
        }
    }

  private:
    enum class CollectorState { WAITING_FOR_OUTPUT, SWEEPING, CALCULATING, COMPLETE };

    void parse_sweep_line(const std::string& line) {
        static const std::regex freq_regex(R"(Testing frequency ([\d.]+) Hz)");
        std::smatch match;
        if (std::regex_search(line, match, freq_regex) && match.size() == 2) {
            try {
                float freq = std::stof(match[1].str());
                last_sweep_freq_ = freq;

                if (collector_state_ != CollectorState::SWEEPING) {
                    collector_state_ = CollectorState::SWEEPING;
                }

                // Progress: 3-55% range mapped from min_freq to max_freq
                float range = max_freq_ - min_freq_;
                float progress_frac = (range > 0) ? (freq - min_freq_) / range : 0.0f;
                int percent = 3 + static_cast<int>(progress_frac * 52.0f);
                percent = std::clamp(percent, 3, 55);

                char status[64];
                snprintf(status, sizeof(status), "Testing frequency %.0f Hz", freq);
                emit_progress(percent, status);
            } catch (const std::exception&) {
                // Ignore parse errors
            }
        }
    }

    void parse_shaper_line(const std::string& line) {
        // Kalico bleeding-edge format (both smoothers and discrete shapers):
        // Fitted smoother 'smooth_mzv' frequency = 42.6 Hz (vibration score = 1.23%, smoothing ~=
        // 0.085, combined score = 1.234e-02) Fitted shaper 'mzv' frequency = 36.7 Hz (vibration
        // score = 1.23%, smoothing ~= 0.140, combined score = 2.345e-02)
        static const std::regex kalico_regex(
            R"(Fitted (?:shaper|smoother) '([\w]+)' frequency = ([\d.]+) Hz \(vibration score = ([\d.]+)%, smoothing ~= ([\d.]+))");

        // Standard Klipper format:
        // Fitted shaper 'mzv' frequency = 36.7 Hz (vibrations = 7.2%, smoothing ~= 0.140)
        static const std::regex klipper_regex(
            R"(Fitted shaper '(\w+)' frequency = ([\d.]+) Hz \(vibrations = ([\d.]+)%, smoothing ~= ([\d.]+)\))");

        std::smatch match;
        bool matched = std::regex_search(line, match, kalico_regex) ||
                       std::regex_search(line, match, klipper_regex);

        if (matched && match.size() >= 5) {
            ShaperFitData fit;
            fit.type = match[1].str();
            try {
                fit.frequency = std::stof(match[2].str());
                fit.vibrations = std::stof(match[3].str());
                fit.smoothing = std::stof(match[4].str());
            } catch (const std::exception& e) {
                spdlog::warn("[InputShaperCollector] Failed to parse values: {}", e.what());
                return;
            }

            spdlog::debug("[InputShaperCollector] Parsed: {} @ {:.1f} Hz (vib: {:.1f}%)", fit.type,
                          fit.frequency, fit.vibrations);
            shaper_fits_.push_back(fit);

            // Emit progress in CALCULATING phase: 55-95% range, 8% per shaper fitted
            // Standard Klipper has 5 shapers (reaches 95%), Kalico may have 10+ (caps at 95%)
            int calc_progress = 55 + static_cast<int>(shaper_fits_.size()) * 8;
            calc_progress = std::min(calc_progress, 95);
            char status[64];
            snprintf(status, sizeof(status), "Fitted %s at %.1f Hz", fit.type.c_str(),
                     fit.frequency);
            emit_progress(calc_progress, status);
        }
    }

    void parse_max_accel_line(const std::string& line) {
        static const std::regex accel_regex(R"(suggested max_accel <= (\d+))");
        std::smatch match;
        if (std::regex_search(line, match, accel_regex) && match.size() == 2) {
            try {
                float max_accel = std::stof(match[1].str());
                // Attach to the most recently parsed shaper fit
                if (!shaper_fits_.empty()) {
                    shaper_fits_.back().max_accel = max_accel;
                    spdlog::debug("[InputShaperCollector] {} max_accel: {:.0f}",
                                  shaper_fits_.back().type, max_accel);
                }
            } catch (const std::exception&) {
                // Ignore parse errors
            }
        }
    }

    void parse_recommendation(const std::string& line) {
        // Try new Klipper format first: "Recommended shaper_type_x = mzv, shaper_freq_x = 53.8 Hz"
        static const std::regex rec_new(
            R"(Recommended shaper_type_\w+ = (\w+), shaper_freq_\w+ = ([\d.]+) Hz)");
        // Kalico smoother format: "Recommended smoother_type_x = smooth_mzv, smoother_freq_x = 42.6
        // Hz"
        static const std::regex rec_smoother(
            R"(Recommended smoother_type_\w+ = (\w+), smoother_freq_\w+ = ([\d.]+) Hz)");
        // Legacy format: "Recommended shaper is mzv @ 36.7 Hz"
        static const std::regex rec_old(R"(Recommended shaper is (\w+) @ ([\d.]+) Hz)");

        std::smatch match;
        bool matched = std::regex_search(line, match, rec_new) ||
                       std::regex_search(line, match, rec_smoother) ||
                       std::regex_search(line, match, rec_old);

        if (matched && match.size() == 3) {
            recommended_type_ = match[1].str();
            try {
                recommended_freq_ = std::stof(match[2].str());
            } catch (const std::exception&) {
                recommended_freq_ = 0.0f;
            }
            spdlog::info("[InputShaperCollector] Recommendation: {} @ {:.1f} Hz", recommended_type_,
                         recommended_freq_);
        }
    }

    void parse_csv_path(const std::string& line) {
        static const std::regex csv_regex(R"(calibration data written to (\S+\.csv))");
        std::smatch match;
        if (std::regex_search(line, match, csv_regex) && match.size() == 2) {
            csv_path_ = match[1].str();
            spdlog::info("[InputShaperCollector] CSV path: {}", csv_path_);
        }
    }

    void emit_progress(int percent, const std::string& status) {
        if (on_progress_) {
            on_progress_(percent);
        }
        spdlog::trace("[InputShaperCollector] Progress: {}% - {}", percent, status);
    }

    void complete_success() {
        if (completed_.exchange(true)) {
            return;
        }

        spdlog::info("[InputShaperCollector] Complete with {} shaper options", shaper_fits_.size());
        unregister();

        // Emit 100% progress
        emit_progress(100, "Complete");

        if (on_success_) {
            InputShaperResult result;
            result.axis = axis_;
            result.shaper_type = recommended_type_;
            result.shaper_freq = recommended_freq_;
            result.csv_path = csv_path_;

            // Find recommended shaper's details and populate all_shapers
            for (const auto& fit : shaper_fits_) {
                if (fit.type == recommended_type_) {
                    result.smoothing = fit.smoothing;
                    result.vibrations = fit.vibrations;
                    result.max_accel = fit.max_accel;
                }

                ShaperOption option;
                option.type = fit.type;
                option.frequency = fit.frequency;
                option.vibrations = fit.vibrations;
                option.smoothing = fit.smoothing;
                option.max_accel = fit.max_accel;
                result.all_shapers.push_back(option);
            }

            // Parse frequency response data from calibration CSV
            if (!result.csv_path.empty()) {
                auto csv_data = helix::calibration::parse_shaper_csv(result.csv_path, axis_);
                if (!csv_data.frequencies.empty()) {
                    result.freq_response.reserve(csv_data.frequencies.size());
                    for (size_t i = 0; i < csv_data.frequencies.size(); ++i) {
                        result.freq_response.emplace_back(
                            csv_data.frequencies[i],
                            i < csv_data.raw_psd.size() ? csv_data.raw_psd[i] : 0.0f);
                    }
                    result.shaper_curves = std::move(csv_data.shaper_curves);
                    spdlog::debug(
                        "[InputShaperCollector] parsed {} freq bins, {} shaper curves from CSV",
                        result.freq_response.size(), result.shaper_curves.size());
                } else {
                    // Klipper reported a CSV path but we couldn't read any data
                    // from it (missing/unreadable file or malformed CSV). Flag it
                    // so the UI can tell the user the chart is unavailable instead
                    // of silently showing a blank graph.
                    result.chart_data_unavailable = true;
                    spdlog::warn("[InputShaperCollector] CSV reported at {} but no frequency data "
                                 "could be read — chart unavailable",
                                 result.csv_path);
                }
            }

            on_success_(result);
        }
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true)) {
            return;
        }

        spdlog::error("[InputShaperCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("SHAPER_CALIBRATE", message);
            on_error_(err);
        }
    }

    // Internal struct for collecting fits before building final result
    struct ShaperFitData {
        std::string type;
        float frequency = 0.0f;
        float vibrations = 0.0f;
        float smoothing = 0.0f;
        float max_accel = 0.0f;
    };

    MoonrakerClient& client_;
    char axis_;
    AdvancedProgressCallback on_progress_;
    InputShaperCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};

    CollectorState collector_state_ = CollectorState::WAITING_FOR_OUTPUT;
    float min_freq_ = 5.0f;
    float max_freq_ = 100.0f;
    float last_sweep_freq_ = 0.0f;
    std::string csv_path_;
    std::chrono::steady_clock::time_point last_activity_;

    std::vector<ShaperFitData> shaper_fits_;
    std::string recommended_type_;
    float recommended_freq_ = 0.0f;
};

/**
 * @brief State machine for collecting MEASURE_AXES_NOISE responses
 *
 * Klipper sends noise measurement results as console output lines via notify_gcode_response.
 * This class collects and parses those lines to extract the noise level.
 *
 * Expected output format:
 *   "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396 (z)"
 *
 * Error handling:
 *   - "Unknown command" - MEASURE_AXES_NOISE not available (no accelerometer)
 *   - "Error"/"error"/"!! " - Klipper error messages
 */
class NoiseCheckCollector : public std::enable_shared_from_this<NoiseCheckCollector> {
  public:
    NoiseCheckCollector(MoonrakerClient& client,
                        MoonrakerAdvancedAPI::NoiseCheckCallback on_success,
                        MoonrakerAdvancedAPI::ErrorCallback on_error)
        : client_(client), on_success_(std::move(on_success)), on_error_(std::move(on_error)) {}

    ~NoiseCheckCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "noise_check_collector_" + std::to_string(++s_collector_id);

        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });

        registered_.store(true);
        spdlog::debug("[NoiseCheckCollector] Started collecting responses (handler: {})",
                      handler_name_);
    }

    void unregister() {
        bool was_registered = registered_.exchange(false);
        if (was_registered) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[NoiseCheckCollector] Unregistered callback");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load()) {
            return;
        }

        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[NoiseCheckCollector] Received: {}", line);

        // Check for unknown command error (no accelerometer configured)
        if (line.find("Unknown command") != std::string::npos &&
            line.find("MEASURE_AXES_NOISE") != std::string::npos) {
            complete_error("MEASURE_AXES_NOISE requires [adxl345] accelerometer in printer.cfg");
            return;
        }

        // Parse noise level line: "Axes noise for xy-axis accelerometer: 57.956 (x), ..."
        if (line.find("Axes noise") != std::string::npos) {
            parse_noise_line(line);
            return;
        }

        // Error detection
        if (line.rfind("!! ", 0) == 0 ||                // Emergency errors
            line.rfind("Error:", 0) == 0 ||             // Standard errors
            line.find("error:") != std::string::npos) { // Python traceback
            complete_error(line);
        }
    }

  private:
    void parse_noise_line(const std::string& line) {
        // Klipper output format:
        // "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396 (z)"
        static const std::regex noise_regex(
            R"(Axes noise.*:\s*([\d.]+)\s*\(x\),\s*([\d.]+)\s*\(y\),\s*([\d.]+)\s*\(z\))");

        std::smatch match;
        if (std::regex_search(line, match, noise_regex) && match.size() == 4) {
            try {
                float noise_x = std::stof(match[1].str());
                float noise_y = std::stof(match[2].str());
                float noise_z = std::stof(match[3].str());

                spdlog::info("[NoiseCheckCollector] Noise: x={:.2f}, y={:.2f}, z={:.2f}", noise_x,
                             noise_y, noise_z);

                // Zero reading on X or Y means accelerometer isn't working on that axis
                constexpr float MIN_NOISE = 0.001f;
                if (noise_x < MIN_NOISE || noise_y < MIN_NOISE) {
                    std::string dead_axes;
                    if (noise_x < MIN_NOISE)
                        dead_axes += "X";
                    if (noise_y < MIN_NOISE) {
                        if (!dead_axes.empty())
                            dead_axes += " and ";
                        dead_axes += "Y";
                    }
                    complete_error("Accelerometer reading zero on " + dead_axes +
                                   " axis — check wiring and axes_map configuration");
                    return;
                }

                // Report max of x,y as the overall noise level
                float noise = std::max(noise_x, noise_y);
                complete_success(noise);
            } catch (const std::exception& e) {
                spdlog::warn("[NoiseCheckCollector] Failed to parse noise value: {}", e.what());
                complete_error("Failed to parse noise measurement");
            }
        }
    }

    void complete_success(float noise_level) {
        if (completed_.exchange(true)) {
            return;
        }

        spdlog::info("[NoiseCheckCollector] Complete with noise level: {:.6f}", noise_level);
        unregister();

        if (on_success_) {
            on_success_(noise_level);
        }
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true)) {
            return;
        }

        spdlog::error("[NoiseCheckCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("MEASURE_AXES_NOISE", message);
            on_error_(err);
        }
    }

    MoonrakerClient& client_;
    MoonrakerAdvancedAPI::NoiseCheckCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};
};

/**
 * @brief State machine for collecting BED_MESH_CALIBRATE progress
 *
 * Klipper sends probing progress as console output lines via notify_gcode_response.
 * This class collects and parses those lines to provide real-time progress updates.
 *
 * Expected output formats:
 *   Probing point 5/25
 *   Probe point 5 of 25
 *
 * Completion markers:
 *   "Mesh Bed Leveling Complete"
 *   "Mesh bed leveling complete"
 *
 * Error handling:
 *   - "!! " prefix - Klipper emergency/critical errors
 *   - "Error:" prefix - Standard Klipper errors
 *   - "error:" in line - Python traceback errors
 */
class BedMeshProgressCollector : public std::enable_shared_from_this<BedMeshProgressCollector> {
  public:
    using ProgressCallback = std::function<void(int current, int total)>;

    BedMeshProgressCollector(MoonrakerClient& client, ProgressCallback on_progress,
                             MoonrakerAdvancedAPI::SuccessCallback on_complete,
                             MoonrakerAdvancedAPI::ErrorCallback on_error, int expected_probes = 0,
                             int probe_samples = 1)
        : client_(client), on_progress_(std::move(on_progress)),
          on_complete_(std::move(on_complete)), on_error_(std::move(on_error)),
          expected_probes_(expected_probes), point_counter_(probe_samples) {}

    ~BedMeshProgressCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "bed_mesh_collector_" + std::to_string(++s_collector_id);

        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });

        registered_.store(true);
        spdlog::debug("[BedMeshProgressCollector] Started collecting responses (handler: {})",
                      handler_name_);
    }

    void unregister() {
        bool was_registered = registered_.exchange(false);
        if (was_registered) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[BedMeshProgressCollector] Unregistered callback");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    /// Called when the JSON-RPC response returns successfully — the command
    /// finished on the Klipper side.  Acts as a fallback completion signal in
    /// case the gcode_response stream didn't contain a recognised completion
    /// marker (e.g. "Mesh Bed Leveling Complete").
    void on_command_finished() {
        complete_success();
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load()) {
            return;
        }

        // notify_gcode_response format: {"method": "notify_gcode_response", "params": ["line"]}
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[BedMeshProgressCollector] Received: {}", line);

        // Check for errors first
        if (line.rfind("!! ", 0) == 0 ||                // Emergency errors
            line.rfind("Error:", 0) == 0 ||             // Standard errors
            line.find("error:") != std::string::npos) { // Python traceback
            complete_error(line);
            return;
        }

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("BED_MESH_CALIBRATE") != std::string::npos) {
            complete_error("BED_MESH_CALIBRATE requires [bed_mesh] in printer.cfg");
            return;
        }

        // Try to parse probe progress
        parse_probe_line(line);

        // Check for completion markers
        if (line.find("Mesh Bed Leveling Complete") != std::string::npos ||
            line.find("Mesh bed leveling complete") != std::string::npos) {
            complete_success();
            return;
        }
    }

  private:
    void parse_probe_line(const std::string& line) {
        if (auto pp = helix::parse_probe_progress(line)) {
            current_probe_ = pp->current;
            total_probes_ = pp->total;
            spdlog::debug("[BedMeshProgressCollector] Progress: {}/{}", pp->current, pp->total);
            if (on_progress_) {
                on_progress_(pp->current, pp->total);
            }
            return;
        }

        // Fallback: count "probe at X,Y is z=Z" lines (standard Klipper probe
        // output).  Some firmware variants don't emit the "Probing point X/Y"
        // progress line but do emit per-probe result lines.
        //
        // These lines are per SAMPLE, not per mesh point, so they are deduped by
        // position rather than divided by the configured sample count — the
        // divisor only works when we recognise the printer's probe section name,
        // and the Qidi Q2's is not one we enumerate, so a 36-point mesh counted
        // to 72 (#1224). ProbePointCounter keeps the divisor as a fallback for
        // lines whose coordinates do not parse.
        if (auto mesh_point = point_counter_.feed(line)) {
            spdlog::debug("[BedMeshProgressCollector] Probe sample line #{} → point {}/{}",
                          point_counter_.sample_lines(), *mesh_point, expected_probes_);
            if (on_progress_) {
                on_progress_(*mesh_point, expected_probes_);
            }
        }
    }

    void complete_success() {
        if (completed_.exchange(true)) {
            return; // Already completed
        }

        spdlog::info("[BedMeshProgressCollector] Complete ({}/{} probes)", current_probe_,
                     total_probes_);
        unregister();

        if (on_complete_) {
            on_complete_();
        }
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true)) {
            return;
        }

        spdlog::error("[BedMeshProgressCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("BED_MESH_CALIBRATE", message);
            on_error_(err);
        }
    }

    MoonrakerClient& client_;
    ProgressCallback on_progress_;
    MoonrakerAdvancedAPI::SuccessCallback on_complete_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};

    int current_probe_ = 0;
    int total_probes_ = 0;
    int expected_probes_ = 0; // hint from configfile (0 = unknown)
    // Dedupes "probe at X,Y is z=Z" samples into mesh points. Seeded with the
    // configured sample count, which it uses only when a line's coordinates
    // cannot be parsed.
    helix::ProbePointCounter point_counter_;
};

void MoonrakerAdvancedAPI::start_bed_mesh_calibrate(BedMeshProgressCallback on_progress,
                                                    SuccessCallback on_complete,
                                                    ErrorCallback on_error, int expected_probes,
                                                    int probe_samples) {
    spdlog::info("[MoonrakerAPI] Starting bed mesh calibration with progress tracking "
                 "(expected_probes={}, probe_samples={})",
                 expected_probes, probe_samples);

    // Create collector to track progress
    auto collector = std::make_shared<BedMeshProgressCollector>(client_, std::move(on_progress),
                                                                std::move(on_complete), on_error,
                                                                expected_probes, probe_samples);

    collector->start();

    // Execute the calibration command
    // Note: No PROFILE= parameter - user will name the mesh after completion
    api_.execute_gcode(
        "BED_MESH_CALIBRATE",
        [collector]() {
            // JSON-RPC returned — command finished on Klipper side.
            // Some firmware variants don't emit "Mesh Bed Leveling Complete"
            // in gcode_response, so use the RPC return as a fallback signal.
            spdlog::debug("[MoonrakerAPI] BED_MESH_CALIBRATE command finished");
            collector->on_command_finished();
        },
        [collector, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[MoonrakerAPI] BED_MESH_CALIBRATE response timed out "
                             "(calibration may still be running)");
            } else {
                spdlog::error("[MoonrakerAPI] BED_MESH_CALIBRATE failed: {}", err.message);
            }
            collector->mark_completed();
            if (on_error) {
                on_error(err);
            }
        },
        CALIBRATION_TIMEOUT_MS);
}

void MoonrakerAdvancedAPI::calculate_screws_tilt(ScrewTiltCallback on_success,
                                                 ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Starting SCREWS_TILT_CALCULATE");

    // Create a collector to handle async response parsing
    // The collector will self-destruct when complete via shared_ptr ref counting
    auto collector = std::make_shared<ScrewsTiltCollector>(client_, on_success, on_error);
    collector->start();

    // Send the G-code command
    // printer.gcode.script blocks until the command finishes, so the success callback
    // fires after all probing is done and all notify_gcode_response lines have been sent.
    api_.execute_gcode(
        "SCREWS_TILT_CALCULATE",
        [collector]() {
            // JSON-RPC returned — command fully executed, all results should be collected
            spdlog::debug("[Moonraker API] SCREWS_TILT_CALCULATE command finished");
            collector->on_command_finished();
        },
        [collector, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[Moonraker API] SCREWS_TILT_CALCULATE response timed out "
                             "(probing may still be running)");
            } else {
                spdlog::error("[Moonraker API] Failed to send SCREWS_TILT_CALCULATE: {}",
                              err.message);
            }
            collector->mark_completed();
            collector->unregister();
            if (on_error) {
                on_error(err);
            }
        },
        CALIBRATION_TIMEOUT_MS);
}

void MoonrakerAdvancedAPI::run_qgl(SuccessCallback /*on_success*/, ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] run_qgl() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("QGL not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::run_z_tilt_adjust(SuccessCallback /*on_success*/,
                                             ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] run_z_tilt_adjust() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("Z-tilt adjust not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::start_resonance_test(char axis, AdvancedProgressCallback on_progress,
                                                InputShaperCallback on_complete,
                                                ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Starting SHAPER_CALIBRATE AXIS={}", axis);

    // Create collector to handle async response parsing
    auto collector =
        std::make_shared<InputShaperCollector>(client_, axis, on_progress, on_complete, on_error);
    collector->start();

    // Send the G-code command
    // SHAPER_CALIBRATE sweeps 5-100 Hz (~95s) then calculates best shapers (~30-60s)
    std::string cmd = "SHAPER_CALIBRATE AXIS=";
    cmd += axis;

    api_.execute_gcode(
        cmd, []() { spdlog::debug("[Moonraker API] SHAPER_CALIBRATE command accepted"); },
        [collector, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[Moonraker API] SHAPER_CALIBRATE response timed out "
                             "(calibration may still be running)");
            } else {
                spdlog::error("[Moonraker API] Failed to send SHAPER_CALIBRATE: {}", err.message);
            }
            collector->mark_completed();
            collector->unregister();
            if (on_error) {
                on_error(err);
            }
        },
        SHAPER_TIMEOUT_MS);
}

void MoonrakerAdvancedAPI::start_klippain_shaper_calibration(const std::string& /*axis*/,
                                                             SuccessCallback /*on_success*/,
                                                             ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] start_klippain_shaper_calibration() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("Klippain Shake&Tune not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::set_input_shaper(char axis, const std::string& shaper_type,
                                            double frequency, SuccessCallback on_success,
                                            ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Setting input shaper: {}={} @ {:.1f} Hz", axis, shaper_type,
                 frequency);

    // Build SET_INPUT_SHAPER command
    std::ostringstream cmd;
    cmd << "SET_INPUT_SHAPER SHAPER_FREQ_" << axis << "=" << frequency << " SHAPER_TYPE_" << axis
        << "=" << shaper_type;

    api_.execute_gcode(cmd.str(), on_success, on_error);
}

void MoonrakerAdvancedAPI::measure_axes_noise(NoiseCheckCallback on_complete,
                                              ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Starting MEASURE_AXES_NOISE");

    // Create collector to handle async response parsing
    auto collector = std::make_shared<NoiseCheckCollector>(client_, on_complete, on_error);
    collector->start();

    // Send the G-code command
    api_.execute_gcode(
        "MEASURE_AXES_NOISE",
        []() { spdlog::debug("[Moonraker API] MEASURE_AXES_NOISE command accepted"); },
        [collector, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[Moonraker API] MEASURE_AXES_NOISE response timed out");
            } else {
                spdlog::error("[Moonraker API] Failed to send MEASURE_AXES_NOISE: {}", err.message);
            }
            collector->mark_completed();
            collector->unregister();
            if (on_error) {
                on_error(err);
            }
        },
        SHAPER_TIMEOUT_MS);
}

void MoonrakerAdvancedAPI::get_input_shaper_config(InputShaperConfigCallback on_success,
                                                   ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying input shaper configuration");

    // Query configfile to get saved input_shaper settings from printer.cfg
    // (the input_shaper runtime object is empty — config lives in configfile)
    json params = {{"objects", json::object({{"configfile", json::array({"config"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success, on_error](json response) {
            try {
                InputShaperConfig config;

                if (response.contains("result") && response["result"].contains("status") &&
                    response["result"]["status"].contains("configfile") &&
                    response["result"]["status"]["configfile"].contains("config") &&
                    response["result"]["status"]["configfile"]["config"].contains("input_shaper")) {
                    const auto& shaper =
                        response["result"]["status"]["configfile"]["config"]["input_shaper"];

                    config.shaper_type_x = shaper.value("shaper_type_x", "");
                    config.shaper_type_y = shaper.value("shaper_type_y", "");

                    // configfile returns frequencies as strings
                    if (shaper.contains("shaper_freq_x")) {
                        auto& val = shaper["shaper_freq_x"];
                        config.shaper_freq_x =
                            val.is_string() ? std::stof(val.get<std::string>()) : val.get<float>();
                    }
                    if (shaper.contains("shaper_freq_y")) {
                        auto& val = shaper["shaper_freq_y"];
                        config.shaper_freq_y =
                            val.is_string() ? std::stof(val.get<std::string>()) : val.get<float>();
                    }
                    if (shaper.contains("damping_ratio_x")) {
                        auto& val = shaper["damping_ratio_x"];
                        config.damping_ratio_x =
                            val.is_string() ? std::stof(val.get<std::string>()) : val.get<float>();
                    }
                    if (shaper.contains("damping_ratio_y")) {
                        auto& val = shaper["damping_ratio_y"];
                        config.damping_ratio_y =
                            val.is_string() ? std::stof(val.get<std::string>()) : val.get<float>();
                    }

                    // Input shaper is configured if at least one axis has a type set
                    config.is_configured =
                        !config.shaper_type_x.empty() || !config.shaper_type_y.empty();

                    spdlog::info(
                        "[Moonraker API] Input shaper config: X={}@{:.1f}Hz, Y={}@{:.1f}Hz",
                        config.shaper_type_x, config.shaper_freq_x, config.shaper_type_y,
                        config.shaper_freq_y);
                } else {
                    spdlog::debug(
                        "[Moonraker API] Input shaper section not found in printer config");
                    config.is_configured = false;
                }

                if (on_success) {
                    on_success(config);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker API] Failed to parse input shaper config: {}", e.what());
                if (on_error) {
                    MoonrakerError err = MoonrakerError::unknown(
                        std::string("Failed to parse input shaper config: ") + e.what());
                    on_error(err);
                }
            }
        },
        on_error);
}

void MoonrakerAdvancedAPI::get_machine_limits(MachineLimitsCallback on_success,
                                              ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying machine limits from toolhead");

    // Query toolhead object for current velocity/acceleration limits
    json params = {{"objects", {{"toolhead", nullptr}}}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success, on_error](json response) {
            try {
                if (!response.contains("result") || !response["result"].contains("status") ||
                    !response["result"]["status"].contains("toolhead")) {
                    spdlog::warn("[Moonraker API] Toolhead object not available in response");
                    if (on_error) {
                        MoonrakerError err =
                            MoonrakerError::unknown("Toolhead object not available");
                        on_error(err);
                    }
                    return;
                }

                const auto& toolhead = response["result"]["status"]["toolhead"];
                MachineLimits limits;

                // Extract limits with safe defaults
                limits.max_velocity = toolhead.value("max_velocity", 0.0);
                limits.max_accel = toolhead.value("max_accel", 0.0);
                limits.max_accel_to_decel = toolhead.value("max_accel_to_decel", 0.0);
                limits.square_corner_velocity = toolhead.value("square_corner_velocity", 0.0);
                limits.max_z_velocity = toolhead.value("max_z_velocity", 0.0);
                limits.max_z_accel = toolhead.value("max_z_accel", 0.0);

                spdlog::info("[Moonraker API] Machine limits: vel={:.0f} accel={:.0f} "
                             "accel_to_decel={:.0f} scv={:.1f} z_vel={:.0f} z_accel={:.0f}",
                             limits.max_velocity, limits.max_accel, limits.max_accel_to_decel,
                             limits.square_corner_velocity, limits.max_z_velocity,
                             limits.max_z_accel);

                if (on_success) {
                    on_success(limits);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker API] Failed to parse machine limits: {}", e.what());
                if (on_error) {
                    MoonrakerError err = MoonrakerError::unknown(
                        std::string("Failed to parse machine limits: ") + e.what());
                    on_error(err);
                }
            }
        },
        on_error);
}

void MoonrakerAdvancedAPI::set_machine_limits(const MachineLimits& limits,
                                              SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Setting machine limits");

    // Warn about Z limits that cannot be set at runtime
    if (limits.max_z_velocity > 0 || limits.max_z_accel > 0) {
        spdlog::warn("[Moonraker API] max_z_velocity and max_z_accel cannot be set "
                     "via SET_VELOCITY_LIMIT - they require config changes");
    }

    // Build SET_VELOCITY_LIMIT command with only non-zero parameters
    // Use fixed precision to avoid floating point representation issues
    std::ostringstream cmd;
    cmd << std::fixed << std::setprecision(1);
    cmd << "SET_VELOCITY_LIMIT";

    bool has_params = false;

    if (limits.max_velocity > 0) {
        cmd << " VELOCITY=" << limits.max_velocity;
        has_params = true;
    }
    if (limits.max_accel > 0) {
        cmd << " ACCEL=" << limits.max_accel;
        has_params = true;
    }
    if (limits.max_accel_to_decel > 0) {
        cmd << " ACCEL_TO_DECEL=" << limits.max_accel_to_decel;
        has_params = true;
    }
    if (limits.square_corner_velocity > 0) {
        cmd << " SQUARE_CORNER_VELOCITY=" << limits.square_corner_velocity;
        has_params = true;
    }

    if (!has_params) {
        spdlog::warn("[Moonraker API] set_machine_limits called with no valid parameters");
        if (on_error) {
            MoonrakerError err =
                MoonrakerError::validation_error("", "No valid machine limit parameters provided");
            on_error(err);
        }
        return;
    }

    spdlog::debug("[Moonraker API] Executing: {}", cmd.str());
    api_.execute_gcode(cmd.str(), on_success, on_error);
}

void MoonrakerAdvancedAPI::save_config(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Sending SAVE_CONFIG");
    api_.execute_gcode("SAVE_CONFIG", std::move(on_success), std::move(on_error));
}

void MoonrakerAdvancedAPI::execute_macro(const std::string& name,
                                         const std::map<std::string, std::string>& params,
                                         SuccessCallback on_success, ErrorCallback on_error,
                                         uint32_t timeout_ms, bool suppress_auto_toast) {
    // Validate macro name - only allow alphanumeric, underscore (standard Klipper macro names)
    if (name.empty()) {
        spdlog::error("[Moonraker API] execute_macro() called with empty name");
        if (on_error) {
            MoonrakerError err =
                MoonrakerError::validation_error("execute_macro", "Macro name cannot be empty");
            on_error(err);
        }
        return;
    }

    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            spdlog::error("[Moonraker API] Invalid macro name '{}' contains illegal character '{}'",
                          name, c);
            if (on_error) {
                MoonrakerError err = MoonrakerError::validation_error(
                    "execute_macro", "Macro name contains illegal characters");
                on_error(err);
            }
            return;
        }
    }

    // Build G-code: MACRO_NAME KEY1=value1 KEY2=value2
    std::ostringstream gcode;
    gcode << name;

    for (const auto& [key, value] : params) {
        // Validate param key - only alphanumeric and underscore
        bool key_valid = !key.empty();
        for (char c : key) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                key_valid = false;
                break;
            }
        }
        if (!key_valid) {
            spdlog::warn("[Moonraker API] Skipping invalid param key '{}'", key);
            continue;
        }

        // Validate param value - reject dangerous characters that could enable G-code injection
        // Allow: alphanumeric, underscore, hyphen, dot, space (for human-readable values)
        bool value_valid = true;
        for (char c : value) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' &&
                c != ' ') {
                value_valid = false;
                break;
            }
        }
        if (!value_valid) {
            spdlog::warn("[Moonraker API] Skipping param with unsafe value: {}={}", key, value);
            continue;
        }

        // Safe to include - quote if it has spaces
        if (value.find(' ') != std::string::npos) {
            gcode << " " << key << "=\"" << value << "\"";
        } else {
            gcode << " " << key << "=" << value;
        }
    }

    std::string gcode_str = gcode.str();
    spdlog::debug("[Moonraker API] Executing macro: {}", gcode_str);

    // Default to MACRO_TIMEOUT_MS (5 min) — user macros can do anything
    uint32_t effective_timeout = timeout_ms > 0 ? timeout_ms : MoonrakerAPI::MACRO_TIMEOUT_MS;
    api_.execute_gcode(gcode_str, std::move(on_success), std::move(on_error), effective_timeout,
                       /*silent=*/suppress_auto_toast);
}

std::vector<MacroInfo> MoonrakerAdvancedAPI::get_user_macros(bool /*include_system*/) const {
    spdlog::warn("[Moonraker API] get_user_macros() not yet implemented");
    return {};
}

// ============================================================================
// Advanced Panel Operations - PID Calibration
// ============================================================================

void MoonrakerAdvancedAPI::get_heater_pid_values(
    const std::string& heater, MoonrakerAdvancedAPI::PIDCalibrateCallback on_complete,
    MoonrakerAdvancedAPI::ErrorCallback on_error) {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [heater, on_complete, on_error](json response) {
            try {
                if (!response.contains("result") || !response["result"].contains("status") ||
                    !response["result"]["status"].contains("configfile") ||
                    !response["result"]["status"]["configfile"].contains("settings")) {
                    spdlog::debug("[Moonraker API] configfile.settings not available in response");
                    if (on_error) {
                        on_error(MoonrakerError::unknown("configfile.settings not available",
                                                         "get_pid_values"));
                    }
                    return;
                }

                const json& settings = response["result"]["status"]["configfile"]["settings"];

                if (!settings.contains(heater)) {
                    if (on_error) {
                        on_error(MoonrakerError::unknown("Heater '" + heater + "' not in config",
                                                         "get_pid_values"));
                    }
                    return;
                }

                const json& h = settings[heater];
                if (h.contains("pid_kp") && h.contains("pid_ki") && h.contains("pid_kd")) {
                    float kp = h["pid_kp"].get<float>();
                    float ki = h["pid_ki"].get<float>();
                    float kd = h["pid_kd"].get<float>();
                    spdlog::debug(
                        "[Moonraker API] Fetched PID values for {}: Kp={:.3f} Ki={:.3f} Kd={:.3f}",
                        heater, kp, ki, kd);
                    if (on_complete) {
                        on_complete(kp, ki, kd);
                    }
                } else {
                    if (on_error) {
                        on_error(MoonrakerError::unknown(
                            "No PID values for heater '" + heater + "'", "get_pid_values"));
                    }
                }
            } catch (const std::exception& ex) {
                spdlog::warn("[Moonraker API] Error parsing PID values: {}", ex.what());
                if (on_error) {
                    on_error(MoonrakerError::unknown(std::string("Parse error: ") + ex.what(),
                                                     "get_pid_values"));
                }
            }
        },
        [on_error](const MoonrakerError& err) {
            spdlog::debug("[Moonraker API] Failed to fetch PID values: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        });
}

void MoonrakerAdvancedAPI::get_heater_control_type(
    const std::string& heater, MoonrakerAdvancedAPI::HeaterControlTypeCallback on_complete,
    MoonrakerAdvancedAPI::ErrorCallback on_error) {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [heater, on_complete, on_error](json response) {
            try {
                if (!response.contains("result") || !response["result"].contains("status") ||
                    !response["result"]["status"].contains("configfile") ||
                    !response["result"]["status"]["configfile"].contains("settings")) {
                    spdlog::debug(
                        "[Moonraker API] configfile.settings not available for control type query");
                    if (on_error) {
                        on_error(MoonrakerError::unknown("configfile.settings not available",
                                                         "get_heater_control_type"));
                    }
                    return;
                }

                const json& settings = response["result"]["status"]["configfile"]["settings"];

                if (!settings.contains(heater)) {
                    if (on_error) {
                        on_error(MoonrakerError::unknown("Heater '" + heater + "' not in config",
                                                         "get_heater_control_type"));
                    }
                    return;
                }

                const json& h = settings[heater];
                std::string control = h.value("control", "pid");
                spdlog::debug("[Moonraker API] Heater '{}' control type: {}", heater, control);
                if (on_complete) {
                    on_complete(control);
                }
            } catch (const std::exception& ex) {
                spdlog::warn("[Moonraker API] Error parsing heater control type: {}", ex.what());
                if (on_error) {
                    on_error(MoonrakerError::unknown(std::string("Parse error: ") + ex.what(),
                                                     "get_heater_control_type"));
                }
            }
        },
        [on_error](const MoonrakerError& err) {
            spdlog::debug("[Moonraker API] Failed to fetch heater control type: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        });
}

void MoonrakerAdvancedAPI::start_pid_calibrate(
    const std::string& heater, int target_temp,
    MoonrakerAdvancedAPI::PIDCalibrateCallback on_complete, ErrorCallback on_error,
    PIDProgressCallback on_progress) {
    spdlog::info("[MoonrakerAPI] Starting PID calibration for {} at {}°C", heater, target_temp);

    auto collector = std::make_shared<PIDCalibrateCollector>(client_, std::move(on_complete),
                                                             on_error, std::move(on_progress));
    collector->start();

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "PID_CALIBRATE HEATER=%s TARGET=%d", heater.c_str(), target_temp);

    // silent=true: PID errors are handled by the collector and UI panel, not global toast
    api_.execute_gcode(
        cmd, nullptr,
        [collector, on_error](const MoonrakerError& err) {
            // The notify_gcode_response collector — not this RPC — is the authority for
            // PID_CALIBRATE completion. Slow-cooling beds can run longer than the RPC
            // timeout (#988); on timeout we keep the collector registered so the eventual
            // "PID parameters:" result line still completes the calibration. The UI panel
            // owns the user-facing "taking longer than expected" backstop. A genuine RPC
            // error (heater misconfigured, connection lost) is still terminal.
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[MoonrakerAPI] PID_CALIBRATE RPC timed out; collector still "
                             "listening for result (calibration may still be running)");
                return;
            }
            spdlog::error("[MoonrakerAPI] Failed to send PID_CALIBRATE: {}", err.message);
            collector->mark_completed();
            collector->unregister();
            if (on_error)
                on_error(err);
        },
        PID_TIMEOUT_MS, true);
}

void MoonrakerAdvancedAPI::start_mpc_calibrate(
    const std::string& heater, int target_temp, int fan_breakpoints,
    MoonrakerAdvancedAPI::MPCCalibrateCallback on_complete, ErrorCallback on_error,
    MPCProgressCallback on_progress) {
    spdlog::info("[MoonrakerAPI] Starting MPC calibration for {} at {}°C (fan_breakpoints={})",
                 heater, target_temp, fan_breakpoints);

    auto collector = std::make_shared<MPCCalibrateCollector>(
        client_, std::move(on_complete), on_error, std::move(on_progress), fan_breakpoints > 0);
    collector->start();

    std::string cmd = fmt::format("MPC_CALIBRATE HEATER={} TARGET={}", heater, target_temp);
    if (fan_breakpoints > 0) {
        cmd += fmt::format(" FAN_BREAKPOINTS={}", fan_breakpoints);
    }

    // silent=true: MPC errors are handled by the collector and UI panel, not global toast
    api_.execute_gcode(
        cmd, nullptr,
        [collector, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[MoonrakerAPI] MPC_CALIBRATE response timed out "
                             "(calibration may still be running)");
            } else {
                spdlog::error("[MoonrakerAPI] Failed to send MPC_CALIBRATE: {}", err.message);
            }
            collector->mark_completed();
            collector->unregister();
            if (on_error)
                on_error(err);
        },
        MPC_TIMEOUT_MS, true);
}

// ============================================================================
// Belt Tension Operations
// ============================================================================

void MoonrakerAdvancedAPI::detect_belt_hardware(BeltHardwareCallback on_complete,
                                                ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Detecting belt tension hardware capabilities");

    // Step 1: Query printer.objects.list. The response is unused - accelerometer
    // presence comes from AccelSensorManager below, not from parsing this list.
    json params = json::object();
    client_.send_jsonrpc(
        "printer.objects.list", params,
        [this, on_complete, on_error](const json& /*response*/) {
            helix::calibration::BeltTensionHardware hw;

            // Use AccelSensorManager as the single source of truth for
            // accelerometer detection (discovers from configfile.config,
            // handles all chip types including beacon)
            hw.has_adxl = helix::sensors::AccelSensorManager::instance().has_sensors();

            spdlog::info("[MoonrakerAPI] Belt HW scan: adxl={}", hw.has_adxl);

            // Step 2: Query kinematics type
            json query_params;
            query_params["objects"]["configfile"] = json::array({"settings"});
            client_.send_jsonrpc(
                "printer.objects.query", query_params,
                [hw, on_complete, on_error](const json& config_response) mutable {
                    try {
                        if (config_response.contains("result") &&
                            config_response["result"].contains("status") &&
                            config_response["result"]["status"].contains("configfile") &&
                            config_response["result"]["status"]["configfile"].contains(
                                "settings")) {
                            const auto& settings =
                                config_response["result"]["status"]["configfile"]["settings"];

                            if (settings.contains("printer") &&
                                settings["printer"].contains("kinematics")) {
                                hw.kinematics_name =
                                    settings["printer"]["kinematics"].get<std::string>();

                                if (hw.kinematics_name == "corexy" ||
                                    hw.kinematics_name == "corexz") {
                                    hw.kinematics = helix::calibration::KinematicsType::COREXY;
                                } else if (hw.kinematics_name == "cartesian") {
                                    hw.kinematics = helix::calibration::KinematicsType::CARTESIAN;
                                } else {
                                    hw.kinematics = helix::calibration::KinematicsType::UNKNOWN;
                                }
                            }
                        }

                        spdlog::info("[MoonrakerAPI] Belt HW kinematics: {} (type={})",
                                     hw.kinematics_name, static_cast<int>(hw.kinematics));

                        if (on_complete)
                            on_complete(hw);
                    } catch (const std::exception& e) {
                        spdlog::error("[MoonrakerAPI] Failed to parse kinematics: {}", e.what());
                        if (on_error)
                            on_error(MoonrakerError::json_rpc_error(
                                "", fmt::format("Failed to detect kinematics: {}", e.what())));
                    }
                },
                [on_error](const MoonrakerError& err) {
                    spdlog::error("[MoonrakerAPI] Kinematics query failed: {}", err.message);
                    if (on_error)
                        on_error(err);
                });
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[MoonrakerAPI] Object list query failed: {}", err.message);
            if (on_error)
                on_error(err);
        });
}

void MoonrakerAdvancedAPI::test_belt_resonance(const std::string& axis_param,
                                               const std::string& output_name,
                                               AdvancedProgressCallback on_progress,
                                               BeltResonanceCallback on_complete,
                                               ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Starting belt resonance test: axis={}, name={}", axis_param,
                 output_name);

    // Build G-code: TEST_RESONANCES AXIS=<param> OUTPUT=raw_data NAME=<name>
    std::string gcode =
        fmt::format("TEST_RESONANCES AXIS={} OUTPUT=raw_data NAME={}", axis_param, output_name);

    api_.execute_gcode(
        gcode,
        [output_name, on_complete]() {
            spdlog::info("[MoonrakerAPI] Belt resonance test complete for {}", output_name);
            // The CSV file will be at /tmp/raw_data_<name>*.csv
            if (on_complete)
                on_complete(output_name);
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[MoonrakerAPI] Belt resonance test failed: {}", err.message);
            if (on_error)
                on_error(err);
        },
        BELT_TENSION_TIMEOUT_MS);
}

void MoonrakerAdvancedAPI::excite_belt_at_frequency(const std::string& axis_param, float freq_hz,
                                                    SuccessCallback on_complete,
                                                    ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Exciting belt at {:.1f} Hz, axis={}", freq_hz, axis_param);

    // Narrow frequency band: holds near freq_hz for ~5 seconds
    // FREQ_START=F FREQ_END=F+0.5 HZ_PER_SEC=0.1 -> 5 seconds of excitation
    std::string gcode = fmt::format(
        "TEST_RESONANCES AXIS={} FREQ_START={:.1f} FREQ_END={:.1f} HZ_PER_SEC=0.1 OUTPUT=raw_data",
        axis_param, freq_hz, freq_hz + 0.5f);

    api_.execute_gcode(
        gcode,
        [on_complete]() {
            spdlog::debug("[MoonrakerAPI] Belt excitation complete");
            if (on_complete)
                on_complete();
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[MoonrakerAPI] Belt excitation failed: {}", err.message);
            if (on_error)
                on_error(err);
        },
        30000); // 30 second timeout for fixed-freq excitation
}

void MoonrakerAdvancedAPI::download_accel_csv(const std::string& name,
                                              std::function<void(const std::string&)> on_complete,
                                              ErrorCallback on_error) {
    spdlog::debug("[MoonrakerAPI] Downloading accel CSV for: {}", name);

    // List files in data_store directory to find the CSV.
    // Klipper stores TEST_RESONANCES OUTPUT=raw_data files in the data directory,
    // accessible via Moonraker's file API under the 'config' root.
    json params;
    params["path"] = "data_store";
    params["root"] = "config";

    client_.send_jsonrpc(
        "server.files.list", params,
        [this, name, on_complete, on_error](const json& response) {
            std::string target_prefix = "raw_data_" + name;
            std::string best_file;

            try {
                if (!response.contains("result")) {
                    spdlog::error("[MoonrakerAPI] File list response missing 'result' field");
                    if (on_error)
                        on_error(MoonrakerError::json_rpc_error(
                            "", "File list response missing 'result' field"));
                    return;
                }
                const auto& result = response["result"];
                if (!result.is_array()) {
                    spdlog::error("[MoonrakerAPI] File list 'result' is not an array");
                    if (on_error)
                        on_error(MoonrakerError::json_rpc_error(
                            "", "File list 'result' is not an array"));
                    return;
                }
                for (const auto& file : result) {
                    std::string filename = file.value("path", "");
                    if (filename.find(target_prefix) != std::string::npos &&
                        filename.find(".csv") != std::string::npos) {
                        if (filename > best_file) {
                            best_file = filename;
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("[MoonrakerAPI] Failed to parse file list: {}", e.what());
                if (on_error)
                    on_error(MoonrakerError::json_rpc_error("", "Failed to find CSV data file"));
                return;
            }

            if (best_file.empty()) {
                spdlog::error("[MoonrakerAPI] No CSV file found matching: {}", target_prefix);
                if (on_error)
                    on_error(
                        MoonrakerError::json_rpc_error("", "No accelerometer data file found"));
                return;
            }

            spdlog::info("[MoonrakerAPI] Found CSV file: {}", best_file);

            // Download the CSV file content
            json dl_params;
            dl_params["filename"] = "data_store/" + best_file;
            dl_params["root"] = "config";
            client_.send_jsonrpc(
                "server.files.get_file", dl_params,
                [on_complete, on_error](const json& file_response) {
                    try {
                        std::string csv_data;
                        if (file_response.contains("result")) {
                            const auto& result = file_response["result"];
                            if (result.is_string()) {
                                csv_data = result.get<std::string>();
                            } else {
                                csv_data = result.dump();
                            }
                        } else if (file_response.is_string()) {
                            csv_data = file_response.get<std::string>();
                        } else {
                            csv_data = file_response.dump();
                        }
                        if (on_complete)
                            on_complete(csv_data);
                    } catch (const std::exception& e) {
                        spdlog::error("[MoonrakerAPI] Failed to read CSV data: {}", e.what());
                        if (on_error)
                            on_error(MoonrakerError::json_rpc_error(
                                "", fmt::format("Failed to read CSV data: {}", e.what())));
                    }
                },
                [on_error](const MoonrakerError& err) {
                    spdlog::error("[MoonrakerAPI] Failed to download CSV: {}", err.message);
                    if (on_error)
                        on_error(err);
                });
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[MoonrakerAPI] Failed to list data files: {}", err.message);
            if (on_error)
                on_error(err);
        });
}

// ============================================================================
