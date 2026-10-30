// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_fan_state.cpp
 * @brief Fan state management extracted from PrinterState
 *
 * Manages fan subjects including main part-cooling fan speed, multi-fan
 * tracking with per-fan subjects, and fan metadata for UI display.
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_fan_state.h"

#include "config.h"
#include "device_display_name.h"
#include "fan_gcode.h"
#include "hardware_object_table.h"
#include "hardware_role_registry.h"
#include "json_utils.h"
#include "lvgl.h"
#include "state/subject_macros.h"
#include "text_io.h"
#include "translation_loader.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

const char* fan_role_label(FanRole role, FanLabelLength length) {
    struct Spellings {
        const char* chip;
        const char* word;
        const char* full;
    };
    static constexpr Spellings kRoles[] = {
        {TR_NOOP("Part"), TR_NOOP("Part"), TR_NOOP("Part Fan")},
        {TR_NOOP("HE"), TR_NOOP("Hotend"), TR_NOOP("Hotend Fan")},
        {TR_NOOP("Chamber"), TR_NOOP("Chamber"), TR_NOOP("Chamber Fan")},
        {TR_NOOP("Exhaust"), TR_NOOP("Exhaust"), TR_NOOP("Exhaust Fan")},
        {TR_NOOP("Aux"), TR_NOOP("Aux"), TR_NOOP("Aux Fan")},
    };
    const Spellings& s = kRoles[static_cast<int>(role)];
    switch (length) {
    case FanLabelLength::Chip:
        return s.chip;
    case FanLabelLength::Word:
        return s.word;
    case FanLabelLength::Full:
        return s.full;
    }
    return s.full;
}

const char* fan_type_label(FanType type) {
    switch (type) {
    case FanType::PART_COOLING:
        return fan_role_label(FanRole::Part, FanLabelLength::Chip);
    case FanType::HEATER_FAN:
        return fan_role_label(FanRole::Hotend, FanLabelLength::Chip);
    case FanType::CONTROLLER_FAN:
        return TR_NOOP("Controller");
    case FanType::TEMPERATURE_FAN:
        return TR_NOOP("Temp");
    case FanType::GENERIC_FAN:
        return TR_NOOP("Generic");
    case FanType::OUTPUT_PIN_FAN:
        return TR_NOOP("Output Pin");
    }
    return TR_NOOP("Fan");
}

FanRoleConfig FanRoleConfig::from_config(Config* config,
                                         const std::vector<std::string>& discovered_fans) {
    FanRoleConfig roles;
    if (!config) {
        return roles;
    }
    // Resolve all four fan roles without saving (persist=false), then write once if any changed.
    // This avoids up to 4 independent disk writes on first-heal.
    bool any_changed = false;
    auto resolve_one = [&](HardwareRoleId id, std::string& field) {
        const auto* desc = role_descriptor(id);
        if (!desc)
            return;
        const std::string key = config->df() + desc->config_key;
        const std::string dflt =
            desc->canonical_default ? std::string(desc->canonical_default) : std::string();
        const std::string saved = config->get<std::string>(key, dflt);
        std::string resolved = resolve_role_from_config(id, config, discovered_fans, false);
        field = resolved;
        if (!resolved.empty() && resolved != saved) {
            config->set<std::string>(key, resolved);
            any_changed = true;
        }
    };
    resolve_one(HardwareRoleId::PartFan, roles.part_fan);
    resolve_one(HardwareRoleId::HotendFan, roles.hotend_fan);
    resolve_one(HardwareRoleId::ChamberFan, roles.chamber_fan);
    resolve_one(HardwareRoleId::ExhaustFan, roles.exhaust_fan);
    resolve_one(HardwareRoleId::AuxFan, roles.aux_fan);
    if (any_changed) {
        if (!config->save()) {
            spdlog::warn("[FanRoleConfig] Failed to persist batched fan role heals");
        }
    }
    return roles;
}

