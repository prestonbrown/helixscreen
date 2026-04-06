// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_tune_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_toast_manager.h"
#include "ui_z_offset_indicator.h"

#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<PrintTuneOverlay> g_print_tune_overlay;

PrintTuneOverlay& get_print_tune_overlay() {
    if (!g_print_tune_overlay) {
        g_print_tune_overlay = std::make_unique<PrintTuneOverlay>();
        StaticPanelRegistry::instance().register_destroy("PrintTuneOverlay",
                                                         []() { g_print_tune_overlay.reset(); });
    }
    return *g_print_tune_overlay;
}

// ============================================================================
// XML EVENT CALLBACKS (free functions using global accessor)
// ============================================================================

// Speed adjust: increment/decrement by delta from user_data
static void on_tune_speed_adjust_cb(lv_event_t* e) {
    const char* delta_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (!delta_str)
        return;
    get_print_tune_overlay().handle_speed_adjust(atoi(delta_str));
}

// Flow adjust: increment/decrement by delta from user_data
static void on_tune_flow_adjust_cb(lv_event_t* e) {
    const char* delta_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (!delta_str)
        return;
    get_print_tune_overlay().handle_flow_adjust(atoi(delta_str));
}

static void on_tune_reset_clicked_cb(lv_event_t* /*e*/) {
    get_print_tune_overlay().handle_reset();
}

// Z-offset step amount selector (user_data = index "0"-"3")
static void on_tune_z_step_cb(lv_event_t* e) {
    const char* idx_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (!idx_str)
        return;
    get_print_tune_overlay().handle_z_step_select(atoi(idx_str));
}

// Z-offset direction adjust (user_data = "-1" closer or "1" farther)
static void on_tune_z_adjust_cb(lv_event_t* e) {
    const char* dir_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (!dir_str)
        return;
    get_print_tune_overlay().handle_z_adjust(atoi(dir_str));
}

static void on_tune_save_z_offset_cb(lv_event_t* /*e*/) {
    get_print_tune_overlay().handle_save_z_offset();
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintTuneOverlay::PrintTuneOverlay() {
    spdlog::debug("[PrintTuneOverlay] Created");
}

PrintTuneOverlay::~PrintTuneOverlay() {
    // Clean up observers before subjects
    speed_observer_.reset();
    gcode_speed_observer_.reset();
    max_velocity_observer_.reset();
    extruder_vel_observer_.reset();

    // Clean up subjects
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Panel widget is owned by LVGL parent, will be cleaned up when parent is deleted
    tune_panel_ = nullptr;

    spdlog::trace("[PrintTuneOverlay] Destroyed");
}

// ============================================================================
// SHOW (PUBLIC ENTRY POINT)
// ============================================================================

void PrintTuneOverlay::show(lv_obj_t* parent_screen, MoonrakerAPI* api,
                            PrinterState& printer_state) {
    spdlog::debug("[PrintTuneOverlay] show() called");

    // Store dependencies
    parent_screen_ = parent_screen;
    api_ = api;
    printer_state_ = &printer_state;

    // Initialize subjects if not already done (before XML creation)
    if (!subjects_initialized_) {
        init_subjects_internal();
    }

    // Create panel lazily
    if (!tune_panel_ && parent_screen_) {
        tune_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "print_tune_panel", nullptr));
        if (!tune_panel_) {
            spdlog::error("[PrintTuneOverlay] Failed to create panel from XML");
            NOTIFY_ERROR(lv_tr("Failed to load print tune panel"));
            return;
        }

        // Setup panel (back button, etc.)
        setup_panel();
        lv_obj_add_flag(tune_panel_, LV_OBJ_FLAG_HIDDEN);

        // Keep base class in sync for cleanup and get_root()
        overlay_root_ = tune_panel_;

        spdlog::info("[PrintTuneOverlay] Panel created");
    }

    if (!tune_panel_) {
        spdlog::error("[PrintTuneOverlay] Cannot show - panel not created");
        return;
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(tune_panel_, this);

    // Push onto navigation stack (on_activate will be called after animation)
    NavigationManager::instance().push_overlay(tune_panel_);
}

// ============================================================================
// INTERNAL: INITIALIZATION
// ============================================================================

