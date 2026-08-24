// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_sensor_manager.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>
#include <cctype>

// CRITICAL: Subject updates trigger lv_obj_invalidate() which asserts if called
// during LVGL rendering. WebSocket callbacks run on libhv's event loop thread,
// not the main LVGL thread. We must defer subject updates to the main thread
// via ui_queue_update() to avoid the "Invalidate area not allowed during rendering"
// assertion.

namespace helix {

// ============================================================================
// Singleton
// ============================================================================

FilamentSensorManager& FilamentSensorManager::instance() {
    static FilamentSensorManager instance;
    return instance;
}

FilamentSensorManager::FilamentSensorManager() : startup_time_(std::chrono::steady_clock::now()) {}

FilamentSensorManager::~FilamentSensorManager() = default;

// ============================================================================
// ISensorManager Interface
// ============================================================================

std::string FilamentSensorManager::category_name() const {
    return "filament_switch";
}

void FilamentSensorManager::discover(const std::vector<std::string>& klipper_objects) {
    // Filter to only filament sensor objects and delegate to discover_sensors
    std::vector<std::string> sensor_names;
    for (const auto& obj : klipper_objects) {
        // Match filament_switch_sensor and filament_motion_sensor prefixes
        if (obj.rfind("filament_switch_sensor ", 0) == 0 ||
            obj.rfind("filament_motion_sensor ", 0) == 0) {
            sensor_names.push_back(obj);
        }
    }
    discover_sensors(sensor_names);
}

void FilamentSensorManager::load_config(const nlohmann::json& /*config*/) {
    // This manager uses legacy Config-based persistence
    // Delegate to the file-based config loader
    load_config_from_file();
}

nlohmann::json FilamentSensorManager::save_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Build and return the config JSON
    // Also save to file for legacy compatibility
    nlohmann::json config;
    config["master_enabled"] = master_enabled_;

    nlohmann::json sensors_array = nlohmann::json::array();
    for (const auto& sensor : sensors_) {
        nlohmann::json sensor_json;
        sensor_json["klipper_name"] = sensor.klipper_name;
        sensor_json["role"] = role_to_config_string(sensor.role);
        sensor_json["enabled"] = sensor.enabled;
        sensor_json["type"] = type_to_config_string(sensor.type);
        sensors_array.push_back(sensor_json);
    }
    config["sensors"] = sensors_array;

    return config;
}

// ============================================================================
// Initialization
// ============================================================================

void FilamentSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    spdlog::trace("[FilamentSensorManager] Initializing subjects");

    // Initialize all subjects with SubjectManager for automatic cleanup
    // Role-state encoding (filament_runout_detected, filament_toolhead_detected,
    // filament_entry_detected, probe_triggered):
    //   -1 = no sensor configured for this role (hide indicator entirely)
    //    0 = sensor enabled, no filament / not triggered (empty/red)
    //    1 = sensor enabled, filament present / triggered (loaded/green)
    //    2 = sensor configured but DISABLED (master toggle off or per-sensor
    //        enabled=false) — render as "off/unknown" so the user can see
    //        runout protection is inactive instead of mistaking a hidden
    //        indicator for "everything is fine".
    UI_MANAGED_SUBJECT_INT(runout_detected_, -1, "filament_runout_detected", subjects_);
    // Print-scoped runout (FIX B): same encoding as filament_runout_detected but
    // considers only the active print's used tools (lane truth). Driven by
    // PrintStatusPanel via set_scoped_runout(); the in-print badge binds this.
    UI_MANAGED_SUBJECT_INT(scoped_runout_, -1, "filament_runout_scoped", subjects_);
    UI_MANAGED_SUBJECT_INT(toolhead_detected_, -1, "filament_toolhead_detected", subjects_);
    UI_MANAGED_SUBJECT_INT(entry_detected_, -1, "filament_entry_detected", subjects_);
    UI_MANAGED_SUBJECT_INT(probe_triggered_, -1, "probe_triggered", subjects_);
    UI_MANAGED_SUBJECT_INT(any_runout_, 0, "filament_any_runout", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_active_, 0, "filament_motion_active", subjects_);
    UI_MANAGED_SUBJECT_INT(master_enabled_subject_, master_enabled_ ? 1 : 0,
                           "filament_master_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "filament_sensor_count", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "FilamentSensorManager", []() { FilamentSensorManager::instance().deinit_subjects(); });

    spdlog::trace("[FilamentSensorManager] Subjects initialized");
}

void FilamentSensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[FilamentSensorManager] Deinitializing subjects");

    // Deinitialize all subjects to disconnect observers before lv_deinit()
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::trace("[FilamentSensorManager] Subjects deinitialized");
}

