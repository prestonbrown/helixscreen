// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_auto_state.h"

#include "color_utils.h"
#include "config.h"
#include "core/observer_factory.h"
#include "json_utils.h"
#include "led/led_color_utils.h"
#include "led/led_controller.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::led {

LedAutoState& LedAutoState::instance() {
    static LedAutoState s_instance;
    return s_instance;
}

void LedAutoState::init(PrinterState& printer_state) {
    // Clean up any stale observers from a previous init() call to prevent
    // deferred callbacks from firing with outdated state
    unsubscribe_observers();
    last_applied_key_.clear();

    printer_state_ = &printer_state;

    load_config();

    if (enabled_) {
        subscribe_observers();
    }

    initialized_ = true;
    spdlog::info("[LedAutoState] Initialized (enabled={})", enabled_);
}

void LedAutoState::deinit() {
    unsubscribe_observers();

    printer_state_ = nullptr;
    initialized_ = false;
    enabled_ = false;
    last_applied_key_.clear();
    mappings_.clear();
    strips_.clear();

    spdlog::info("[LedAutoState] Deinitialized");
}

void LedAutoState::set_enabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }

    enabled_ = enabled;
    spdlog::info("[LedAutoState] Set enabled={}", enabled);

    if (enabled && printer_state_) {
        subscribe_observers();
        // Evaluate immediately so LEDs reflect current state
        evaluate();
    } else {
        unsubscribe_observers();
        last_applied_key_.clear();
    }
}

void LedAutoState::set_mapping(const std::string& state_key, const LedStateAction& action) {
    mappings_[state_key] = action;
}

const LedStateAction* LedAutoState::get_mapping(const std::string& state_key) const {
    auto it = mappings_.find(state_key);
    if (it != mappings_.end()) {
        return &it->second;
    }
    return nullptr;
}

void LedAutoState::evaluate() {
    if (!enabled_ || !initialized_) {
        return;
    }

    // Reset dedup so the current state gets applied
    last_applied_key_.clear();
    on_state_changed();
}

void LedAutoState::on_state_changed() {
    if (!enabled_ || !initialized_) {
        return;
    }

    std::string key = compute_state_key();
    if (key == last_applied_key_) {
        return; // Deduplicate — same state, no re-apply
    }

    auto it = mappings_.find(key);
    if (it != mappings_.end()) {
        spdlog::info("[LedAutoState] State changed to '{}', applying action (type={})", key,
                     it->second.action_type);
        apply_action(it->second);
        last_applied_key_ = key;
    } else {
        spdlog::debug("[LedAutoState] State '{}' has no mapping, skipping", key);
    }
}

std::string LedAutoState::compute_state_key() const {
    if (!printer_state_) {
        return "idle";
    }

    // Check klippy state first — error takes priority
    auto* klippy_subj = printer_state_->network_state().get_klippy_state_subject();
    if (klippy_subj) {
        auto klippy = static_cast<KlippyState>(lv_subject_get_int(klippy_subj));
        if (klippy == KlippyState::ERROR) {
            return "error";
        }
    }

    // Check print job state
    if (printer_state_->are_subjects_initialized()) {
        // RAW_PRINT_STATE_OK: these names are LED THEME KEYS (see the JSON
        // themes), not internal state. There is no "preparing" key, and a
        // pre-print block already falls through to "heating" below, which is
        // what the machine is in fact doing.
        auto print_state = printer_state_->print_state().get_print_job_state();
        switch (print_state) {
        case PrintJobState::PRINTING:
            return "printing";
        case PrintJobState::PAUSED:
            return "paused";
        case PrintJobState::COMPLETE:
            return "complete";
        case PrintJobState::ERROR:
            return "error";
        // RAW_PRINT_STATE_OK: theme-key arms; see the note above the switch.
        case PrintJobState::STANDBY:
        case PrintJobState::CANCELLED:
            break; // Fall through to heating/idle check
        }
    }

    // Check if heating (extruder target > 0 and not printing)
    auto* ext_target_subj =
        printer_state_->temperature_state().get_active_extruder_target_subject();
    if (ext_target_subj) {
        int target_deci = lv_subject_get_int(ext_target_subj);
        if (target_deci > 0) {
            return "heating";
        }
    }

    return "idle";
}