void PrinterFanState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterFanState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterFanState] Initializing subjects (register_xml={})", register_xml);

    // Fan subjects
    INIT_SUBJECT_INT(fan_speed, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(fans_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(primary_fans_version, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterFanState] Subjects initialized successfully");
}

void PrinterFanState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterFanState] Deinitializing subjects");

    // Signal subject death FIRST — sets bool to false so ALL ObserverGuards detect
    // dead subjects, then clear the map to release shared_ptr references. (#816)
    for (auto& [name, lifetime] : fan_speed_lifetimes_) {
        if (lifetime)
            *lifetime = false;
    }
    fan_speed_lifetimes_.clear();

    // Now safe to deinit subjects (lv_subject_deinit frees attached observers)
    for (auto& [name, subject_ptr] : fan_speed_subjects_) {
        if (subject_ptr) {
            lv_subject_deinit(subject_ptr.get());
        }
    }
    fan_speed_subjects_.clear();

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterFanState::update_from_status(const nlohmann::json& status) {
    // Update main part-cooling fan speed
    if (status.contains("fan")) {
        const auto& fan = status["fan"];
        spdlog::trace("[PrinterFanState] Received fan status update: {}",
                      helix::json_util::safe_dump(fan));

        if (fan.contains("speed") && fan["speed"].is_number()) {
            double speed = fan["speed"].get<double>();
            int speed_pct = units::to_percent(normalize_speed("fan", speed));
            spdlog::trace("[PrinterFanState] Fan speed update: {}%", speed_pct);
            lv_subject_set_int(&fan_speed_, speed_pct);

            // Also update multi-fan tracking
            update_fan_speed("fan", speed);
        }
    }

    // Check for other fan types in the status update
    // Moonraker sends fan objects as top-level keys: "heater_fan hotend_fan", "fan_generic xyz"
    for (const auto& [key, value] : status.items()) {
        const hw::ObjectKind kind = hw::classify_object(key).kind;

        // Fan objects other than the bare part fan and output_pin fans report "speed"
        if (hw::is_fan(kind) && kind != hw::ObjectKind::Fan &&
            kind != hw::ObjectKind::OutputPinFan) {
            if (value.is_object() && value.contains("speed") && value["speed"].is_number()) {
                double speed = value["speed"].get<double>();
                update_fan_speed(key, speed);

                // If this is the configured part fan, also update the main fan_speed_ subject
                // so the hero slider tracks the actual part fan speed
                if (!roles_.part_fan.empty() && key == roles_.part_fan) {
                    int speed_pct = units::to_percent(normalize_speed(key, speed));
                    lv_subject_set_int(&fan_speed_, speed_pct);
                }
            }
        }

        // Handle output_pin fan objects (Creality-style)
        // These report {"value": 0.0-1.0} instead of {"speed": 0.0-1.0}
        if (kind == hw::ObjectKind::OutputPinFan) {
            if (value.is_object() && value.contains("value") && value["value"].is_number()) {
                double speed = value["value"].get<double>();
                update_fan_speed(key, speed);

                // If this is the configured part fan, also update the main fan_speed_ subject
                if (!roles_.part_fan.empty() && key == roles_.part_fan) {
                    int speed_pct = units::to_percent(speed);
                    lv_subject_set_int(&fan_speed_, speed_pct);
                }
            }
        }
    }

    // Parse fan_feedback RPM data (Creality-specific tachometer module)
    if (status.contains("fan_feedback")) {
        const auto& fb = status["fan_feedback"];
        if (fb.is_object()) {
            for (int i = 0; i < 10; i++) {
                std::string key = "fan" + std::to_string(i) + "_speed";
                if (fb.contains(key) && fb[key].is_number()) {
                    int rpm = fb[key].get<int>();
                    update_fan_rpm("output_pin fan" + std::to_string(i), rpm);
                }
            }
        }
    }
}

FanType PrinterFanState::classify_fan_type(const std::string& object_name) const {
    return classify_fan_type(object_name, roles_.part_fan);
}

int PrinterFanState::count_extra_controllable_fans(const std::vector<std::string>& fan_objects,
                                                   const std::string& configured_part_fan) {
    int count = 0;
    for (const auto& name : fan_objects) {
        const FanType type = classify_fan_type(name, configured_part_fan);
        if (type != FanType::PART_COOLING && is_fan_controllable(type)) {
            ++count;
        }
    }
    return count;
}