void FilamentSensorManager::discover_sensors(const std::vector<std::string>& klipper_sensor_names) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Reset grace period timer - now anchored to Moonraker connection, not app startup
    // This ensures we wait for sensor state to stabilize AFTER connection is established
    startup_time_ = std::chrono::steady_clock::now();

    spdlog::debug("[FilamentSensorManager] Discovering {} sensors", klipper_sensor_names.size());

    // Clear existing sensors but preserve state for reconnection
    sensors_.clear();
    initial_status_received_ = false;

    for (const auto& klipper_name : klipper_sensor_names) {
        std::string sensor_name;
        FilamentSensorType type = FilamentSensorType::SWITCH; // Default, overwritten by parse

        if (!parse_klipper_name(klipper_name, sensor_name, type)) {
            spdlog::warn("[FilamentSensorManager] Failed to parse sensor name: {}", klipper_name);
            continue;
        }

        FilamentSensorConfig config(klipper_name, sensor_name, type);

        // Auto-assign RUNOUT role if sensor name suggests it's a runout sensor
        // (e.g., "runout", "fsensor_runout", "runout_sensor")
        // Only assign if no other sensor already has RUNOUT role
        std::string lower_name = sensor_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (lower_name.find("runout") != std::string::npos) {
            bool runout_already_assigned = false;
            for (const auto& s : sensors_) {
                if (s.role == FilamentSensorRole::RUNOUT) {
                    runout_already_assigned = true;
                    break;
                }
            }
            if (!runout_already_assigned) {
                config.role = FilamentSensorRole::RUNOUT;
                spdlog::debug(
                    "[FilamentSensorManager] Auto-assigned RUNOUT role to '{}' based on name",
                    sensor_name);
            }
        }

        sensors_.push_back(config);

        // Initialize state if not already present
        if (states_.find(klipper_name) == states_.end()) {
            FilamentSensorState state;
            state.available = true;
            states_[klipper_name] = state;
        } else {
            states_[klipper_name].available = true;
        }

        spdlog::debug("[FilamentSensorManager] Discovered sensor: {} (type: {})", sensor_name,
                      type == FilamentSensorType::MOTION ? "motion" : "switch");
    }

    // Mark sensors that disappeared as unavailable
    for (auto& [name, state] : states_) {
        bool found = false;
        for (const auto& sensor : sensors_) {
            if (sensor.klipper_name == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            state.available = false;
        }
    }

    // Update sensor count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&sensor_count_, static_cast<int>(sensors_.size()));
    }

    spdlog::debug("[FilamentSensorManager] Discovered {} filament sensors", sensors_.size());
}

bool FilamentSensorManager::has_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !sensors_.empty();
}

std::vector<FilamentSensorConfig> FilamentSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_; // Return thread-safe copy
}

size_t FilamentSensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

// ============================================================================
// Configuration
// ============================================================================

void FilamentSensorManager::load_config_from_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[FilamentSensorManager] Loading config from file");

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[FilamentSensorManager] Config not initialized");
        return;
    }

    // Build path using default printer prefix
    std::string base_path = config->df() + "filament_sensors";

    // Load master enable
    master_enabled_ = config->get<bool>(base_path + "/master_enabled", true);
    if (subjects_initialized_) {
        lv_subject_set_int(&master_enabled_subject_, master_enabled_ ? 1 : 0);
    }

    // Load per-sensor config
    try {
        const json* sensors_node = config->try_get_json(base_path + "/sensors");
        if (sensors_node != nullptr && sensors_node->is_array()) {
            for (const auto& sensor_json : *sensors_node) {
                if (!sensor_json.contains("klipper_name")) {
                    continue;
                }

                std::string klipper_name = sensor_json["klipper_name"].get<std::string>();
                auto* sensor = find_config(klipper_name);

                if (sensor) {
                    // Update existing sensor config
                    if (sensor_json.contains("role")) {
                        sensor->role =
                            role_from_config_string(sensor_json["role"].get<std::string>());
                    }
                    if (sensor_json.contains("enabled")) {
                        sensor->enabled = sensor_json["enabled"].get<bool>();
                    }
                    spdlog::debug(
                        "[FilamentSensorManager] Loaded config for {}: role={}, enabled={}",
                        klipper_name, role_to_config_string(sensor->role), sensor->enabled);
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("[FilamentSensorManager] No sensor config found: {}", e.what());
    }

    update_subjects();

    // Log final state of all sensors at INFO for debugging
    spdlog::debug("[FilamentSensorManager] Config loaded, master_enabled={}", master_enabled_);
    for (const auto& sensor : sensors_) {
        spdlog::debug("[FilamentSensorManager]   {} -> role={}, enabled={}", sensor.klipper_name,
                      role_to_config_string(sensor.role), sensor.enabled);
    }
}

void FilamentSensorManager::save_config_to_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[FilamentSensorManager] Saving config to file");

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[FilamentSensorManager] Config not initialized");
        return;
    }

    // Build path using default printer prefix
    std::string base_path = config->df() + "filament_sensors";

    // Build filament_sensors config
    json fs_config;
    fs_config["master_enabled"] = master_enabled_;

    json sensors_array = json::array();
    for (const auto& sensor : sensors_) {
        json sensor_json;
        sensor_json["klipper_name"] = sensor.klipper_name;
        sensor_json["role"] = role_to_config_string(sensor.role);
        sensor_json["enabled"] = sensor.enabled;
        sensor_json["type"] = type_to_config_string(sensor.type);
        sensors_array.push_back(sensor_json);
    }
    fs_config["sensors"] = sensors_array;

    // Set the config using JSON pointer path
    config->get_json(base_path) = fs_config;
    config->save();

    spdlog::info("[FilamentSensorManager] Config saved to file");
}

void FilamentSensorManager::set_sensor_role(const std::string& klipper_name,
                                            FilamentSensorRole role) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // If assigning a role, clear it from any other sensor first
    if (role != FilamentSensorRole::NONE) {
        for (auto& sensor : sensors_) {
            if (sensor.role == role && sensor.klipper_name != klipper_name) {
                spdlog::debug("[FilamentSensorManager] Clearing role {} from {}",
                              role_to_config_string(role), sensor.sensor_name);
                sensor.role = FilamentSensorRole::NONE;
            }
        }
    }

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->role = role;
        spdlog::info("[FilamentSensorManager] Set role for {} to {}", sensor->sensor_name,
                     role_to_config_string(role));
        update_subjects();
    }
}

