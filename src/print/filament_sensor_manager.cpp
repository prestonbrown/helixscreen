// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_sensor_manager.h"

#include "ui_error_reporting.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "core/text_io.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "print_lifecycle_state.h"
#include "printer_discovery.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>

// Subject updates go through publish(): status frames arrive on libhv's event
// loop thread, and a subject set there would invalidate widgets during LVGL
// rendering. publish() defers refresh_subjects() to the main thread through the
// lifetime token, and drops it once deinit_subjects() has run.

namespace helix {

using sensors::SensorEntry;

namespace {

[[nodiscard]] FilamentSensorRole filament_role(const SensorEntry& sensor) {
    return sensors::role_of<FilamentSensorRole>(sensor);
}

[[nodiscard]] FilamentSensorType type_of(const SensorEntry& sensor) {
    return static_cast<FilamentSensorType>(sensor.type);
}

[[nodiscard]] constexpr uint8_t role_id(FilamentSensorRole role) {
    return static_cast<uint8_t>(role);
}

/// Translation key of a role's Settings dropdown option.
[[nodiscard]] constexpr const char* settings_label(FilamentSensorRole role) {
    switch (role) {
    case FilamentSensorRole::RUNOUT:
        return "Runout";
    case FilamentSensorRole::TOOLHEAD:
        return "Toolhead";
    case FilamentSensorRole::ENTRY:
        return "Entry";
    case FilamentSensorRole::NONE:
        break;
    }
    return "None";
}

/// FNV-1a, for the signatures that tell a refresh whether its inputs moved.
class Fnv1a {
  public:
    void mix(uint32_t v) {
        h_ ^= v;
        h_ *= 16777619u;
    }
    void mix(const std::string& s) {
        for (char c : s) {
            mix(static_cast<unsigned char>(c));
        }
    }
    [[nodiscard]] uint32_t value() const {
        return h_;
    }

  private:
    uint32_t h_ = 2166136261u;
};

/// What can quiet the toast for one sensor edge. Runout detection and its
/// subjects fire regardless; only the toast is at stake.
struct ToastSuppressionTerms {
    bool startup_grace = false;
    /// A load or unload moves filament past sensors on purpose.
    bool ams_active = false;
    bool idle_unload_expected = false;
    /// The backend's own prompt is already telling the story (#1388). A backend
    /// that owns nothing, and no backend at all, keeps the toast: there it is the
    /// only runout signal.
    bool runout_surface_owned = false;
    bool wizard_active = false;
    /// An unload the user asked for ends by dragging filament off the sensor, and
    /// that edge lands seconds AFTER the action returns to IDLE, past ams_active.
    /// Only the REMOVAL side counts: an insertion in the same window is still news,
    /// and the manual-pull prompt on this same edge (ui_manual_pull_prompt.cpp) is a
    /// separate, deliberately-armed INFO.
    bool post_unload_removal = false;
};

/// Why this edge gets no toast, or nullptr when nothing suppresses it.
[[nodiscard]] const char* suppression_reason(const ToastSuppressionTerms& terms) {
    if (terms.startup_grace) {
        return "startup grace period";
    }
    if (terms.ams_active) {
        return "AMS operation";
    }
    if (terms.idle_unload_expected) {
        return "idle unload expected";
    }
    if (terms.runout_surface_owned) {
        return "backend owns the runout surface during the job";
    }
    if (terms.wizard_active) {
        return "wizard active";
    }
    if (terms.post_unload_removal) {
        return "removal after a requested unload";
    }
    return nullptr;
}

} // namespace

// ============================================================================
// Singleton
// ============================================================================

FilamentSensorManager::Options FilamentSensorManager::make_options() {
    Options o;
    o.name = "FilamentSensorManager";
    o.kinds = {hw::ObjectKind::FilamentSwitch, hw::ObjectKind::FilamentMotion};
    o.count_subject = "filament_sensor_count";
    o.revision_subject = "filament_sensor_revision";
    o.config_key = "filament_sensors";
    o.settings_section = "filament";
    for (const auto& r : kFilamentSensorRoles) {
        // Every assignable role has one holder; RUNOUT has one per lane, a rule
        // set_role() applies itself.
        const bool assignable = r.value != FilamentSensorRole::NONE;
        o.roles.push_back({role_id(r.value), r.id, settings_label(r.value),
                           /*exclusive=*/assignable, /*offered=*/true});
    }
    for (const auto& t : kFilamentSensorTypes) {
        o.types.push_back({static_cast<uint8_t>(t.value), t.id, t.display});
    }
    return o;
}

bool FilamentSensorManager::listed_in_settings(const SensorEntry& entry) const {
    return is_standalone_filament_sensor(get_printer_state().get_discovery(), entry.display_name,
                                         AmsState::instance().filament_hardware_absent());
}

sensors::SensorRowView FilamentSensorManager::row(size_t index) const {
    sensors::SensorRowView view = SensorManager::row(index);
    // Enabled means nothing for a sensor with no role.
    if (view.role == role_id(FilamentSensorRole::NONE)) {
        view.toggle = sensors::RowToggle::None;
    }
    return view;
}

FilamentSensorManager& FilamentSensorManager::instance() {
    static FilamentSensorManager instance;
    return instance;
}

FilamentSensorManager::FilamentSensorManager()
    : SensorManager(make_options()), startup_time_(std::chrono::steady_clock::now()) {}

// ============================================================================
// Discovery and Config
// ============================================================================

void FilamentSensorManager::on_entries_changed() {
    // A sensor missing from this discovery keeps its reading for a later
    // reconnect, marked unavailable; its lane goes with its entry.
    auto listed = [this](const std::string& name) {
        return std::any_of(entries_locked().begin(), entries_locked().end(),
                           [&](const SensorEntry& e) { return e.klipper_name == name; });
    };
    for (const auto& sensor : entries_locked()) {
        readings_[sensor.klipper_name].available = true;
    }
    for (auto& [name, reading] : readings_) {
        if (!listed(name)) {
            reading.available = false;
        }
    }
    prune_vanished_locked(lanes_);
}

void FilamentSensorManager::on_discovered(const PrinterDiscovery& /*hw*/) {
    // Reset grace period timer - now anchored to Moonraker connection, not app startup
    // This ensures we wait for sensor state to stabilize AFTER connection is established
    startup_time_ = std::chrono::steady_clock::now();

    // A dwell measures how long one sensor has been clear. Rediscovery may not
    // return that sensor at all, and the stabilization grace restarts here
    // regardless, so an outstanding dwell has nothing left to confirm.
    pending_removal_toast_.clear();
    observed_runouts_.clear();
    initial_status_received_ = false;

    std::set<std::string> now;
    for (auto& sensor : entries_locked()) {
        now.insert(sensor.klipper_name);
        // Values kept across a reconnect are a baseline, not a report.
        readings_[sensor.klipper_name].reported = false;

        // Auto-assign RUNOUT role if sensor name suggests it's a runout sensor
        // (e.g., "runout", "fsensor_runout", "runout_sensor"), only to a name
        // new to this manager and only if no other sensor already has RUNOUT.
        const std::string lower_name = helix::text_io::to_lower(sensor.display_name);
        if (known_.count(sensor.klipper_name) == 0 &&
            lower_name.find("runout") != std::string::npos) {
            const bool runout_already_assigned =
                std::any_of(entries_locked().begin(), entries_locked().end(), [](const auto& s) {
                    return filament_role(s) == FilamentSensorRole::RUNOUT;
                });
            if (!runout_already_assigned) {
                sensor.role = role_id(FilamentSensorRole::RUNOUT);
                spdlog::debug(
                    "[FilamentSensorManager] Auto-assigned RUNOUT role to '{}' based on name",
                    sensor.display_name);
            }
        }
        spdlog::debug("[FilamentSensorManager] Discovered sensor: {} (type: {})",
                      sensor.display_name,
                      type_of(sensor) == FilamentSensorType::MOTION ? "motion" : "switch");
    }
    known_ = std::move(now);
    spdlog::debug("[FilamentSensorManager] Discovered {} filament sensors",
                  entries_locked().size());
}

void FilamentSensorManager::read_entry_extra(SensorEntry& entry, const nlohmann::json& saved) {
    const auto lane = saved.find("lane");
    if (lane == saved.end()) {
        return;
    }
    if (lane->is_number_integer() && json_util::as_int(*lane, -1) >= 0) {
        lanes_[entry.klipper_name] = json_util::as_int(*lane, -1);
    } else {
        spdlog::warn("[FilamentSensorManager] {}: ignoring lane {} (expected "
                     "a slot index, 0 or more)",
                     entry.klipper_name, json_util::safe_dump(*lane));
    }
}

void FilamentSensorManager::write_entry_extra(const SensorEntry& entry, nlohmann::json& out) const {
    if (const auto lane = lanes_.find(entry.klipper_name);
        lane != lanes_.end() && lane->second >= 0) {
        out["lane"] = lane->second;
    }
}

void FilamentSensorManager::read_extra_settings(const nlohmann::json& saved) {
    master_enabled_ = true;
    const auto master = saved.find("master_enabled");
    if (master != saved.end() && master->is_boolean()) {
        master_enabled_ = json_util::as_bool(*master, true);
    }
    spdlog::debug("[FilamentSensorManager] Config loaded, master_enabled={}", master_enabled_);
}

void FilamentSensorManager::write_extra_settings(nlohmann::json& out) const {
    out["master_enabled"] = master_enabled_;
}

std::vector<FilamentSensorConfig> FilamentSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    std::vector<FilamentSensorConfig> out;
    for (const auto& sensor : entries_locked()) {
        FilamentSensorConfig config(sensor.klipper_name, sensor.display_name, type_of(sensor));
        config.role = filament_role(sensor);
        config.enabled = sensor.enabled;
        const auto lane = lanes_.find(sensor.klipper_name);
        config.lane = lane != lanes_.end() ? lane->second : -1;
        out.push_back(std::move(config));
    }
    return out;
}

