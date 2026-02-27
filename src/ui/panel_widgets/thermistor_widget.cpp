// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermistor_widget.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "observer_factory.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {
void register_thermistor_widget() {
    register_widget_factory("thermistor", []() {
        auto& ps = get_printer_state();
        return std::make_unique<ThermistorWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "thermistor_clicked_cb",
                             ThermistorWidget::thermistor_clicked_cb);
    lv_xml_register_event_cb(nullptr, "thermistor_picker_backdrop_cb",
                             ThermistorWidget::thermistor_picker_backdrop_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::centi_to_degrees_f;
using helix::ui::temperature::format_temperature_f;

/// Strip redundant " Temperature" suffix — the widget context already implies it
static void strip_temperature_suffix(std::string& name) {
    const std::string suffix = " Temperature";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
    }
}

// File-local helper: get the shared PanelWidgetConfig instance
static helix::PanelWidgetConfig& get_widget_config_ref() {
    static helix::PanelWidgetConfig config("home", *Config::get_instance());
    config.load();
    return config;
}

ThermistorWidget::ThermistorWidget(PrinterState& /*printer_state*/) {
    std::strcpy(temp_buffer_, "--\xC2\xB0"
                              "C"); // "--°C"
}

ThermistorWidget::~ThermistorWidget() {
    detach();
}

void ThermistorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);
    }

    // Cache label pointers
    temp_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_temp");
    name_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_name");

    // Load saved sensor selection from config
    load_config();

    // If no sensor saved, auto-select first available
    if (selected_sensor_.empty()) {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        auto sensors = tsm.get_sensors_sorted();
        if (!sensors.empty()) {
            select_sensor(sensors.front().klipper_name);
        }
    } else {
        // Re-bind observer to saved sensor
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        SubjectLifetime lifetime;
        lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_, lifetime);
        if (subject) {
            std::weak_ptr<bool> weak_alive = alive_;
            temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
                subject, this,
                [weak_alive](ThermistorWidget* self, int temp) {
                    if (weak_alive.expired())
                        return;
                    self->on_temp_changed(temp);
                },
                lifetime);
        }
        update_display();
    }

    spdlog::debug("[ThermistorWidget] Attached (sensor: {})",
                  selected_sensor_.empty() ? "none" : selected_sensor_);
}

void ThermistorWidget::detach() {
    *alive_ = false;
    dismiss_sensor_picker();
    temp_observer_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    temp_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[ThermistorWidget] Detached");
}

void ThermistorWidget::handle_clicked() {
    spdlog::info("[ThermistorWidget] Clicked - showing sensor picker");
    show_sensor_picker();
}

void ThermistorWidget::select_sensor(const std::string& klipper_name) {
    if (klipper_name == selected_sensor_) {
        return;
    }

    // Reset existing observer
    temp_observer_.reset();

    selected_sensor_ = klipper_name;

    // Look up display name from sensor config
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();
    display_name_ = klipper_name; // fallback
    for (const auto& s : sensors) {
        if (s.klipper_name == klipper_name) {
            display_name_ = s.display_name;
            break;
        }
    }

    strip_temperature_suffix(display_name_);

    // Subscribe to this sensor's temperature subject
    SubjectLifetime lifetime;
    lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
    if (subject) {
        std::weak_ptr<bool> weak_alive = alive_;
        temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
            subject, this,
            [weak_alive](ThermistorWidget* self, int temp) {
                if (weak_alive.expired())
                    return;
                self->on_temp_changed(temp);
            },
            lifetime);
    } else {
        spdlog::warn("[ThermistorWidget] No subject for sensor: {}", klipper_name);
    }

    update_display();
    save_config();

    spdlog::info("[ThermistorWidget] Selected sensor: {} ({})", display_name_, klipper_name);
}