void FilamentSensorManager::set_sensor_enabled(const std::string& klipper_name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->enabled = enabled;
        spdlog::info("[FilamentSensorManager] Set enabled for {} to {}", sensor->sensor_name,
                     enabled);
        update_subjects();
    }
}

void FilamentSensorManager::set_master_enabled(bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        master_enabled_ = enabled;
    }

    if (subjects_initialized_) {
        lv_subject_set_int(&master_enabled_subject_, enabled ? 1 : 0);
    }

    spdlog::info("[FilamentSensorManager] Master enabled set to {}", enabled);
    update_subjects();
}

bool FilamentSensorManager::is_master_enabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return master_enabled_;
}

// ============================================================================
// Bypass runout arming
// ============================================================================

int FilamentSensorManager::arm_runout_sensors_for_bypass(IMoonrakerAPI* api) {
    if (!api) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Respect the user's global monitoring switch — arming firmware sensors
    // the user deliberately turned monitoring off for would be a settings
    // change smuggled in as a state change.
    if (!master_enabled_) {
        return 0;
    }

    int armed = 0;
    for (const auto& sensor : sensors_) {
        // ALL runout-role sensors, not find_config_by_role()'s first — on
        // multi-lane hardware (Snapmaker U1) four sensors share the role.
        if (sensor.role != FilamentSensorRole::RUNOUT) {
            continue;
        }
        auto& state = states_[sensor.klipper_name];
        // Arm only a sensor we have observed (available = exists in Klipper)
        // that the firmware currently holds DISABLED. The state default
        // (enabled=true) is "no fresh status yet" — skip rather than guess.
        if (!state.available || state.enabled) {
            continue;
        }
        if (std::find(bypass_armed_.begin(), bypass_armed_.end(), sensor.klipper_name) !=
            bypass_armed_.end()) {
            continue; // already ours from a previous arm
        }
        send_firmware_sensor_enable(api, sensor, true);
        // Optimistic flip; the next status frame confirms. Keeps a second arm
        // call (e.g. two backends transitioning) idempotent even before the
        // echo lands.
        state.enabled = true;
        bypass_armed_.push_back(sensor.klipper_name);
        spdlog::info("[FilamentSensorManager] Bypass: armed runout sensor {} at firmware level",
                     sensor.sensor_name);
        ++armed;
    }
    return armed;
}

int FilamentSensorManager::restore_runout_sensors_after_bypass(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (bypass_armed_.empty()) {
        return 0;
    }
    if (!api) {
        // No handle to send with — keep the armed set so a later transition
        // (or shutdown path that reacquires one) can still restore.
        return 0;
    }

    int restored = 0;
    for (const auto& name : bypass_armed_) {
        // Only restore sensors still present in our config: sending
        // SET_FILAMENT_SENSOR for a removed Klipper object errors, and the
        // error surfaces as an unexplained toast.
        const auto* sensor = find_config(name);
        if (!sensor) {
            continue;
        }
        send_firmware_sensor_enable(api, *sensor, false);
        if (auto it = states_.find(name); it != states_.end()) {
            it->second.enabled = false;
        }
        spdlog::info("[FilamentSensorManager] Bypass: restored runout sensor {} to disabled",
                     sensor->sensor_name);
        ++restored;
    }
    bypass_armed_.clear();
    return restored;
}

bool FilamentSensorManager::has_bypass_armed_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !bypass_armed_.empty();
}

void FilamentSensorManager::set_moonraker_api(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    api_ = api;
    spdlog::debug("[FilamentSensorManager] Moonraker API {} for bypass arming",
                  api ? "set" : "cleared");
}

void FilamentSensorManager::on_bypass_active_changed(bool active) {
    // Entry point for the bypass transition. All policy lives here in the
    // sensor layer; AMS code only reports the transition (AmsState's
    // any_bypass_active() edge) and Application supplies the API handle via
    // set_moonraker_api(). Deliberately NOT auto-restored at app shutdown:
    // firmware toggles this sensor around its own operations anyway
    // (Creality's macros save/restore it per sequence), and restoring on a
    // path where the Moonraker client may already be gone would guess.
    if (active) {
        arm_runout_sensors_for_bypass(api_);
    } else {
        restore_runout_sensors_after_bypass(api_);
    }
}

void FilamentSensorManager::send_firmware_sensor_enable(IMoonrakerAPI* api,
                                                        const FilamentSensorConfig& sensor,
                                                        bool enabled) {
    // SET_FILAMENT_SENSOR takes the bare sensor name — the part after the
    // `[filament_switch_sensor ...]` / `[filament_motion_sensor ...]` section
    // prefix — which is exactly FilamentSensorConfig::sensor_name
    // (parse_klipper_name's split at discovery). Same convention Creality's
    // own macros use (`SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0`).
    // Log-only error disposition: a failed arm/restore must not toast in the
    // middle of a bypass toggle; Klipper's `!!` broadcast still surfaces it.
    const char* what = enabled ? "arm" : "restore";
    api->execute_gcode(
        fmt::format("SET_FILAMENT_SENSOR SENSOR={} ENABLE={}", sensor.sensor_name, enabled ? 1 : 0),
        []() {},
        [what](const MoonrakerError& err) {
            spdlog::warn("[FilamentSensorManager] Bypass sensor {} failed: {}", what, err.message);
        },
        0, /*silent=*/true, nullptr, /*caller_surfaces_errors=*/false);
}