void FilamentSensorManager::set_role(std::string_view klipper_name, uint8_t role_value) {
    const auto role = static_cast<FilamentSensorRole>(role_value);
    bool assigned_found = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        auto& sensors = entries_locked();
        const auto assigned =
            std::find_if(sensors.begin(), sensors.end(),
                         [&](const SensorEntry& s) { return s.klipper_name == klipper_name; });

        // A role has one holder, except RUNOUT, which has one per lane: assigning it
        // clears it from any other sensor watching the same lane, and from every
        // sensor watching none. Heads with a sensor each keep one RUNOUT holder
        // apiece. The other roles' readers look up a single holder.
        if (role != FilamentSensorRole::NONE) {
            const int assigned_lane =
                assigned != sensors.end() ? lane_index_for_sensor(*assigned) : -1;
            for (auto& sensor : sensors) {
                const int lane = lane_index_for_sensor(sensor);
                const bool shares_lane = role != FilamentSensorRole::RUNOUT || assigned_lane < 0 ||
                                         lane < 0 || lane == assigned_lane;
                if (filament_role(sensor) == role && sensor.klipper_name != klipper_name &&
                    shares_lane) {
                    spdlog::debug("[FilamentSensorManager] Clearing role {} from {}",
                                  role_to_config_string(role), sensor.display_name);
                    sensor.role = role_id(FilamentSensorRole::NONE);
                }
            }
        }

        if (assigned != sensors.end()) {
            assigned->role = role_value;
            assigned_found = true;
            spdlog::info("[FilamentSensorManager] Set role for {} to {}", assigned->display_name,
                         role_to_config_string(role));
        }
    }
    if (assigned_found) {
        publish();
    }
}

void FilamentSensorManager::set_master_enabled(bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        master_enabled_ = enabled;
    }
    spdlog::info("[FilamentSensorManager] Master enabled set to {}", enabled);
    publish();
}

bool FilamentSensorManager::is_master_enabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return master_enabled_;
}

// ============================================================================
// Bypass runout arming
// ============================================================================

