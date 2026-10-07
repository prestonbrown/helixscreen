// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "sensor_state.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

#include <string>
#include <vector>

class IMoonrakerAPI;

namespace helix {

struct PowerDeviceWidgetTestAccess; // test-only friend (tests/test_helpers/)

/// Home panel widget for toggling individual Moonraker power devices.
/// Uses the multi_instance system: base ID "power_device" with dynamic
/// instance IDs like "power_device:1", "power_device:2", etc.
/// Tap toggles device power; configure button opens device picker.
/// When unconfigured, tap also opens picker.
class PowerDeviceWidget : public TiledPanelWidget {
  public:
    explicit PowerDeviceWidget(const std::string& instance_id);
    ~PowerDeviceWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    std::string get_component_name() const override {
        return "panel_widget_power_device";
    }
    const char* id() const override {
        return instance_id_.c_str();
    }

    void handle_clicked();
    static void power_device_clicked_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    std::string device_name_;
    std::string icon_name_; // Custom icon, empty = "power_cycle" default

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* badge_obj_ = nullptr;
    lv_obj_t* icon_obj_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* lock_icon_ = nullptr;

    ObserverGuard status_observer_;
    helix::AsyncLifetimeGuard lifetime_;

    /// Device list, icon grid and energy-sensor chips, raised by a tap on an
    /// unconfigured tile or the edit-mode gear. A device row or a sensor chip
    /// applies and closes; an icon applies live and leaves the card up.
    class DevicePicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(DevicePicker)

      public:
        explicit DevicePicker(PowerDeviceWidget& owner) : owner_(owner) {}

        /// Repaint the grid's selection ring after a live icon change.
        void refresh_icon_highlights();

      protected:
        const char* xml_component_name() const override {
            return "power_device_picker";
        }
        /// Two columns side by side need more room than the single-list pickers.
        CardWidth card_width() const override {
            return {60, 260, 420};
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        using PickFn = void (*)(PowerDeviceWidget&, const std::string&);

        /// What a generated row needs to act on a tap: the device or sensor id it
        /// stands for, what picking it does, and the picker that owns it.
        /// Heap-allocated per row, hung off its user_data and freed by that row's
        /// own LV_EVENT_DELETE handler.
        struct RowPayload {
            DevicePicker* picker;
            std::string value;
            PickFn on_pick;
        };

        /// Create one row from @p component under @p parent, labelled @p label,
        /// that hides the card and calls @p on_pick with its value when tapped.
        void add_row(lv_obj_t* parent, const char* component, const char* label_name,
                     const std::string& label, const std::string& value, bool selected,
                     PickFn on_pick);

        PowerDeviceWidget& owner_;
    };

    // Sensor/energy page members
    std::string sensor_id_;
    lv_obj_t* carousel_ = nullptr;
    lv_obj_t* energy_page_ = nullptr;
    lv_obj_t* energy_power_label_ = nullptr;
    lv_obj_t* energy_voltage_label_ = nullptr;
    lv_obj_t* energy_current_label_ = nullptr;
    lv_obj_t* energy_energy_label_ = nullptr;
    ObserverGuard power_observer_;
    ObserverGuard voltage_observer_;
    ObserverGuard current_observer_;
    ObserverGuard energy_observer_;
    SubjectLifetime power_lifetime_;
    SubjectLifetime voltage_lifetime_;
    SubjectLifetime current_lifetime_;
    SubjectLifetime energy_lifetime_;

    bool is_all_devices() const {
        return device_name_ == "__all__";
    }

    // __all__ mode: aggregate state tracking
    bool all_power_on_ = false;
    ObserverGuard power_count_observer_;

    void refresh_all_devices_state();
    void handle_all_devices_toggle();
    void update_all_devices_display(bool any_on);

    IMoonrakerAPI* get_api() const;
    void update_display(int status);
    void show_device_picker();
    void select_device(const std::string& name);
    void select_icon(const std::string& name);
    void select_sensor(const std::string& sensor_id);
    void save_config();
    void setup_carousel();
    void teardown_carousel();
    void attach_sensor_observers();
    void detach_sensor_observers();
    void update_energy_label(const std::string& key, lv_obj_t* label, int centi_value);
    std::string auto_match_sensor() const;

    friend struct PowerDeviceWidgetTestAccess;

    /// Measure the state line only while there is one: an unconfigured tile
    /// shows none, and reserving it would leave the badge above an empty gap.
    void apply_status_presence();

    /// 1 while a device is configured, so the state line is drawn.
    lv_subject_t has_status_subject_{};
    std::string has_status_name_ = instance_id_ + "_has_status";
    SubjectManager subjects_;

    static TileSizing::Content status_content(bool has_status) {
        return TileSizing::Content{
            has_status ? "LOCKED" : "",  has_status ? "LOCKED" : "", "Power", has_status, "",
            /*label_always_drawn=*/true, TileSizing::IconBox::Disc};
    }

    // Declared after every member the picker's callbacks touch, so it is torn
    // down (hiding the card) before any of them.
    DevicePicker picker_{*this};
};

} // namespace helix
