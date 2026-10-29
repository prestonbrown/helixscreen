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

/// The sentence the user reads for a latched rejection. The fault cause names
/// the classified fault when the backend reported one.
static std::string chamber_rejection_text(chamber::CommandRejection kind,
                                          chamber::RefusalCause cause, const char* fault_text) {
    switch (kind) {
    case chamber::CommandRejection::Conflict:
        return lv_tr("The heater didn't accept the command.");
    case chamber::CommandRejection::Auth:
        return lv_tr("The heater did not accept the printer's credentials.");
    case chamber::CommandRejection::Refused:
        switch (cause) {
        case chamber::RefusalCause::TurnedOff:
            return lv_tr("The heater was turned off at the heater.");
        case chamber::RefusalCause::Fault:
            return fault_text && fault_text[0]
                       ? fmt::format(fmt::runtime(lv_tr("The heater reports a fault: {}.")),
                                     fault_text)
                       : std::string(lv_tr("The heater reports a fault."));
        case chamber::RefusalCause::Inhibited:
            return lv_tr("The heater is inhibited.");
        case chamber::RefusalCause::Unspecified:
            break;
        }
        return lv_tr("The heater didn't accept the command.");
    case chamber::CommandRejection::None:
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

    lv_subject_init_int(&active_extruder_pa_, -1);
    subjects_.register_subject(&active_extruder_pa_,
                               register_xml ? "extruder_pressure_advance" : nullptr);
    if (register_xml) {
        helix::xml::register_subject_in_current_scope("extruder_pressure_advance",
                                                      &active_extruder_pa_);
    }

    // Heater duty cycle, whole percent, -1 = the heater reports none.
    INIT_SUBJECT_INT(bed_power, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_power, -1, subjects_,
                     register_xml); // SUBJECT_OK: read by pointer via
                                    // PrinterState::get_heater_power_subject(HeaterType::Chamber)

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
    INIT_SUBJECT_INT(chamber_heater_command_rejected, 0, subjects_, register_xml);
    chamber_heater_command_rejected_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT(chamber_heater_rejection_seq, 0, subjects_, false);
    chamber_heater_rejection_seq_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_STRING(chamber_heater_rejection_text, "", subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_dryer_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(chamber_dryer_reported, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(chamber_dryer_text, "", subjects_, register_xml);

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
    if (chamber_heater_command_rejected_lifetime_)
        *chamber_heater_command_rejected_lifetime_ = false;
    chamber_heater_command_rejected_lifetime_.reset();
    if (chamber_heater_rejection_seq_lifetime_)
        *chamber_heater_rejection_seq_lifetime_ = false;
    chamber_heater_rejection_seq_lifetime_.reset();
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
    for (auto& [name, info] : extruders_) {
        if (info.temp_lifetime)
            *info.temp_lifetime = false;
        info.temp_lifetime.reset();
        if (info.target_lifetime)
            *info.target_lifetime = false;
        info.target_lifetime.reset();
        if (info.power_lifetime)
            *info.power_lifetime = false;
        info.power_lifetime.reset();
    }

    // Now safe to deinit dynamic per-extruder subjects
    for (auto& [name, info] : extruders_) {
        if (info.temp_subject) {
            lv_subject_deinit(info.temp_subject.get());
        }
        if (info.target_subject) {
            lv_subject_deinit(info.target_subject.get());
        }
        if (info.power_subject) {
            lv_subject_deinit(info.power_subject.get());
        }
    }
    extruders_.clear();

    // Reset active extruder to default
    active_extruder_name_ = "extruder";

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

namespace {

/// K as the active pressure-advance subject stores it: x 10000.
int pa_to_subject(float k) {
    return static_cast<int>(std::lround(k * 10000.0f));
}

/// Single extruder: "Nozzle". Multiple: "Nozzle 1", "Nozzle 2", ... in the
/// current language.
std::string extruder_display_name(size_t index, bool multi) {
    if (!multi) {
        return lv_tr("Nozzle");
    }
    return std::string(lv_tr("Nozzle")) + " " +
           helix::ui::lane_number_text(static_cast<int>(index));
}

} // namespace

void PrinterTemperatureState::init_extruders(const std::vector<std::string>& heaters) {
    // Accept "extruder" and "extruderN" (digit suffix), reject "extruder_stepper" etc.
    std::vector<std::string> extruder_names;
    for (const auto& name : heaters) {
        if (is_extruder_name(name)) {
            extruder_names.push_back(name);
        }
    }

    // Discovery re-runs on every klippy ready. A tool still present keeps its
    // subjects and lifetimes, so every observer bound to it stays live; only a
    // tool that disappeared is freed.
    bool changed = false;
    for (auto it = extruders_.begin(); it != extruders_.end();) {
        if (std::find(extruder_names.begin(), extruder_names.end(), it->first) !=
            extruder_names.end()) {
            ++it;
            continue;
        }
        // Signal subject death FIRST - sets the pointed-to bool to false so that
        // ALL ObserverGuards (including those in other services that still hold
        // shared_ptr copies) detect the subject as dead and skip
        // lv_observer_remove() on the about-to-be-freed observers. (#816)
        auto& info = it->second;
        for (auto* lt : {&info.temp_lifetime, &info.target_lifetime, &info.power_lifetime}) {
            if (*lt)
                **lt = false;
            lt->reset();
        }
        for (auto* subj : {&info.temp_subject, &info.target_subject, &info.power_subject}) {
            if (*subj)
                lv_subject_deinit(subj->get());
        }
        it = extruders_.erase(it);
        changed = true;
    }

    for (const auto& name : extruder_names) {
        if (extruders_.count(name) != 0) {
            continue;
        }
        ExtruderInfo info;
        info.name = name;

        // Create heap-allocated subjects (stable across rehash)
        info.temp_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.temp_subject.get(), 0);
        info.temp_lifetime = std::make_shared<bool>(true);

        info.target_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.target_subject.get(), 0);
        info.target_lifetime = std::make_shared<bool>(true);

        // Duty in whole percent, -1 until the heater reports one - the same
        // unknown-sentinel the active-extruder and bed power subjects use.
        info.power_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.power_subject.get(), -1);
        info.power_lifetime = std::make_shared<bool>(true);

        spdlog::trace("[PrinterTemperatureState] Registered extruder: {}", name);
        extruders_.emplace(name, std::move(info));
        changed = true;
    }

    // The tool count decides between "Nozzle" and "Nozzle N", so a survivor's
    // label can change when another tool comes or goes.
    refresh_display_names();

    // The version tells consumers the extruder SET changed; an unchanged set
    // leaves every binding valid, so it does not bump.
    if (changed) {
        lv_subject_set_int(&extruder_version_, lv_subject_get_int(&extruder_version_) + 1);
    }
    spdlog::debug("[PrinterTemperatureState] Initialized {} extruders (version {}{})",
                  extruders_.size(), lv_subject_get_int(&extruder_version_),
                  changed ? "" : ", unchanged");
}

void PrinterTemperatureState::refresh_display_names() {
    std::vector<std::string> names;
    names.reserve(extruders_.size());
    for (const auto& [name, info] : extruders_) {
        names.push_back(name);
    }
    // Same order init_extruders() numbers them in.
    std::sort(names.begin(), names.end());
    const bool multi = names.size() > 1;
    for (size_t i = 0; i < names.size(); ++i) {
        extruders_[names[i]].display_name = extruder_display_name(i, multi);
    }
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

lv_subject_t* PrinterTemperatureState::get_extruder_power_subject(const std::string& name,
                                                                  SubjectLifetime& lifetime) {
    auto it = extruders_.find(name);
    if (it != extruders_.end() && it->second.power_subject) {
        lifetime = it->second.power_lifetime;
        return it->second.power_subject.get();
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
        lv_subject_set_int(&active_extruder_target_, new_target);
    }
    // Duty resyncs with them: Moonraker deltas omit unchanged fields, so the
    // new tool may not carry a power frame for a long time, and without this
    // the mirror keeps displaying the previous tool's duty.
    if (info.power_subject) {
        int new_power = lv_subject_get_int(info.power_subject.get());
        lv_subject_set_int(&active_extruder_power_, new_power);
    }
    lv_subject_set_int(&active_extruder_pa_,
                       info.pressure_advance < 0.0f ? -1 : pa_to_subject(info.pressure_advance));
}

std::optional<float>
PrinterTemperatureState::extruder_pressure_advance(const std::string& name) const {
    auto it = extruders_.find(name);
    if (it == extruders_.end() || it->second.pressure_advance < 0.0f) {
        return std::nullopt;
    }
    return it->second.pressure_advance;
}

const std::string& PrinterTemperatureState::active_extruder_name() const {
    return active_extruder_name_;
}

void PrinterTemperatureState::update_from_status(const nlohmann::json& status) {
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
        lv_subject_set_int(subject, pct);
    };

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
            lv_subject_set_int(info.target_subject.get(), target_deci);
        }

        if (data.contains("pressure_advance") && data["pressure_advance"].is_number()) {
            info.pressure_advance = data["pressure_advance"].get<float>();
        }

        if (data.contains("nozzle_diameter") && data["nozzle_diameter"].is_number()) {
            const double d = data["nozzle_diameter"].get<double>();
            if (std::isfinite(d) && d > 0.0) {
                info.nozzle_diameter = d;
            }
        }
        if (data.contains("nozzle_volume_type") && data["nozzle_volume_type"].is_string()) {
            info.nozzle_volume_type = data["nozzle_volume_type"].get<std::string>();
        }

        publish_power(name, info.power_subject.get());
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
            lv_subject_set_int(&active_extruder_target_, target_deci);
        }

        if (active.contains("pressure_advance") && active["pressure_advance"].is_number()) {
            lv_subject_set_int(&active_extruder_pa_,
                               pa_to_subject(active["pressure_advance"].get<float>()));
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
                    } else if (++chamber_offline_run_ >= kChamberOfflineConsecutiveReports) {
                        lv_subject_set_int(&chamber_heater_offline_, 1);
                    }
                }
                // Another controller is driving the heater: the device's own
                // web UI or a button on the unit. Informational, not a fault.
                if (d->externally_controlled.has_value()) {
                    lv_subject_set_int(&chamber_heater_externally_controlled_,
                                       *d->externally_controlled ? 1 : 0);
                }
                if (d->drying_active.has_value()) {
                    chamber_dryer_running_ = *d->drying_active;
                    lv_subject_set_int(&chamber_dryer_reported_, 1);
                }
                if (d->drying_remaining_s.has_value()) {
                    chamber_dryer_remaining_s_ = *d->drying_remaining_s;
                    chamber_dryer_remaining_reported_ = true;
                }
                if (d->drying_target_c.has_value()) {
                    chamber_dryer_target_c_ = *d->drying_target_c;
                }
                if (d->link_error.has_value() && !d->link_error->empty()) {
                    spdlog::debug("[PrinterTemperatureState] Chamber heater link error: "
                                  "backend={} detail={}",
                                  backend->id(), *d->link_error);
                }
                if (d->wait_active.has_value()) {
                    chamber_wait_active_ = *d->wait_active;
                }
                if (d->wait_target_c.has_value()) {
                    chamber_wait_target_c_ = *d->wait_target_c;
                }
                apply_chamber_rejection(*d, backend->id());
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
    // is one, so the switch state agrees with the percent beside it. Backends
    // with a pin but no reported speed fall back to the pin. Neither surface
    // in the frame (delta) leaves the subjects at their last values.
    if (chamber_filter_fan_percent_ >= 0 ||
        lv_subject_get_int(&chamber_filter_fan_requested_) >= 0) {
        const int running = chamber_filter_fan_percent_ >= 0
                                ? (chamber_filter_fan_percent_ > 0 ? 1 : 0)
                                : lv_subject_get_int(&chamber_filter_fan_requested_);
        lv_subject_set_int(&chamber_filter_fan_on_, running);
    }

    // The dryer readout carries the live chamber temperature, so it follows
    // every frame, not only the ones that mention the cycle.
    if (chamber_dryer_running_ || lv_subject_get_int(&chamber_dryer_active_) != 0) {
        publish_chamber_dryer();
    }

    release_chamber_rejection_if_target_followed();

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