int FilamentSensorManager::arm_runout_sensors_for_bypass(IMoonrakerAPI* api) {
    if (!api) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex());

    // Respect the user's global monitoring switch — arming firmware sensors
    // the user deliberately turned monitoring off for would be a settings
    // change smuggled in as a state change.
    if (!master_enabled_) {
        return 0;
    }

    int armed = 0;
    for (const auto& sensor : entries_locked()) {
        // ALL runout-role sensors, not the first holder — on multi-lane
        // hardware (Snapmaker U1) four sensors share the role.
        if (filament_role(sensor) != FilamentSensorRole::RUNOUT) {
            continue;
        }
        auto& state = readings_[sensor.klipper_name];
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
                     sensor.display_name);
        ++armed;
    }
    return armed;
}

int FilamentSensorManager::restore_runout_sensors_after_bypass(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex());

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
        const auto sensor =
            std::find_if(entries_locked().begin(), entries_locked().end(),
                         [&](const SensorEntry& s) { return s.klipper_name == name; });
        if (sensor == entries_locked().end()) {
            continue;
        }
        send_firmware_sensor_enable(api, *sensor, false);
        if (auto* st = state(name)) {
            st->enabled = false;
        }
        spdlog::info("[FilamentSensorManager] Bypass: restored runout sensor {} to disabled",
                     sensor->display_name);
        ++restored;
    }
    bypass_armed_.clear();
    return restored;
}

void FilamentSensorManager::set_moonraker_api(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
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
                                                        const SensorEntry& sensor, bool enabled) {
    // SET_FILAMENT_SENSOR takes the bare sensor name — the part after the
    // `[filament_switch_sensor ...]` / `[filament_motion_sensor ...]` section
    // prefix — which is exactly SensorEntry::display_name (the instance the
    // hardware object table splits off at discovery). Same convention Creality's
    // own macros use (`SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0`).
    // The command is the builtin's name, renamed when a macro wraps it, so a
    // wrapper never records the toggle as a user setting.
    // Log-only error disposition: a failed arm/restore must not toast in the
    // middle of a bypass toggle; Klipper's `!!` broadcast still surfaces it.
    const char* what = enabled ? "arm" : "restore";
    api->execute_gcode(
        fmt::format("{} SENSOR={} ENABLE={}", api->hardware().sensor_toggle_command(),
                    sensor.display_name, enabled ? 1 : 0),
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
    std::lock_guard<std::recursive_mutex> lock(mutex());

    if (!master_enabled_ || role == FilamentSensorRole::NONE) {
        return false;
    }

    // Presence question, not a runout decision: the pre-print check and the
    // load/unload buttons want to know whether filament is physically there,
    // and a firmware stand-down (SET_FILAMENT_SENSOR ENABLE=0) leaves the
    // reading live. Runout alerting uses monitors_runout() instead.
    const auto* config = find_monitoring_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto* st = state(config->klipper_name);
    if (!st || !st->available) {
        return false;
    }

    return st->filament_detected;
}

bool FilamentSensorManager::is_sensor_available(FilamentSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());

    if (!master_enabled_ || role == FilamentSensorRole::NONE) {
        return false;
    }

    // Same presence split as is_filament_detected: the sensor must exist and
    // be user-enabled, but a firmware stand-down does not hide it.
    const auto* config = find_monitoring_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto* st = state(config->klipper_name);
    return st && st->available;
}

bool FilamentSensorManager::has_any_runout() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());

    // Grace-period gate intentionally NOT applied here. Toast suppression for
    // the initial Moonraker status burst lives in update_from_status. Blocking
    // has_any_runout for the first 5s after startup masked legitimate runout
    // detection when the user navigated to the print-status panel right after
    // helix-screen restart on an already-paused print. The state default
    // (filament_detected=true) handles the pre-update window safely.

    if (!master_enabled_) {
        return false;
    }

    for (const auto& sensor : entries_locked()) {
        if (!monitors_runout(sensor)) {
            spdlog::trace(
                "[FilamentSensorManager] has_any_runout: skipping {} (enabled={}, role={})",
                sensor.display_name, sensor.enabled, role_to_config_string(filament_role(sensor)));
            continue;
        }

        auto* st = state(sensor.klipper_name);
        if (st && st->available && !st->filament_detected) {
            spdlog::debug("[FilamentSensorManager] has_any_runout: TRUE - {} ({}) has no filament",
                          sensor.display_name, role_to_config_string(filament_role(sensor)));
            return true;
        }
    }

    return false;
}

bool FilamentSensorManager::has_real_runout() const {
    return real_runout_lane().has_value();
}

