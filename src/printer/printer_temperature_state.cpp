// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_temperature_state.cpp
 * @brief Temperature state management extracted from PrinterState
 *
 * Manages extruder and bed temperature subjects with decidegree precision.
 * Supports multiple extruders via dynamic ExtruderInfo map. The "active extruder"
 * subjects track whichever extruder is currently selected, defaulting to "extruder".
 */

#include "printer_temperature_state.h"

#include "ui_temperature_utils.h"

#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "state/subject_macros.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

void PrinterTemperatureState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterTemperatureState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterTemperatureState] Initializing subjects (register_xml={})",
                  register_xml);

    // Active extruder subjects (track whichever extruder is currently active)
    // XML names stay as "extruder_temp"/"extruder_target" for XML binding compatibility
    lv_subject_init_int(&active_extruder_temp_, 0);
    subjects_.register_subject(&active_extruder_temp_);
    if (register_xml) {
        lv_xml_register_subject(nullptr, "extruder_temp", &active_extruder_temp_);
    }

    lv_subject_init_int(&active_extruder_target_, 0);
    subjects_.register_subject(&active_extruder_target_);
    if (register_xml) {
        lv_xml_register_subject(nullptr, "extruder_target", &active_extruder_target_);
    }

    // Bed and chamber temperature subjects
    INIT_SUBJECT_INT(bed_temp, 0, subjects_, register_xml);
    bed_temp_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(bed_target, 0, subjects_, register_xml);
    bed_target_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_temp, 0, subjects_, register_xml);
    chamber_temp_lifetime_ = std::make_shared<bool>(true);
    // chamber_target / chamber_fan_target: internal inputs only — intentionally NOT
    // XML-registered. Bind chamber_effective_target + chamber_mode instead (#display-canon).
    INIT_SUBJECT_INT(chamber_target, 0, subjects_, false);
    chamber_target_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_fan_target, 0, subjects_, false);
    chamber_fan_target_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_effective_target, 0, subjects_, register_xml);
    chamber_effective_target_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_mode, helix::ChamberMode::Off, subjects_, register_xml);
    chamber_mode_lifetime_ = std::make_shared<bool>(true);

    // Extruder version subject (bumped when extruder list changes)
    INIT_SUBJECT_INT(extruder_version, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterTemperatureState] Subjects initialized successfully");
}

void PrinterTemperatureState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterTemperatureState] Deinitializing subjects");

    // Signal subject death FIRST — sets the pointed-to bool to false so that
    // ALL ObserverGuards detect the subject as dead and skip lv_observer_remove()
    // on the about-to-be-freed observers. Then reset the shared_ptr. (#816)
    if (bed_temp_lifetime_)
        *bed_temp_lifetime_ = false;
    bed_temp_lifetime_.reset();
    if (bed_target_lifetime_)
        *bed_target_lifetime_ = false;
    bed_target_lifetime_.reset();
    if (chamber_temp_lifetime_)
        *chamber_temp_lifetime_ = false;
    chamber_temp_lifetime_.reset();
    if (chamber_target_lifetime_)
        *chamber_target_lifetime_ = false;
    chamber_target_lifetime_.reset();
    if (chamber_fan_target_lifetime_)
        *chamber_fan_target_lifetime_ = false;
    chamber_fan_target_lifetime_.reset();
    if (chamber_effective_target_lifetime_)
        *chamber_effective_target_lifetime_ = false;
    chamber_effective_target_lifetime_.reset();
    if (chamber_mode_lifetime_)
        *chamber_mode_lifetime_ = false;
    chamber_mode_lifetime_.reset();
    for (auto& [name, info] : extruders_) {
        if (info.temp_lifetime)
            *info.temp_lifetime = false;
        info.temp_lifetime.reset();
        if (info.target_lifetime)
            *info.target_lifetime = false;
        info.target_lifetime.reset();
    }

    // Now safe to deinit dynamic per-extruder subjects
    for (auto& [name, info] : extruders_) {
        if (info.temp_subject) {
            lv_subject_deinit(info.temp_subject.get());
        }
        if (info.target_subject) {
            lv_subject_deinit(info.target_subject.get());
        }
    }
    extruders_.clear();

    // Reset active extruder to default
    active_extruder_name_ = "extruder";

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterTemperatureState::register_xml_subjects() {
    if (!subjects_initialized_) {
        spdlog::warn("[PrinterTemperatureState] Cannot register XML subjects - not initialized");
        return;
    }

    spdlog::debug("[PrinterTemperatureState] Re-registering subjects with XML system");
    lv_xml_register_subject(nullptr, "extruder_temp", &active_extruder_temp_);
    lv_xml_register_subject(nullptr, "extruder_target", &active_extruder_target_);
    lv_xml_register_subject(nullptr, "bed_temp", &bed_temp_);
    lv_xml_register_subject(nullptr, "bed_target", &bed_target_);
    lv_xml_register_subject(nullptr, "chamber_temp", &chamber_temp_);
    // chamber_target / chamber_fan_target intentionally omitted — internal inputs only.
    lv_xml_register_subject(nullptr, "chamber_effective_target", &chamber_effective_target_);
    lv_xml_register_subject(nullptr, "chamber_mode", &chamber_mode_);
    lv_xml_register_subject(nullptr, "extruder_version", &extruder_version_);
}

