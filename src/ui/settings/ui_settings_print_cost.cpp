// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_print_cost.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_panel_common.h"
#include "ui_utils.h"

#include "print_cost.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix::settings {

namespace {

constexpr const char* kDefaultKey = "default";
constexpr float kMaxPricePerKg = 100000.0f;
constexpr float kMaxWatts = 10000.0f;
constexpr float kMaxPricePerKwh = 1000.0f;

/// Row i's material: "" for the default row 0, else kCostMaterials[i - 1].
std::string row_material(size_t row) {
    return row == 0 ? std::string() : std::string(SettingsManager::kCostMaterials[row - 1]);
}

/// The XML subject name of price row i; static storage, since the XML keeps the pointer.
const char* row_subject_name(size_t row) {
    static const auto kNames = [] {
        std::array<std::string, std::size(SettingsManager::kCostMaterials) + 1> names;
        names[0] = std::string("print_cost_price_") + kDefaultKey;
        for (size_t i = 1; i < names.size(); ++i) {
            names[i] = std::string("print_cost_price_") + SettingsManager::kCostMaterials[i - 1];
        }
        return names;
    }();
    return kNames[row].c_str();
}

/// @p v to @p decimals places: the keypad hands back a float, and 24.99 should
/// be saved as 24.99, not 24.989999771118164.
double rounded(float v, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(static_cast<double>(v) * scale) / scale;
}

/// A setting's field text: the number, or "Not set" for zero.
void format_value(char* buf, size_t size, double value, int decimals) {
    if (value > 0.0) {
        std::snprintf(buf, size, "%.*f", decimals, value);
    } else {
        std::snprintf(buf, size, "%s", lv_tr("Not set"));
    }
}

} // namespace

void PrintCostOverlay::init_subjects() {
    UI_MANAGED_SUBJECT_INT(currency_subject_, 0, "print_cost_currency", subjects_);
    for (size_t i = 0; i < kPriceRows; ++i) {
        UI_MANAGED_SUBJECT_STRING(price_subjects_[i], price_bufs_[i], "", row_subject_name(i),
                                  subjects_);
    }
    UI_MANAGED_SUBJECT_STRING(watts_subject_, watts_buf_, "", "print_cost_watts", subjects_);
    UI_MANAGED_SUBJECT_STRING(kwh_subject_, kwh_buf_, "", "print_cost_kwh", subjects_);
}

void PrintCostOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_print_cost_field_clicked",
         [](lv_event_t* e) {
             const auto* key = static_cast<const char*>(lv_event_get_user_data(e));
             get_print_cost_overlay().edit_field(key ? key : "");
         }},
        {"on_print_cost_currency_changed",
         [](lv_event_t* e) { get_print_cost_overlay().handle_currency_changed(e); }},
    });
}

lv_obj_t* PrintCostOverlay::create(lv_obj_t* parent) {
    overlay_root_ = helix::ui::create_xml_hidden(parent, "print_cost_overlay");
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    return overlay_root_;
}

void PrintCostOverlay::before_show() {
    refresh_values();
}

void PrintCostOverlay::refresh_values() {
    auto& settings = SettingsManager::instance();
    for (size_t i = 0; i < kPriceRows; ++i) {
        format_value(price_bufs_[i], sizeof(price_bufs_[i]),
                     settings.get_cost_price_per_kg(row_material(i)), 2);
        lv_subject_set_string(&price_subjects_[i], price_bufs_[i]);
    }
    format_value(watts_buf_, sizeof(watts_buf_), settings.get_cost_watts(), 0);
    lv_subject_set_string(&watts_subject_, watts_buf_);
    format_value(kwh_buf_, sizeof(kwh_buf_), settings.get_cost_price_per_kwh(), 3);
    lv_subject_set_string(&kwh_subject_, kwh_buf_);

    // The dropdown's own options are the list of codes; the saved code picks one.
    lv_obj_t* row =
        overlay_root_ ? lv_obj_find_by_name(overlay_root_, "row_cost_currency") : nullptr;
    lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    if (dropdown) {
        const int32_t index =
            lv_dropdown_get_option_index(dropdown, settings.get_cost_currency().c_str());
        lv_subject_set_int(&currency_subject_, index < 0 ? 0 : index);
    }
}

void PrintCostOverlay::handle_currency_changed(lv_event_t* e) {
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    char code[16] = {};
    lv_dropdown_get_selected_str(dropdown, code, sizeof(code));
    if (code[0] != '\0') {
        SettingsManager::instance().set_cost_currency(code);
    }
}

void PrintCostOverlay::edit_field(const std::string& key) {
    auto& settings = SettingsManager::instance();
    ui_keypad_config_t config{};
    config.min_value = 0.0f;
    config.allow_decimal = true;

    if (key == "watts") {
        config.initial_value = static_cast<float>(settings.get_cost_watts());
        config.max_value = kMaxWatts;
        config.allow_decimal = false;
        config.title_label = lv_tr("Printer power");
        config.unit_label = "W";
        config.on_confirm = [this](float v) {
            SettingsManager::instance().set_cost_watts(rounded(v, 0));
            refresh_values();
        };
    } else if (key == "kwh") {
        config.initial_value = static_cast<float>(settings.get_cost_price_per_kwh());
        config.max_value = kMaxPricePerKwh;
        config.title_label = lv_tr("Price per kWh");
        config.unit_label = "";
        config.on_confirm = [this](float v) {
            SettingsManager::instance().set_cost_price_per_kwh(rounded(v, 4));
            refresh_values();
        };
    } else {
        const std::string material = key == kDefaultKey ? std::string() : key;
        config.initial_value = static_cast<float>(settings.get_cost_price_per_kg(material));
        config.max_value = kMaxPricePerKg;
        config.max_fraction_digits = 2;
        // The keypad keeps the title pointer while it is open.
        editing_title_ = material.empty() ? std::string(lv_tr("Any other material")) : key;
        config.title_label = editing_title_.c_str();
        config.unit_label = "/ kg";
        config.on_confirm = [this, material](float v) {
            SettingsManager::instance().set_cost_price_per_kg(material, rounded(v, 2));
            refresh_values();
        };
    }
    ui_keypad_show(&config);
}

} // namespace helix::settings
