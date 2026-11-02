// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware_validator.h"

#include "ui_nav.h"
#include "ui_panel_settings.h"
#include "ui_toast_manager.h"

#include "config.h"
#include "filament_sensor_manager.h"
#include "hardware_object_table.h"
#include "hardware_role_registry.h"
#include "i18n_plural.h"
#include "json_utils.h"
#include "lvgl.h"
#include "printer_discovery.h"
#include "printer_hardware.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
#include "text_io.h"
#include "translation_loader.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <chrono>
#include <ctime>

using namespace helix;

namespace {

/// Read a string member of @p obj without throwing, returning "" when the key is
/// absent OR holds a non-string.
///
/// json::value(key, default) is NOT a safe probe: it throws type_error.302 the
/// moment the key exists with a different type, and a hand-edited or
/// half-written settings.json ("name": 3, "name": null) hits exactly that.
/// validate_configured_hardware() runs inside the discovery-complete callback
/// (application.cpp) with no enclosing try/catch, so a throw there escapes an
/// LVGL/queue frame. Type-check instead of blanket-catching, so genuine
/// programming errors still surface.
std::string json_string_member(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/// Whether a configured strip id is something printer.objects can report.
/// A configured strip list spans every LED backend: macro devices are synthetic
/// "macro:NAME" ids and WLED strips are served over Moonraker's HTTP proxy, so
/// neither ever appears in a Klipper object list. Only a Klipper-object strip can
/// be judged present or absent against discovery.
bool led_strip_is_klipper_object(const std::string& strip_id) {
    if (strip_id.find(':') != std::string::npos) {
        return false;
    }
    return strip_id.rfind("wled ", 0) != 0;
}

/// The configured LED strips, in precedence order: leds/auto_state/strips, then
/// the legacy selection keys an older config may still hold (leds/selected_strips,
/// leds/selected array, leds/strip single string). Empty when no LED is
/// configured. Both validators ask this one question through here — a configured
/// LED must read as configured whichever key it was saved under.
std::vector<std::string> configured_led_strips(Config* config) {
    std::vector<std::string> strips;
    if (config == nullptr) {
        return strips;
    }
    auto prune_empty = [](std::vector<std::string>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [](const std::string& n) { return n.empty(); }),
                v.end());
    };
    strips = config->get_string_array(config->df() + helix::wizard::kLedAutoStateStrips);
    prune_empty(strips);
    if (strips.empty()) {
        strips = config->get_string_array(config->df() + helix::wizard::kLedSelectedStrips);
        prune_empty(strips);
    }
    if (strips.empty()) {
        strips = config->get_string_array(config->df() + helix::wizard::LED_SELECTED);
        prune_empty(strips);
    }
    if (strips.empty()) {
        const json* led_strip = config->try_get_json(config->df() + helix::wizard::LED_STRIP);
        if (led_strip != nullptr && led_strip->is_string() &&
            !led_strip->get<std::string>().empty()) {
            strips.push_back(led_strip->get<std::string>());
        }
    }
    return strips;
}

} // namespace

// =============================================================================
// HardwareSnapshot Implementation
// =============================================================================

json HardwareSnapshot::to_json() const {
    return json{{"timestamp", timestamp}, {"heaters", heaters},
                {"sensors", sensors},     {"fans", fans},
                {"leds", leds},           {"filament_sensors", filament_sensors}};
}

HardwareSnapshot HardwareSnapshot::from_json(const json& j) {
    HardwareSnapshot snapshot;

    if (j.contains("timestamp") && j["timestamp"].is_string()) {
        snapshot.timestamp = j["timestamp"].get<std::string>();
    }

    // An absent or non-array field leaves that list empty; an array holding a
    // non-string element discards the whole snapshot, so the caller never acts
    // on a half-populated one.
    const auto read_string_array = [&j](const char* key, std::vector<std::string>& out) -> bool {
        const json* v = helix::json_util::find_member(j, key);
        if (v == nullptr || !v->is_array()) {
            return true;
        }
        for (const auto& elem : *v) {
            if (!elem.is_string()) {
                return false;
            }
        }
        out = v->get<std::vector<std::string>>();
        return true;
    };

    if (!read_string_array("heaters", snapshot.heaters) ||
        !read_string_array("sensors", snapshot.sensors) ||
        !read_string_array("fans", snapshot.fans) || !read_string_array("leds", snapshot.leds) ||
        !read_string_array("filament_sensors", snapshot.filament_sensors)) {
        spdlog::warn("[HardwareValidator] Snapshot field unreadable; discarding snapshot");
        return HardwareSnapshot{};
    }

    // A stored list can name an object discovery does not count as a heater;
    // kept, it would read as removed hardware on the next comparison.
    snapshot.heaters.erase(std::remove_if(snapshot.heaters.begin(), snapshot.heaters.end(),
                                          [](const std::string& name) {
                                              return !helix::hw::is_heater(
                                                  helix::hw::classify_object(name).kind);
                                          }),
                           snapshot.heaters.end());

    return snapshot;
}