void PrinterTemperatureState::init_extruders(const std::vector<std::string>& heaters) {
    // Signal subject death FIRST — sets the pointed-to bool to false so that
    // ALL ObserverGuards (including those in other services that still hold
    // shared_ptr copies) detect the subject as dead and skip lv_observer_remove()
    // on the about-to-be-freed observers. (#816)
    for (auto& [name, info] : extruders_) {
        if (info.temp_lifetime)
            *info.temp_lifetime = false;
        info.temp_lifetime.reset();
        if (info.target_lifetime)
            *info.target_lifetime = false;
        info.target_lifetime.reset();
    }

    // Now safe to deinit existing per-extruder subjects
    for (auto& [name, info] : extruders_) {
        if (info.temp_subject) {
            lv_subject_deinit(info.temp_subject.get());
        }
        if (info.target_subject) {
            lv_subject_deinit(info.target_subject.get());
        }
    }
    extruders_.clear();

    // Filter for extruder* names and count them
    std::vector<std::string> extruder_names;
    for (const auto& name : heaters) {
        // Accept "extruder" and "extruderN" (digit suffix), reject "extruder_stepper" etc.
        if (is_extruder_name(name)) {
            extruder_names.push_back(name);
        }
    }
    // Sort so the "Nozzle N" suffix matches the lexical (and numeric, for N < 10)
    // order — both the chip row and the config modal sort by name elsewhere, so
    // skipping the sort here would otherwise misalign labels with extruder index
    // when Klipper returns heaters in a non-deterministic order.
    std::sort(extruder_names.begin(), extruder_names.end());

    bool multi = extruder_names.size() > 1;

    extruders_.reserve(extruder_names.size());
    for (size_t i = 0; i < extruder_names.size(); ++i) {
        const auto& name = extruder_names[i];
        ExtruderInfo info;
        info.name = name;

        // Single extruder: "Nozzle". Multiple: "Nozzle 1", "Nozzle 2", ...
        // Translated at init time; mid-session language changes won't refresh
        // cached names until the next extruder rediscover (e.g., reconnect).
        if (multi) {
            info.display_name = std::string(lv_tr("Nozzle")) + " " + std::to_string(i + 1);
        } else {
            info.display_name = lv_tr("Nozzle");
        }

        // Create heap-allocated subjects (stable across rehash)
        info.temp_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.temp_subject.get(), 0);
        info.temp_lifetime = std::make_shared<bool>(true);

        info.target_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.target_subject.get(), 0);
        info.target_lifetime = std::make_shared<bool>(true);

        spdlog::trace("[PrinterTemperatureState] Registered extruder: {} -> \"{}\"", name,
                      info.display_name);
        extruders_.emplace(name, std::move(info));
    }

    // Bump version to notify UI of extruder list change
    lv_subject_set_int(&extruder_version_, lv_subject_get_int(&extruder_version_) + 1);
    spdlog::debug("[PrinterTemperatureState] Initialized {} extruders (version {})",
                  extruders_.size(), lv_subject_get_int(&extruder_version_));
}

lv_subject_t* PrinterTemperatureState::get_extruder_temp_subject(const std::string& name) {
    auto it = extruders_.find(name);
    if (it != extruders_.end() && it->second.temp_subject) {
        return it->second.temp_subject.get();
    }
    return nullptr;
}

lv_subject_t* PrinterTemperatureState::get_extruder_temp_subject(const std::string& name,
                                                                 SubjectLifetime& lifetime) {
    auto it = extruders_.find(name);
    if (it != extruders_.end() && it->second.temp_subject) {
        lifetime = it->second.temp_lifetime;
        return it->second.temp_subject.get();
    }
    lifetime.reset();
    return nullptr;
}

lv_subject_t* PrinterTemperatureState::get_extruder_target_subject(const std::string& name) {
    auto it = extruders_.find(name);
    if (it != extruders_.end() && it->second.target_subject) {
        return it->second.target_subject.get();
    }
    return nullptr;
}

lv_subject_t* PrinterTemperatureState::get_extruder_target_subject(const std::string& name,
                                                                   SubjectLifetime& lifetime) {
    auto it = extruders_.find(name);
    if (it != extruders_.end() && it->second.target_subject) {
        lifetime = it->second.target_lifetime;
        return it->second.target_subject.get();
    }
    lifetime.reset();
    return nullptr;
}