void PrintTuneOverlay::init_subjects_internal() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize tune panel subjects
    UI_MANAGED_SUBJECT_STRING(tune_speed_subject_, tune_speed_buf_, "100%", "tune_speed_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_flow_subject_, tune_flow_buf_, "100%", "tune_flow_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_z_offset_subject_, tune_z_offset_buf_, "0.000mm",
                              "tune_z_offset_display", subjects_);

    // Actual speed/flow display subjects (read-only, updated by observers)
    UI_MANAGED_SUBJECT_STRING(tune_actual_speed_subject_, tune_actual_speed_buf_, "",
                              "tune_actual_speed_display", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_actual_flow_subject_, tune_actual_flow_buf_, "",
                              "tune_actual_flow_display", subjects_);

    // Z-offset direction button icons (kinematic-aware, like motion panel)
    UI_MANAGED_SUBJECT_STRING(z_closer_icon_subject_, z_closer_icon_buf_, "arrow_down",
                              "tune_z_closer_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(z_farther_icon_subject_, z_farther_icon_buf_, "arrow_up",
                              "tune_z_farther_icon", subjects_);

    // Z-offset step amount boolean subjects (L040: one per button for bind_style radio pattern)
    UI_MANAGED_SUBJECT_INT(z_step_active_subjects_[0], 0, "z_step_0_active", subjects_);
    UI_MANAGED_SUBJECT_INT(z_step_active_subjects_[1], 0, "z_step_1_active", subjects_);
    UI_MANAGED_SUBJECT_INT(z_step_active_subjects_[2], 1, "z_step_2_active", subjects_); // default
    UI_MANAGED_SUBJECT_INT(z_step_active_subjects_[3], 0, "z_step_3_active", subjects_);

    // Register XML event callbacks
    register_xml_callbacks({
        {"on_tune_speed_adjust", on_tune_speed_adjust_cb},
        {"on_tune_flow_adjust", on_tune_flow_adjust_cb},
        {"on_tune_reset_clicked", on_tune_reset_clicked_cb},
        {"on_tune_save_z_offset", on_tune_save_z_offset_cb},
        {"on_tune_z_step", on_tune_z_step_cb},
        {"on_tune_z_adjust", on_tune_z_adjust_cb},
    });

    subjects_initialized_ = true;
    spdlog::debug("[PrintTuneOverlay] Subjects initialized");
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void PrintTuneOverlay::on_activate() {
    OverlayBase::on_activate();
    sync_to_state();
    spdlog::debug("[PrintTuneOverlay] Activated - synced to state");
}

void PrintTuneOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    spdlog::debug("[PrintTuneOverlay] Deactivated");
}

void PrintTuneOverlay::setup_panel() {
    if (!tune_panel_ || !parent_screen_) {
        return;
    }

    // Use standard overlay panel setup for back button handling
    ui_overlay_panel_setup_standard(tune_panel_, parent_screen_, "overlay_header",
                                    "overlay_content");

    // Update Z-offset icons based on printer kinematics
    update_z_offset_icons(tune_panel_);

    // Observe speed-related subjects for live actual speed display
    if (printer_state_) {
        speed_observer_ = helix::ui::observe_int_sync<PrintTuneOverlay>(
            printer_state_->get_speed_factor_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_actual_speed_display(); });
        gcode_speed_observer_ = helix::ui::observe_int_sync<PrintTuneOverlay>(
            printer_state_->get_gcode_speed_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_actual_speed_display(); });
        max_velocity_observer_ = helix::ui::observe_int_sync<PrintTuneOverlay>(
            printer_state_->get_max_velocity_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_actual_speed_display(); });
        // Observe extruder velocity for live flow display
        extruder_vel_observer_ = helix::ui::observe_int_sync<PrintTuneOverlay>(
            printer_state_->get_live_extruder_velocity_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_actual_flow_display(); });
    }

    spdlog::debug("[PrintTuneOverlay] Panel setup complete");
}