// ============================================================================
// State Queries
// ============================================================================

bool FilamentSensorManager::is_filament_detected(FilamentSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!master_enabled_ || role == FilamentSensorRole::NONE) {
        return false;
    }

    const auto* config = find_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end() || !it->second.available) {
        return false;
    }

    return it->second.filament_detected;
}

bool FilamentSensorManager::is_sensor_available(FilamentSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!master_enabled_ || role == FilamentSensorRole::NONE) {
        return false;
    }

    const auto* config = find_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto it = states_.find(config->klipper_name);
    return it != states_.end() && it->second.available;
}

std::optional<FilamentSensorState>
FilamentSensorManager::get_sensor_state(FilamentSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto* config = find_config_by_role(role);
    if (!config) {
        return std::nullopt;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end()) {
        return std::nullopt;
    }

    return it->second; // Return thread-safe copy
}

bool FilamentSensorManager::has_any_runout() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Grace-period gate intentionally NOT applied here. Toast suppression for
    // the initial Moonraker status burst lives in update_from_status. Blocking
    // has_any_runout for the first 5s after startup masked legitimate runout
    // detection when the user navigated to the print-status panel right after
    // helix-screen restart on an already-paused print. The state default
    // (filament_detected=true) handles the pre-update window safely.

    if (!master_enabled_) {
        return false;
    }

    for (const auto& sensor : sensors_) {
        if (!sensor.enabled || sensor.role == FilamentSensorRole::NONE) {
            spdlog::trace(
                "[FilamentSensorManager] has_any_runout: skipping {} (enabled={}, role={})",
                sensor.sensor_name, sensor.enabled, role_to_config_string(sensor.role));
            continue;
        }

        auto it = states_.find(sensor.klipper_name);
        if (it != states_.end() && it->second.available && !it->second.filament_detected) {
            spdlog::debug("[FilamentSensorManager] has_any_runout: TRUE - {} ({}) has no filament",
                          sensor.sensor_name, role_to_config_string(sensor.role));
            return true;
        }
    }

    return false;
}

namespace {
// Map a per-lane filament sensor short name to its AMS slot index. Snapmaker U1
// names its per-tool motion sensors "e0_filament" .. "e3_filament" (and the
// matching filament_switch_sensor form). Returns the slot index, or -1 if the
// name does not encode a lane (e.g. a single-extruder "runout" sensor) — in
// which case the caller must NOT lane-scope it.
[[nodiscard]] int lane_index_for_sensor(const std::string& sensor_name) {
    // Match "e<N>_filament" where <N> is one or more digits.
    if (sensor_name.size() < 3 || sensor_name.front() != 'e') {
        return -1;
    }
    size_t pos = 1;
    int value = 0;
    while (pos < sensor_name.size() && std::isdigit(static_cast<unsigned char>(sensor_name[pos]))) {
        value = value * 10 + (sensor_name[pos] - '0');
        ++pos;
    }
    if (pos == 1) {
        return -1; // no digits after 'e'
    }
    if (sensor_name.compare(pos, std::string::npos, "_filament") != 0) {
        return -1;
    }
    return value;
}
} // namespace

bool FilamentSensorManager::has_real_runout() const {
    // Snapshot the sensors that report no filament, then release mutex_ before
    // asking AmsState anything. AmsState calls into this class from under its own
    // recursive lock, so taking its lock from inside ours closes an ABBA cycle
    // (TSan: lock-order-inversion). Only the snapshot needs our lock; the runout
    // decision below is pure reads over that snapshot.
    struct Candidate {
        std::string sensor_name;
        FilamentSensorRole role;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        if (!master_enabled_) {
            return false;
        }

        for (const auto& sensor : sensors_) {
            if (!sensor.enabled || sensor.role == FilamentSensorRole::NONE) {
                continue;
            }

            auto it = states_.find(sensor.klipper_name);
            if (it == states_.end() || !it->second.available || it->second.filament_detected) {
                continue; // sensor present and filament detected -> not a runout
            }
            candidates.push_back({sensor.sensor_name, sensor.role});
        }
    }

    AmsBackend* backend = AmsState::instance().get_backend();

    for (const auto& sensor : candidates) {
        // This sensor reports no filament. Decide whether it is a real runout.
        // If it maps to an AMS lane and the backend says that lane is EMPTY /
        // not-present, it is an intentionally-empty lane, not a runout.
        const int lane = backend ? lane_index_for_sensor(sensor.sensor_name) : -1;
        if (lane >= 0) {
            const SlotInfo slot = backend->get_slot_info(lane);
            if (!slot.is_present()) {
                spdlog::debug("[FilamentSensorManager] has_real_runout: ignoring {} - lane {} "
                              "is empty/never-loaded (not a runout)",
                              sensor.sensor_name, lane);
                continue;
            }

            // A lane that JUST finished an unload (unload_finish) is expected to
            // go empty when the user pulls the filament out — the firmware reports
            // the slot AVAILABLE again, so the empty-lane check above no longer
            // catches it, but the sensor still flips to "no filament" seconds
            // later and would pop the runout-guidance modal. Suppress the runout
            // for that lane within the post-unload grace window. This is lane- and
            // time-scoped, so a genuine mid-print runout on a different (loaded,
            // in-use) lane — which was never recently unloaded — is NOT suppressed.
            if (AmsState::instance().was_slot_recently_unloaded(lane)) {
                spdlog::debug("[FilamentSensorManager] suppressing runout on lane {} — "
                              "recently unloaded ({})",
                              lane, sensor.sensor_name);
                continue;
            }
        }

        spdlog::debug("[FilamentSensorManager] has_real_runout: TRUE - {} ({}) lost filament",
                      sensor.sensor_name, role_to_config_string(sensor.role));
        return true;
    }

    return false;
}