FanType PrinterFanState::classify_fan_type(const std::string& object_name,
                                           const std::string& configured_part_fan) {
    if (object_name == "fan") {
        return FanType::PART_COOLING;
    }
    // Check if this fan is the wizard-configured part cooling fan
    if (!configured_part_fan.empty() && object_name == configured_part_fan) {
        return FanType::PART_COOLING;
    }
    const auto cls = hw::classify_object(object_name);
    switch (cls.kind) {
    case hw::ObjectKind::HeaterFan:
        return FanType::HEATER_FAN;
    case hw::ObjectKind::ControllerFan:
        return FanType::CONTROLLER_FAN;
    case hw::ObjectKind::TemperatureFan:
        return FanType::TEMPERATURE_FAN;
    case hw::ObjectKind::OutputPinFan:
        // A hotend-area pin is driven with the heater: shown like a heater_fan, never commanded.
        return output_pin_fan_is_firmware_driven(cls.instance) ? FanType::HEATER_FAN
                                                               : FanType::OUTPUT_PIN_FAN;
    default:
        return FanType::GENERIC_FAN;
    }
}

PrimaryFans PrinterFanState::classify_primary_fans() const {
    PrimaryFans out;

    // Seed part/hotend from the first fan of each type; aux is resolved separately
    // (it prefers commandable fans and must exclude the resolved part fan).
    for (const auto& fan : fans_) {
        if (fan.type == FanType::PART_COOLING && out.part.empty())
            out.part = fan.object_name;
        else if (fan.type == FanType::HEATER_FAN && out.hotend.empty())
            out.hotend = fan.object_name;
    }

    out.part = resolve_part_fan(out.part);
    out.aux = resolve_aux_fan(out.part, out.hotend);
    return out;
}

PrimaryFanSlots PrinterFanState::possible_primary_fan_slots() const {
    PrimaryFanSlots out;
    bool part_cooling = false;
    int commandable = 0, aux = 0;
    for (const auto& fan : fans_) {
        part_cooling |= fan.type == FanType::PART_COOLING;
        out.hotend |= fan.type == FanType::HEATER_FAN;
        commandable += fan.type == FanType::GENERIC_FAN || fan.type == FanType::OUTPUT_PIN_FAN;
        aux += is_aux_fan(fan.type);
    }
    // resolve_part_fan() takes the configured part fan or a commandable one.
    out.part = part_cooling || commandable > 0;
    // Without a part cooling fan, a commandable fan always holds the part slot.
    out.aux = aux - (!part_cooling && commandable > 0 ? 1 : 0) > 0;
    return out;
}

std::string PrinterFanState::resolve_part_fan(const std::string& configured) const {
    // The bare [fan] is Klipper's canonical part cooling object, but some printers
    // leave it a stale 0% stub and drive a manually-commandable named fan instead
    // (output_pin fanN / fan_generic; user Discord report, e3f92c3f4). Auto-controlled
    // fans (heater/controller/temperature) can never be the M106 part fan, so they are
    // never candidates — promoting an idle controller_fan/temperature_fan over the
    // live [fan] is what stuck the Sovol SV08 part fan at 0% (#1124).
    //
    // Pick by which fan has actually spun (sticky: ever_ran survives a fan turning
    // off, so the slot doesn't flick to a running auxiliary fan while the real [fan]
    // is briefly idle at first layer / on a bridge). classify re-runs on fan
    // start/stop via primary_fans_version:
    //   1. configured part fan has run             -> keep it
    //   2. else a commandable named fan that ran   -> promote it (stale-"fan" printers)
    //   3. else keep the configured part fan; if none configured, the front-most
    //      commandable fan (the real part fan is a named object).
    auto has_run = [this](const std::string& name) -> bool {
        for (const auto& fan : fans_)
            if (fan.object_name == name)
                return fan.ever_ran || fan.speed_percent > 0;
        return false;
    };

    if (!configured.empty() && has_run(configured))
        return configured; // (1) the configured part fan proved itself

    std::string first_candidate;
    for (const auto& fan : fans_) {
        if (!is_fan_controllable(fan.type) || fan.object_name == configured)
            continue;
        if (first_candidate.empty())
            first_candidate = fan.object_name;
        if (fan.ever_ran || fan.speed_percent > 0)
            return fan.object_name; // (2) a commandable named fan that ran is the part fan
    }

    if (!configured.empty())
        return configured;  // (3a) keep canonical part fan (never ran, or no commandable peer)
    return first_candidate; // (3b) no configured part fan -> front-most commandable, may be empty
}

