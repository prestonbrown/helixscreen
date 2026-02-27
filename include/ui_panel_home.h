// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"
#include "ui_panel_print_status.h" // For RunoutGuidanceModal

#include "panel_widget.h"
#include "subject_managed_panel.h"
#include "tips_manager.h"

#include <memory>
#include <vector>

namespace helix {
enum class PrintJobState;
}

/**
 * @brief Home panel - Main dashboard showing printer status and quick actions
 *
 * Manages grid lifecycle, widget dispatch, tip-of-the-day, print card,
 * printer image snapshot, and filament runout modal. Widget-specific behavior
 * (LED, power, network, temperature, fans, etc.) lives in PanelWidget subclasses.
 *
 * @see TipsManager for tip of the day functionality
 */

class HomePanel : public PanelBase {
  public:
    HomePanel(helix::PrinterState& printer_state, MoonrakerAPI* api);
    ~HomePanel() override;

    void init_subjects() override;
    void deinit_subjects();
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;
    const char* get_name() const override {
        return "Home Panel";
    }
    const char* get_xml_component_name() const override {
        return "home_panel";
    }

    /// Rebuild the widget list from current PanelWidgetConfig
    void populate_widgets();

    /// Reload printer image, type/host overlay, and delegate to widgets
    void reload_from_config();

    /// Re-check printer image setting and update the home panel image widget
    void refresh_printer_image();

    /// Trigger a deferred runout check (used after wizard completes)
    void trigger_idle_runout_check();

  private:
    SubjectManager subjects_;
    lv_subject_t status_subject_;
    lv_subject_t printer_type_subject_;
    lv_subject_t printer_host_subject_;
    lv_subject_t printer_info_visible_;

    char status_buffer_[512];
    char printer_type_buffer_[64];
    char printer_host_buffer_[64];

    helix::PrintingTip current_tip_;
    helix::PrintingTip pending_tip_;
    lv_timer_t* tip_rotation_timer_ = nullptr;
    lv_obj_t* tip_label_ = nullptr;
    bool tip_animating_ = false;

    // Pre-scaled printer image snapshot
    lv_draw_buf_t* cached_printer_snapshot_ = nullptr;
    lv_timer_t* snapshot_timer_ = nullptr;

    // Active PanelWidget instances (factory-created, lifecycle-managed)
    std::vector<std::unique_ptr<helix::PanelWidget>> active_widgets_;

    ObserverGuard ams_slot_count_observer_;

    // Print card observers
    ObserverGuard print_state_observer_;
    ObserverGuard print_progress_observer_;
    ObserverGuard print_time_left_observer_;
    ObserverGuard print_thumbnail_path_observer_;

    // Filament runout observer and modal
    ObserverGuard filament_runout_observer_;
    ObserverGuard image_changed_observer_;
    RunoutGuidanceModal runout_modal_;
    bool runout_modal_shown_ = false;

    // Print card widgets (looked up after XML creation)
    lv_obj_t* print_card_thumb_ = nullptr;
    lv_obj_t* print_card_active_thumb_ = nullptr;
    lv_obj_t* print_card_label_ = nullptr;

    // Grid and widget lifecycle
    void setup_widget_gate_observers();
    void cache_widget_references();

    // Tip of the day
    void update_tip_of_day();
    void start_tip_fade_transition(const helix::PrintingTip& new_tip);
    void apply_pending_tip();

    // Printer image snapshot
    void schedule_printer_image_snapshot();
    void take_printer_image_snapshot();

    // Panel-level click handlers (not widget-delegated)
    void handle_print_card_clicked();
    void handle_tip_text_clicked();
    void handle_tip_rotation_timer();
    void handle_printer_status_clicked();
    void handle_printer_manager_clicked();
    void handle_ams_clicked();

    // Panel-level static callbacks
    static void print_card_clicked_cb(lv_event_t* e);
    static void tip_text_clicked_cb(lv_event_t* e);
    static void printer_status_clicked_cb(lv_event_t* e);
    static void printer_manager_clicked_cb(lv_event_t* e);
    static void ams_clicked_cb(lv_event_t* e);
    static void tip_rotation_timer_cb(lv_timer_t* timer);

    void update_ams_indicator(int slot_count);

    // Print card update methods
    void on_print_state_changed(helix::PrintJobState state);
    void on_print_progress_or_time_changed();
    void on_print_thumbnail_path_changed(const char* path);
    void update_print_card_from_state();
    void update_print_card_label(int progress, int time_left_secs);
    void reset_print_card_to_idle();

    // Filament runout handling
    void check_and_show_idle_runout_modal();
    void show_idle_runout_modal();
};

// Global instance accessor (needed by main.cpp)
HomePanel& get_global_home_panel();