std::vector<std::string> HardwareSnapshot::get_removed(const HardwareSnapshot& current) const {
    std::vector<std::string> removed;

    // Helper to find items in 'old' but not in 'current'
    auto find_removed = [&removed](const std::vector<std::string>& old_list,
                                   const std::vector<std::string>& current_list) {
        for (const auto& item : old_list) {
            auto it = std::find(current_list.begin(), current_list.end(), item);
            if (it == current_list.end()) {
                removed.push_back(item);
            }
        }
    };

    find_removed(heaters, current.heaters);
    find_removed(sensors, current.sensors);
    find_removed(fans, current.fans);
    find_removed(leds, current.leds);
    find_removed(filament_sensors, current.filament_sensors);

    return removed;
}

std::vector<std::string> HardwareSnapshot::get_added(const HardwareSnapshot& current) const {
    std::vector<std::string> added;

    // Helper to find items in 'current' but not in 'old'
    auto find_added = [&added](const std::vector<std::string>& old_list,
                               const std::vector<std::string>& current_list) {
        for (const auto& item : current_list) {
            auto it = std::find(old_list.begin(), old_list.end(), item);
            if (it == old_list.end()) {
                added.push_back(item);
            }
        }
    };

    find_added(heaters, current.heaters);
    find_added(sensors, current.sensors);
    find_added(fans, current.fans);
    find_added(leds, current.leds);
    find_added(filament_sensors, current.filament_sensors);

    return added;
}

// =============================================================================
// HardwareValidator Implementation
// =============================================================================

namespace {

// Validation judges what discovery found. A backend can report its hardware
// absent only after its first status frame, which comes before a reconnect's
// validation but after a cold boot's, so the claim is taken as discovered.
constexpr bool kFilamentHardwareAbsent = false;

} // namespace

HardwareValidationResult HardwareValidator::validate(Config* config,
                                                     const helix::PrinterDiscovery& hardware) {
    HardwareValidationResult result;

    spdlog::debug("[HardwareValidator] Starting hardware validation...");

    // Surface user-silenced hardware so debug bundles capture what's been
    // ignored. Today's ignore list has no timestamp/version metadata, so this
    // log is the only visible signal that warnings are being suppressed.
    log_ignored_hardware(config);

    // Step 1: Check critical hardware exists
    validate_critical_hardware(hardware, result);

    // Step 2: Check configured hardware exists
    validate_configured_hardware(config, hardware, result);

    // Step 3: Find newly discovered hardware not in config
    validate_new_hardware(config, hardware, result);

    // Step 4: Compare against previous session
    auto previous_snapshot = load_session_snapshot(config);
    if (previous_snapshot) {
        auto current_snapshot = create_snapshot(hardware);
        validate_session_changes(*previous_snapshot, current_snapshot, config, result);
    }

    // Log summary
    if (result.has_issues()) {
        spdlog::info("[HardwareValidator] Validation complete: {} critical, {} expected missing, "
                     "{} new, {} changed",
                     result.critical_missing.size(), result.expected_missing.size(),
                     result.newly_discovered.size(), result.changed_from_last_session.size());
    } else {
        spdlog::debug("[HardwareValidator] Validation complete: no issues found");
    }

    return result;
}

// Static callback for toast action button - navigates to Settings and opens overlay
static void on_hardware_toast_view_clicked(void* /*user_data*/) {
    spdlog::debug("[HardwareValidator] Toast 'View' clicked - opening Hardware Health overlay");
    helix::nav::set_active(PanelId::Settings);
    get_global_settings_panel().handle_hardware_health_clicked();
}