std::string PrinterFanState::resolve_aux_fan(const std::string& part,
                                             const std::string& hotend) const {
    // The compact row has one aux slot. Prefer a user-commandable fan (fan_generic /
    // output_pin) — the one a user actually toggles and wants to see — over an
    // auto-controlled controller_fan / temperature_fan whose reading rarely changes
    // and looks "stuck". Falls back to the first auto-controlled fan when there is no
    // commandable candidate. Excludes the fans already shown in part/hotend (#1124).
    std::string commandable, auto_controlled;
    for (const auto& fan : fans_) {
        if (fan.object_name == part || fan.object_name == hotend || !is_aux_fan(fan.type))
            continue;
        bool user_commandable =
            fan.type == FanType::GENERIC_FAN || fan.type == FanType::OUTPUT_PIN_FAN;
        if (user_commandable) {
            if (commandable.empty())
                commandable = fan.object_name;
        } else if (auto_controlled.empty()) {
            auto_controlled = fan.object_name;
        }
    }
    return !commandable.empty() ? commandable : auto_controlled;
}

void PrinterFanState::apply_roles(const FanRoleConfig& roles) {
    // Copy both out first: init_fans assigns straight into the members these
    // alias, and passing a member as a const& argument to the call that
    // overwrites it is the kind of self-assignment that works until someone
    // reorders two lines inside init_fans.
    const std::vector<std::string> objects = discovered_objects_;
    const std::unordered_map<std::string, double> max_power = fan_max_power_;
    init_fans(objects, roles, max_power);
}

void PrinterFanState::refresh_primary_fans_selection() {
    PrimaryFans now = classify_primary_fans();
    if (now != primary_fans_cache_) {
        primary_fans_cache_ = now;
        lv_subject_set_int(&primary_fans_version_, lv_subject_get_int(&primary_fans_version_) + 1);
        spdlog::debug("[PrinterFanState] Primary fans reassigned: part='{}' hotend='{}' aux='{}'",
                      now.part, now.hotend, now.aux);
    }
}

std::string PrinterFanState::get_role_display_name(const std::string& object_name) const {
    auto it = role_display_names_.find(object_name);
    if (it != role_display_names_.end()) {
        return it->second;
    }
    return {};
}

std::string PrinterFanState::disambiguate_chamber_fan_name(const std::string& object_name,
                                                           FanType type,
                                                           const std::string& base_name) const {
    // Some printers (e.g. Creality K2 Plus) expose two distinct Klipper objects
    // that share the "chamber_fan" suffix: the PTC heater element's cooling fan
    // (heater_fan chamber_fan) and the chamber cooling fan (temperature_fan
    // chamber_fan). Both otherwise resolve to a flat "Chamber Fan", which is
    // ambiguous. Refine the label by role so each reads distinctly.
    if (extract_device_suffix(object_name) != "chamber_fan") {
        return base_name;
    }
    switch (type) {
    case FanType::HEATER_FAN:
        return "Chamber Heater Fan";
    case FanType::TEMPERATURE_FAN:
        return "Chamber Cooling Fan";
    default:
        return base_name;
    }
}

std::string PrinterFanState::resolve_display_name(const std::string& object_name,
                                                  FanType type) const {
    // Name priority: custom name > role name > auto-generated
    auto* config = Config::get_instance();
    std::string custom = config->get<std::string>(config->df() + "fans/names/" + object_name, "");
    if (!custom.empty()) {
        return custom;
    }

    const std::string role_name = get_role_display_name(object_name);
    const std::string english = disambiguate_chamber_fan_name(
        object_name, type,
        role_name.empty() ? get_display_name(object_name, DeviceType::FAN) : role_name);
    return lv_tr(english.c_str());
}

void PrinterFanState::refresh_display_names() {
    for (auto& fan : fans_) {
        fan.display_name = resolve_display_name(fan.object_name, fan.type);
    }
    lv_subject_set_int(&fans_version_, lv_subject_get_int(&fans_version_) + 1);
}