void PrintTuneOverlay::sync_to_state() {
    if (!tune_panel_ || !printer_state_) {
        return;
    }

    // Get current values from PrinterState
    int speed = lv_subject_get_int(printer_state_->get_speed_factor_subject());
    int flow = lv_subject_get_int(printer_state_->get_flow_factor_subject());

    // Update our cached values and displays
    speed_percent_ = speed;
    flow_percent_ = flow;
    update_display();

    // Sync Z offset from PrinterState
    int z_offset_microns = lv_subject_get_int(printer_state_->get_gcode_z_offset_subject());
    update_z_offset_display(z_offset_microns);

    // Sync the visual indicator
    lv_obj_t* indicator = lv_obj_find_by_name(tune_panel_, "z_offset_indicator");
    if (indicator) {
        ui_z_offset_indicator_set_value(indicator, z_offset_microns);
    }

    // Update actual speed/flow displays
    update_actual_speed_display();
    update_actual_flow_display();

    spdlog::debug("[PrintTuneOverlay] Synced to state: speed={}%, flow={}%", speed, flow);
}

// ============================================================================
// ICON UPDATES
// ============================================================================

void PrintTuneOverlay::update_z_offset_icons(lv_obj_t* /*panel*/) {
    if (!printer_state_) {
        spdlog::warn("[PrintTuneOverlay] Cannot update icons - no printer_state_");
        return;
    }

    // Get kinematics type from PrinterState
    // 0 = unknown, 1 = bed moves Z (CoreXY), 2 = head moves Z (Cartesian/Delta)
    int kin = lv_subject_get_int(printer_state_->get_printer_bed_moves_subject());
    bool bed_moves_z = (kin == 1);

    // Set icon names via string subjects (bind_icon in XML)
    // Closer = more squish. On bed-moves-Z (CoreXY): bed rises (expand_up).
    // On head-moves-Z (Cartesian): head descends (arrow_down).
    const char* closer_icon = bed_moves_z ? "arrow_expand_up" : "arrow_down";
    const char* farther_icon = bed_moves_z ? "arrow_expand_down" : "arrow_up";

    std::strncpy(z_closer_icon_buf_, closer_icon, sizeof(z_closer_icon_buf_) - 1);
    lv_subject_copy_string(&z_closer_icon_subject_, z_closer_icon_buf_);
    std::strncpy(z_farther_icon_buf_, farther_icon, sizeof(z_farther_icon_buf_) - 1);
    lv_subject_copy_string(&z_farther_icon_subject_, z_farther_icon_buf_);

    spdlog::debug("[PrintTuneOverlay] Z-offset icons: closer={}, farther={}", closer_icon,
                  farther_icon);
}

// ============================================================================
// DISPLAY UPDATES
// ============================================================================

void PrintTuneOverlay::update_display() {
    helix::format::format_percent(speed_percent_, tune_speed_buf_, sizeof(tune_speed_buf_));
    lv_subject_copy_string(&tune_speed_subject_, tune_speed_buf_);

    helix::format::format_percent(flow_percent_, tune_flow_buf_, sizeof(tune_flow_buf_));
    lv_subject_copy_string(&tune_flow_subject_, tune_flow_buf_);
}

void PrintTuneOverlay::update_speed_flow_display(int speed_percent, int flow_percent) {
    speed_percent_ = speed_percent;
    flow_percent_ = flow_percent;

    if (subjects_initialized_) {
        update_display();
    }
}

void PrintTuneOverlay::update_z_offset_display(int microns) {
    // Update display from PrinterState (microns -> mm)
    current_z_offset_ = microns / 1000.0;

    if (subjects_initialized_) {
        helix::format::format_distance_mm(current_z_offset_, 3, tune_z_offset_buf_,
                                          sizeof(tune_z_offset_buf_));
        lv_subject_copy_string(&tune_z_offset_subject_, tune_z_offset_buf_);
    }

    spdlog::trace("[PrintTuneOverlay] Z-offset display updated: {}um ({}mm)", microns,
                  current_z_offset_);
}

void PrintTuneOverlay::update_actual_speed_display() {
    if (!printer_state_ || !subjects_initialized_)
        return;

    int gcode_speed = lv_subject_get_int(printer_state_->get_gcode_speed_subject());
    int speed_factor = lv_subject_get_int(printer_state_->get_speed_factor_subject());
    int max_velocity = lv_subject_get_int(printer_state_->get_max_velocity_subject());

    // Effective speed = gcode_speed * speed_factor / 100
    int effective_speed = gcode_speed * speed_factor / 100;

    if (max_velocity > 0) {
        std::snprintf(tune_actual_speed_buf_, sizeof(tune_actual_speed_buf_), "%d / %d mm/s",
                      effective_speed, max_velocity);
    } else {
        std::snprintf(tune_actual_speed_buf_, sizeof(tune_actual_speed_buf_), "%d mm/s",
                      effective_speed);
    }
    lv_subject_copy_string(&tune_actual_speed_subject_, tune_actual_speed_buf_);
}