void HardwareValidator::notify_user(const HardwareValidationResult& result) {
    if (!result.has_issues()) {
        return;
    }

    std::string message;
    ToastSeverity severity = ToastSeverity::INFO;

    if (result.has_critical()) {
        if (result.critical_missing.size() == 1) {
            message = fmt::format(lv_tr("Critical hardware missing: {}"),
                                  result.critical_missing[0].hardware_name);
        } else {
            const size_t critical = result.critical_missing.size();
            message = fmt::format(fmt::runtime(helix::ui::tr_plural(
                                      nullptr, "{} critical hardware issues", critical)),
                                  critical);
        }
        severity = ToastSeverity::ERROR;
    } else if (!result.expected_missing.empty() || !result.changed_from_last_session.empty()) {
        size_t count = result.expected_missing.size() + result.changed_from_last_session.size();
        message =
            fmt::format(fmt::runtime(helix::ui::tr_plural("{} configured item not found",
                                                          "{} configured items not found", count)),
                        count);
        severity = ToastSeverity::WARNING;
    } else {
        // Build intelligent message based on hardware types
        size_t led_count = 0, sensor_count = 0, other_count = 0;
        for (const auto& issue : result.newly_discovered) {
            if (issue.hardware_type == HardwareType::LED) {
                led_count++;
            } else if (issue.hardware_type == HardwareType::FILAMENT_SENSOR) {
                sensor_count++;
            } else {
                other_count++;
            }
        }

        if (led_count > 0 && sensor_count == 0 && other_count == 0) {
            message = fmt::format(
                fmt::runtime(helix::ui::tr_plural("LED strip available for lighting control",
                                                  "{} LED strips available", led_count)),
                led_count);
        } else if (sensor_count > 0 && led_count == 0 && other_count == 0) {
            message = fmt::format(
                fmt::runtime(helix::ui::tr_plural("Filament sensor available for runout detection",
                                                  "{} filament sensors available", sensor_count)),
                sensor_count);
        } else {
            const size_t discovered = result.newly_discovered.size();
            message = fmt::format(fmt::runtime(helix::ui::tr_plural(
                                      nullptr, "{} new hardware available", discovered)),
                                  discovered);
        }
        severity = ToastSeverity::INFO;
    }

    // Show toast with action button to navigate to Hardware Health section
    ToastManager::instance().show_with_action(severity, message.c_str(), lv_tr("View"),
                                              on_hardware_toast_view_clicked, nullptr, 8000);
    spdlog::debug("[HardwareValidator] Notified user ({}): {}",
                  severity == ToastSeverity::ERROR     ? "error"
                  : severity == ToastSeverity::WARNING ? "warning"
                                                       : "info",
                  message);
}

void HardwareValidator::save_session_snapshot(Config* config,
                                              const helix::PrinterDiscovery& hardware) {
    if (!config) {
        return;
    }

    // Create current snapshot using the hardware discovery
    auto snapshot = create_snapshot(hardware);

    // Generate ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&time_t_now, &tm_buf);
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    snapshot.timestamp = ts_buf;

    // Save to config
    config->set<json>(config->df() + "hardware/last_snapshot", snapshot.to_json());
    config->save();
    spdlog::debug("[HardwareValidator] Saved session snapshot with {} heaters, {} fans, {} leds",
                  snapshot.heaters.size(), snapshot.fans.size(), snapshot.leds.size());
}

HardwareSnapshot HardwareValidator::create_snapshot(const helix::PrinterDiscovery& hardware) {
    HardwareSnapshot snapshot;

    snapshot.heaters = hardware.heaters();
    snapshot.sensors = hardware.sensors();
    snapshot.fans = hardware.fans();
    snapshot.leds = hardware.leds();
    snapshot.filament_sensors = hardware.filament_sensor_names();

    return snapshot;
}