bool PrinterFanState::is_fan_controllable(FanType type) {
    return type == FanType::PART_COOLING || type == FanType::GENERIC_FAN ||
           type == FanType::OUTPUT_PIN_FAN;
}

bool PrinterFanState::is_aux_fan(FanType type) {
    return type == FanType::CONTROLLER_FAN || type == FanType::TEMPERATURE_FAN ||
           type == FanType::GENERIC_FAN || type == FanType::OUTPUT_PIN_FAN;
}

double PrinterFanState::normalize_speed(const std::string& object_name, double raw_speed) const {
    std::string key = object_name;
    key = helix::text_io::to_lower(key);
    auto it = fan_max_power_.find(key);
    double max_power = (it != fan_max_power_.end() && it->second > 0.0) ? it->second : 1.0;
    double normalized = raw_speed / max_power;
    if (normalized < 0.0)
        normalized = 0.0;
    if (normalized > 1.0)
        normalized = 1.0;
    return normalized;
}

void PrinterFanState::init_fans(const std::vector<std::string>& fan_objects,
                                const FanRoleConfig& roles,
                                const std::unordered_map<std::string, double>& max_power) {
    fan_max_power_ = max_power;
    discovered_objects_ = fan_objects;

    // Build new subject map, reusing existing subjects for fans that persist
    // across reconnections. Only deinit subjects for fans that disappeared.
    std::unordered_map<std::string, std::unique_ptr<lv_subject_t>> new_subjects;
    std::unordered_map<std::string, SubjectLifetime> new_lifetimes;
    new_subjects.reserve(fan_objects.size());
    new_lifetimes.reserve(fan_objects.size());

    // Store configured fan roles for classification and naming
    roles_ = roles;
    role_display_names_.clear();

    // Build role-based display name overrides for configured fans.
    // Configured fans use their role name; unconfigured fans use auto-generated names.
    const std::pair<const std::string&, FanRole> assigned[] = {
        {roles_.part_fan, FanRole::Part},       {roles_.hotend_fan, FanRole::Hotend},
        {roles_.chamber_fan, FanRole::Chamber}, {roles_.exhaust_fan, FanRole::Exhaust},
        {roles_.aux_fan, FanRole::Aux},
    };
    for (const auto& [object, role] : assigned) {
        // The bare [fan] keeps its own name, "Part Cooling Fan".
        if (!object.empty() && !(role == FanRole::Part && object == "fan")) {
            role_display_names_[object] = fan_role_label(role, FanLabelLength::Full);
        }
    }

    spdlog::trace("[PrinterFanState] Fan role config: part='{}' hotend='{}' chamber='{}' "
                  "exhaust='{}' aux='{}' ({} display overrides)",
                  roles_.part_fan, roles_.hotend_fan, roles_.chamber_fan, roles_.exhaust_fan,
                  roles_.aux_fan, role_display_names_.size());

    // Carry live telemetry across the rebuild for fans that persist, exactly as
    // the subject map below does. init_fans() re-runs on every
    // reapply_hardware_roles() while the subscription stays live, and Moonraker's
    // notify_status_update is DIFFERENTIAL: a fan holding a steady speed reports
    // nothing afterwards, so anything zeroed here has no event that would ever
    // restore it. Losing speed_percent freezes the readout; losing ever_ran also
    // drops the part slot back to the front-most commandable fan, stranding it on
    // a dead [fan] at 0% while All Fans stays correct (#1181).
    std::unordered_map<std::string, FanInfo> previous;
    previous.reserve(fans_.size());
    for (auto& fan : fans_)
        previous.emplace(fan.object_name, std::move(fan));

    fans_.clear();
    fans_.reserve(fan_objects.size());

    for (const auto& obj_name : fan_objects) {
        // Skip bare "fan" only when a DIFFERENT, LIVE fan is configured as part
        // cooling — some printers expose an empty "fan" object that never reports
        // speed data. Never skip the real [fan] just because a stale role names an
        // absent object.
        if (obj_name == "fan" && !roles_.part_fan.empty() && roles_.part_fan != "fan" &&
            std::find(fan_objects.begin(), fan_objects.end(), roles_.part_fan) !=
                fan_objects.end()) {
            spdlog::debug("[PrinterFanState] Skipping bare 'fan' — part fan is '{}'",
                          roles_.part_fan);
            continue;
        }

        FanInfo info;
        info.object_name = obj_name;
        info.type = classify_fan_type(obj_name);
        info.is_controllable = is_fan_controllable(info.type);
        info.speed_percent = 0;

        // Same object name = same physical fan, the assumption the subject reuse
        // below already makes. Identity fields (type, controllability, display
        // name) are recomputed from the fresh config; only the live readings ride
        // along (#1181).
        auto prior = previous.find(obj_name);
        if (prior != previous.end()) {
            info.speed_percent = prior->second.speed_percent;
            info.ever_ran = prior->second.ever_ran;
            info.rpm = prior->second.rpm;
        }

        info.display_name = resolve_display_name(obj_name, info.type);

        spdlog::trace("[PrinterFanState] Registered fan: {} -> \"{}\" (type={}, controllable={})",
                      obj_name, info.display_name, static_cast<int>(info.type),
                      info.is_controllable);
        const int carried_speed = info.speed_percent;
        fans_.push_back(std::move(info));

        // Reuse existing subject if this fan was already tracked, otherwise create new
        auto existing = fan_speed_subjects_.find(obj_name);
        if (existing != fan_speed_subjects_.end() && existing->second) {
            // Reuse — keep the subject alive so observers stay valid, and hold it
            // to the same value the struct carried forward. Writing 0 here would
            // re-open the struct/subject split that update_fan_speed's
            // unconditional write exists to absorb (#1181).
            lv_subject_set_int(existing->second.get(), carried_speed);
            new_subjects.emplace(obj_name, std::move(existing->second));
            // Reuse existing lifetime token too (observers still hold valid weak_ptrs)
            auto lifetime_it = fan_speed_lifetimes_.find(obj_name);
            if (lifetime_it != fan_speed_lifetimes_.end()) {
                new_lifetimes.emplace(obj_name, std::move(lifetime_it->second));
            } else {
                new_lifetimes.emplace(obj_name, std::make_shared<bool>(true));
            }
            spdlog::trace("[PrinterFanState] Reused speed subject for fan: {}", obj_name);
        } else {
            auto subject_ptr = std::make_unique<lv_subject_t>();
            lv_subject_init_int(subject_ptr.get(), 0);
            new_subjects.emplace(obj_name, std::move(subject_ptr));
            new_lifetimes.emplace(obj_name, std::make_shared<bool>(true));
            spdlog::trace("[PrinterFanState] Created speed subject for fan: {}", obj_name);
        }
    }

    // Signal subject death for orphaned fans — sets bool to false so ALL
    // ObserverGuards detect the dead subject, even if other services still hold
    // shared_ptr copies. Then reset to release our reference. (#816)
    for (auto& [name, lifetime] : fan_speed_lifetimes_) {
        if (new_lifetimes.find(name) == new_lifetimes.end()) {
            spdlog::trace("[PrinterFanState] Expiring lifetime token for orphaned fan: {}", name);
            if (lifetime)
                *lifetime = false;
            lifetime.reset();
        }
    }

    // Now safe to deinit orphaned subjects (observers already invalidated above)
    for (auto& [name, subject_ptr] : fan_speed_subjects_) {
        if (subject_ptr) {
            spdlog::trace("[PrinterFanState] Deiniting orphaned speed subject for fan: {}", name);
            lv_subject_deinit(subject_ptr.get());
        }
    }
    fan_speed_subjects_ = std::move(new_subjects);
    fan_speed_lifetimes_ = std::move(new_lifetimes);

    // Seed the primary-role cache from the fresh (idle) fan set so the first
    // fan start/stop after discovery is compared against a known baseline.
    primary_fans_cache_ = classify_primary_fans();

    // Initialize and bump version to notify UI. fans_version signals the
    // structural change; primary_fans_version follows once speeds arrive.
    lv_subject_set_int(&fans_version_, lv_subject_get_int(&fans_version_) + 1);
    spdlog::debug("[PrinterFanState] Initialized {} fans with {} speed subjects (version {})",
                  fans_.size(), fan_speed_subjects_.size(), lv_subject_get_int(&fans_version_));
}