std::optional<int> FilamentSensorManager::real_runout_lane() const {
    // Snapshot the sensors that report no filament, then release the mutex before
    // asking AmsState anything. AmsState calls into this class from under its own
    // recursive lock, so taking its lock from inside ours closes an ABBA cycle
    // (TSan: lock-order-inversion). Only the snapshot needs our lock; the runout
    // decision below is pure reads over that snapshot.
    struct Candidate {
        std::string sensor_name;
        FilamentSensorRole role;
        int lane;
        bool stood_down;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());

        if (!master_enabled_) {
            return std::nullopt;
        }

        for (const auto& sensor : entries_locked()) {
            if (!monitors_runout(sensor)) {
                continue;
            }

            auto* st = state(sensor.klipper_name);
            if (!st || !st->available || st->filament_detected) {
                continue; // sensor present and filament detected -> not a runout
            }
            candidates.push_back({sensor.display_name, filament_role(sensor),
                                  lane_index_for_sensor(sensor), !st->enabled});
        }
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    // The lane being fed. A backend that cannot name one (none picked, a
    // bypass, more than one current lane) reports a negative value.
    const int current_slot = backend ? backend->get_current_slot() : -1;
    // A sensor's lane is a slot index for a lane sensor, but a tool index for
    // a per-toolhead sensor on a multi-extruder system whose slots outnumber
    // its tools. Matching either index counts as being fed, so a sensor is set
    // aside only when it can be neither.
    const int current_tool = backend ? backend->get_current_tool() : -1;

    for (const auto& sensor : candidates) {
        // This sensor reports no filament. Decide whether it is a real runout.
        // If it maps to an AMS lane and the backend says that lane is EMPTY /
        // not-present, it is an intentionally-empty lane, not a runout.
        // A lane the backend has no slot for is no lane: the sensor stays
        // unscoped rather than reading as an empty slot.
        const SlotInfo slot =
            backend && sensor.lane >= 0 ? backend->get_slot_info(sensor.lane) : SlotInfo{};
        const int lane = slot.slot_index >= 0 ? sensor.lane : -1;
        if (lane >= 0) {
            if (!slot.is_present()) {
                spdlog::debug("[FilamentSensorManager] has_real_runout: ignoring {} - lane {} "
                              "is empty/never-loaded (not a runout)",
                              sensor.sensor_name, lane);
                continue;
            }

            // A runout is about the lane being fed. Every other lane's sensor
            // is idle, whatever the backend says sits in that lane: a head
            // whose filament waits in its feeder, never loaded to the nozzle,
            // reads empty at its own switch without anything running out.
            const bool fed = lane == current_slot || lane == current_tool;
            if (current_slot >= 0 && !fed) {
                spdlog::debug("[FilamentSensorManager] has_real_runout: ignoring {} - lane {} "
                              "is not the one being fed (slot {}, tool {})",
                              sensor.sensor_name, lane, current_slot, current_tool);
                continue;
            }

            // A stood-down sensor counts only while its lane is the one being
            // fed. With no lane named, nothing says it still is.
            if (sensor.stood_down && !fed) {
                spdlog::debug("[FilamentSensorManager] has_real_runout: ignoring {} - stood "
                              "down, and lane {} is no longer the current one",
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
        return lane;
    }

    return std::nullopt;
}

namespace {
// Resolve the AMS slot a logical tool routes to, honoring an explicit remap and
// otherwise the backend's own firmware default map.
//
// @p routing must be the SAME map PrintSelectDetailView::get_effective_remap()
// filtered against, or the lanes scanned here are not the lanes routed to: a
// mismatch silently scans lane 0.
int slot_for_tool(int tool, const std::map<int, int>& remap,
                  const helix::FirmwareRouting& routing) {
    auto it = remap.find(tool);
    if (it != remap.end() && it->second >= 0) {
        return it->second;
    }
    return routing.head(tool);
}
} // namespace

FilamentSensorManager::ScopedRunoutScan FilamentSensorManager::scan_required_lanes(
    const std::set<int>& tools_used, const std::map<int, int>& remap, bool read_stood_down) const {
    // Caller holds the mutex (recursive). Single source of truth for both
    // find_empty_required_lanes() and compute_scoped_runout_value() — dedups the
    // runout-config lookup, backend fetch, availability gate, and per-lane scan.
    ScopedRunoutScan scan;

    if (tools_used.empty()) {
        scan.no_used_tools = true;
        return scan;
    }

    // Runout protection state. The monitoring pick returns the first RUNOUT
    // sensor the firmware is running; on multi-lane (Snapmaker) all four share
    // the role, so any one gating works for the "is runout protection active"
    // question.
    const auto* runout_cfg = find_monitoring_config_by_role(FilamentSensorRole::RUNOUT);
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
        // A sensor the firmware stood down has no reading worth falling back
        // to (Klipper is not acting on it), so it leaves the badge hidden.
        if (auto* st = state(runout_cfg->klipper_name); st && st->available && st->enabled) {
            scan.sensor_available = true;
            scan.aggregate_detected = st->filament_detected;
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

    // One routing for the whole scan, from the same backend whose slots we read.
    const helix::FirmwareRouting routing = scan.backend->firmware_default_routing();

    for (int tool : tools_used) {
        const int slot = slot_for_tool(tool, remap, routing);
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
            continue;
        }

        // Where the slot status is not a filament reading, a RUNOUT sensor
        // watching that slot is. A firmware stand-down leaves the reading live,
        // which the pre-print check relies on while every head is held down.
        if (!scan.backend->slot_status_tracks_filament()) {
            for (const auto& sensor : entries_locked()) {
                if (filament_role(sensor) != FilamentSensorRole::RUNOUT || !sensor.enabled ||
                    lane_index_for_sensor(sensor) != slot) {
                    continue;
                }
                auto* st = state(sensor.klipper_name);
                if (st && st->available && (read_stood_down || st->enabled) &&
                    !st->filament_detected) {
                    spdlog::debug("[FilamentSensorManager] required tool {} -> lane {} is empty "
                                  "({} reads no filament)",
                                  tool, slot, sensor.display_name);
                    scan.empty_lanes.emplace_back(tool, slot);
                    break;
                }
            }
        }
    }

    return scan;
}

std::vector<std::pair<int, int>>
FilamentSensorManager::find_empty_required_lanes(const std::set<int>& tools_used,
                                                 const std::map<int, int>& remap) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());

    const ScopedRunoutScan scan = scan_required_lanes(tools_used, remap, /*read_stood_down=*/true);

    // No lane truth, no runout protection, or no fresh data -> no genuinely-empty
    // required lanes to report. The caller falls back to the aggregate sensor
    // check (non-AMS path) when there is no backend.
    if (!scan.backend || !scan.runout_enabled || !scan.sensor_available) {
        return {};
    }
    return scan.empty_lanes;
}

// The badge is scoped to the tools the RUNNING file uses. During a preparing
// window get_tools_used() still describes the previous job, so widening this to
// job_holds_machine() would scope the badge to the wrong file instead of hiding
// it. Deliberately narrower than the lifecycle's is_active().
//
// Kept as its own name because the QUESTION is about get_tools_used()'s
// freshness, not about who owns the toolhead; the two are coextensional today
// and could legitimately diverge. Sharing the boolean means they cannot drift by
// accident, only on purpose.
bool print_scopes_runout_badge(PrintJobState state) {
    return printer_has_job(state);
}

int FilamentSensorManager::compute_scoped_runout_value(const std::set<int>& tools_used,
                                                       const std::map<int, int>& remap) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());

    const ScopedRunoutScan scan = scan_required_lanes(tools_used, remap, /*read_stood_down=*/false);

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