std::optional<HardwareSnapshot> HardwareValidator::load_session_snapshot(Config* config) {
    if (!config) {
        return std::nullopt;
    }

    const json* snapshot_json = config->try_get_json(config->df() + "hardware/last_snapshot");
    if (snapshot_json == nullptr || snapshot_json->is_null() || snapshot_json->empty()) {
        spdlog::debug("[HardwareValidator] No previous session snapshot found");
        return std::nullopt;
    }

    auto snapshot = HardwareSnapshot::from_json(*snapshot_json);
    if (snapshot.is_empty()) {
        return std::nullopt;
    }

    spdlog::debug("[HardwareValidator] Loaded previous snapshot from {}", snapshot.timestamp);
    return snapshot;
}

bool HardwareValidator::is_hardware_optional(Config* config, const std::string& hardware_name) {
    if (!config) {
        return false;
    }

    // Read-only probe — get_string_array() never creates the node (#1129).
    const auto optional_list = config->get_string_array(config->df() + "hardware/optional");
    return std::find(optional_list.begin(), optional_list.end(), hardware_name) !=
           optional_list.end();
}

void HardwareValidator::set_hardware_optional(Config* config, const std::string& hardware_name,
                                              bool optional) {
    if (!config) {
        return;
    }

    // Ensure the hardware/optional array exists
    json& optional_list = config->get_json(config->df() + "hardware/optional");
    if (optional_list.is_null() || !optional_list.is_array()) {
        optional_list = json::array();
    }

    // Find if already in list
    auto it = std::find(optional_list.begin(), optional_list.end(), hardware_name);
    bool in_list = (it != optional_list.end());

    if (optional && !in_list) {
        // Add to list
        optional_list.push_back(hardware_name);
        spdlog::info("[HardwareValidator] Marked '{}' as optional", hardware_name);
    } else if (!optional && in_list) {
        // Remove from list
        optional_list.erase(it);
        spdlog::info("[HardwareValidator] Unmarked '{}' as optional", hardware_name);
    }

    config->save();
}

void HardwareValidator::add_expected_hardware(Config* config, const std::string& hardware_name) {
    if (!config || hardware_name.empty()) {
        return;
    }

    // Ensure the hardware/expected array exists
    json& expected_list = config->get_json(config->df() + "hardware/expected");
    if (expected_list.is_null() || !expected_list.is_array()) {
        expected_list = json::array();
    }

    // Check if already in list
    auto it = std::find(expected_list.begin(), expected_list.end(), hardware_name);
    if (it == expected_list.end()) {
        expected_list.push_back(hardware_name);
        spdlog::info("[HardwareValidator] Added '{}' to expected hardware", hardware_name);
        config->save();
    } else {
        spdlog::debug("[HardwareValidator] '{}' already in expected list", hardware_name);
    }
}

size_t HardwareValidator::acknowledge_discovered_hardware(Config* config,
                                                          const helix::PrinterDiscovery& hardware) {
    if (!config) {
        return 0;
    }

    // Exactly the categories validate_new_hardware() reports on. AMS-managed
    // sensors are excluded there too, so recording them would be dead weight.
    std::vector<std::string> names;
    for (const auto& fan : hardware.fans()) {
        names.push_back(fan);
    }
    for (const auto& led : hardware.leds()) {
        names.push_back(led);
    }
    for (const auto& sensor : hardware.filament_sensor_names()) {
        if (PrinterHardware::is_ams_sensor(sensor, hardware, kFilamentHardwareAbsent)) {
            continue;
        }
        names.push_back(sensor);
    }

    json& expected_list = config->get_json(config->df() + "hardware/expected");
    if (expected_list.is_null() || !expected_list.is_array()) {
        expected_list = json::array();
    }

    size_t added = 0;
    for (const auto& name : names) {
        if (name.empty()) {
            continue;
        }
        if (std::find(expected_list.begin(), expected_list.end(), name) != expected_list.end()) {
            continue;
        }
        expected_list.push_back(name);
        ++added;
    }

    if (added > 0) {
        spdlog::info("[HardwareValidator] Accepted {} discovered object(s) as expected", added);
        if (!config->save()) {
            spdlog::warn("[HardwareValidator] Failed to save accepted hardware");
        }
    }
    return added;
}

// =============================================================================
// Private Validation Helpers
// =============================================================================

