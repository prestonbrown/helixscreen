// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "grid_edit_mode.h"
#include "panel_widget.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

struct HomePanelTestAccess; // test-only friend (tests/test_helpers/)

/**
 * @brief Home panel - Main dashboard showing printer status and quick actions
 *
 * Pure grid container: all visible elements (printer image, tips, print status,
 * temperature, network, LED, power, etc.) are placed as PanelWidgets by
 * PanelWidgetManager. Widget-specific behavior lives in PanelWidget subclasses
 * which self-register their own XML callbacks, observers, and lifecycle.
 */

class HomePanel : public PanelBase {
  public:
    HomePanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~HomePanel() override;

    void init_subjects() override;
    void deinit_subjects();
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    /// Complete the parts of setup that depend on the widget config / AMS
    /// detection state. Called after the first-run wizard finishes (so Moonraker
    /// has had a chance to report ams_slot_count), or immediately after setup
    /// when no wizard will run. Idempotent.
    void finalize_setup();
    /// setup() is deliberately minimal, so a rebuild leaves the fresh widget
    /// tree empty and the recycled panel widgets still bound to the old one.
    /// Re-run the finalize pass on the new tree.
    void repopulate() override;
    void on_activate() override;
    void on_deactivate() override;
    const char* get_name() const override {
        return "Home Panel";
    }
    const char* get_xml_component_name() const override {
        return "home_panel";
    }

    /// Rebuild the widget list from current PanelWidgetConfig.
    /// @param force  If false (gate observer path), skip rebuild when the
    ///               visible widget ID list hasn't changed. If true (config
    ///               change, grid edit mode), always rebuild.
    void populate_widgets(bool force = true);

    /// Apply printer-level config (delegates to PrinterImageWidget)
    void apply_printer_config();

    /// Delegate printer image refresh to PrinterImageWidget if active
    void refresh_printer_image();

    /// Trigger a deferred runout check (delegates to PrintStatusWidget)
    void trigger_idle_runout_check();

    /// Navigate the carousel to page 0 (main page) if not already there
    void go_to_main_page();

    /// Exit grid edit mode (called by navbar done button)
    void exit_grid_edit_mode();

    /// Open widget catalog overlay (called by navbar + button)
    void open_widget_catalog();

  private:
    SubjectManager subjects_;
    bool populating_widgets_ = false; // Reentrancy guard for populate_widgets()
    bool panel_active_ = false;       // Whether on_activate() has been called
    bool finalized_ = false;          // Whether finalize_setup() has run

    // Cached image path for skipping redundant refresh_printer_image() calls
    std::string last_printer_image_path_;

    // Grid edit mode state machine (long-press to rearrange widgets)
    helix::GridEditMode grid_edit_mode_;

    // Press-point tracking for edit-mode entry. Edit mode requires a deliberate,
    // stationary hold; we record where the finger landed and reject the
    // long-press if it has drifted (an accidental rest-then-linger, not a hold).
    lv_point_t press_start_point_{};
    bool press_point_valid_ = false;
    /// True if the finger has moved beyond the edit-mode cancel threshold since
    /// the press began. Returns false if no valid press point is being tracked.
    bool finger_drifted_since_press() const;

    // Image change observer (triggers printer image refresh)
    ObserverGuard image_changed_observer_;

    // Multi-page carousel state
    lv_obj_t* carousel_ = nullptr;
    lv_obj_t* carousel_host_ = nullptr;
    lv_obj_t* add_page_tile_ = nullptr;
    lv_obj_t* arrow_left_ = nullptr;
    lv_obj_t* arrow_right_ = nullptr;
    std::vector<std::vector<std::unique_ptr<helix::PanelWidget>>> page_widgets_;
    std::vector<lv_obj_t*> page_containers_;
    std::vector<std::vector<std::string>> page_visible_ids_;
    int active_page_index_ = 0;
    lv_subject_t page_subject_{};
    ObserverGuard page_observer_;

    static constexpr int MAX_PAGES = 8;

    void build_carousel();
    void rebuild_carousel();
    void on_page_changed(int new_page);
    void on_add_page_clicked();
    void update_arrow_visibility(int page);
    void populate_page(int page_index, bool force);

    // Grid and widget lifecycle
    void setup_widget_gate_observers();

    // Panel-level click handlers (not widget-delegated)
    void handle_ams_clicked();

    // Panel-level static callbacks
    static void ams_clicked_cb(lv_event_t* e);
    static void on_home_grid_pressed(lv_event_t* e);
    static void on_home_grid_long_press(lv_event_t* e);
    static void on_home_grid_clicked(lv_event_t* e);
    static void on_home_grid_pressing(lv_event_t* e);
    static void on_home_grid_released(lv_event_t* e);

    friend struct ::HomePanelTestAccess;
};

// Global instance accessor (needed by main.cpp)
HomePanel& get_global_home_panel();