void ThermistorWidget::on_temp_changed(int centidegrees) {
    float deg = centi_to_degrees_f(centidegrees);
    format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));

    if (temp_label_) {
        lv_label_set_text(temp_label_, temp_buffer_);
    }

    spdlog::trace("[ThermistorWidget] {} = {:.1f}°C", display_name_, deg);
}

void ThermistorWidget::update_display() {
    if (temp_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(temp_label_, "--\xC2\xB0"
                                           "C");
        } else {
            // Read current value from subject
            auto& tsm = helix::sensors::TemperatureSensorManager::instance();
            lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_);
            if (subject) {
                float deg = centi_to_degrees_f(lv_subject_get_int(subject));
                format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));
                lv_label_set_text(temp_label_, temp_buffer_);
            } else {
                lv_label_set_text(temp_label_, "--\xC2\xB0"
                                               "C");
            }
        }
    }

    if (name_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(name_label_, "Select sensor");
        } else {
            lv_label_set_text(name_label_, display_name_.c_str());
        }
    }
}

void ThermistorWidget::load_config() {
    auto& wc = get_widget_config_ref();
    auto config = wc.get_widget_config("thermistor");
    if (config.contains("sensor") && config["sensor"].is_string()) {
        selected_sensor_ = config["sensor"].get<std::string>();

        // Resolve display name
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        auto sensors = tsm.get_sensors_sorted();
        display_name_ = selected_sensor_;
        for (const auto& s : sensors) {
            if (s.klipper_name == selected_sensor_) {
                display_name_ = s.display_name;
                break;
            }
        }

        strip_temperature_suffix(display_name_);
        spdlog::debug("[ThermistorWidget] Loaded config: sensor={}", selected_sensor_);
    }
}

void ThermistorWidget::save_config() {
    if (selected_sensor_.empty()) {
        return;
    }
    nlohmann::json config;
    config["sensor"] = selected_sensor_;

    auto& wc = get_widget_config_ref();
    wc.set_widget_config("thermistor", config);

    spdlog::debug("[ThermistorWidget] Saved config: sensor={}", selected_sensor_);
}

