// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_auto_state.h"

#include "color_utils.h"
#include "config.h"
#include "led/led_controller.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::led {

LedAutoState& LedAutoState::instance() {
    static LedAutoState s_instance;
    return s_instance;
}

void LedAutoState::init(PrinterState& printer_state) {
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
    auto* klippy_subj = printer_state_->get_klippy_state_subject();
    if (klippy_subj) {
        auto klippy = static_cast<KlippyState>(lv_subject_get_int(klippy_subj));
        if (klippy == KlippyState::ERROR) {
            return "error";
        }
    }

    // Check print job state
    auto* print_subj = printer_state_->get_print_state_enum_subject();
    if (print_subj) {
        auto print_state = static_cast<PrintJobState>(lv_subject_get_int(print_subj));
        switch (print_state) {
        case PrintJobState::PRINTING:
            return "printing";
        case PrintJobState::PAUSED:
            return "paused";
        case PrintJobState::COMPLETE:
            return "complete";
        case PrintJobState::ERROR:
            return "error";
        case PrintJobState::STANDBY:
        case PrintJobState::CANCELLED:
            break; // Fall through to heating/idle check
        }
    }

    // Check if heating (extruder target > 0 and not printing)
    auto* ext_target_subj = printer_state_->get_active_extruder_target_subject();
    if (ext_target_subj) {
        int target_centi = lv_subject_get_int(ext_target_subj);
        if (target_centi > 0) {
            return "heating";
        }
    }

    return "idle";
}

void LedAutoState::apply_action(const LedStateAction& action) {
    auto& ctrl = LedController::instance();

    if (action.action_type == "off") {
        ctrl.turn_off_all();
    } else if (action.action_type == "color") {
        double r = ((action.color >> 16) & 0xFF) / 255.0;
        double g = ((action.color >> 8) & 0xFF) / 255.0;
        double b = (action.color & 0xFF) / 255.0;
        double scale = action.brightness / 100.0;
        ctrl.set_color_all(r * scale, g * scale, b * scale, 0.0);
    } else if (action.action_type == "brightness") {
        ctrl.set_brightness_all(action.brightness);
    } else if (action.action_type == "effect") {
        ctrl.effects().activate_effect(action.effect_name);
        ctrl.sync_light_state(true);
    } else if (action.action_type == "wled_preset") {
        for (const auto& strip : ctrl.wled().strips()) {
            ctrl.wled().set_preset(strip.name, action.wled_preset);
        }
        ctrl.sync_light_state(true);
    } else if (action.action_type == "macro") {
        // Custom G-code — resulting light state is unknowable, skip sync
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

    using helix::ui::observe_int_sync;

    auto* print_subj = printer_state_->get_print_state_enum_subject();
    if (print_subj) {
        print_state_observer_ = observe_int_sync<LedAutoState>(
            print_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); });
    }

    auto* klippy_subj = printer_state_->get_klippy_state_subject();
    if (klippy_subj) {
        klippy_state_observer_ = observe_int_sync<LedAutoState>(
            klippy_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); });
    }

    auto* ext_target_subj = printer_state_->get_active_extruder_target_subject();
    if (ext_target_subj) {
        extruder_target_observer_ = observe_int_sync<LedAutoState>(
            ext_target_subj, this, [](LedAutoState* self, int) { self->on_state_changed(); });
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
    if (!cfg) {
        setup_default_mappings();
        return;
    }

    // === One-time migration from old /led/auto_state/ paths ===
    auto& old_enabled_json = cfg->get_json("/led/auto_state/enabled");
    if (old_enabled_json.is_boolean()) {
        auto& new_enabled_json = cfg->get_json(cfg->df() + "leds/auto_state/enabled");
        if (!new_enabled_json.is_boolean()) {
            spdlog::info("[LedAutoState] Migrating config from /led/auto_state/ to active printer");
            cfg->set(cfg->df() + "leds/auto_state/enabled", old_enabled_json.get<bool>());
            auto& old_mappings = cfg->get_json("/led/auto_state/mappings");
            if (old_mappings.is_object()) {
                cfg->set(cfg->df() + "leds/auto_state/mappings", old_mappings);
            }
            cfg->save();
        }
    }

    // Enabled flag
    enabled_ = cfg->get<bool>(cfg->df() + "leds/auto_state/enabled", false);

    // Mappings
    mappings_.clear();
    auto& mappings_json = cfg->get_json(cfg->df() + "leds/auto_state/mappings");
    if (mappings_json.is_object()) {
        for (auto it = mappings_json.begin(); it != mappings_json.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            LedStateAction action;
            action.action_type = it.value().value("action", "color");
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
            action.brightness = it.value().value("brightness", 100);
            action.effect_name = it.value().value("effect_name", "");
            action.wled_preset = it.value().value("wled_preset", 0);
            action.macro_gcode = it.value().value("macro_gcode", "");
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
    if (!cfg) {
        return;
    }

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

    cfg->save();
    spdlog::debug("[LedAutoState] Saved config");
}

} // namespace helix::led