void PrinterFanState::update_fan_speed(const std::string& object_name, double speed) {
    int speed_pct = units::to_percent(normalize_speed(object_name, speed));

    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            bool changed = fan.speed_percent != speed_pct;
            bool was_running = fan.speed_percent > 0;

            if (changed) {
                fan.speed_percent = speed_pct;
                if (speed_pct > 0)
                    fan.ever_ran = true;
            }

            // Always fire the per-fan subject — lv_subject_set_int no-ops
            // internally when the value hasn't changed (lv_observer.c
            // lv_subject_notify_if_changed), so there is no perf cost when
            // struct and subject agree.
            //
            // The write is deliberately NOT gated on the struct comparison. A
            // gated write can only ever restore the subject when the struct also
            // moves, so any divergence becomes permanent: the subject waits for a
            // change that, on a differential feed, may never come. Nothing in the
            // tree is known to diverge them today — init_fans() carries struct and
            // subject forward together — and this write is what keeps that true
            // cheaply rather than by argument (#1181).
            auto it = fan_speed_subjects_.find(object_name);
            if (it != fan_speed_subjects_.end() && it->second) {
                lv_subject_set_int(it->second.get(), speed_pct);
                if (changed) {
                    spdlog::trace("[PrinterFanState] Fan {} speed updated to {}%", object_name,
                                  speed_pct);
                }
            } else if (changed) {
                spdlog::debug("[PrinterFanState] Dropping speed update for '{}' — subject "
                              "not initialized",
                              object_name);
            }

            // A fan starting or stopping can change which fan owns the part
            // slot (runtime-adaptive selection). Only a zero-crossing can flip
            // it, so skip the recompute for same-running-state changes (#1124).
            if (changed && was_running != (speed_pct > 0)) {
                refresh_primary_fans_selection();
            }
            return;
        }
    }
    // Fan not in list - this is normal during initial status before discovery
}

