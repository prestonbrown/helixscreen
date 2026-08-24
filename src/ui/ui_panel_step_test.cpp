// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_step_test.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"

#include "app_globals.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

using namespace helix;

// Step definitions for vertical progress (retract wizard)
static const ui_step_t VERTICAL_STEPS[] = {{"Nozzle heating", StepState::Completed},
                                           {"Prepare to retract", StepState::Active},
                                           {"Retracting", StepState::Pending},
                                           {"Retract done", StepState::Pending}};
static const int VERTICAL_STEP_COUNT = sizeof(VERTICAL_STEPS) / sizeof(VERTICAL_STEPS[0]);

// Step definitions for horizontal progress (leveling wizard)
static const ui_step_t HORIZONTAL_STEPS[] = {{"Homing", StepState::Completed},
                                             {"Leveling", StepState::Active},
                                             {"Vibration test", StepState::Pending},
                                             {"Completed", StepState::Pending}};
static const int HORIZONTAL_STEP_COUNT = sizeof(HORIZONTAL_STEPS) / sizeof(HORIZONTAL_STEPS[0]);

// ============================================================================
// CONSTRUCTOR
// ============================================================================

StepTestPanel::StepTestPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // StepTestPanel doesn't use PrinterState or IMoonrakerAPI, but we accept
    // them for interface consistency with other panels
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void StepTestPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Register XML event callbacks (must be done BEFORE XML is created)
    register_xml_callbacks({
        {"on_step_test_prev", on_prev_clicked},
        {"on_step_test_next", on_next_clicked},
        {"on_step_test_complete", on_complete_clicked},
    });

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized, event callbacks registered", get_name());
}

void StepTestPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Create the step progress widgets
    create_progress_widgets();

    // Note: Button handlers are now wired via XML event_cb declarations
    // and registered in init_subjects() via lv_xml_register_event_cb()

    spdlog::info("[{}] Setup complete", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void StepTestPanel::create_progress_widgets() {
    // Find container widgets
    lv_obj_t* vertical_container = lv_obj_find_by_name(panel_, "vertical_progress_container");
    lv_obj_t* horizontal_container = lv_obj_find_by_name(panel_, "horizontal_progress_container");

    spdlog::debug("[{}] Found containers: vertical={}, horizontal={}", get_name(),
                  static_cast<void*>(vertical_container), static_cast<void*>(horizontal_container));

    if (!vertical_container || !horizontal_container) {
        spdlog::error("[{}] Failed to find progress containers", get_name());
        return;
    }

    // Create vertical progress widget with theme colors from step_test_panel scope
    vertical_widget_ = ui_step_progress_create(vertical_container, VERTICAL_STEPS,
                                               VERTICAL_STEP_COUNT, false, "step_test_panel");
    if (!vertical_widget_) {
        spdlog::error("[{}] Failed to create vertical progress widget", get_name());
        return;
    }

    // Create horizontal progress widget with theme colors from step_test_panel scope
    horizontal_widget_ = ui_step_progress_create(horizontal_container, HORIZONTAL_STEPS,
                                                 HORIZONTAL_STEP_COUNT, true, "step_test_panel");
    if (!horizontal_widget_) {
        spdlog::error("[{}] Failed to create horizontal progress widget", get_name());
        return;
    }

    // Initialize current steps (step 1 = index 1) and apply styling
    vertical_step_ = 1;
    horizontal_step_ = 1;
    ui_step_progress_set_current(vertical_widget_, vertical_step_);
    ui_step_progress_set_current(horizontal_widget_, horizontal_step_);
}

// ============================================================================
// BUTTON HANDLERS
// ============================================================================

void StepTestPanel::handle_prev() {
    // Move both wizards back one step
    if (vertical_step_ > 0) {
        vertical_step_--;
        ui_step_progress_set_current(vertical_widget_, vertical_step_);
    }

    if (horizontal_step_ > 0) {
        horizontal_step_--;
        ui_step_progress_set_current(horizontal_widget_, horizontal_step_);
    }

    spdlog::debug("[{}] Previous step: vertical={}, horizontal={}", get_name(), vertical_step_,
                  horizontal_step_);
}

void StepTestPanel::handle_next() {
    // Move both wizards forward one step
    if (vertical_step_ < VERTICAL_STEP_COUNT - 1) {
        vertical_step_++;
        ui_step_progress_set_current(vertical_widget_, vertical_step_);
    }

    if (horizontal_step_ < HORIZONTAL_STEP_COUNT - 1) {
        horizontal_step_++;
        ui_step_progress_set_current(horizontal_widget_, horizontal_step_);
    }

    spdlog::debug("[{}] Next step: vertical={}, horizontal={}", get_name(), vertical_step_,
                  horizontal_step_);
}

void StepTestPanel::handle_complete() {
    // Complete all steps for both wizards
    vertical_step_ = VERTICAL_STEP_COUNT - 1;
    horizontal_step_ = HORIZONTAL_STEP_COUNT - 1;

    ui_step_progress_set_current(vertical_widget_, vertical_step_);
    ui_step_progress_set_current(horizontal_widget_, horizontal_step_);

    spdlog::debug("[{}] All steps completed", get_name());
}

// ============================================================================
// STATIC EVENT CALLBACKS (registered via lv_xml_register_event_cb)
// ============================================================================

void StepTestPanel::on_prev_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[StepTestPanel] on_prev_clicked");
    (void)e; // Unused - we use global accessor
    get_global_step_test_panel().handle_prev();
    LVGL_SAFE_EVENT_CB_END();
}

void StepTestPanel::on_next_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[StepTestPanel] on_next_clicked");
    (void)e; // Unused - we use global accessor
    get_global_step_test_panel().handle_next();
    LVGL_SAFE_EVENT_CB_END();
}

void StepTestPanel::on_complete_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[StepTestPanel] on_complete_clicked");
    (void)e; // Unused - we use global accessor
    get_global_step_test_panel().handle_complete();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<StepTestPanel> g_step_test_panel;

StepTestPanel& get_global_step_test_panel() {
    if (!g_step_test_panel) {
        g_step_test_panel = std::make_unique<StepTestPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("StepTestPanel",
                                                         []() { g_step_test_panel.reset(); });
    }
    return *g_step_test_panel;
}