namespace {
// Firmware-default head a logical tool routes to with no remap: tools 0..3 map
// to their identity head, anything else falls back to head 0. Mirrors
// PrintSelectDetailView::get_effective_remap()'s default_head().
int default_head_for_tool(int tool) {
    return (tool >= 0 && tool <= 3) ? tool : 0;
}

// Resolve the AMS slot a logical tool routes to, honoring an explicit remap.
int slot_for_tool(int tool, const std::map<int, int>& remap) {
    auto it = remap.find(tool);
    if (it != remap.end() && it->second >= 0) {
        return it->second;
    }
    return default_head_for_tool(tool);
}
} // namespace

FilamentSensorManager::ScopedRunoutScan
FilamentSensorManager::scan_required_lanes(const std::set<int>& tools_used,
                                           const std::map<int, int>& remap) const {
    // Caller holds mutex_ (recursive). Single source of truth for both
    // find_empty_required_lanes() and compute_scoped_runout_value() — dedups the
    // runout-config lookup, backend fetch, availability gate, and per-lane scan.
    ScopedRunoutScan scan;

    if (tools_used.empty()) {
        scan.no_used_tools = true;
        return scan;
    }

    // Runout protection state. find_config_by_role returns the first RUNOUT
    // sensor; on multi-lane (Snapmaker) all four share the role, so any one
    // gating works for the "is runout protection active" question.
    const auto* runout_cfg = find_config_by_role(FilamentSensorRole::RUNOUT);
    scan.runout_configured = (runout_cfg != nullptr);
    if (!runout_cfg) {
        return scan;
    }
    scan.runout_enabled = master_enabled_ && runout_cfg->enabled;

    // Lane truth requires an AMS backend. Scope to the ACTIVE backend (index 0):
    // get_slot_info() takes a per-backend slot index, and tool→slot remaps are
    // resolved against that backend. Multi-backend global-slot resolution (AFC
    // with 2+ units) is a follow-up; until AmsState exposes a global resolver,
    // a single active backend is the correct, consistent scope.
    scan.backend = AmsState::instance().get_backend();
    if (!scan.backend) {
        // No lane truth — capture the aggregate runout sensor reading so the
        // caller can fall back to the unscoped behavior (non-AMS printers).
        if (auto it = states_.find(runout_cfg->klipper_name);
            it != states_.end() && it->second.available) {
            scan.sensor_available = true;
            scan.aggregate_detected = it->second.filament_detected;
        }
        return scan;
    }

    // Freshness gate (FIX issue 8): don't warn from stale filament_exist before
    // any Moonraker status has been processed. `available` is set true the
    // moment a sensor is DISCOVERED (the Klipper object exists), so it does NOT
    // mean "fresh data" — the correct signal is initial_status_received_, set on
    // the first update_from_status(). (The pre-print check runs on the print
    // button, long after connection, so the discovery grace period — which only
    // suppresses startup notification spam — is not the right gate here.)
    scan.sensor_available = initial_status_received_;

    for (int tool : tools_used) {
        const int slot = slot_for_tool(tool, remap);
        const SlotInfo info = scan.backend->get_slot_info(slot);

        // FIX issue 6: an unresolvable slot (no such slot -> slot_index<0) or a
        // slot whose status is genuinely UNKNOWN is NOT a "lane is empty" event.
        // Only flag a slot that resolved and reports a known non-present status
        // (EMPTY). Skipping avoids a false warning for an out-of-range tool.
        if (info.slot_index < 0 || info.status == SlotStatus::UNKNOWN) {
            spdlog::debug("[FilamentSensorManager] required tool {} -> slot {} unresolved/unknown "
                          "(status={}) — not treated as empty",
                          tool, slot, static_cast<int>(info.status));
            continue;
        }

        if (!info.is_present()) {
            spdlog::debug("[FilamentSensorManager] required tool {} -> lane {} is empty "
                          "(lane truth)",
                          tool, slot);
            scan.empty_lanes.emplace_back(tool, slot);
        }
    }

    return scan;
}

std::vector<std::pair<int, int>>
FilamentSensorManager::find_empty_required_lanes(const std::set<int>& tools_used,
                                                 const std::map<int, int>& remap) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const ScopedRunoutScan scan = scan_required_lanes(tools_used, remap);

    // No lane truth, no runout protection, or no fresh data -> no genuinely-empty
    // required lanes to report. The caller falls back to the aggregate sensor
    // check (non-AMS path) when there is no backend.
    if (!scan.backend || !scan.runout_enabled || !scan.sensor_available) {
        return {};
    }
    return scan.empty_lanes;
}