std::optional<ClogMotionSensor>
FilamentSensorManager::clog_motion_sensor(int active_lane, bool filament_hardware_absent) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());

    if (!master_enabled_) {
        return std::nullopt;
    }

    // An AMS-owned sensor is hidden from Settings > Filament sensors, so the
    // user cannot have enabled it there, and its backend may run it as a
    // presence switch outside a print, where an empty head would read jammed.
    const auto& discovery = get_printer_state().get_discovery();
    const SensorEntry* first = nullptr;
    const SensorEntry* first_armed = nullptr;
    const SensorEntry* first_stopped = nullptr;
    const SensorEntry* on_lane = nullptr;
    for (const auto& sensor : entries_locked()) {
        if (type_of(sensor) != FilamentSensorType::MOTION || !sensor.enabled ||
            !is_standalone_filament_sensor(discovery, sensor.display_name,
                                           filament_hardware_absent)) {
            continue;
        }
        const auto* st = state(sensor.klipper_name);
        if (!st || !st->available) {
            continue;
        }
        if (!first) {
            first = &sensor;
        }
        if (!first_armed && st->enabled) {
            first_armed = &sensor;
        }
        if (!first_stopped && st->enabled && !st->filament_detected) {
            first_stopped = &sensor;
        }
        if (!on_lane && active_lane >= 0 && lane_index_for_sensor(sensor) == active_lane) {
            on_lane = &sensor;
        }
    }

    // With no sensor on the active lane, the worst reading speaks: an encoder
    // only stops seeing movement when its own extruder feeds, so an idle head
    // keeps reading detected and cannot mask the jammed one.
    const SensorEntry* pick = on_lane         ? on_lane
                              : first_stopped ? first_stopped
                              : first_armed   ? first_armed
                                              : first;
    if (!pick) {
        return std::nullopt;
    }
    const auto* st = state(pick->klipper_name);
    ClogMotionSensor out;
    out.sensor_name = pick->display_name;
    out.armed = st->enabled;
    out.filament_moving = st->filament_detected;
    return out;
}

int FilamentSensorManager::motion_signature() const {
    // FNV-1a over everything clog_motion_sensor() reads, so the subject moves
    // exactly when a fresh pick could come out differently.
    Fnv1a h;
    h.mix(master_enabled_ ? 1u : 0u);
    for (const auto& sensor : entries_locked()) {
        if (type_of(sensor) != FilamentSensorType::MOTION) {
            continue;
        }
        h.mix(sensor.klipper_name);
        const auto* st = state(sensor.klipper_name);
        h.mix((sensor.enabled ? 1u : 0u) | (st && st->available ? 2u : 0u) |
              (st && st->enabled ? 4u : 0u) | (st && st->filament_detected ? 8u : 0u) |
              (static_cast<uint32_t>(lane_index_for_sensor(sensor) + 1) << 4));
    }
    return static_cast<int>(h.value());
}

uint32_t FilamentSensorManager::config_signature() const {
    Fnv1a h;
    h.mix(master_enabled_ ? 1u : 0u);
    for (const auto& sensor : entries_locked()) {
        h.mix(sensor.klipper_name);
        h.mix(static_cast<uint32_t>(sensor.role) | (sensor.enabled ? 0x100u : 0u));
    }
    return h.value();
}

// ============================================================================
// State Updates
// ============================================================================