void PrinterTemperatureState::set_active_extruder(const std::string& name) {
    // Verify the extruder exists in our map
    auto it = extruders_.find(name);
    if (it == extruders_.end()) {
        spdlog::warn("[PrinterTemperatureState] Unknown extruder '{}', keeping '{}'", name,
                     active_extruder_name_);
        return;
    }

    if (name == active_extruder_name_) {
        return; // No change needed
    }

    spdlog::info("[PrinterTemperatureState] Active extruder: {} -> {}", active_extruder_name_,
                 name);
    active_extruder_name_ = name;

    // Sync current values from per-extruder subjects to active subjects
    const auto& info = it->second;
    if (info.temp_subject) {
        int new_temp = lv_subject_get_int(info.temp_subject.get());
        int old_temp = lv_subject_get_int(&active_extruder_temp_);
        lv_subject_set_int(&active_extruder_temp_, new_temp);
        if (old_temp == new_temp) {
            lv_subject_notify(&active_extruder_temp_);
        }
    }
    if (info.target_subject) {
        int new_target = lv_subject_get_int(info.target_subject.get());
        if (lv_subject_get_int(&active_extruder_target_) != new_target) {
            lv_subject_set_int(&active_extruder_target_, new_target);
        }
    }
}

const std::string& PrinterTemperatureState::active_extruder_name() const {
    return active_extruder_name_;
}