void PrintTuneOverlay::update_actual_flow_display() {
    if (!printer_state_ || !subjects_initialized_)
        return;

    // live_extruder_velocity is in centimm/s (x100)
    int vel_centimm = lv_subject_get_int(printer_state_->get_live_extruder_velocity_subject());

    // Volumetric flow = extruder_velocity * pi * (d/2)^2
    // For 1.75mm filament: cross-section area = 2.405 mm^2
    // TODO: support 2.85mm filament (area = 6.379 mm^2) via printer config
    static constexpr double FILAMENT_AREA_175 = 2.405;
    double flow_mm3_s = (vel_centimm / 100.0) * FILAMENT_AREA_175;

    if (flow_mm3_s >= 0.1) {
        std::snprintf(tune_actual_flow_buf_, sizeof(tune_actual_flow_buf_), "%.1f mm\xC2\xB3/s",
                      flow_mm3_s);
    } else {
        tune_actual_flow_buf_[0] = '\0';
    }
    lv_subject_copy_string(&tune_actual_flow_subject_, tune_actual_flow_buf_);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void PrintTuneOverlay::handle_speed_adjust(int delta) {
    speed_percent_ = std::clamp(speed_percent_ + delta, 50, 200);
    update_display();

    if (api_) {
        int value = speed_percent_;
        std::string gcode = "M220 S" + std::to_string(value);
        api_->execute_gcode(
            gcode, [value]() { spdlog::debug("[PrintTuneOverlay] Speed set to {}%", value); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintTuneOverlay] Failed to set speed: {}", err.message);
                NOTIFY_ERROR(lv_tr("Failed to set print speed: {}"), err.user_message());
            });
    }
}

void PrintTuneOverlay::handle_flow_adjust(int delta) {
    flow_percent_ = std::clamp(flow_percent_ + delta, 75, 125);
    update_display();

    if (api_) {
        int value = flow_percent_;
        std::string gcode = "M221 S" + std::to_string(value);
        api_->execute_gcode(
            gcode, [value]() { spdlog::debug("[PrintTuneOverlay] Flow set to {}%", value); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintTuneOverlay] Failed to set flow: {}", err.message);
                NOTIFY_ERROR(lv_tr("Failed to set flow rate: {}"), err.user_message());
            });
    }
}

void PrintTuneOverlay::handle_reset() {
    speed_percent_ = 100;
    flow_percent_ = 100;
    update_display();

    if (api_) {
        api_->execute_gcode(
            "M220 S100", []() { spdlog::debug("[PrintTuneOverlay] Speed reset to 100%"); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Failed to reset speed: {}"), err.user_message());
            });
        api_->execute_gcode(
            "M221 S100", []() { spdlog::debug("[PrintTuneOverlay] Flow reset to 100%"); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Failed to reset flow: {}"), err.user_message());
            });
    }
}

