// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix {
class PrinterState;

/// Home widget displaying part, hotend, and auxiliary fan speeds in a compact stack.
/// Fan icons spin proportionally to fan speed when animations are enabled.
/// Clicking opens the fan control overlay.
class FanStackWidget : public PanelWidget {
  public:
    FanStackWidget(const std::string& instance_id, PrinterState& printer_state);
    ~FanStackWidget() override;

    void set_config(const nlohmann::json& config) override;
    std::string get_component_name() const override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    const char* id() const override {
        return instance_id_.c_str();
    }

    /// XML event callback — opens fan control overlay
    static void on_fan_stack_clicked(lv_event_t* e);

  private:
    std::string instance_id_;
    PrinterState& printer_state_;
    nlohmann::json config_;

    // Per-instance config
    std::string selected_fan_; // Specific fan object_name (empty = auto-classify)
    std::string icon_name_;    // Custom icon name (empty = default "fan")

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* fan_control_panel_ = nullptr;

    // Labels, names, and icons for each fan row (stack mode)
    lv_obj_t* part_label_ = nullptr;
    lv_obj_t* hotend_label_ = nullptr;
    lv_obj_t* aux_label_ = nullptr;
    lv_obj_t* aux_row_ = nullptr;
    lv_obj_t* part_icon_ = nullptr;
    lv_obj_t* hotend_icon_ = nullptr;
    lv_obj_t* aux_icon_ = nullptr;

    // Per-fan observers. lifetime_ is declared LAST below so reverse-
    // declaration destruction tears it down first, expiring captured tokens
    // before any observer destructs (including carousel_observers_ further
    // down). Mirrors AX3CKAKB sweep — see 40dd27a9c / 45abc8c2a.
    ObserverGuard part_observer_;
    ObserverGuard hotend_observer_;
    ObserverGuard aux_observer_;

    // Version observer to detect fan discovery
    ObserverGuard version_observer_;

    // Animation settings observer
    ObserverGuard anim_settings_observer_;

    // Resolved fan object names and display names
    std::string part_fan_name_;
    std::string hotend_fan_name_;
    std::string aux_fan_name_;
    std::string part_display_name_;
    std::string hotend_display_name_;
    std::string aux_display_name_;

    // Cached speeds for animation updates
    int part_speed_ = 0;
    int hotend_speed_ = 0;
    int aux_speed_ = 0;

    bool animations_enabled_ = false;
    bool rebuilding_carousel_ = false;
    uint32_t carousel_gen_ = 0;

    // Carousel mode: per-page tracking for arc + label + icon updates
    struct CarouselPage {
        lv_obj_t* arc = nullptr;
        lv_obj_t* speed_label = nullptr;
        lv_obj_t* fan_icon = nullptr;
        std::string object_name;
        bool is_controllable = false;
        bool syncing = false;            ///< true while a programmatic arc update is in flight
        uint32_t last_user_input_ms = 0; ///< lv_tick_get() of the most recent drag/tap
    };
    std::vector<CarouselPage> carousel_pages_;
    std::vector<ObserverGuard> carousel_observers_;

    /// Send a fan speed command (optimistic update + Moonraker call).
    /// Used by the carousel's in-place arc control path.
    void send_carousel_fan_speed(const std::string& object_name, int speed_percent);

    /// Static dispatch for arc VALUE_CHANGED events on controllable carousel pages.
    static void on_carousel_arc_value_changed(lv_event_t* e);

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer (including carousel_observers_ above) destructs.
    helix::AsyncLifetimeGuard lifetime_;

    bool is_carousel_mode() const;
    void attach_stack(lv_obj_t* widget_obj);
    void attach_carousel(lv_obj_t* widget_obj);

    void handle_clicked();
    void bind_fans();
    void bind_carousel_fans();

    /// Display-mode rows plus the icon grid, raised by the edit-mode gear.
    /// Picking an icon applies immediately and leaves the card up, so Done and
    /// a tap on the backdrop both simply close it.
    class ConfigurePicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ConfigurePicker)

      public:
        explicit ConfigurePicker(FanStackWidget& owner) : owner_(owner) {}

        /// Repaint the grid's selection border after a live icon change.
        void refresh_icon_highlights();

      protected:
        const char* xml_component_name() const override {
            return "fan_stack_picker";
        }
        /// Half the screen, clamped so the icon grid keeps a full row of cells on
        /// a 480px panel and the card does not sprawl on a 1024px one.
        CardWidth card_width() const override {
            return {50, 200, 360};
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        /// What a generated row needs to act on a tap: the value it stands for
        /// (a display_mode for a mode row, an icon name for a grid cell) and the
        /// picker that owns it. Heap-allocated per row, hung off the row's
        /// user_data and freed by that row's own LV_EVENT_DELETE handler.
        struct RowPayload {
            ConfigurePicker* picker;
            std::string value;
        };

        FanStackWidget& owner_;
    };

    ConfigurePicker picker_{*this};

    void show_fan_picker();
    void select_fan(const std::string& object_name);
    void select_icon(const std::string& name);
    void save_fan_config();
    void update_label(lv_obj_t* label, int speed_pct);
    void update_fan_animation(lv_obj_t* icon, int speed_pct);
    void refresh_all_animations();

    static void set_icon_pivot(lv_obj_t* icon);
    ObserverGuard bind_fan_observer(const std::string& fan_name,
                                    std::function<void(int speed)> on_update);
    void setup_common_observers(std::function<void()> on_anim_changed,
                                std::function<void()> on_fans_version);
};

} // namespace helix