static HardwareType hardware_type_for(helix::HardwareCategory cat) {
    switch (cat) {
    case helix::HardwareCategory::Fan:
        return HardwareType::FAN;
    case helix::HardwareCategory::Heater:
        return HardwareType::HEATER;
    case helix::HardwareCategory::Led:
        return HardwareType::LED;
    case helix::HardwareCategory::FilamentSensor:
        return HardwareType::FILAMENT_SENSOR;
    default:
        return HardwareType::OTHER;
    }
}

// =============================================================================
// Private Validation Methods
// =============================================================================

void HardwareValidator::validate_critical_hardware(const helix::PrinterDiscovery& hardware,
                                                   HardwareValidationResult& result) {
    const auto& heaters = hardware.heaters();

    // Check for extruder
    bool has_extruder = false;
    for (const auto& h : heaters) {
        if (h.find("extruder") != std::string::npos) {
            has_extruder = true;
            break;
        }
    }
    if (!has_extruder) {
        result.critical_missing.push_back(HardwareIssue::critical(
            "extruder", HardwareType::HEATER,
            TR_NOOP("No extruder heater found. Check [extruder] in printer.cfg")));
    }

    // Check for heater_bed (note: not all printers have heated beds)
    bool has_bed = contains_name(heaters, "heater_bed");
    if (!has_bed) {
        // This is a warning, not critical - some printers don't have heated beds
        spdlog::debug("[HardwareValidator] No heater_bed found (may be intentional)");
    }
}

void HardwareValidator::validate_configured_hardware(Config* config,
                                                     const helix::PrinterDiscovery& hardware,
                                                     HardwareValidationResult& result) {
    if (!config) {
        return;
    }

    const auto& fans = hardware.fans();
    const auto& leds = hardware.leds();
    const auto& filament_sensors = hardware.filament_sensor_names();

    // Role-bearing heater/fan validation via registry.
    // Bypasses is_hardware_optional for role targets: a configured role pointing at
    // an optional object is a stale role, not a silent drop.
    for (const auto& desc : helix::hardware_role_registry()) {
        const std::vector<std::string>* discovered = nullptr;
        if (desc.category == helix::HardwareCategory::Fan)
            discovered = &hardware.fans();
        else if (desc.category == helix::HardwareCategory::Heater)
            discovered = &hardware.heaters();
        if (!discovered)
            continue;

        const std::string key = config->df() + desc.config_key;
        // Read with an EMPTY default (NOT the canonical default): an absent key
        // means this role is not configured for THIS printer (a bed-less printer
        // has no heaters/bed key). Absent is legitimate, not a problem to flag.
        std::string saved = config->get<std::string>(key, "");
        if (saved.empty())
            continue; // unconfigured role

        auto res = helix::resolve_role(desc, saved, *discovered);
        // Confident heals are resolved+persisted upstream (FanRoleConfig::from_config and the
        // heater heal block in the discovery sequence) BEFORE validate() runs, so an AutoHealed
        // role is already Resolved here. We surface ONLY Unresolved NON-GUIDED roles as a
        // warning. Guided roles (every current registry role) are routed to the targeted
        // reconfig wizard via helix::unresolved_guided_steps()/the collector — toasting them
        // here too would double-notify (spec §3.4: guided → reconfig only; non-guided →
        // warning). After this change the validator surfaces none of the current registry
        // roles by design; the collector + wizard are the authority. Do not re-add an
        // AutoHealed branch here without moving the upstream pre-heal, or you reintroduce an
        // every-boot toast.
        if (res.status == helix::RoleResolutionStatus::Unresolved && !desc.guided) {
            result.expected_missing.push_back(
                HardwareIssue::warning(saved, hardware_type_for(desc.category),
                                       TR_NOOP("Configured hardware no longer present"),
                                       /*optional=*/false));
        }
    }

    // Check configured fan (aux). AuxFan is a GUIDED registry role, so the loop above
    // routes a stale key to the reconfig wizard instead of warning here; this check is
    // what keeps a missing aux fan visible as a hardware issue. Some presets (e.g.
    // AD5M Pro ForgeX) map a fifth fan role; without it the fan would silently
    // disappear.
    std::string aux_fan = config->get<std::string>(config->df() + "fans/aux", "");
    if (!aux_fan.empty() && !contains_name(fans, aux_fan) &&
        !is_hardware_optional(config, aux_fan)) {
        result.expected_missing.push_back(HardwareIssue::warning(
            aux_fan, HardwareType::FAN, TR_NOOP("Configured aux fan not found")));
    }

    // Check configured LEDs. Synthetic strips are skipped: discovery cannot see them,
    // so their absence from it is not evidence of anything.
    for (const auto& led_name : configured_led_strips(config)) {
        if (!led_strip_is_klipper_object(led_name)) {
            continue;
        }
        if (!contains_name(leds, led_name) && !is_hardware_optional(config, led_name)) {
            result.expected_missing.push_back(HardwareIssue::warning(
                led_name, HardwareType::LED, TR_NOOP("Configured LED strip not found")));
        }
    }

    // Check configured filament sensors
    for (const auto& sensor_name : FilamentSensorManager::instance().saved_sensor_names()) {
        if (!contains_name(filament_sensors, sensor_name) &&
            !is_hardware_optional(config, sensor_name)) {
            result.expected_missing.push_back(
                HardwareIssue::warning(sensor_name, HardwareType::FILAMENT_SENSOR,
                                       TR_NOOP("Configured filament sensor not found")));
        }
    }

    // Check expected hardware array (includes AMS and other generic hardware)
    // Items are added by wizard completion
    {
        for (const auto& hw_name : config->get_string_array(config->df() + "hardware/expected")) {
            {
                if (hw_name.empty())
                    continue;

                // A filament system's entry must find that system detected.
                const AmsType ams_type = ams_type_for_expected_hardware(hw_name);
                if (ams_type != AmsType::NONE) {
                    if (!hardware.has_filament_system(ams_type) &&
                        !is_hardware_optional(config, hw_name)) {
                        result.expected_missing.push_back(HardwareIssue::warning(
                            hw_name, HardwareType::OTHER, TR_NOOP("AMS/MMU system not detected")));
                        spdlog::debug("[HardwareValidator] Expected AMS hardware '{}' not found",
                                      hw_name);
                    }
                }
                // Non-AMS hardware is already checked above via specific config paths
            }
        }
    }
}