std::vector<std::string> LedAutoState::targets() const {
    auto& ctrl = LedController::instance();
    const auto switchable = ctrl.switchable_ids();
    std::vector<std::string> out;
    for (const auto& id : strips_) {
        if (std::find(switchable.begin(), switchable.end(), id) != switchable.end()) {
            out.push_back(id);
        }
    }
    return out.empty() ? ctrl.light_targets("") : out;
}

void LedAutoState::apply_action(const LedStateAction& action) {
    auto& ctrl = LedController::instance();

    // silent=true on every send: these fire on printer-state transitions, not
    // user input, so the busy-queue toast (which exists to explain a command
    // the user made) must not be claimed by them — and the print-state change
    // at every print start lands squarely inside the busy gate's window.
    if (action.action_type == "off") {
        ctrl.set_power(targets(), false, /*silent=*/true);
    } else if (action.action_type == "color") {
        ctrl.set_look(targets(), action.color, 0.0, action.brightness, /*silent=*/true);
    } else if (action.action_type == "brightness") {
        ctrl.set_brightness(targets(), action.brightness, /*silent=*/true);
    } else if (action.action_type == "effect") {
        ctrl.effects().activate_effect(action.effect_name, nullptr, nullptr, nullptr, true,
                                       /*silent=*/true);
    } else if (action.action_type == "wled_preset") {
        for (const auto& id : targets()) {
            if (ctrl.backend_for_strip(id) == LedBackendType::WLED) {
                ctrl.wled().set_preset(id, action.wled_preset);
            }
        }
    } else if (action.action_type == "macro") {
        ctrl.macro().execute_custom_action(action.macro_gcode);
    } else {
        spdlog::warn("[LedAutoState] Unknown action type: '{}'", action.action_type);
    }
}

void LedAutoState::setup_default_mappings() {
    mappings_["idle"] = {"color", 0xFFFFFF, 50, "", 0, ""};
    mappings_["heating"] = {"color", 0xFFD700, 100, "", 0, ""};
    mappings_["printing"] = {"color", 0xFFFFFF, 100, "", 0, ""};
    mappings_["paused"] = {"color", 0xFFD700, 50, "", 0, ""};
    mappings_["error"] = {"color", 0xFF0000, 100, "", 0, ""};
    mappings_["complete"] = {"color", 0x66BB6A, 100, "", 0, ""};
}

void LedAutoState::subscribe_observers() {
    if (!printer_state_) {
        return;
    }

    using helix::ui::observe;

    // RAW_PRINT_STATE_OK: the LED state names are theme keys keyed off what the
    // printer reports; see state_name_for_theme() below.
    auto* print_subj = printer_state_->print_state().get_print_state_enum_subject();
    if (print_subj) {
        print_state_observer_ = observe<int>(
            print_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); },
            printer_state_->get_subjects_lifetime());
    }

    auto* klippy_subj = printer_state_->network_state().get_klippy_state_subject();
    if (klippy_subj) {
        klippy_state_observer_ = observe<int>(
            klippy_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); },
            printer_state_->get_subjects_lifetime());
    }

    auto* ext_target_subj =
        printer_state_->temperature_state().get_active_extruder_target_subject();
    if (ext_target_subj) {
        extruder_target_observer_ = observe<int>(
            ext_target_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); },
            printer_state_->get_subjects_lifetime());
    }

    spdlog::debug("[LedAutoState] Subscribed to printer state observers");
}

void LedAutoState::unsubscribe_observers() {
    print_state_observer_.reset();
    klippy_state_observer_.reset();
    extruder_target_observer_.reset();

    spdlog::debug("[LedAutoState] Unsubscribed from printer state observers");
}

// ============================================================================
// Config persistence
// ============================================================================