void PrintTuneOverlay::handle_z_offset_changed(double delta) {
    // Clamp to safe range to prevent accidental bed crashes or huge offsets
    double new_offset = current_z_offset_ + delta;
    if (new_offset < Z_OFFSET_MIN || new_offset > Z_OFFSET_MAX) {
        spdlog::warn("[PrintTuneOverlay] Z-offset {:.3f}mm clamped to [{}, {}]", new_offset,
                     Z_OFFSET_MIN, Z_OFFSET_MAX);
        new_offset = std::clamp(new_offset, Z_OFFSET_MIN, Z_OFFSET_MAX);
        delta = new_offset - current_z_offset_;
        if (std::abs(delta) < 0.0005)
            return; // Already at limit
    }

    // Round to nearest micron to prevent floating-point drift from repeated additions
    current_z_offset_ = std::round(new_offset * 1000.0) / 1000.0;
    helix::format::format_distance_mm(current_z_offset_, 3, tune_z_offset_buf_,
                                      sizeof(tune_z_offset_buf_));
    lv_subject_copy_string(&tune_z_offset_subject_, tune_z_offset_buf_);

    // Track pending delta for "unsaved adjustment" notification in Controls panel
    if (printer_state_) {
        int delta_microns = static_cast<int>(std::lround(delta * 1000.0));
        printer_state_->add_pending_z_offset_delta(delta_microns);

        // Immediately update the gcode_z_offset subject so Controls panel reflects the change
        // (otherwise it waits for Moonraker to broadcast the status update)
        int current_microns = static_cast<int>(std::lround(current_z_offset_ * 1000.0));
        if (auto* subj = printer_state_->get_gcode_z_offset_subject()) {
            lv_subject_set_int(subj, current_microns);
        }
    }

    spdlog::debug("[PrintTuneOverlay] Z-offset adjust: {:+.3f}mm (total: {:.3f}mm)", delta,
                  current_z_offset_);

    // Update the visual indicator
    if (tune_panel_) {
        lv_obj_t* indicator = lv_obj_find_by_name(tune_panel_, "z_offset_indicator");
        if (indicator) {
            int microns = static_cast<int>(current_z_offset_ * 1000.0);
            ui_z_offset_indicator_set_value(indicator, microns);
            ui_z_offset_indicator_flash_direction(indicator, delta > 0 ? 1 : -1);
        }
    }

    // Send SET_GCODE_OFFSET Z_ADJUST command to Klipper.
    // MOVE=1 makes the toolhead physically move to the new offset immediately,
    // which is essential for baby stepping during a print. Without it, the offset
    // only takes effect on the next Z move in gcode. Only add MOVE=1 when all
    // axes are homed (matching Mainsail behavior) to avoid Klipper errors.
    if (api_) {
        bool all_homed = false;
        if (printer_state_) {
            const char* axes = lv_subject_get_string(printer_state_->get_homed_axes_subject());
            all_homed = axes && strchr(axes, 'x') && strchr(axes, 'y') && strchr(axes, 'z');
        }

        char gcode[96];
        std::snprintf(gcode, sizeof(gcode), "SET_GCODE_OFFSET Z_ADJUST=%.3f%s", delta,
                      all_homed ? " MOVE=1" : "");
        api_->execute_gcode(
            gcode, [delta]() { spdlog::debug("[PrintTuneOverlay] Z adjusted {:+.3f}mm", delta); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintTuneOverlay] Z-offset adjust failed: {}", err.message);
                NOTIFY_ERROR(lv_tr("Z-offset failed: {}"), err.user_message());
            });
    }
}

void PrintTuneOverlay::handle_z_step_select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(Z_STEP_AMOUNTS))) {
        spdlog::warn("[PrintTuneOverlay] Invalid step index: {}", idx);
        return;
    }
    selected_z_step_idx_ = idx;

    // Update boolean subjects (only one active at a time, like filament panel)
    for (int i = 0; i < static_cast<int>(std::size(Z_STEP_AMOUNTS)); i++) {
        lv_subject_set_int(&z_step_active_subjects_[i], i == idx ? 1 : 0);
    }

    spdlog::debug("[PrintTuneOverlay] Z-offset step selected: {}mm", Z_STEP_AMOUNTS[idx]);
}

void PrintTuneOverlay::handle_z_adjust(int direction) {
    double amount = Z_STEP_AMOUNTS[selected_z_step_idx_];
    handle_z_offset_changed(direction * amount);
}

void PrintTuneOverlay::handle_save_z_offset() {
    if (printer_state_) {
        auto strategy = printer_state_->get_z_offset_calibration_strategy();
        if (helix::zoffset::is_auto_saved(strategy))
            return;
    }

    save_z_offset_modal_.set_on_confirm([this]() {
        if (!api_ || !printer_state_)
            return;

        auto strategy = printer_state_->get_z_offset_calibration_strategy();
        helix::zoffset::apply_and_save(
            api_, strategy,
            []() {
                spdlog::info("[PrintTuneOverlay] Z-offset saved — Klipper restarting");
                ToastManager::instance().show(
                    ToastSeverity::WARNING, lv_tr("Z-offset saved - Klipper restarting..."), 5000);
            },
            [](const std::string& error) {
                spdlog::error("[PrintTuneOverlay] Save failed: {}", error);
                NOTIFY_ERROR(lv_tr("Save failed: {}"), error);
            });
    });
    save_z_offset_modal_.show(lv_screen_active());
}
