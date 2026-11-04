// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "helix/xml/indexed_subject_pool.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

class ThermistorTestAccess;

/// Home widget displaying a user-selected temperature sensor reading.
/// A tap on a sensor with a settable target (heater_generic, temperature_fan)
/// opens the temperature keypad for it; a tap on a read-only sensor opens a
/// context menu to choose which sensor to monitor. Edit mode chooses sensors
/// for either. Selection persists via PanelWidgetConfig per-widget config.
class ThermistorWidget : public TiledPanelWidget {
  public:
    explicit ThermistorWidget(const std::string& instance_id);
    ~ThermistorWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override;
    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }
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
    /// A tap on a picker row; the row index is the event's user_data.
    static void picker_row_cb(lv_event_t* e);
    /// A tap on a carousel page.
    static void carousel_page_cb(lv_event_t* e);

  private:
    /// Keypad confirm: sends the value to the heater the keypad was opened for.
    /// That name is held outside the widget, so a tile rebuilt while the keypad
    /// is up cannot leave the callback holding a dead widget.
    static void target_keypad_cb(float value, void* user_data);
    friend class ThermistorTestAccess;

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

      public:
        /// Publish the rows' subjects. Runs before the card is built, so the XML
        /// <repeat> expands with its rows in place.
        void fill_rows();
        /// Choose row @p row: close the card and bind the widget to its sensor.
        void row_tapped(size_t row);

      private:
        std::vector<std::string> row_sensors_; ///< klipper_name per row

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

      public:
        /// Publish the rows' subjects. Runs before the card is built, so the XML
        /// <repeat> expands with its rows in place.
        void fill_rows();
        /// Flip row @p row's checkbox.
        void row_tapped(size_t row);

      private:
        std::vector<std::string> row_sensors_; ///< klipper_name per row

        /// Read the checked rows into the widget's sensor list, then close.
        void commit();

        ThermistorWidget& owner_;
    };

    std::string instance_id_;

    /// Built with the widget so its subjects exist before the manager parses

    /// this tile's component.

    std::string icon_name_; // Custom icon, empty = "thermometer" default

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    nlohmann::json config_;
    std::vector<std::string> sensors_; // klipper_names for carousel mode

    std::string selected_sensor_; // klipper_name (e.g., "temperature_sensor mcu_temp")
    std::string display_name_;    // Pretty name for label
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_; // shares temp_lifetime_: one token covers both
    SubjectLifetime temp_lifetime_;

    // The selected sensor's reading and target (decidegrees), mirrored into
    // per-instance subjects the tile's temp_display binds to. The manager's
    // own per-sensor subjects are not XML-registered.
    SubjectManager subjects_;
    lv_subject_t temp_subject_{};
    lv_subject_t target_subject_{};
    lv_subject_t available_subject_{}; ///< 1 while the selected sensor is reported
    std::string temp_subject_name_;
    std::string target_subject_name_;
    std::string available_subject_name_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;

    // Carousel mode: each page binds the reading and name subjects of its slot
    // in these pools (thermistor_carousel_page). Declared before the observers
    // that write them, so those are torn down first.
    /// Outlives page_temp_ and page_name_, which keep a pointer to it as their name prefix.
    std::string page_prefix_;
    std::unique_ptr<helix::xml::IndexedSubjectPool> page_temp_;
    std::unique_ptr<helix::xml::IndexedSubjectPool> page_name_;
    size_t carousel_page_count_ = 0;
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
    /// The count observer's first notification repeats the bind attach just
    /// ran; every later one is a change in the sensor set and rebinds, even at
    /// an unchanged count.
    bool sensor_count_seen_ = false;

    // The two context menus this widget raises
    SensorPicker sensor_picker_{*this};
    ConfigurePicker configure_picker_{*this};

    bool is_carousel_mode() const;
    void attach_single();
    void attach_carousel();
    void bind_carousel_sensors();
    /// Show @p decidegrees on carousel page @p page.
    void show_page_temp(size_t page, int decidegrees);

    /// Null every cached widget pointer (single-mode labels, carousel page
    /// labels, tile root) without touching observers or the widget tree.
    /// Shared by detach() and the raw-delete hook.
    void forget_tile_widgets();

    void on_hooked_root_deleted() override;

    void show_configure_picker();
    void apply_sensor_selection(const std::vector<std::string>& selected);
    void resolve_display_name();
    void on_temp_changed(int decidegrees);
    void update_display();
    void save_config();
    void show_sensor_picker();
    /// Observe the selected sensor's temperature and target subjects.
    void bind_selected_sensor();
    void open_target_keypad();
};

} // namespace helix