void PrinterFanState::update_fan_rpm(const std::string& object_name, int rpm) {
    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            fan.rpm = rpm;
            return;
        }
    }
}

lv_subject_t* PrinterFanState::get_fan_speed_subject(const std::string& object_name,
                                                     SubjectLifetime& lifetime) {
    auto it = fan_speed_subjects_.find(object_name);
    if (it != fan_speed_subjects_.end() && it->second) {
        auto lt = fan_speed_lifetimes_.find(object_name);
        if (lt != fan_speed_lifetimes_.end()) {
            lifetime = lt->second;
        }
        return it->second.get();
    }
    lifetime.reset();
    return nullptr;
}

lv_subject_t* PrinterFanState::get_fan_speed_subject(const std::string& object_name) {
    auto it = fan_speed_subjects_.find(object_name);
    if (it != fan_speed_subjects_.end() && it->second) {
        return it->second.get();
    }
    return nullptr;
}

void PrinterFanState::rename_fan(const std::string& object_name, const std::string& new_name) {
    // Save to config
    auto* config = Config::get_instance();
    std::string key = config->df() + "fans/names/" + object_name;
    config->set<std::string>(key, new_name);

    if (config->save()) {
        spdlog::info("[PrinterFanState] Persisted name '{}' for '{}'", new_name, object_name);
    } else {
        spdlog::error("[PrinterFanState] Failed to persist name '{}' for '{}'", new_name,
                      object_name);
    }

    // Update in-memory display name
    bool matched = false;
    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            matched = true;
            if (new_name.empty()) {
                // Revert to role name or auto-generated name
                fan.display_name = resolve_display_name(object_name, fan.type);
            } else {
                fan.display_name = new_name;
            }
            spdlog::info("[PrinterFanState] Renamed '{}' -> '{}'", object_name, fan.display_name);
            break;
        }
    }

    // Discovery can drop a fan between the rename modal opening and the confirm — a
    // Klipper restart empties fans_. The name is persisted above either way and takes
    // effect when the object is discovered again.
    if (!matched) {
        spdlog::warn("[PrinterFanState] '{}' is not a discovered fan; name persisted, display "
                     "name applies when it returns",
                     object_name);
    }

    // Bump version so UI rebuilds
    lv_subject_set_int(&fans_version_, lv_subject_get_int(&fans_version_) + 1);
}

} // namespace helix