int FilamentSensorManager::compute_scoped_runout_value(const std::set<int>& tools_used,
                                                       const std::map<int, int>& remap) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const ScopedRunoutScan scan = scan_required_lanes(tools_used, remap);

    if (scan.no_used_tools || !scan.runout_configured) {
        return -1; // No tools to scope, or no runout sensor -> hide.
    }
    if (!scan.runout_enabled) {
        return 2; // Runout protection disabled -> muted.
    }

    if (!scan.backend) {
        // No lane truth: fall back to the aggregate runout-role value so single-
        // extruder / non-AMS printers behave exactly as the unscoped subject.
        if (!scan.sensor_available) {
            return -1; // Sensor transiently unavailable -> hide.
        }
        return scan.aggregate_detected ? 1 : 0;
    }

    // Lane truth not yet fresh -> no opinion rather than a premature red badge.
    if (!scan.sensor_available) {
        return -1;
    }

    // 0 (runout/red) if ANY required lane is genuinely empty, else 1 (loaded).
    return scan.empty_lanes.empty() ? 1 : 0;
}

bool FilamentSensorManager::is_motion_active() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!master_enabled_) {
        return false;
    }

    for (const auto& sensor : sensors_) {
        if (sensor.type != FilamentSensorType::MOTION || !sensor.enabled) {
            continue;
        }

        auto it = states_.find(sensor.klipper_name);
        if (it != states_.end() && it->second.available && it->second.enabled) {
            // Motion sensor is active when Klipper reports it as enabled
            // and we've seen recent detection events
            return true;
        }
    }

    return false;
}

// ============================================================================
// State Updates
// ============================================================================