void FilamentSensorManager::update_from_status(const json& status) {
    // Suppress toast notifications for initial state at startup
    // (similar to USB manager - users don't need to be told filament is present)
    auto now = std::chrono::steady_clock::now();
    bool within_grace_period =
        (now - startup_time_) < app_constants::startup::kSensorStabilizationPeriod;

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
    /// Removals whose dwell expired with the sensor still clear. Kept apart from
    /// `notifications` because there is no sensor edge here to hand the
    /// state-change callback - only a toast that was owed and is now due.
    std::vector<FilamentSensorRole> dwell_expired_removals;
    StateChangeCallback callback_copy;
    bool any_changed = false;
    bool need_subject_update = false;

    // Read AMS state BEFORE taking the mutex. AmsState notifies this class from under
    // its own recursive lock (sync_from_backend -> on_bypass_active_changed), so
    // acquiring AmsState's lock from inside ours closes an ABBA cycle that TSan
    // reports as a potential deadlock. These are whole-printer advisory flags, not
    // per-sensor, and our lock never protected AmsState's state anyway.
    const bool ams_active = AmsState::instance().is_filament_operation_active();
    // Peeked, never consumed - the idle runout modal owns the one shot.
    const bool post_unload_grace = AmsState::instance().post_unload_runout_grace_armed();
    // The derived lifecycle, read once beside the other whole-printer flags for
    // the same reason (our lock never protected it): both phase-aware terms
    // below need it.
    const auto lifecycle = get_printer_state().print_state().get_print_lifecycle();
    const bool job_owns_machine = job_holds_machine(lifecycle);
    const AmsType backend_type = AmsState::instance().primary_type().value_or(AmsType::NONE);
    const BackendTraits backend_traits =
        AmsState::instance().primary_traits().value_or(BackendTraits{});
    // A backend whose firmware unloads filament back to its hub between prints
    // (AD5X IFS) makes the head sensor going empty while idle firmware behaviour,
    // not a user-facing event. "Between prints" is the lifecycle's Idle/terminal
    // side, not merely "not PRINTING": a head-empty during a pre-print block is
    // not the firmware's idle auto-unload and must not be swallowed.
    const bool idle_unload_expected =
        backend_traits.unloads_to_hub_between_prints && !job_owns_machine;
    // While a job holds the machine, a backend that raises its own runout fault
    // owns the runout surface for that phase: its prompt is already telling the
    // user what happened, with recovery actions derived from live hardware, so
    // the sensor-edge toast restates it (#1388). Ownership-scoped, never a
    // blanket muzzle: on a backend with no error hook of its own, and with no
    // backend at all, this toast is the only runout signal there is.
    // The backend declares that through raises_runout_fault_during_job.
    const bool runout_surface_owned_during_job =
        job_owns_machine && backend_traits.raises_runout_fault_during_job;
    // A tool change pulls filament off the toolhead sensor and feeds the next
    // lane past it. At the edge that is identical to a runout; only duration
    // separates them. Where a backend could be changing tools and a job holds
    // the machine, the removal toast serves a dwell instead of firing on the
    // edge, and filament returning inside the dwell cancels it. Backends that
    // already own the runout surface never reach here, and a printer with no
    // AMS has no tool change to confuse this with, so both keep the edge toast.
    const bool dwell_removal_toast = job_owns_machine && backend_type != AmsType::NONE;
    // How the last job ended, which outlives the lifecycle's trip back to Idle.
    // A print that finished on its own terms takes its end-of-print filament
    // handling with it; any other ending stopped with the sensor already empty.
    const auto print_outcome = static_cast<PrintOutcome>(
        lv_subject_get_int(get_printer_state().print_state().get_print_outcome_subject()));

    // A sensor whose edges are the user's business at all: the feature is on, and
    // this sensor is enabled with a filament role.
    const auto sensor_announces = [this](const SensorEntry& s) {
        return master_enabled_ && s.enabled && filament_role(s) != FilamentSensorRole::NONE;
    };

    // Phase 1: Update state under lock, collect notifications
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());

        // Copy callback for use outside lock
        callback_copy = state_change_callback_;

        if (!job_owns_machine && !observed_runouts_.empty()) {
            observed_runouts_.clear();
            any_changed = true;
        }

        // Process filament_switch_sensor updates
        for (const auto& sensor : entries_locked()) {
            // Build the Klipper object key (e.g., "filament_switch_sensor fsensor")
            std::string key = sensor.klipper_name;

            // Moonraker keys status by the full object name
            if (!status.contains(key)) {
                continue;
            }

            const auto& sensor_data = status[key];
            auto& state = readings_[sensor.klipper_name];
            FilamentSensorState old_state = state;
            state.reported = true;

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
                // A stand-down/stand-up with filament_detected unchanged is
                // still a subject-level change: the runout badge mutes or
                // unmutes with the firmware flag.
                if (state.enabled != old_state.enabled) {
                    any_changed = true;
                }
                // Honest armed-set bookkeeping: a sensor we armed for bypass
                // that the firmware now reports disabled was stood down by
                // someone else (vendor macros toggle this sensor around their
                // own operations), so we no longer own its state and a later
                // bypass disengage must not send a restore for it.
                if (!state.enabled) {
                    auto armed_it =
                        std::find(bypass_armed_.begin(), bypass_armed_.end(), sensor.klipper_name);
                    if (armed_it != bypass_armed_.end()) {
                        bypass_armed_.erase(armed_it);
                        spdlog::debug(
                            "[FilamentSensorManager] Bypass: {} reported disabled by firmware — "
                            "dropped from the armed set",
                            sensor.display_name);
                    }
                }
            }

            // Motion sensors have additional fields
            if (type_of(sensor) == FilamentSensorType::MOTION) {
                if (auto it = sensor_data.find("detection_count");
                    it != sensor_data.end() && it->is_number_integer()) {
                    state.detection_count = it->get<int>();
                }
            }

            // Check for state change
            if (state.filament_detected != old_state.filament_detected) {
                any_changed = true;
                if (state.filament_detected) {
                    observed_runouts_.erase(sensor.klipper_name);
                }

                // Log at WARN if this is a runout (filament gone) on an active sensor
                if (!state.filament_detected && filament_role(sensor) != FilamentSensorRole::NONE &&
                    sensor.enabled && state.enabled) {
                    spdlog::warn("[FilamentSensorManager] RUNOUT: {} ({}) filament gone",
                                 sensor.display_name, role_to_config_string(filament_role(sensor)));
                } else {
                    spdlog::debug("[FilamentSensorManager] Sensor {} state changed: {} -> {}",
                                  sensor.display_name,
                                  old_state.filament_detected ? "detected" : "empty",
                                  state.filament_detected ? "detected" : "empty");
                }

                // Queue notification for after lock release
                Notification notif;
                notif.klipper_name = sensor.klipper_name;
                notif.sensor_name = sensor.display_name;
                notif.old_state = old_state;
                notif.new_state = state;
                notif.role = filament_role(sensor);
                // A sensor the firmware stood down (or just stood up) does not
                // announce edges: the change happened outside its monitoring
                // window, so we did not observe it. Re-enabling a sensor that
                // reads empty IS a runout for the decision queries (Klipper
                // resumes acting on it), but the removal itself was not seen,
                // so no "filament removed" toast for it.
                const bool observed_while_monitoring = old_state.enabled && state.enabled;
                // Judged on the state going INTO the frame: a pause macro can
                // stand the sensor down inside the same status batch that
                // carries the runout.
                if (!state.filament_detected && old_state.reported && old_state.enabled &&
                    job_owns_machine && !ams_active && sensor.enabled &&
                    filament_role(sensor) != FilamentSensorRole::NONE) {
                    observed_runouts_.insert(sensor.klipper_name);
                }
                ToastSuppressionTerms terms;
                terms.startup_grace = within_grace_period;
                terms.ams_active = ams_active;
                terms.idle_unload_expected = idle_unload_expected;
                terms.runout_surface_owned = runout_surface_owned_during_job;
                terms.wizard_active = is_wizard_active();
                terms.post_unload_removal = !state.filament_detected && post_unload_grace;
                const char* const suppressed_by = suppression_reason(terms);
                const bool announces = sensor_announces(sensor);
                if (suppressed_by && announces) {
                    spdlog::debug("[FilamentSensorManager] Suppressing toast for {} - {} ({})",
                                  sensor.display_name, suppressed_by,
                                  ams_type_to_string(backend_type));
                }
                notif.should_toast = !suppressed_by && announces && observed_while_monitoring;
                // A removal that owes a dwell is recorded, not announced. A
                // refill inside the dwell drops the record and silences the
                // insertion with it: nothing was said, so nothing needs saying.
                // An edge already suppressed above records nothing - it is not
                // owed a toast to defer.
                if (notif.should_toast && !state.filament_detected && dwell_removal_toast) {
                    pending_removal_toast_[sensor.klipper_name] = now;
                    notif.should_toast = false;
                    spdlog::debug("[FilamentSensorManager] Holding removal toast for {} - {}s "
                                  "dwell, a tool change refills before it expires",
                                  sensor.display_name, kRunoutToastDwell.count());
                } else if (state.filament_detected &&
                           pending_removal_toast_.erase(sensor.klipper_name) > 0) {
                    notif.should_toast = false;
                    spdlog::debug(
                        "[FilamentSensorManager] {} refilled inside the dwell - not a runout",
                        sensor.display_name);
                }
                notifications.push_back(notif);
            }

            // The pause macro stands sensors down only once the job reads
            // Paused (PrinterState applies print_stats before this runs). A
            // stand-down while the job runs on means the firmware moved the
            // print to another sensor, or the user took a misreading one off
            // duty: either way, its runout is no longer this job's.
            if (old_state.enabled && !state.enabled && lifecycle != PrintState::Paused) {
                observed_runouts_.erase(sensor.klipper_name);
            }
        }

        // Dwell expiry. Swept on every status payload rather than on a timer, so
        // a removal that outlasts its dwell is still announced when the sensor
        // itself has gone quiet - during a print Moonraker pushes status far
        // faster than the dwell, so the toast lands within a tick of coming due.
        for (auto it = pending_removal_toast_.begin(); it != pending_removal_toast_.end();) {
            auto* state_it = state(it->first);
            // Refilled, or gone from the sensor set: nothing left to confirm.
            if (!state_it || state_it->filament_detected) {
                it = pending_removal_toast_.erase(it);
                continue;
            }
            // The job ended. One that COMPLETED did so on its own terms, and the
            // end-of-print unload some backends run afterwards belongs to the
            // idle-side suppressions. Any other ending stopped with this sensor
            // already empty, which is the runout the dwell was opened to
            // confirm - and where the backend raises no fault of its own, this
            // toast is the only account the user gets. A runout_gcode that
            // cancels the print lands here.
            if (!job_owns_machine && print_outcome == PrintOutcome::COMPLETE) {
                it = pending_removal_toast_.erase(it);
                continue;
            }
            if (now - it->second < kRunoutToastDwell) {
                ++it;
                continue;
            }
            // Something is already accounting for the empty sensor by the time
            // the dwell is up - an AMS operation moving filament past it, or the
            // wizard walking the user through one. The same terms silence the
            // edge, and a removal the user is performing needs no warning.
            if (ams_active || is_wizard_active()) {
                it = pending_removal_toast_.erase(it);
                continue;
            }
            auto cfg =
                std::find_if(entries_locked().begin(), entries_locked().end(),
                             [&](const SensorEntry& s) { return s.klipper_name == it->first; });
            if (state_it && !state_it->enabled) {
                // Stood down mid-dwell: the sensor stopped monitoring, so the
                // removal it was confirming is no longer ours to announce.
                spdlog::debug("[FilamentSensorManager] Dropping dwell toast for {} - firmware "
                              "stood the sensor down",
                              it->first);
            } else if (cfg != entries_locked().end() && sensor_announces(*cfg)) {
                dwell_expired_removals.push_back(filament_role(*cfg));
                spdlog::warn("[FilamentSensorManager] RUNOUT CONFIRMED: {} ({}) still clear "
                             "after {}s",
                             cfg->display_name, role_to_config_string(filament_role(*cfg)),
                             kRunoutToastDwell.count());
            }
            it = pending_removal_toast_.erase(it);
        }

        // Always update subjects on first status (initial_status_received_ handles this)
        // and on any state change. Without this, subjects stay at -1 ("no sensor")
        // when the initial Moonraker status matches the default state (filament_detected=true).
        need_subject_update = any_changed || !initial_status_received_;
        initial_status_received_ = true;
    }
    // Lock released here

    if (need_subject_update) {
        publish();
    }

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

    for (FilamentSensorRole role : dwell_expired_removals) {
        NOTIFY_WARNING("{}: Filament removed", role_to_display_string(role));
    }
}