void LedAutoState::load_config() {
    auto* cfg = Config::get_instance();

    // NOTE: the one-time fold of the legacy /led/auto_state/ block into the
    // active printer lives in migrate_v19_to_v20() (config.cpp). It used to run
    // here on every load_config(), and its get_json() probes re-created the
    // /led orphan on every boot (#1129).

    // Enabled flag
    enabled_ = cfg->get<bool>(cfg->df() + "leds/auto_state/enabled", false);
    strips_ = cfg->get_string_array(cfg->df() + kAutoStateStripsPath);

    // Mappings
    mappings_.clear();
    const nlohmann::json* mappings_json = cfg->try_get_json(cfg->df() + "leds/auto_state/mappings");
    if (mappings_json != nullptr && mappings_json->is_object()) {
        for (auto it = mappings_json->begin(); it != mappings_json->end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            LedStateAction action;
            action.action_type = helix::json_util::safe_string(it.value(), "action", "color");
            // Color: accept both integer (legacy) and "#RRGGBB" string
            auto color_it = it.value().find("color");
            if (color_it != it.value().end()) {
                if (color_it->is_number()) {
                    action.color = static_cast<uint32_t>(color_it->get<int>());
                } else if (color_it->is_string()) {
                    uint32_t rgb = 0;
                    if (helix::parse_hex_color(color_it->get<std::string>().c_str(), rgb)) {
                        action.color = rgb;
                    }
                }
            }
            action.brightness = helix::json_util::safe_int(it.value(), "brightness", 100);
            action.effect_name = helix::json_util::safe_string(it.value(), "effect_name", "");
            action.wled_preset = helix::json_util::safe_int(it.value(), "wled_preset", 0);
            action.macro_gcode = helix::json_util::safe_string(it.value(), "macro_gcode", "");
            mappings_[it.key()] = action;
        }
    }

    // If no mappings were loaded, use defaults
    if (mappings_.empty()) {
        setup_default_mappings();
    }

    spdlog::debug("[LedAutoState] Loaded config: enabled={}, {} mappings", enabled_,
                  mappings_.size());
}

void LedAutoState::save_config() {
    auto* cfg = Config::get_instance();

    cfg->set(cfg->df() + "leds/auto_state/enabled", enabled_);

    nlohmann::json mappings_json = nlohmann::json::object();
    for (const auto& [key, action] : mappings_) {
        nlohmann::json obj;
        obj["action"] = action.action_type;
        obj["color"] = helix::color_to_hex_string(action.color);
        obj["brightness"] = action.brightness;
        if (!action.effect_name.empty()) {
            obj["effect_name"] = action.effect_name;
        }
        if (action.wled_preset != 0) {
            obj["wled_preset"] = action.wled_preset;
        }
        if (!action.macro_gcode.empty()) {
            obj["macro_gcode"] = action.macro_gcode;
        }
        mappings_json[key] = obj;
    }
    cfg->set(cfg->df() + "leds/auto_state/mappings", mappings_json);

    // An absent key already means the chamber light, and an array there marks the
    // legacy selection as migrated, so an empty list is only written over an array.
    const nlohmann::json* saved_strips = cfg->try_get_json(cfg->df() + kAutoStateStripsPath);
    if (!strips_.empty() || (saved_strips != nullptr && saved_strips->is_array())) {
        cfg->set(cfg->df() + kAutoStateStripsPath, nlohmann::json(strips_));
    }

    cfg->save();
    spdlog::debug("[LedAutoState] Saved config");
}

void stage_light_selection(const SelectionMigration& m) {
    auto* cfg = Config::get_instance();
    // Written directly: LedAutoState::save_config() on an uninitialised instance
    // would save empty mappings over the user's.
    cfg->set(cfg->df() + kAutoStateStripsPath, nlohmann::json(m.auto_state_strips));
    if (!m.light_button.empty()) {
        cfg->set(cfg->df() + kLightButtonPendingPath, m.light_button);
    }
    auto& auto_state = LedAutoState::instance();
    if (auto_state.is_initialized()) {
        auto_state.set_strips(m.auto_state_strips);
    }
    cfg->save();
}

} // namespace helix::led