// A rejection latches from the first report of one until the device says
// nothing is outstanding or the heater's target follows the request. The
// appliance retries a conflict on its own, so a latch that clears within a
// frame or two is the common case and only the log hears of it.
void PrinterTemperatureState::apply_chamber_rejection(const chamber::ChamberHeaterDiagnostics& d,
                                                      std::string_view backend_id) {
    if (d.rejection_target_c.has_value()) {
        chamber_rejection_target_c_ = *d.rejection_target_c;
    }
    bool new_event = false;
    if (d.rejection_seq.has_value() && *d.rejection_seq != chamber_rejection_vendor_seq_) {
        new_event = chamber_rejection_vendor_seq_ >= 0;
        chamber_rejection_vendor_seq_ = *d.rejection_seq;
    }
    if (d.rejection.has_value()) {
        chamber_rejection_kind_ = *d.rejection;
        chamber_refusal_cause_ = d.refusal_cause.value_or(chamber::RefusalCause::Unspecified);
    }
    if (chamber_rejection_kind_ == chamber::CommandRejection::None) {
        clear_chamber_rejection();
        return;
    }
    if (!chamber_rejection_latched_ || new_event) {
        const std::string code = d.rejection_code.value_or("");
        if (!chamber_rejection_latched_) {
            spdlog::warn("[PrinterTemperatureState] Chamber heater rejected a target: backend={} "
                         "code={} seq={} requested={}",
                         backend_id, code, chamber_rejection_vendor_seq_,
                         chamber_rejection_target_c_);
        } else {
            spdlog::debug("[PrinterTemperatureState] Chamber heater rejected again: backend={} "
                          "code={} seq={}",
                          backend_id, code, chamber_rejection_vendor_seq_);
        }
        chamber_rejection_latched_ = true;
        chamber_target_at_rejection_ = lv_subject_get_int(&chamber_target_);
        lv_subject_set_int(&chamber_heater_rejection_seq_,
                           lv_subject_get_int(&chamber_heater_rejection_seq_) + 1);
    }
    const std::string text =
        chamber_rejection_text(chamber_rejection_kind_, chamber_refusal_cause_,
                               lv_subject_get_string(&chamber_heater_fault_reason_text_));
    if (text != lv_subject_get_string(&chamber_heater_rejection_text_)) {
        lv_subject_copy_string(&chamber_heater_rejection_text_, text.c_str());
    }
    lv_subject_set_int(&chamber_heater_command_rejected_, 1);
}

