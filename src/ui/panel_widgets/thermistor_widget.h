// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

/// Home widget displaying a user-selected temperature sensor reading.
/// Click opens a context menu to choose which sensor to monitor.
/// Selection persists via PanelWidgetConfig per-widget config.
class ThermistorWidget : public PanelWidget {
  public:
    explicit ThermistorWidget(const std::string& instance_id);
    ~ThermistorWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override;
    const char* id() const override {
        return instance_id_.c_str();
    }
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Called from static event callback
    void handle_clicked();

    /// Select a sensor by klipper_name, update display, save config
    void select_sensor(const std::string& klipper_name);

    /// Select icon for this widget instance
    void select_icon(const std::string& name);

    // Static event callbacks (XML-registered)
    static void thermistor_clicked_cb(lv_event_t* e);

  private:
    /// Single-select list of the printer's temperature sensors, raised by a tap on
    /// a single-sensor widget. Picking a row binds the widget to that sensor and
    /// closes the card; a tap outside it chooses nothing.
    class SensorPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(SensorPicker)

      public:
        explicit SensorPicker(ThermistorWidget& owner) : owner_(owner) {}

      protected:
        const char* xml_component_name() const override {
            return "thermistor_sensor_picker";
        }
        /// 30% of the screen, clamped so the list stays readable on a 480px panel
        /// and does not sprawl on a 1024px one.
        CardWidth card_width() const override {
            return {30, 160, 240};
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        /// What a row needs to act on a tap: which sensor it names, and the picker
        /// that owns it. Heap-allocated per row, hung off the row's user_data and
        /// freed by that row's own LV_EVENT_DELETE handler.
        struct RowPayload {
            SensorPicker* picker;
            std::string klipper_name;
        };

        ThermistorWidget& owner_;
    };

    /// Multi-select sensor list plus the icon grid, raised from edit mode or by a
    /// tap on a carousel page. The checkboxes are the edit, so both the Done button
    /// and a tap on the backdrop commit them.
    class ConfigurePicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ConfigurePicker)

      public:
        explicit ConfigurePicker(ThermistorWidget& owner) : owner_(owner) {}

        /// Repaint the icon grid's selection ring against the widget's current icon.
        /// No-op when the card is not on screen.
        void refresh_icon_highlights();

      protected:
        const char* xml_component_name() const override {
            return "thermistor_configure_picker";
        }
        /// Wider than the sensor picker: this card carries a header row, a Done
        /// button and a wrapping icon grid alongside the sensor list.
        CardWidth card_width() const override {
            return {40, 200, 320};
        }
        void on_created(lv_obj_t* backdrop) override;
        /// A tap outside the card applies the checkbox selection rather than
        /// dropping it. on_close_clicked() inherits this, so Done takes the same
        /// path.
        void on_backdrop_clicked() override;

      private:
        /// Which sensor a row names, so the commit sweep can read it back off the
        /// row. Heap-allocated per row and freed by that row's LV_EVENT_DELETE.
        struct RowPayload {
            ConfigurePicker* picker;
            std::string klipper_name;
        };

        /// Read the checkbox states back into the widget's sensor list, then close.
        void commit();

        ThermistorWidget& owner_;
    };

    std::string instance_id_;
    std::string icon_name_; // Custom icon, empty = "thermometer" default

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    nlohmann::json config_;
    std::vector<std::string> sensors_; // klipper_names for carousel mode

    std::string selected_sensor_; // klipper_name (e.g., "temperature_sensor mcu_temp")
    std::string display_name_;    // Pretty name for label
    ObserverGuard temp_observer_;
    SubjectLifetime temp_lifetime_;
    char temp_buffer_[16] = {};

    // Carousel mode
    struct CarouselPage {
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* name_label = nullptr;
        char temp_buffer[16] = {};
    };
    std::vector<CarouselPage> carousel_pages_;
    std::vector<ObserverGuard> carousel_observers_;
    std::vector<SubjectLifetime> carousel_lifetimes_;
    ObserverGuard version_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer (temp_observer_, carousel_observers_, version_observer_)
    // destructs. Without this, queued observer callbacks captured via
    // tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    bool binding_in_progress_ = false; // reentrancy guard for bind_carousel_sensors

    // The two context menus this widget raises
    SensorPicker sensor_picker_{*this};
    ConfigurePicker configure_picker_{*this};

    bool is_carousel_mode() const;
    void attach_single();
    void attach_carousel();
    void bind_carousel_sensors();
    void show_configure_picker();
    void apply_sensor_selection(const std::vector<std::string>& selected);
    void resolve_display_name();
    void on_temp_changed(int decidegrees);
    void update_display();
    void save_config();
    void show_sensor_picker();
};

} // namespace helix
