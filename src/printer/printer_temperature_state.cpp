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

#include "chamber_heater_backend.h"
#include "display_numbering.h"
#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "state/subject_macros.h"
#include "unit_conversions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

/// Generic fault kind -> translated UI phrase. The banner binds this; the raw
/// vendor code behind the kind goes to the log, never the screen.
static const char* chamber_fault_reason_text(chamber::FaultReason kind) {
    switch (kind) {
    case chamber::FaultReason::Overtemp:
        return lv_tr("Heater over-temperature");
    case chamber::FaultReason::SensorFault:
        return lv_tr("Heater sensor fault");
    case chamber::FaultReason::CommsLoss:
        return lv_tr("Heater connection lost");
    case chamber::FaultReason::Other:
        return lv_tr("Heater fault");
    case chamber::FaultReason::None:
        break;
    }
    return "";
}

void PrinterTemperatureState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterTemperatureState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterTemperatureState] Initializing subjects (register_xml={})",
                  register_xml);

    // Active extruder subjects (track whichever extruder is currently active).
    // The XML names are "extruder_temp"/"extruder_target" while the members carry
    // the active_ prefix, so INIT_SUBJECT_INT (which derives one from the other)
    // does not fit. The name still goes to the manager: it withdraws each name
    // before freeing its subject, and a PrinterState owned by a stack-allocated
    // test fixture does not outlive the process, so without it the XML scope
    // keeps resolving these names into storage that has gone away.
    lv_subject_init_int(&active_extruder_temp_, 0);
    subjects_.register_subject(&active_extruder_temp_, register_xml ? "extruder_temp" : nullptr);
    if (register_xml) {
        helix::xml::register_subject_in_current_scope("extruder_temp", &active_extruder_temp_);
    }

    lv_subject_init_int(&active_extruder_target_, 0);
    subjects_.register_subject(&active_extruder_target_,
                               register_xml ? "extruder_target" : nullptr);
    if (register_xml) {
        helix::xml::register_subject_in_current_scope("extruder_target", &active_extruder_target_);
    }

    lv_subject_init_int(&active_extruder_power_, -1);
    subjects_.register_subject(&active_extruder_power_, register_xml ? "extruder_power" : nullptr);
    if (register_xml) {
        helix::xml::register_subject_in_current_scope("extruder_power", &active_extruder_power_);
    }

    // Heater duty cycle, whole percent, -1 = the heater reports none.
    INIT_SUBJECT_INT(bed_power, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_power, -1, subjects_, register_xml);

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

    // Chamber-heater diagnostics subjects (backend-provided; ints that can be
    // legitimately absent default to -1 "unknown", flags to 0, strings to "").
    INIT_SUBJECT_INT(chamber_heater_fault, 0, subjects_, register_xml);
    chamber_heater_fault_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_heater_inhibited, 0, subjects_, register_xml);
    chamber_heater_inhibited_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_heater_offline, 0, subjects_, register_xml);
    chamber_heater_offline_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_heater_externally_controlled, 0, subjects_, register_xml);
    chamber_heater_externally_controlled_lifetime_ = std::make_shared<bool>(true);
    // Translated UI text derived from the backend's generic FaultReason kind —
    // vendor codes die at the backend border and only surface in logs.
    INIT_SUBJECT_STRING(chamber_heater_fault_reason_text, "", subjects_, register_xml);
    chamber_heater_fault_reason_text_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_filter_fan_on, -1, subjects_, register_xml);
    chamber_filter_fan_on_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_filter_fan_requested, -1, subjects_, register_xml);
    chamber_filter_fan_requested_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_filter_fan_device_driven, 0, subjects_, register_xml);
    chamber_filter_fan_device_driven_lifetime_ = std::make_shared<bool>(true);
    chamber_filter_fan_percent_ = -1;
    // Display strings written alongside the raw ints — XML has no deci/percent
    // formatter (bind_text-fmt prints the raw int), and the fan toggle needs a
    // translated On/Off label, so the parse block owns the formatting.
    INIT_SUBJECT_STRING(chamber_heater_element_temp_text, "--", subjects_, register_xml);
    chamber_heater_element_temp_text_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_STRING(chamber_filter_fan_percent_text, "--", subjects_, register_xml);
    chamber_filter_fan_percent_text_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_STRING(chamber_filter_fan_on_text, lv_tr("Filter Fan: Off"), subjects_,
                        register_xml);
    chamber_filter_fan_on_text_lifetime_ = std::make_shared<bool>(true);
    // Icon-name subject for the compact portrait card's icon-button toggle
    // (bind_icon); mirrors chamber_filter_fan_on_text, set from the same
    // running state.
    INIT_SUBJECT_STRING(chamber_filter_fan_icon, "fan_off", subjects_, register_xml);
    chamber_filter_fan_icon_lifetime_ = std::make_shared<bool>(true);

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
    if (chamber_heater_fault_lifetime_)
        *chamber_heater_fault_lifetime_ = false;
    chamber_heater_fault_lifetime_.reset();
    if (chamber_heater_inhibited_lifetime_)
        *chamber_heater_inhibited_lifetime_ = false;
    chamber_heater_inhibited_lifetime_.reset();
    if (chamber_heater_offline_lifetime_)
        *chamber_heater_offline_lifetime_ = false;
    chamber_heater_offline_lifetime_.reset();
    if (chamber_heater_externally_controlled_lifetime_)
        *chamber_heater_externally_controlled_lifetime_ = false;
    chamber_heater_externally_controlled_lifetime_.reset();
    if (chamber_heater_fault_reason_text_lifetime_)
        *chamber_heater_fault_reason_text_lifetime_ = false;
    chamber_heater_fault_reason_text_lifetime_.reset();
    if (chamber_filter_fan_on_lifetime_)
        *chamber_filter_fan_on_lifetime_ = false;
    chamber_filter_fan_on_lifetime_.reset();
    if (chamber_filter_fan_requested_lifetime_)
        *chamber_filter_fan_requested_lifetime_ = false;
    chamber_filter_fan_requested_lifetime_.reset();
    if (chamber_filter_fan_device_driven_lifetime_)
        *chamber_filter_fan_device_driven_lifetime_ = false;
    chamber_filter_fan_device_driven_lifetime_.reset();
    if (chamber_heater_element_temp_text_lifetime_)
        *chamber_heater_element_temp_text_lifetime_ = false;
    chamber_heater_element_temp_text_lifetime_.reset();
    if (chamber_filter_fan_percent_text_lifetime_)
        *chamber_filter_fan_percent_text_lifetime_ = false;
    chamber_filter_fan_percent_text_lifetime_.reset();
    if (chamber_filter_fan_on_text_lifetime_)
        *chamber_filter_fan_on_text_lifetime_ = false;
    chamber_filter_fan_on_text_lifetime_.reset();
    if (chamber_filter_fan_icon_lifetime_)
        *chamber_filter_fan_icon_lifetime_ = false;
    chamber_filter_fan_icon_lifetime_.reset();
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
            info.display_name = std::string(lv_tr("Nozzle")) + " " +
                                helix::ui::lane_number_text(static_cast<int>(i));
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

    // Klipper reports duty as 0.0-1.0 on a heater object. Publish whole percent
    // and leave the subject alone when the field is absent: a frame that omits
    // it says nothing, and a temperature_fan never carries one at all.
    auto publish_power = [&status](const std::string& object, lv_subject_t* subject) {
        if (object.empty() || !status.contains(object)) {
            return;
        }
        const auto& obj = status[object];
        if (!obj.contains("power") || !obj["power"].is_number()) {
            return;
        }
        int pct = static_cast<int>(std::lround(obj["power"].get<double>() * 100.0));
        pct = std::clamp(pct, 0, 100);
        if (lv_subject_get_int(subject) != pct) {
            lv_subject_set_int(subject, pct);
        }
    };
    publish_power(active_extruder_name_, &active_extruder_power_);
    publish_power("heater_bed", &bed_power_);
    publish_power(chamber_heater_name_, &chamber_power_);

    // The chamber reading comes from whichever object owns it, and the target
    // from the heater alone. Those are the same object unless a sensor has
    // been assigned to the chamber role by hand, which is the one way a probe
    // outranks the heater that measures its own chamber.
    const std::string& chamber_source = chamber_temperature_source();
    if (!chamber_source.empty() && status.contains(chamber_source)) {
        const auto& chamber = status[chamber_source];

        if (chamber.contains("temperature") && chamber["temperature"].is_number()) {
            int temp_deci = helix::units::json_to_decidegrees(chamber, "temperature");
            if (lv_subject_get_int(&chamber_temp_) != temp_deci) {
                lv_subject_set_int(&chamber_temp_, temp_deci);
                spdlog::trace("[PrinterTemperatureState] Chamber temp ({}): {}.{}C", chamber_source,
                              temp_deci / 10, temp_deci % 10);
            }
        }
    }

    if (!chamber_heater_name_.empty() && status.contains(chamber_heater_name_)) {
        const auto& chamber = status[chamber_heater_name_];

        // A temperature_fan's target is a cooling threshold, not a heating
        // command: Klipper always reports its configured target_temp (40.0 on
        // the K1C) even at speed 0. Wherever discovery resolves a
        // temperature_fan into the heater slot (no heater_generic exists),
        // that target flows through the cooling-fan branch below instead and
        // is neutralized by the resting-target comparison in
        // chamber_effective_setpoint().
        if (chamber_heater_name_.rfind("temperature_fan ", 0) != 0 && chamber.contains("target") &&
            chamber["target"].is_number()) {
            int target_deci = helix::units::json_to_decidegrees(chamber, "target");
            if (lv_subject_get_int(&chamber_target_) != target_deci) {
                lv_subject_set_int(&chamber_target_, target_deci);
                spdlog::trace("[PrinterTemperatureState] Chamber target: {}.{}C", target_deci / 10,
                              target_deci % 10);
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

    // Chamber-heater diagnostics (backend-provided, capability-gated). Absent
    // object in a delta frame = no news, and the same holds field-wise: a
    // delta carries only changed fields, so every unengaged optional (the
    // frame did not mention it) leaves the subject at its last value. An
    // engaged unknown — empty reason, negative percent, NAN temp — is a real
    // report and does update. Vendor schema translation lives in the backend
    // (chamber_heater_backend.h).
    if (!chamber_diagnostics_object_.empty() && status.contains(chamber_diagnostics_object_)) {
        const auto* backend = chamber::backend_by_id(chamber_backend_id_);
        if (backend) {
            if (auto d = backend->parse_diagnostics(status[chamber_diagnostics_object_])) {
                if (d->fault.has_value()) {
                    lv_subject_set_int(&chamber_heater_fault_, *d->fault ? 1 : 0);
                }
                if (d->inhibited.has_value()) {
                    lv_subject_set_int(&chamber_heater_inhibited_, *d->inhibited ? 1 : 0);
                }
                if (d->fault_reason_kind.has_value()) {
                    lv_subject_copy_string(&chamber_heater_fault_reason_text_,
                                           chamber_fault_reason_text(*d->fault_reason_kind));
                }
                if (d->fault_reason.has_value() && !d->fault_reason->empty()) {
                    // Vendor code is log-only — the UI shows the translated kind.
                    spdlog::debug(
                        "[PrinterTemperatureState] Chamber heater fault: backend={} reason={}",
                        backend->id(), *d->fault_reason);
                }
                if (d->element_temp_c.has_value()) {
                    if (std::isnan(*d->element_temp_c)) {
                        lv_subject_copy_string(&chamber_heater_element_temp_text_, "--");
                    } else {
                        // Canonical decimal-drop rule (one decimal <100°C, whole
                        // degrees at/above) — format_temperature_f wraps
                        // format_temp_number plus the unit.
                        helix::ui::temperature::format_temperature_f(
                            static_cast<float>(*d->element_temp_c),
                            chamber_heater_element_temp_text_buf_,
                            sizeof(chamber_heater_element_temp_text_buf_));
                        lv_subject_copy_string(&chamber_heater_element_temp_text_,
                                               chamber_heater_element_temp_text_buf_);
                    }
                }
                if (d->filter_fan_percent.has_value()) {
                    lv_subject_copy_string(
                        &chamber_filter_fan_percent_text_,
                        *d->filter_fan_percent < 0
                            ? "--"
                            : fmt::format("{}%", *d->filter_fan_percent).c_str());
                    chamber_filter_fan_percent_ = *d->filter_fan_percent;
                }
                if (d->filter_fan_driver.has_value()) {
                    lv_subject_set_int(
                        &chamber_filter_fan_device_driven_,
                        *d->filter_fan_driver == chamber::FilterFanDriver::Device ? 1 : 0);
                }
                // Offline is asserted only on a report that the link is down.
                // A backend that never speaks to connectivity leaves this 0,
                // so a plain heater_generic chamber never claims to be offline.
                if (d->device_connected.has_value()) {
                    if (*d->device_connected) {
                        chamber_offline_run_ = 0;
                        lv_subject_set_int(&chamber_heater_offline_, 0);
                    } else if (++chamber_offline_run_ >= CHAMBER_OFFLINE_CONSECUTIVE_REPORTS) {
                        lv_subject_set_int(&chamber_heater_offline_, 1);
                    }
                }
                // Another controller is driving the heater: the device's own
                // web UI or a button on the unit. Informational, not a fault.
                if (d->externally_controlled.has_value()) {
                    lv_subject_set_int(&chamber_heater_externally_controlled_,
                                       *d->externally_controlled ? 1 : 0);
                }
                if (d->link_error.has_value() && !d->link_error->empty()) {
                    spdlog::debug("[PrinterTemperatureState] Chamber heater link error: "
                                  "backend={} detail={}",
                                  backend->id(), *d->link_error);
                }
            }
        }
    }
    if (!chamber_filter_fan_pin_.empty() && status.contains(chamber_filter_fan_pin_)) {
        const auto& pin = status[chamber_filter_fan_pin_];
        if (pin.contains("value") && pin["value"].is_number()) {
            // The pin is our REQUEST, not the fan: the device also runs this
            // fan on its own (heater warmup, thermal purge).
            lv_subject_set_int(&chamber_filter_fan_requested_,
                               pin["value"].get<double>() > 0.5 ? 1 : 0);
        }
    }
    // Filter-fan running state: the speed the backend reports wins when there
    // is one, so the label/icon agree with the percent beside them. Backends
    // with a pin but no reported speed fall back to the pin. Neither surface
    // in the frame (delta) leaves the subjects at their last values.
    if (chamber_filter_fan_percent_ >= 0 ||
        lv_subject_get_int(&chamber_filter_fan_requested_) >= 0) {
        const int running = chamber_filter_fan_percent_ >= 0
                                ? (chamber_filter_fan_percent_ > 0 ? 1 : 0)
                                : lv_subject_get_int(&chamber_filter_fan_requested_);
        lv_subject_set_int(&chamber_filter_fan_on_, running);
        lv_subject_copy_string(&chamber_filter_fan_on_text_,
                               running ? lv_tr("Filter Fan: On") : lv_tr("Filter Fan: Off"));
        lv_subject_copy_string(&chamber_filter_fan_icon_, running ? "fan" : "fan_off");
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
