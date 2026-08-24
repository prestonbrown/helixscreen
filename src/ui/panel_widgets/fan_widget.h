// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>
#include <string>

namespace helix {

/// Home widget displaying a user-selected fan speed reading.
/// Click opens a context menu to choose which fan to monitor.
/// Selection persists via PanelWidgetConfig per-widget config.
class FanWidget : public PanelWidget {
  public:
    explicit FanWidget(const std::string& instance_id);
    ~FanWidget() override;

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

    /// Select a fan by object_name, update display, save config
    void select_fan(const std::string& object_name);

    // Static event callbacks (XML-registered)
    static void fan_widget_clicked_cb(lv_event_t* e);

  private:
    /// Single-select list of the printer's fans, raised by the edit-mode gear.
    /// The card hangs under the widget tile and carries the shared backdrop,
    /// positioning and dismissal behaviour from ContextMenu.
    class FanPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(FanPicker)

      public:
        explicit FanPicker(FanWidget& owner) : owner_(owner) {}

      protected:
        const char* xml_component_name() const override {
            return "fan_picker";
        }
        /// 30% of the screen, clamped so the list stays readable on a 480px panel
        /// and does not sprawl on a 1024px one.
        CardWidth card_width() const override {
            return {30, 160, 240};
        }
        void on_created(lv_obj_t* menu) override;

      private:
        /// What a row needs to act on a tap: which fan it names, and the picker
        /// that owns it. Heap-allocated per row, hung off the row's user_data and
        /// freed by that row's own LV_EVENT_DELETE handler.
        struct RowPayload {
            FanPicker* picker;
            std::string object_name;
        };

        FanWidget& owner_;
    };

    std::string instance_id_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* speed_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* fan_icon_ = nullptr;

    nlohmann::json config_;
    std::string selected_fan_; // object_name (e.g., "heater_fan hotend_fan")
    std::string display_name_;
    SubjectLifetime speed_lifetime_; // Before observer: destroyed after observer in ~dtor
    ObserverGuard speed_observer_;
    ObserverGuard version_observer_;
    helix::AsyncLifetimeGuard lifetime_;
    char speed_buffer_[16] = {};

    // Fan picker context menu (edit-mode gear only)
    FanPicker picker_{*this};

    // Cached fan-control overlay opened on normal tap
    lv_obj_t* fan_control_panel_ = nullptr;

    void auto_select_first_fan();
    void bind_speed_observer();
    void on_speed_changed(int speed_pct);
    void update_display();
    void save_config();
    void show_fan_picker();
    void resolve_display_name();
};

} // namespace helix