void FilamentSensorManager::set_state_change_callback(StateChangeCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    state_change_callback_ = std::move(callback);
}

// ============================================================================
// LVGL Subjects
// ============================================================================

bool FilamentSensorManager::parse_status(const nlohmann::json& /*status*/) {
    return false;
}

void FilamentSensorManager::register_subjects(SubjectManager& subjects) {
    // Role-state encoding (filament_runout_detected, filament_toolhead_detected,
    // filament_entry_detected):
    //   -1 = no sensor configured for this role (hide indicator entirely)
    //    0 = sensor enabled, no filament (empty/red)
    //    1 = sensor enabled, filament present (loaded/green)
    //    2 = sensor configured but DISABLED (master toggle off or per-sensor
    //        enabled=false) — render as "off/unknown" so the user can see
    //        runout protection is inactive instead of mistaking a hidden
    //        indicator for "everything is fine".
    UI_MANAGED_SUBJECT_INT(runout_detected_, -1, "filament_runout_detected", subjects);
    // Print-scoped runout: same encoding as filament_runout_detected but
    // considers only the active print's used tools (lane truth). Driven by
    // PrintStatusPanel via set_scoped_runout(); the in-print badge binds this.
    UI_MANAGED_SUBJECT_INT(scoped_runout_, -1, "filament_runout_scoped", subjects);
    UI_MANAGED_SUBJECT_INT(toolhead_detected_, -1, "filament_toolhead_detected", subjects);
    UI_MANAGED_SUBJECT_INT(entry_detected_, -1, "filament_entry_detected", subjects);
    UI_MANAGED_SUBJECT_INT(any_runout_, 0, "filament_any_runout", subjects);
    UI_MANAGED_SUBJECT_INT(motion_signature_, 0, "filament_motion_signature", subjects);
    UI_MANAGED_SUBJECT_INT(master_enabled_subject_, is_master_enabled() ? 1 : 0,
                           "filament_master_enabled", subjects);
    UI_MANAGED_SUBJECT_INT(config_revision_, 0, "filament_sensor_config_revision", subjects);
    published_config_signature_.reset();
}