void ThermistorWidget::show_sensor_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();
    if (sensors.empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    // Create picker from XML
    picker_backdrop_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "thermistor_sensor_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[ThermistorWidget] Failed to create sensor picker from XML");
        return;
    }

    // Find the sensor_list container
    lv_obj_t* sensor_list = lv_obj_find_by_name(picker_backdrop_, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        helix::ui::safe_delete(picker_backdrop_);
        return;
    }

    // Resolve responsive spacing tokens before building rows
    auto get_token = [](const char* name, int fallback) {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_xs = get_token("space_xs", 4);
    int space_sm = get_token("space_sm", 6);
    int space_md = get_token("space_md", 10);

    // Cap sensor list height at 2/3 of screen
    int screen_h = lv_obj_get_height(parent_screen_);
    lv_obj_set_style_max_height(sensor_list, screen_h * 2 / 3, 0);

    // Populate sensor rows
    for (const auto& sensor : sensors) {
        bool is_selected = (sensor.klipper_name == selected_sensor_);

        // Create a row button for each sensor
        lv_obj_t* row = lv_obj_create(sensor_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Highlight selected row
        if (is_selected) {
            lv_obj_set_style_bg_opa(row, 30, 0);
        } else {
            lv_obj_set_style_bg_opa(row, 0, 0);
        }

        // Sensor display name
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, sensor.display_name.c_str());
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, lv_font_get_default(), 0);

        // Current temperature
        auto state = tsm.get_sensor_state(sensor.klipper_name);
        char temp_buf[16];
        if (state && state->available) {
            helix::ui::temperature::format_temperature(static_cast<int>(state->temperature),
                                                       temp_buf, sizeof(temp_buf));
        } else {
            std::strcpy(temp_buf, "--\xC2\xB0"
                                  "C");
        }
        lv_obj_t* temp = lv_label_create(row);
        lv_label_set_text(temp, temp_buf);
        lv_obj_set_style_text_font(temp, lv_font_get_default(), 0);
        lv_obj_set_style_text_opa(temp, 180, 0);

        // Store klipper_name as user_data for click handler
        // Allocate string on heap; cleaned up when picker is dismissed (lv_obj_delete)
        auto* klipper_name_copy = new std::string(sensor.klipper_name);
        lv_obj_set_user_data(row, klipper_name_copy);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                // Walk up to find the backdrop, which has the ThermistorWidget pointer
                // The picker_backdrop's parent screen has the widget via
                // get_global_home_panel But simpler: use static active instance
                // pattern Actually, just use a static pointer set before showing
                // picker
                if (ThermistorWidget::s_active_picker_) {
                    std::string sensor_name = *name_ptr;
                    ThermistorWidget::s_active_picker_->select_sensor(sensor_name);
                    ThermistorWidget::s_active_picker_->dismiss_sensor_picker();
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback — if LVGL deletes picker_backdrop_ via parent
    // deletion (e.g., user navigates away), clear our pointer to prevent dangling access
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<ThermistorWidget*>(lv_event_get_user_data(e));
            if (self) {
                // Clean up heap-allocated strings before LVGL frees the tree
                lv_obj_t* backdrop = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
                if (sensor_list) {
                    uint32_t count = lv_obj_get_child_count(sensor_list);
                    for (uint32_t i = 0; i < count; ++i) {
                        lv_obj_t* row = lv_obj_get_child(sensor_list, i);
                        auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
                        delete name_ptr;
                        lv_obj_set_user_data(row, nullptr);
                    }
                }
                self->picker_backdrop_ = nullptr;
                if (s_active_picker_ == self) {
                    s_active_picker_ = nullptr;
                }
            }
        },
        LV_EVENT_DELETE, this);

    // Position the context menu card near the widget
    lv_obj_t* card = lv_obj_find_by_name(picker_backdrop_, "context_menu");
    if (card && widget_obj_) {
        int screen_w = lv_obj_get_width(parent_screen_);

        // Get actual screen coordinates of the widget
        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        // Position card below the widget, centered horizontally
        int card_w = std::clamp(screen_w * 3 / 10, 160, 240);
        lv_obj_set_width(card, card_w);

        // Force layout so card gets its content height before positioning
        lv_obj_update_layout(card);
        int card_h = lv_obj_get_height(card);

        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;

        // Clamp to screen bounds using responsive margin
        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;
        if (card_y + card_h > screen_h - space_md) {
            // Show above widget instead
            card_y = widget_area.y1 - card_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[ThermistorWidget] Sensor picker shown with {} sensors", sensors.size());
}

void ThermistorWidget::dismiss_sensor_picker() {
    if (!picker_backdrop_) {
        return;
    }

    // Clean up heap-allocated klipper_name strings (only if object is still valid —
    // parent screen deletion auto-frees children, leaving stale pointers)
    if (lv_obj_is_valid(picker_backdrop_)) {
        lv_obj_t* sensor_list = lv_obj_find_by_name(picker_backdrop_, "sensor_list");
        if (sensor_list) {
            uint32_t count = lv_obj_get_child_count(sensor_list);
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t* row = lv_obj_get_child(sensor_list, i);
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
                delete name_ptr;
                lv_obj_set_user_data(row, nullptr);
            }
        }
    }

    helix::ui::safe_delete(picker_backdrop_);
    s_active_picker_ = nullptr;

    spdlog::debug("[ThermistorWidget] Sensor picker dismissed");
}

// Static callbacks
void ThermistorWidget::thermistor_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_clicked_cb");
    auto* widget = panel_widget_from_event<ThermistorWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ThermistorWidget::thermistor_picker_backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_picker_backdrop_cb");
    (void)e;
    if (s_active_picker_) {
        s_active_picker_->dismiss_sensor_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// Static instance for picker callbacks
ThermistorWidget* ThermistorWidget::s_active_picker_ = nullptr;