// The heater taking a target is the answer a rejection waits for: the one we
// asked for when the device named it, otherwise any new nonzero target.
void PrinterTemperatureState::release_chamber_rejection_if_target_followed() {
    if (!chamber_rejection_latched_) {
        return;
    }
    const int target = lv_subject_get_int(&chamber_target_);
    if (target <= 0) {
        return;
    }
    const bool followed =
        !std::isnan(chamber_rejection_target_c_) && chamber_rejection_target_c_ > 0
            ? std::abs(target - static_cast<int>(std::lround(chamber_rejection_target_c_ * 10))) <=
                  5
            : target != chamber_target_at_rejection_;
    if (followed) {
        spdlog::info("[PrinterTemperatureState] Chamber heater took target {}°C; rejection cleared",
                     target / 10.0);
        chamber_rejection_kind_ = chamber::CommandRejection::None;
        clear_chamber_rejection();
    }
}

void PrinterTemperatureState::clear_chamber_rejection() {
    if (!chamber_rejection_latched_) {
        return;
    }
    chamber_rejection_latched_ = false;
    lv_subject_set_int(&chamber_heater_command_rejected_, 0);
    lv_subject_copy_string(&chamber_heater_rejection_text_, "");
}

// The actual chamber temperature sits beside the cycle's target: an appliance
// alone often plateaus below target, and the readout shows that as a number
// rather than a stall. Nothing waits on the target; the appliance's own
// countdown ends the cycle. The countdown uses the AMS dryer's "H:MM left",
// minutes rounded up so the last minute reads 0:01, and is omitted until the
// device reports one.
void PrinterTemperatureState::publish_chamber_dryer() {
    lv_subject_set_int(&chamber_dryer_active_, chamber_dryer_running_ ? 1 : 0);
    if (!chamber_dryer_running_) {
        lv_subject_copy_string(&chamber_dryer_text_, "");
        return;
    }
    const int current_c = static_cast<int>(std::lround(lv_subject_get_int(&chamber_temp_) / 10.0));
    // The compact "41/55°C" is deliberate: the chamber card's dryer row is one
    // line beside a Stop button, and its longest readout ("59/60°C  12:00 left")
    // ellipsizes at 480x320 with the spaced pair format_temperature_pair() draws.
    std::string text = chamber_dryer_target_c_ > 0
                           ? fmt::format("{}/{}°C", current_c, chamber_dryer_target_c_)
                           : fmt::format("{}°C", current_c);
    const int minutes = (chamber_dryer_remaining_s_ + 59) / 60;
    if (minutes > 0) {
        text += fmt::format("  {}:{:02d} {}", minutes / 60, minutes % 60, lv_tr("left"));
    }
    lv_subject_copy_string(&chamber_dryer_text_, text.c_str());
}

} // namespace helix
