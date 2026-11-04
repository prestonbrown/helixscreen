// SPDX-License-Identifier: GPL-3.0-or-later

#include "light_button_config.h"

#include "ui_toast_manager.h"

#include "config.h"
#include "json_utils.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/led_widget.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix {

namespace {

bool is_light_button(const PanelWidgetEntry& e) {
    const PanelWidgetDef* def = find_widget_def(e.id);
    return def != nullptr && std::strcmp(def->id, "led") == 0;
}

std::string pending_value(const Config& cfg) {
    const nlohmann::json* p = cfg.try_get_json(cfg.df() + led::kLightButtonPendingPath);
    return (p != nullptr && p->is_string()) ? p->get<std::string>() : std::string();
}

} // namespace

std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home,
                                                const std::string& pending) {
    std::vector<std::string> keys;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (!e.is_placed() || !is_light_button(e)) {
                continue;
            }
            const std::string key = json_util::safe_string(e.config, "led");
            keys.push_back(key.empty() ? pending : key);
        }
    }
    return keys;
}

bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home) {
    const std::string value = pending_value(cfg);
    if (value.empty()) {
        return false;
    }
    std::vector<std::string> ids;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (e.is_placed() && is_light_button(e) &&
                json_util::safe_string(e.config, "led").empty()) {
                ids.push_back(e.id);
            }
        }
    }
    for (const auto& id : ids) {
        nlohmann::json c = home.get_widget_config(id);
        if (!c.is_object()) {
            c = nlohmann::json::object();
        }
        c["led"] = value;
        home.set_widget_config(id, c);
    }
    cfg.set(cfg.df() + led::kLightButtonPendingPath, nlohmann::json());
    cfg.save();
    return !ids.empty();
}

std::vector<std::string> home_light_button_targets() {
    auto& ctrl = led::LedController::instance();
    std::vector<std::string> keys;
    auto* cfg = Config::get_instance();
    keys = home_light_button_keys(PanelWidgetManager::instance().get_widget_config("home"),
                                  pending_value(*cfg));

    return led::union_light_targets(keys, ctrl.switchable_ids(), ctrl.chamber_light());
}

bool home_light_buttons_lit() {
    auto& ctrl = led::LedController::instance();
    std::vector<led::DeviceState> states;
    for (const auto& id : home_light_button_targets()) {
        states.push_back(ctrl.device_state(id));
    }
    return light_icon_look(states).brightness > 0;
}

LightToggleResult toggle_lights(const std::vector<std::string>& ids, const char* caller) {
    auto& ctrl = led::LedController::instance();
    if (ctrl.light_command_in_flight()) {
        spdlog::debug("[LightButtons] {}: ignoring toggle - LED command already in flight", caller);
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("Light will switch when the current operation finishes"));
        return {LightToggle::Busy, false};
    }
    if (ids.empty()) {
        spdlog::warn("[LightButtons] {}: light toggle with no light to switch", caller);
        return {LightToggle::NoTarget, false};
    }
    const bool on = ctrl.toggle_power(ids);
    spdlog::info("[LightButtons] {}: toggled {} device(s) {}", caller, ids.size(),
                 on ? "ON" : "OFF");
    const bool unconfirmed = std::all_of(ids.begin(), ids.end(), [&](const std::string& id) {
        return ctrl.device_state(id).power == led::PowerState::Unknown;
    });
    return {on ? LightToggle::On : LightToggle::Off, unconfirmed};
}

void settle_light_buttons() {
    auto* cfg = Config::get_instance();
    adopt_pending_light_button(*cfg, PanelWidgetManager::instance().get_widget_config("home"));

    auto& ctrl = led::LedController::instance();
    // A button may name a WLED strip that has not arrived yet, and falling back
    // to the chamber light would spend the startup attempt on the wrong light.
    // The discovery settling runs this again.
    if (ctrl.wled_discovery_pending()) {
        spdlog::debug("[LightButtons] WLED discovery in flight - startup preference waits");
        return;
    }
    ctrl.apply_startup_preference(home_light_button_targets());
}

} // namespace helix