void PrinterTemperatureState::update_from_status(const nlohmann::json& status) {
    // Update dynamic per-extruder subjects
    for (auto& [name, info] : extruders_) {
        if (!status.contains(name)) {
            // Moonraker status updates are DELTAS — an extruder holding steady
            // (or any inactive head on a multi-tool printer) is omitted entirely
            // from the notification. Without re-publishing, its per-extruder temp
            // subject freezes, and the temperature overlay (which graphs one line
            // per head) stops advancing that head's line — it looks stuck, not
            // flat. Re-notify the last-known value so observers fire and the line
            // keeps advancing. Guard on >0 so a never-initialized head doesn't
            // push a spurious 0 (the chart/history 0-filters would drop it anyway).
            if (lv_subject_get_int(info.temp_subject.get()) > 0)
                lv_subject_notify(info.temp_subject.get());
            continue;
        }
        const auto& data = status[name];

        if (data.contains("temperature") && data["temperature"].is_number()) {
            int temp_deci = helix::units::json_to_decidegrees(data, "temperature");
            info.temperature = data["temperature"].get<float>();
            int old_temp = lv_subject_get_int(info.temp_subject.get());
            lv_subject_set_int(info.temp_subject.get(), temp_deci);
            if (old_temp == temp_deci) {
                lv_subject_notify(info.temp_subject.get());
            }
        }

        if (data.contains("target") && data["target"].is_number()) {
            int target_deci = helix::units::json_to_decidegrees(data, "target");
            info.target = data["target"].get<float>();
            // Latch the last non-zero target so it survives cooldown-to-0. The
            // swap-preheat guard consults this to keep the nozzle hot enough to
            // purge the previous material even after it has cooled.
            if (info.target > 0.0f) {
                info.last_nonzero_target = info.target;
            }
            if (lv_subject_get_int(info.target_subject.get()) != target_deci) {
                lv_subject_set_int(info.target_subject.get(), target_deci);
            }
        }
    }

    // Update active extruder subjects from the currently active extruder's data
    if (status.contains(active_extruder_name_)) {
        const auto& active = status[active_extruder_name_];

        if (active.contains("temperature") && active["temperature"].is_number()) {
            int temp_deci = helix::units::json_to_decidegrees(active, "temperature");
            int old_temp = lv_subject_get_int(&active_extruder_temp_);
            lv_subject_set_int(&active_extruder_temp_, temp_deci);
            if (old_temp == temp_deci) {
                lv_subject_notify(&active_extruder_temp_);
            }
        }

        if (active.contains("target") && active["target"].is_number()) {
            int target_deci = helix::units::json_to_decidegrees(active, "target");
            if (lv_subject_get_int(&active_extruder_target_) != target_deci) {
                lv_subject_set_int(&active_extruder_target_, target_deci);
            }
        }
    }

    // Update bed temperature (stored as decidegrees for 0.1C resolution)
    if (status.contains("heater_bed")) {
        const auto& bed = status["heater_bed"];

        if (bed.contains("temperature") && bed["temperature"].is_number()) {
            int temp_deci = helix::units::json_to_decidegrees(bed, "temperature");
            int old_temp = lv_subject_get_int(&bed_temp_);
            lv_subject_set_int(&bed_temp_, temp_deci);
            if (old_temp == temp_deci) {
                lv_subject_notify(&bed_temp_);
            }
            spdlog::trace("[PrinterTemperatureState] Bed temp: {}.{}C", temp_deci / 10,
                          temp_deci % 10);
        }

        if (bed.contains("target") && bed["target"].is_number()) {
            int target_deci = helix::units::json_to_decidegrees(bed, "target");
            if (lv_subject_get_int(&bed_target_) != target_deci) {
                lv_subject_set_int(&bed_target_, target_deci);
                spdlog::trace("[PrinterTemperatureState] Bed target: {}.{}C", target_deci / 10,
                              target_deci % 10);
            }
        }
    }

    // Chamber temperature comes from the heater when one is configured (it has
    // both current temp and target), otherwise from the sensor (temp only).
    // The sensor is NOT a fallback when a heater is configured — on printers
    // that have both (e.g. QIDI Q2: heater_generic chamber + thermal-protection
    // thermistor), the "sensor" often tracks the bed or another nearby heat
    // source and would pollute chamber_temp_ when partial subscription updates
    // omit the heater object.
    if (!chamber_heater_name_.empty()) {
        if (status.contains(chamber_heater_name_)) {
            const auto& chamber = status[chamber_heater_name_];

            if (chamber.contains("temperature") && chamber["temperature"].is_number()) {
                int temp_deci = helix::units::json_to_decidegrees(chamber, "temperature");
                if (lv_subject_get_int(&chamber_temp_) != temp_deci) {
                    lv_subject_set_int(&chamber_temp_, temp_deci);
                    spdlog::trace("[PrinterTemperatureState] Chamber temp (heater): {}.{}C",
                                  temp_deci / 10, temp_deci % 10);
                }
            }

            // A temperature_fan's target is a cooling threshold, not a heating
            // command: Klipper always reports its configured target_temp (40.0 on
            // the K1C) even at speed 0. Wherever discovery resolves a
            // temperature_fan into the heater slot (no heater_generic exists),
            // that target flows through the cooling-fan branch below instead and
            // is neutralized by the resting-target comparison in
            // chamber_effective_setpoint().
            if (chamber_heater_name_.rfind("temperature_fan ", 0) != 0 &&
                chamber.contains("target") && chamber["target"].is_number()) {
                int target_deci = helix::units::json_to_decidegrees(chamber, "target");
                if (lv_subject_get_int(&chamber_target_) != target_deci) {
                    lv_subject_set_int(&chamber_target_, target_deci);
                    spdlog::trace("[PrinterTemperatureState] Chamber target: {}.{}C",
                                  target_deci / 10, target_deci % 10);
                }
            }
        }
    } else if (!chamber_sensor_name_.empty() && status.contains(chamber_sensor_name_)) {
        const auto& chamber = status[chamber_sensor_name_];

        if (chamber.contains("temperature") && chamber["temperature"].is_number()) {
            int temp_deci = helix::units::json_to_decidegrees(chamber, "temperature");
            if (lv_subject_get_int(&chamber_temp_) != temp_deci) {
                lv_subject_set_int(&chamber_temp_, temp_deci);
                spdlog::trace("[PrinterTemperatureState] Chamber temp (sensor): {}.{}C",
                              temp_deci / 10, temp_deci % 10);
            }
        }
    }

    // Chamber cooling-fan target. In COOLING mode (<=40C) the K2 M141 macro parks
    // the setpoint on the temperature_fan's target while the heater target stays
    // 0; surface it as its own subject so a later step can combine heater+fan
    // targets for display. Independent of the heater/sensor branches above —
    // a printer can have both a chamber heater AND a separate cooling fan.
    if (!chamber_cooling_fan_name_.empty() && status.contains(chamber_cooling_fan_name_)) {
        const auto& fan = status[chamber_cooling_fan_name_];
        if (fan.contains("target") && fan["target"].is_number()) {
            int target_deci = helix::units::json_to_decidegrees(fan, "target");
            if (lv_subject_get_int(&chamber_fan_target_) != target_deci) {
                lv_subject_set_int(&chamber_fan_target_, target_deci);
                spdlog::trace("[PrinterTemperatureState] Chamber cooling-fan target: {}.{}C",
                              target_deci / 10, target_deci % 10);
            }
        }
    }

    // Effective chamber setpoint + control mode: delegate to the single source of
    // truth in ui_temperature_utils so the display and data layers can never
    // diverge. Recomputed on EVERY status update so external sets (Mainsail/
    // startup, partial subscription updates touching only one source subject) are
    // always reflected. See chamber_effective_setpoint() for the full rule set.
    auto sp = helix::ui::temperature::chamber_effective_setpoint(
        lv_subject_get_int(&chamber_target_), lv_subject_get_int(&chamber_fan_target_),
        chamber_fan_resting_deci_);
    lv_subject_set_int(&chamber_effective_target_, sp.deci);
    lv_subject_set_int(&chamber_mode_, sp.mode);
}

} // namespace helix