void FilamentSensorManager::refresh_subjects() {
    // Read everything under the lock, set subjects after releasing it: an
    // observer may call back into this manager from another thread's lock
    // order (AmsState's clog meter reads clog_motion_sensor()).
    int runout = -1;
    int toolhead = -1;
    int entry = -1;
    int any_runout_value = 0;
    int signature = 0;
    bool master = true;
    uint32_t config = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());

        // Helper to get subject value for a role. See register_subjects() for
        // the -1/0/1/2 encoding rationale.
        auto get_role_value = [this](FilamentSensorRole role) -> int {
            // The monitoring pick skips sensors the firmware stood down: with
            // several sensors sharing a role (multi-head hardware), a stood-down
            // parked head must not speak for the role while the running head
            // reports filament.
            const auto* config = find_monitoring_config_by_role(role);
            if (!config) {
                return -1; // No sensor configured for this role — hide
            }

            // A configured sensor that is turned off (master toggle or per-sensor)
            // surfaces as "disabled" so the user sees runout protection is OFF
            // rather than seeing a hidden indicator and assuming all is well.
            if (!master_enabled_ || !config->enabled) {
                return 2; // Configured but disabled
            }

            auto* st = state(config->klipper_name);
            if (!st || !st->available) {
                return -1; // Sensor transiently unavailable — hide (not the same
                           // as user-disabled; treat as no-opinion)
            }

            // When every holder is stood down the fallback holder's live reading
            // shows: the subject is the sensor tile's display, and a firmware
            // stand-down is not a user-visible "protection off" state. Runout
            // decisions do not read this value; they ask monitors_runout().
            return st->filament_detected ? 1 : 0;
        };

        runout = get_role_value(FilamentSensorRole::RUNOUT);
        toolhead = get_role_value(FilamentSensorRole::TOOLHEAD);
        entry = get_role_value(FilamentSensorRole::ENTRY);

        // Suppress any_runout during startup grace period to avoid false modal triggers
        // (Moonraker may report sensors as "empty" before Klipper fully initializes)
        const bool in_grace = is_in_startup_grace_period();
        const bool runout_now = has_any_runout();
        any_runout_value = (in_grace || !runout_now) ? 0 : 1;
        if (in_grace && runout_now) {
            spdlog::info(
                "[FilamentSensorManager] Suppressing runout modal during startup grace period");
        }
        signature = motion_signature();
        master = master_enabled_;
        config = config_signature();
    }

    // The revision ticks before the readings move, so an observer of both reads
    // a reading that moves with it as configuration, not a physical event.
    if (published_config_signature_ && *published_config_signature_ != config) {
        lv_subject_set_int(&config_revision_, lv_subject_get_int(&config_revision_) + 1);
    }
    published_config_signature_ = config;

    lv_subject_set_int(&master_enabled_subject_, master ? 1 : 0);
    lv_subject_set_int(&runout_detected_, runout);
    lv_subject_set_int(&toolhead_detected_, toolhead);
    lv_subject_set_int(&entry_detected_, entry);
    lv_subject_set_int(&any_runout_, any_runout_value);
    lv_subject_set_int(&motion_signature_, signature);

    spdlog::trace("[FilamentSensorManager] Subjects updated: runout={}, toolhead={}, entry={}, "
                  "any_runout={}",
                  runout, toolhead, entry, any_runout_value);
}

lv_subject_t* FilamentSensorManager::get_runout_detected_subject() {
    return &runout_detected_;
}

void FilamentSensorManager::set_scoped_runout(int value) {
    if (!subjects_initialized()) {
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

lv_subject_t* FilamentSensorManager::get_motion_signature_subject() {
    return &motion_signature_;
}

lv_subject_t* FilamentSensorManager::get_config_revision_subject() {
    return &config_revision_;
}

bool FilamentSensorManager::is_in_startup_grace_period() const {
    auto now = std::chrono::steady_clock::now();
    return (now - startup_time_) < app_constants::startup::kSensorStabilizationPeriod;
}

// ============================================================================
// Private Helpers
// ============================================================================

int FilamentSensorManager::lane_index_for_sensor(const SensorEntry& sensor) const {
    if (const auto lane = lanes_.find(sensor.klipper_name);
        lane != lanes_.end() && lane->second >= 0) {
        return lane->second;
    }
    const std::string& sensor_name = sensor.display_name;
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

FilamentSensorState* FilamentSensorManager::state(const std::string& klipper_name) {
    const auto it = readings_.find(klipper_name);
    return it == readings_.end() ? nullptr : &it->second;
}

const FilamentSensorState* FilamentSensorManager::state(const std::string& klipper_name) const {
    const auto it = readings_.find(klipper_name);
    return it == readings_.end() ? nullptr : &it->second;
}

bool FilamentSensorManager::monitors_runout(const SensorEntry& sensor) const {
    if (!sensor.enabled || filament_role(sensor) == FilamentSensorRole::NONE) {
        return false;
    }
    auto* st = state(sensor.klipper_name);
    return st && (st->enabled || observed_runouts_.count(sensor.klipper_name) > 0);
}

const SensorEntry*
FilamentSensorManager::find_monitoring_config_by_role(FilamentSensorRole role) const {
    const SensorEntry* fallback = nullptr;
    for (const auto& sensor : entries_locked()) {
        if (filament_role(sensor) != role) {
            continue;
        }
        if (!fallback) {
            fallback = &sensor;
        }
        auto* st = state(sensor.klipper_name);
        if (st && st->enabled) {
            return &sensor;
        }
    }
    return fallback;
}

} // namespace helix