void FilamentSensorManager::update_from_status(const json& status) {
    // Suppress toast notifications for initial state at startup
    // (similar to USB manager - users don't need to be told filament is present)
    auto now = std::chrono::steady_clock::now();
    bool within_grace_period =
        (now - startup_time_) < AppConstants::Startup::SENSOR_STABILIZATION_PERIOD;

    // Collect notifications to send after releasing lock (avoid deadlock)
    struct Notification {
        std::string klipper_name;
        std::string sensor_name;
        FilamentSensorState old_state;
        FilamentSensorState new_state;
        FilamentSensorRole role;
        bool should_toast;
    };
    std::vector<Notification> notifications;
    StateChangeCallback callback_copy;
    bool any_changed = false;

    // Read AMS state BEFORE taking mutex_. AmsState notifies this class from under
    // its own recursive lock (sync_from_backend -> on_bypass_active_changed), so
    // acquiring AmsState's lock from inside ours closes an ABBA cycle that TSan
    // reports as a potential deadlock. These are whole-printer advisory flags, not
    // per-sensor, and our lock never protected AmsState's state anyway.
    const bool ams_active = AmsState::instance().is_filament_operation_active();
    // Peeked, never consumed - the idle runout modal owns the one shot.
    const bool post_unload_grace = AmsState::instance().post_unload_runout_grace_armed();
    // AD5X-IFS auto-unloads filament back into the IFS between prints. The head
    // sensor going empty when the printer is idle is firmware behaviour, not a
    // user-facing event. "Between prints" is the lifecycle's Idle/terminal side, not
    // merely "not PRINTING": a head-empty during a pre-print block is not the
    // firmware's idle auto-unload and must not be swallowed.
    bool ad5x_idle_unload = false;
    if (auto* backend = AmsState::instance().get_backend()) {
        if (backend->get_type() == AmsType::AD5X_IFS) {
            const auto lifecycle = static_cast<PrintState>(
                lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
            if (!job_holds_machine(lifecycle)) {
                ad5x_idle_unload = true;
            }
        }
    }

    // Phase 1: Update state under lock, collect notifications
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // Copy callback for use outside lock
        callback_copy = state_change_callback_;

        // Process filament_switch_sensor updates
        for (const auto& sensor : sensors_) {
            // Build the Klipper object key (e.g., "filament_switch_sensor fsensor")
            std::string key = sensor.klipper_name;

            // Check if this sensor has an update
            // Moonraker sends updates with the full object name as key
            if (!status.contains(key)) {
                // Also try without the prefix for older Moonraker versions
                continue;
            }

            const auto& sensor_data = status[key];
            auto& state = states_[sensor.klipper_name];
            FilamentSensorState old_state = state;

            // Update filament_detected. Subscriptions targeting specific fields
            // (filament_detected, enabled, detection_count) cause Moonraker to send
            // the field as JSON null when the underlying Klipper object lacks it
            // (e.g. Snapmaker U1's filament_motion_sensor reports no detection_count).
            // contains() returns true for null values, so we must explicitly skip
            // null before calling .value(), which throws type_error.302 on null.
            if (auto it = sensor_data.find("filament_detected");
                it != sensor_data.end() && it->is_boolean()) {
                state.filament_detected = it->get<bool>();
            }

            // Firmware enabled flag: switch AND motion sensors both report it
            // (filament_switch_sensor/filament_motion_sensor status). Parsed
            // for every type — the bypass arming path
            // (arm_runout_sensors_for_bypass) needs the switch-sensor reading
            // to know whether there is anything to arm. Same null-skip rule
            // as filament_detected above.
            if (auto it = sensor_data.find("enabled");
                it != sensor_data.end() && it->is_boolean()) {
                state.enabled = it->get<bool>();
                // Honest armed-set bookkeeping: a sensor we armed for bypass
                // that the firmware now reports disabled was stood down by
                // someone else (vendor macros toggle this sensor around their
                // own operations) — we no longer own its state, so a later
                // bypass disengage must not send a restore for it.
                if (!state.enabled) {
                    auto armed_it =
                        std::find(bypass_armed_.begin(), bypass_armed_.end(), sensor.klipper_name);
                    if (armed_it != bypass_armed_.end()) {
                        bypass_armed_.erase(armed_it);
                        spdlog::debug(
                            "[FilamentSensorManager] Bypass: {} reported disabled by firmware — "
                            "dropped from the armed set",
                            sensor.sensor_name);
                    }
                }
            }

            // Motion sensors have additional fields
            if (sensor.type == FilamentSensorType::MOTION) {
                if (auto it = sensor_data.find("detection_count");
                    it != sensor_data.end() && it->is_number_integer()) {
                    state.detection_count = it->get<int>();
                }
            }

            // Check for state change
            if (state.filament_detected != old_state.filament_detected) {
                any_changed = true;

                // Log at WARN if this is a runout (filament gone) on an active sensor
                if (!state.filament_detected && sensor.role != FilamentSensorRole::NONE &&
                    sensor.enabled) {
                    spdlog::warn("[FilamentSensorManager] RUNOUT: {} ({}) filament gone",
                                 sensor.sensor_name, role_to_config_string(sensor.role));
                } else {
                    spdlog::debug("[FilamentSensorManager] Sensor {} state changed: {} -> {}",
                                  sensor.sensor_name,
                                  old_state.filament_detected ? "detected" : "empty",
                                  state.filament_detected ? "detected" : "empty");
                }

                // Queue notification for after lock release
                Notification notif;
                notif.klipper_name = sensor.klipper_name;
                notif.sensor_name = sensor.sensor_name;
                notif.old_state = old_state;
                notif.new_state = state;
                notif.role = sensor.role;
                // Toasts are suppressed during the startup grace period, wizard
                // setup, and active AMS filament operations (load/unload moves
                // filament past sensors intentionally, generating spurious
                // triggers). ams_active and ad5x_idle_unload were read above,
                // before the lock. The runout role is preserved either way, so
                // in-print events still fire.
                // An unload the user asked for ends by dragging filament off the
                // sensor, and that edge lands seconds AFTER the action returns to
                // IDLE — so ams_active above is already false by then and cannot
                // cover it. Warning that filament was removed is noise when the
                // user is the one who just removed it. Peeked, never consumed:
                // the idle runout modal owns the one shot.
                // Only the REMOVAL side is suppressed; an insertion in the same
                // window is still news, and the manual-pull prompt that fires on
                // this same edge (ui_manual_pull_prompt.cpp) is a separate,
                // deliberately-armed INFO and is untouched here.
                const bool post_unload_removal = !state.filament_detected && post_unload_grace;
                notif.should_toast = !within_grace_period && !is_wizard_active() && !ams_active &&
                                     !ad5x_idle_unload && !post_unload_removal && master_enabled_ &&
                                     sensor.enabled && sensor.role != FilamentSensorRole::NONE;
                if (within_grace_period && master_enabled_ && sensor.enabled &&
                    sensor.role != FilamentSensorRole::NONE) {
                    spdlog::debug("[FilamentSensorManager] Suppressing startup toast for {}",
                                  sensor.sensor_name);
                } else if (ams_active && master_enabled_ && sensor.enabled &&
                           sensor.role != FilamentSensorRole::NONE) {
                    spdlog::debug(
                        "[FilamentSensorManager] Suppressing toast during AMS operation for {}",
                        sensor.sensor_name);
                } else if (ad5x_idle_unload && master_enabled_ && sensor.enabled &&
                           sensor.role != FilamentSensorRole::NONE) {
                    spdlog::debug(
                        "[FilamentSensorManager] Suppressing AD5X idle-unload toast for {}",
                        sensor.sensor_name);
                }
                notifications.push_back(notif);
            }
        }

        // Always update subjects on first status (initial_status_received_ handles this)
        // and on any state change. Without this, subjects stay at -1 ("no sensor")
        // when the initial Moonraker status matches the default state (filament_detected=false).
        bool need_subject_update = any_changed || !initial_status_received_;
        initial_status_received_ = true;

        if (need_subject_update) {
            if (sync_mode_) {
                // In test mode, update subjects synchronously
                spdlog::info("[FilamentSensorManager] sync_mode: updating subjects synchronously");
                update_subjects();
            } else {
                // Defer subject updates to main LVGL thread via helix::ui::queue_update()
                // This avoids the "Invalidate area not allowed during rendering" assertion
                // and provides exception safety (try-catch wrapping)
                spdlog::debug("[FilamentSensorManager] async_mode: deferring via ui_queue_update");
                helix::ui::queue_update(
                    [] { FilamentSensorManager::instance().update_subjects_on_main_thread(); });
            }
        }
    }
    // Lock released here

    // Phase 2: Send notifications without holding lock (prevents deadlock)
    for (const auto& notif : notifications) {
        // Fire callback if registered
        if (callback_copy) {
            callback_copy(notif.klipper_name, notif.old_state, notif.new_state);
        }

        // Show toast notification
        if (notif.should_toast) {
            std::string role_name = role_to_display_string(notif.role);
            if (notif.new_state.filament_detected) {
                NOTIFY_INFO("{}: Filament inserted", role_name);
            } else {
                NOTIFY_WARNING("{}: Filament removed", role_name);
            }
        }
    }
}

void FilamentSensorManager::set_state_change_callback(StateChangeCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    state_change_callback_ = std::move(callback);
}

// ============================================================================
// LVGL Subjects
// ============================================================================

lv_subject_t* FilamentSensorManager::get_runout_detected_subject() {
    return &runout_detected_;
}

lv_subject_t* FilamentSensorManager::get_scoped_runout_subject() {
    return &scoped_runout_;
}