void HardwareValidator::validate_new_hardware(Config* config,
                                              const helix::PrinterDiscovery& hardware,
                                              HardwareValidationResult& result) {
    // Load hardware/expected list — items the user has already acknowledged via Save
    std::vector<std::string> expected_hardware;
    if (config) {
        expected_hardware = config->get_string_array(config->df() + "hardware/expected");
    }

    const auto& leds = hardware.leds();

    // Check for LEDs not in config
    // Only suggest if user hasn't configured any LED yet
    const bool has_configured_led = !configured_led_strips(config).empty();

    if (!has_configured_led && !leds.empty()) {
        // User has no LED configured but printer has some
        // Suggest the first "main" LED (prefer ones with "chamber", "case", "light" in name)
        std::string suggested;
        for (const auto& led : leds) {
            std::string lower = led;
            lower = helix::text_io::to_lower(lower);
            if (lower.find("chamber") != std::string::npos ||
                lower.find("case") != std::string::npos ||
                lower.find("light") != std::string::npos) {
                suggested = led;
                break;
            }
        }
        if (suggested.empty() && !leds.empty()) {
            suggested = leds[0];
        }

        if (!suggested.empty() && !contains_name(expected_hardware, suggested)) {
            result.newly_discovered.push_back(HardwareIssue::info(
                suggested, HardwareType::LED,
                TR_NOOP("LED strip available. Add to config for lighting control?")));
        }
    }

    // Check for fans not assigned to any role
    const auto& discovered_fans = hardware.fans();
    std::vector<std::string> configured_fans;
    // "fan" is always the default part cooling fan in Klipper
    configured_fans.push_back("fan");
    if (config) {
        // Collect all fans assigned to roles
        auto add_fan = [&](const std::string& key, const std::string& default_val) {
            std::string name = config->get<std::string>(config->df() + "fans/" + key, default_val);
            if (!name.empty()) {
                configured_fans.push_back(name);
            }
        };
        add_fan("part", "fan");
        add_fan("hotend", "");
        add_fan("chamber", "");
        add_fan("exhaust", "");
        add_fan("aux", "");
    }

    for (const auto& fan : discovered_fans) {
        if (!contains_name(configured_fans, fan) && !contains_name(expected_hardware, fan)) {
            result.newly_discovered.push_back(HardwareIssue::info(
                fan, HardwareType::FAN, TR_NOOP("Fan available but not assigned to any role")));
        }
    }

    // Check for filament sensors not in config
    const auto& discovered_sensors = hardware.filament_sensor_names();
    const std::vector<std::string> configured_names =
        FilamentSensorManager::instance().saved_sensor_names();

    // Find sensors in discovery but not in config
    for (const auto& sensor : discovered_sensors) {
        // Skip AMS/AFC sensors - they're managed by multi-material systems.
        // Discovery-aware: when HH or AFC is detected, also suppress the
        // backend's conventionally-named sensors (extruder/toolhead/
        // tool_start/<lane>_prep/...) so a fresh install doesn't toast.
        if (PrinterHardware::is_ams_sensor(sensor, hardware, kFilamentHardwareAbsent)) {
            spdlog::debug("[HardwareValidator] Skipping AMS sensor: {}", sensor);
            continue;
        }
        if (!contains_name(configured_names, sensor) && !contains_name(expected_hardware, sensor)) {
            result.newly_discovered.push_back(HardwareIssue::info(
                sensor, HardwareType::FILAMENT_SENSOR,
                TR_NOOP("Filament sensor available. Add to config for runout detection?")));
        }
    }
}

