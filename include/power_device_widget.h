// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "panel_widget.h"
#include "sensor_state.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

#include <array>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;

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
    /// A tile_color_grid swatch in the open picker; applies live and leaves the card up.
    static void color_swatch_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    std::string device_name_;
    std::string icon_name_; // Custom icon, empty = "power_cycle" default
    uint32_t color_ = 0;    // User colour (RGB), 0 = the state colours
    int last_status_ = -1;  // Last status drawn, to repaint after a colour change

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* icon_obj_ = nullptr;

    ObserverGuard status_observer_;
    helix::AsyncLifetimeGuard lifetime_;

    /// Device list, icon grid, colour swatches and energy-sensor chips, raised by
    /// a tap on an unconfigured tile or the edit-mode gear. A device row or a
    /// sensor chip applies and closes; an icon or a colour applies live and
    /// leaves the card up.
    class DevicePicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(DevicePicker)

      public:
        explicit DevicePicker(PowerDeviceWidget& owner) : owner_(owner) {}

        /// Repaint the grid's selection ring after a live icon change.
        void refresh_icon_highlights();

        PowerDeviceWidget& owner() {
            return owner_;
        }

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

    // The energy meter's page: the carousel's second page, in reach while
    // sensor_id_ names a meter that exists.
    std::string sensor_id_;
    helix::ui::WidgetRef carousel_;
    /// One meter reading (power, voltage, current, energy): its text, bound by
    /// the energy page, and the observer of the meter's subject that writes it.
    struct EnergyReading {
        helix::Subject text{};
        char buf[24] = {};
        ObserverGuard observer;
        SubjectLifetime lifetime;
        bool placeholder = true; ///< Showing the translated "-- W" until a value arrives
    };
    static constexpr size_t kEnergyReadingCount = 4;
    std::array<EnergyReading, kEnergyReadingCount> readings_;
    /// Re-renders the placeholders a language switch would otherwise leave stale.
    ObserverGuard language_observer_;
    void show_reading_placeholders();

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
    /// Badge, icon and state-line colours for @p status (0=off, 1=on,
    /// 2=locked, -1=unconfigured), with the user colour when one is set.
    void apply_state_colors(int status);
    void show_device_picker();
    void select_device(const std::string& name);
    void select_icon(const std::string& name);
    void select_color(uint32_t rgb);
    void select_sensor(const std::string& sensor_id);
    void save_config();
    /// Bring the energy page into reach for sensor_id_, or keep it out of
    /// reach when no meter of that id exists.
    void show_energy_page();
    void hide_energy_page();
    void attach_sensor_observers();
    void detach_sensor_observers();
    std::string auto_match_sensor() const;

    friend struct PowerDeviceWidgetTestAccess;

    /// Measure the state line only while there is one: an unconfigured tile
    /// shows none, and reserving it would leave the badge above an empty gap.
    void apply_status_presence();

    /// The device name on the tile, or "All Devices"; shown while named_subject_ is 1.
    void set_name_text(const char* text);

    // This instance's tile state, bound by its component. Owned per instance
    // because two power tiles can sit on the home grid at once.
    /// 1 while a device is configured, so the state line is drawn.
    helix::Subject has_status_subject_{};
    /// 0=off, 1=on, 2=locked, -1=unconfigured: the lock overlay follows it.
    helix::Subject state_subject_{};
    /// 1 while name_subject_ holds a name; 0 draws the "Configure" prompt.
    helix::Subject named_subject_{};
    helix::Subject name_subject_{};
    char name_buf_[128] = {};
    helix::Subject status_text_subject_{};
    char status_text_buf_[32] = {};
    /// The badge disc's fill and opacity and the state line's colour.
    helix::Subject accent_subject_{};
    helix::Subject badge_opa_subject_{};
    helix::Subject status_color_subject_{};
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