void FilamentSensorManager::set_scoped_runout(int value) {
    if (!subjects_initialized_) {
        return;
    }
    // Caller (PrintStatusPanel) drives this on the main LVGL thread.
    lv_subject_set_int(&scoped_runout_, value);
}

lv_subject_t* FilamentSensorManager::get_toolhead_detected_subject() {
    return &toolhead_detected_;
}

lv_subject_t* FilamentSensorManager::get_entry_detected_subject() {
    return &entry_detected_;
}

lv_subject_t* FilamentSensorManager::get_any_runout_subject() {
    return &any_runout_;
}

lv_subject_t* FilamentSensorManager::get_motion_active_subject() {
    return &motion_active_;
}

lv_subject_t* FilamentSensorManager::get_master_enabled_subject() {
    return &master_enabled_subject_;
}

lv_subject_t* FilamentSensorManager::get_sensor_count_subject() {
    return &sensor_count_;
}

lv_subject_t* FilamentSensorManager::get_probe_triggered_subject() {
    return &probe_triggered_;
}

bool FilamentSensorManager::is_probe_triggered() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!master_enabled_) {
        return false;
    }

    const auto* config = find_config_by_role(FilamentSensorRole::Z_PROBE);
    if (!config || !config->enabled) {
        return false;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end() || !it->second.available) {
        return false;
    }

    return it->second.filament_detected;
}

bool FilamentSensorManager::is_in_startup_grace_period() const {
    auto now = std::chrono::steady_clock::now();
    return (now - startup_time_) < AppConstants::Startup::SENSOR_STABILIZATION_PERIOD;
}

// ============================================================================
// Private Helpers
// ============================================================================

bool FilamentSensorManager::parse_klipper_name(const std::string& klipper_name,
                                               std::string& sensor_name,
                                               FilamentSensorType& type) const {
    const std::string switch_prefix = "filament_switch_sensor ";
    const std::string motion_prefix = "filament_motion_sensor ";

    if (klipper_name.rfind(switch_prefix, 0) == 0) {
        sensor_name = klipper_name.substr(switch_prefix.length());
        type = FilamentSensorType::SWITCH;
        return !sensor_name.empty();
    }

    if (klipper_name.rfind(motion_prefix, 0) == 0) {
        sensor_name = klipper_name.substr(motion_prefix.length());
        type = FilamentSensorType::MOTION;
        return !sensor_name.empty();
    }

    return false;
}

FilamentSensorConfig* FilamentSensorManager::find_config(const std::string& klipper_name) {
    for (auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const FilamentSensorConfig*
FilamentSensorManager::find_config(const std::string& klipper_name) const {
    for (const auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const FilamentSensorConfig*
FilamentSensorManager::find_config_by_role(FilamentSensorRole role) const {
    for (const auto& sensor : sensors_) {
        if (sensor.role == role) {
            return &sensor;
        }
    }
    return nullptr;
}

void FilamentSensorManager::update_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Helper to get subject value for a role. See init_subjects() for the
    // -1/0/1/2 encoding rationale.
    auto get_role_value = [this](FilamentSensorRole role) -> int {
        const auto* config = find_config_by_role(role);
        if (!config) {
            return -1; // No sensor configured for this role — hide
        }

        // A configured sensor that is turned off (master toggle or per-sensor)
        // surfaces as "disabled" so the user sees runout protection is OFF
        // rather than seeing a hidden indicator and assuming all is well.
        if (!master_enabled_ || !config->enabled) {
            return 2; // Configured but disabled
        }

        auto it = states_.find(config->klipper_name);
        if (it == states_.end() || !it->second.available) {
            return -1; // Sensor transiently unavailable — hide (not the same
                       // as user-disabled; treat as no-opinion)
        }

        return it->second.filament_detected ? 1 : 0;
    };

    // Update per-role subjects
    lv_subject_set_int(&runout_detected_, get_role_value(FilamentSensorRole::RUNOUT));
    lv_subject_set_int(&toolhead_detected_, get_role_value(FilamentSensorRole::TOOLHEAD));
    lv_subject_set_int(&entry_detected_, get_role_value(FilamentSensorRole::ENTRY));
    lv_subject_set_int(&probe_triggered_, get_role_value(FilamentSensorRole::Z_PROBE));

    // Update aggregate subjects
    // Suppress any_runout during startup grace period to avoid false modal triggers
    // (Moonraker may report sensors as "empty" before Klipper fully initializes)
    bool in_grace = is_in_startup_grace_period();
    int any_runout_value = (in_grace || !has_any_runout()) ? 0 : 1;
    if (in_grace && has_any_runout()) {
        spdlog::info(
            "[FilamentSensorManager] Suppressing runout modal during startup grace period");
    }
    lv_subject_set_int(&any_runout_, any_runout_value);
    lv_subject_set_int(&motion_active_, is_motion_active() ? 1 : 0);

    spdlog::trace("[FilamentSensorManager] Subjects updated: runout={}, toolhead={}, entry={}, "
                  "probe={}, any_runout={}",
                  lv_subject_get_int(&runout_detected_), lv_subject_get_int(&toolhead_detected_),
                  lv_subject_get_int(&entry_detected_), lv_subject_get_int(&probe_triggered_),
                  lv_subject_get_int(&any_runout_));
}

void FilamentSensorManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

void FilamentSensorManager::update_subjects_on_main_thread() {
    // This is called by lv_async_call from the main LVGL thread
    // It's safe to update subjects here without causing render-phase assertions
    update_subjects();
}

} // namespace helix