void HardwareValidator::validate_session_changes(const HardwareSnapshot& previous,
                                                 const HardwareSnapshot& current, Config* config,
                                                 HardwareValidationResult& result) {
    // Find hardware that was present before but is now missing
    auto removed = previous.get_removed(current);

    for (const auto& name : removed) {
        // Don't duplicate if already in expected_missing
        bool already_reported = false;
        for (const auto& issue : result.expected_missing) {
            if (issue.hardware_name == name) {
                already_reported = true;
                break;
            }
        }

        if (!already_reported) {
            bool is_optional = is_hardware_optional(config, name);
            if (!is_optional) {
                HardwareType type = guess_hardware_type(name);
                result.changed_from_last_session.push_back(HardwareIssue::warning(
                    name, type,
                    TR_NOOP("Hardware was present in previous session but is now missing"), false));
            }
        }
    }

    spdlog::debug("[HardwareValidator] Session comparison: {} removed, {} added since {}",
                  removed.size(), previous.get_added(current).size(), previous.timestamp);
}

// =============================================================================
// Helper Methods
// =============================================================================

bool HardwareValidator::contains_name(const std::vector<std::string>& vec,
                                      const std::string& name) {
    // Case-insensitive comparison
    std::string lower_name = name;
    lower_name = helix::text_io::to_lower(lower_name);

    for (const auto& item : vec) {
        std::string lower_item = item;
        lower_item = helix::text_io::to_lower(lower_item);
        if (lower_item == lower_name) {
            return true;
        }
    }
    return false;
}

HardwareType HardwareValidator::guess_hardware_type(const std::string& name) {
    std::string lower = name;
    lower = helix::text_io::to_lower(lower);

    if (lower.find("extruder") != std::string::npos ||
        lower.find("heater_bed") != std::string::npos ||
        lower.find("heater_generic") != std::string::npos) {
        return HardwareType::HEATER;
    }

    if (lower.find("temperature_sensor") != std::string::npos ||
        lower.find("temperature_fan") != std::string::npos) {
        return HardwareType::SENSOR;
    }

    if (lower.find("fan") != std::string::npos) {
        return HardwareType::FAN;
    }

    if (lower.find("neopixel") != std::string::npos || lower.find("led") != std::string::npos ||
        lower.find("dotstar") != std::string::npos) {
        return HardwareType::LED;
    }

    if (lower.find("filament") != std::string::npos) {
        return HardwareType::FILAMENT_SENSOR;
    }

    return HardwareType::OTHER;
}

void HardwareValidator::log_ignored_hardware(Config* config) {
    if (!config) {
        return;
    }

    const std::vector<std::string> names =
        config->get_string_array(config->df() + "hardware/optional");
    if (names.empty()) {
        return;
    }

    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += names[i];
    }
    spdlog::info("[HardwareValidator] {} hardware item(s) silenced (ignored): {}", names.size(),
                 joined);
}
