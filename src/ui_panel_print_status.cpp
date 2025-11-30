// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_print_status.h"

#include "runtime_config.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_gcode_viewer.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_toast.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <memory>

// Global instance for legacy API and resize callback
static std::unique_ptr<PrintStatusPanel> g_print_status_panel;

// Helper to get or create the global instance
PrintStatusPanel& get_global_print_status_panel() {
    if (!g_print_status_panel) {
        g_print_status_panel = std::make_unique<PrintStatusPanel>(get_printer_state(), nullptr);
    }
    return *g_print_status_panel;
}

PrintStatusPanel::PrintStatusPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {

    // Subscribe to PrinterState temperature subjects (ObserverGuard handles cleanup)
    extruder_temp_observer_ = ObserverGuard(printer_state_.get_extruder_temp_subject(),
                                            extruder_temp_observer_cb, this);
    extruder_target_observer_ = ObserverGuard(printer_state_.get_extruder_target_subject(),
                                              extruder_target_observer_cb, this);
    bed_temp_observer_ = ObserverGuard(printer_state_.get_bed_temp_subject(),
                                       bed_temp_observer_cb, this);
    bed_target_observer_ = ObserverGuard(printer_state_.get_bed_target_subject(),
                                         bed_target_observer_cb, this);

    // Subscribe to print progress and state
    print_progress_observer_ = ObserverGuard(printer_state_.get_print_progress_subject(),
                                             print_progress_observer_cb, this);
    print_state_observer_ = ObserverGuard(printer_state_.get_print_state_enum_subject(),
                                          print_state_observer_cb, this);
    print_filename_observer_ = ObserverGuard(printer_state_.get_print_filename_subject(),
                                             print_filename_observer_cb, this);

    // Subscribe to speed/flow factors
    speed_factor_observer_ = ObserverGuard(printer_state_.get_speed_factor_subject(),
                                           speed_factor_observer_cb, this);
    flow_factor_observer_ = ObserverGuard(printer_state_.get_flow_factor_subject(),
                                          flow_factor_observer_cb, this);

    // Subscribe to layer tracking for G-code viewer ghost layer updates
    print_layer_observer_ = ObserverGuard(printer_state_.get_print_layer_current_subject(),
                                          print_layer_observer_cb, this);

    spdlog::debug("[{}] Subscribed to PrinterState subjects", get_name());

    // Load configured LED from wizard settings
    Config* config = Config::get_instance();
    if (config) {
        configured_led_ = config->get<std::string>(WizardConfigPaths::LED_STRIP, "");
        if (!configured_led_.empty()) {
            led_state_observer_ = ObserverGuard(printer_state_.get_led_state_subject(),
                                                led_state_observer_cb, this);
            spdlog::debug("[{}] Configured LED: {} (observing state)", get_name(), configured_led_);
        }
    }
}

PrintStatusPanel::~PrintStatusPanel() {
    // ObserverGuard handles observer cleanup automatically
    resize_registered_ = false;

    // Clean up exclude object resources
    if (exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
        exclude_undo_timer_ = nullptr;
    }
    if (exclude_confirm_dialog_) {
        lv_obj_delete(exclude_confirm_dialog_);
        exclude_confirm_dialog_ = nullptr;
    }
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void PrintStatusPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize all 10 subjects with default values
    UI_SUBJECT_INIT_AND_REGISTER_STRING(filename_subject_, filename_buf_, "No print active",
                                        "print_filename");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(progress_text_subject_, progress_text_buf_, "0%",
                                        "print_progress_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(layer_text_subject_, layer_text_buf_, "Layer 0 / 0",
                                        "print_layer_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(elapsed_subject_, elapsed_buf_, "0h 00m", "print_elapsed");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(remaining_subject_, remaining_buf_, "0h 00m",
                                        "print_remaining");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(nozzle_temp_subject_, nozzle_temp_buf_, "0 / 0°C",
                                        "nozzle_temp_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(bed_temp_subject_, bed_temp_buf_, "0 / 0°C",
                                        "bed_temp_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(speed_subject_, speed_buf_, "100%", "print_speed_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(flow_subject_, flow_buf_, "100%", "print_flow_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(pause_button_subject_, pause_button_buf_, "Pause",
                                        "pause_button_text");

    // Preparing state subjects
    UI_SUBJECT_INIT_AND_REGISTER_INT(preparing_visible_subject_, 0, "preparing_visible");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(preparing_operation_subject_, preparing_operation_buf_,
                                        "Preparing...", "preparing_operation");
    UI_SUBJECT_INIT_AND_REGISTER_INT(preparing_progress_subject_, 0, "preparing_progress");

    // Viewer mode subject (0=thumbnail, 1=gcode viewer)
    UI_SUBJECT_INIT_AND_REGISTER_INT(gcode_viewer_mode_subject_, 0, "gcode_viewer_mode");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (14 subjects)", get_name());
}

void PrintStatusPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::info("[{}] Setting up panel...", get_name());

    // Panel width is set via XML using #overlay_panel_width_large (same as print_file_detail)
    // Use standard overlay panel setup for header/content/back button
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(panel_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return;
    }

    // Find thumbnail section for nested widgets
    lv_obj_t* thumbnail_section = lv_obj_find_by_name(overlay_content, "thumbnail_section");
    if (!thumbnail_section) {
        spdlog::error("[{}] thumbnail_section not found!", get_name());
        return;
    }

    // Find G-code viewer, thumbnail, and gradient background widgets
    gcode_viewer_ = lv_obj_find_by_name(thumbnail_section, "print_gcode_viewer");
    print_thumbnail_ = lv_obj_find_by_name(thumbnail_section, "print_thumbnail");
    gradient_background_ = lv_obj_find_by_name(thumbnail_section, "gradient_background");

    if (gcode_viewer_) {
        spdlog::debug("[{}]   ✓ G-code viewer widget found", get_name());

        // Register long-press callback for exclude object feature
        ui_gcode_viewer_set_object_long_press_callback(gcode_viewer_, on_object_long_pressed, this);
        spdlog::debug("[{}]   ✓ Registered long-press callback for exclude object", get_name());
    } else {
        spdlog::error("[{}]   ✗ G-code viewer widget NOT FOUND", get_name());
    }
    if (print_thumbnail_) {
        spdlog::debug("[{}]   ✓ Print thumbnail widget found", get_name());
    }
    if (gradient_background_) {
        spdlog::debug("[{}]   ✓ Gradient background widget found", get_name());
    }

    // Force layout calculation
    lv_obj_update_layout(panel_);

    // Register resize callback
    ui_resize_handler_register(on_resize_static);
    resize_registered_ = true;

    // Wire up event handlers
    spdlog::debug("[{}] Wiring event handlers...", get_name());

    // Nozzle temperature card
    lv_obj_t* nozzle_card = lv_obj_find_by_name(overlay_content, "nozzle_temp_card");
    if (nozzle_card) {
        lv_obj_add_event_cb(nozzle_card, on_nozzle_card_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Nozzle temp card", get_name());
    } else {
        spdlog::error("[{}]   ✗ Nozzle temp card NOT FOUND", get_name());
    }

    // Bed temperature card
    lv_obj_t* bed_card = lv_obj_find_by_name(overlay_content, "bed_temp_card");
    if (bed_card) {
        lv_obj_add_event_cb(bed_card, on_bed_card_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Bed temp card", get_name());
    } else {
        spdlog::error("[{}]   ✗ Bed temp card NOT FOUND", get_name());
    }

    // Light button
    lv_obj_t* light_btn = lv_obj_find_by_name(overlay_content, "btn_light");
    if (light_btn) {
        lv_obj_add_event_cb(light_btn, on_light_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Light button", get_name());
    } else {
        spdlog::error("[{}]   ✗ Light button NOT FOUND", get_name());
    }

    // Pause button
    lv_obj_t* pause_btn = lv_obj_find_by_name(overlay_content, "btn_pause");
    if (pause_btn) {
        lv_obj_add_event_cb(pause_btn, on_pause_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Pause button", get_name());
    } else {
        spdlog::error("[{}]   ✗ Pause button NOT FOUND", get_name());
    }

    // Tune button
    lv_obj_t* tune_btn = lv_obj_find_by_name(overlay_content, "btn_tune");
    if (tune_btn) {
        lv_obj_add_event_cb(tune_btn, on_tune_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Tune button", get_name());
    } else {
        spdlog::error("[{}]   ✗ Tune button NOT FOUND", get_name());
    }

    // Cancel button
    lv_obj_t* cancel_btn = lv_obj_find_by_name(overlay_content, "btn_cancel");
    if (cancel_btn) {
        lv_obj_add_event_cb(cancel_btn, on_cancel_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Cancel button", get_name());
    } else {
        spdlog::error("[{}]   ✗ Cancel button NOT FOUND", get_name());
    }

    // Progress bar widget
    progress_bar_ = lv_obj_find_by_name(overlay_content, "print_progress");
    if (progress_bar_) {
        lv_bar_set_range(progress_bar_, 0, 100);
        lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        spdlog::debug("[{}]   ✓ Progress bar", get_name());
    } else {
        spdlog::error("[{}]   ✗ Progress bar NOT FOUND", get_name());
    }

    // Check if --gcode-file was specified on command line for this panel
    const auto& config = get_runtime_config();
    if (config.gcode_test_file && gcode_viewer_) {
        spdlog::info("[{}] Loading G-code file from command line: {}", get_name(),
                     config.gcode_test_file);
        load_gcode_file(config.gcode_test_file);
    }

    spdlog::info("[{}] Setup complete!", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void PrintStatusPanel::format_time(int seconds, char* buf, size_t buf_size) {
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    std::snprintf(buf, buf_size, "%dh %02dm", hours, minutes);
}

void PrintStatusPanel::show_gcode_viewer(bool show) {
    // Update viewer mode subject - XML bindings handle visibility reactively
    // Mode 0 = thumbnail (gradient + thumbnail visible, gcode hidden)
    // Mode 1 = gcode viewer (gcode visible, gradient + thumbnail hidden)
    lv_subject_set_int(&gcode_viewer_mode_subject_, show ? 1 : 0);

    spdlog::debug("[{}] G-code viewer mode: {}", get_name(), show ? "gcode" : "thumbnail");
}

void PrintStatusPanel::load_gcode_file(const char* file_path) {
    if (!gcode_viewer_ || !file_path) {
        spdlog::warn("[{}] Cannot load G-code: viewer={}, path={}", get_name(),
                     gcode_viewer_ != nullptr, file_path != nullptr);
        return;
    }

    spdlog::info("[{}] Loading G-code file: {}", get_name(), file_path);

    // Register callback to be notified when loading completes
    ui_gcode_viewer_set_load_callback(
        gcode_viewer_,
        [](lv_obj_t* viewer, void* user_data, bool success) {
            auto* self = static_cast<PrintStatusPanel*>(user_data);
            if (!success) {
                spdlog::error("[{}] G-code load failed", self->get_name());
                return;
            }

            // Get layer count from loaded geometry
            int max_layer = ui_gcode_viewer_get_max_layer(viewer);
            spdlog::info("[{}] G-code loaded: {} layers", self->get_name(), max_layer);

            // Show the viewer (hide gradient and thumbnail)
            self->show_gcode_viewer(true);

            // Set print progress to layer 0 (entire model in ghost mode initially)
            ui_gcode_viewer_set_print_progress(viewer, 0);

            // Extract filename from path for display
            const char* filename = ui_gcode_viewer_get_filename(viewer);
            if (!filename) {
                filename = "print.gcode";
            }

            // Start print via MoonrakerAPI
            // In test mode, mock Moonraker handles simulation via observers
            if (self->api_) {
                self->api_->start_print(
                    filename,
                    []() { spdlog::info("[PrintStatusPanel] Print started via Moonraker"); },
                    [](const MoonrakerError& err) {
                        spdlog::error("[PrintStatusPanel] Failed to start print: {}", err.message);
                    });
            } else {
                spdlog::warn("[{}] No API available - G-code loaded but print not started",
                             self->get_name());
            }
        },
        this);

    // Start loading the file
    ui_gcode_viewer_load_file(gcode_viewer_, file_path);
}

void PrintStatusPanel::update_all_displays() {
    // Guard: don't update if subjects aren't initialized yet
    if (!subjects_initialized_) {
        return;
    }

    // Progress text
    std::snprintf(progress_text_buf_, sizeof(progress_text_buf_), "%d%%", current_progress_);
    lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);

    // Layer text
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), "Layer %d / %d", current_layer_,
                  total_layers_);
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

    // Time displays
    format_time(elapsed_seconds_, elapsed_buf_, sizeof(elapsed_buf_));
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);

    format_time(remaining_seconds_, remaining_buf_, sizeof(remaining_buf_));
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);

    // Temperatures
    std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_), "%d / %d°C", nozzle_current_,
                  nozzle_target_);
    lv_subject_copy_string(&nozzle_temp_subject_, nozzle_temp_buf_);

    std::snprintf(bed_temp_buf_, sizeof(bed_temp_buf_), "%d / %d°C", bed_current_, bed_target_);
    lv_subject_copy_string(&bed_temp_subject_, bed_temp_buf_);

    // Speeds
    std::snprintf(speed_buf_, sizeof(speed_buf_), "%d%%", speed_percent_);
    lv_subject_copy_string(&speed_subject_, speed_buf_);

    std::snprintf(flow_buf_, sizeof(flow_buf_), "%d%%", flow_percent_);
    lv_subject_copy_string(&flow_subject_, flow_buf_);

    // Update progress bar widget directly
    if (progress_bar_) {
        lv_bar_set_value(progress_bar_, current_progress_, LV_ANIM_OFF);
    }

    // Update pause button text based on state
    if (current_state_ == PrintState::Paused) {
        std::snprintf(pause_button_buf_, sizeof(pause_button_buf_), "Resume");
    } else {
        std::snprintf(pause_button_buf_, sizeof(pause_button_buf_), "Pause");
    }
    lv_subject_copy_string(&pause_button_subject_, pause_button_buf_);
}

// ============================================================================
// INSTANCE HANDLERS
// ============================================================================

void PrintStatusPanel::handle_nozzle_card_click() {
    spdlog::debug("[{}] Nozzle temp card clicked", get_name());
    // TODO: Show nozzle temperature adjustment panel
}

void PrintStatusPanel::handle_bed_card_click() {
    spdlog::debug("[{}] Bed temp card clicked", get_name());
    // TODO: Show bed temperature adjustment panel
}

void PrintStatusPanel::handle_light_button() {
    spdlog::info("[{}] Light button clicked", get_name());

    // Check if LED is configured
    if (configured_led_.empty()) {
        spdlog::warn("[{}] Light toggle called but no LED configured", get_name());
        return;
    }

    // Toggle to opposite of current state
    bool new_state = !led_on_;

    // Send command to Moonraker
    if (api_) {
        if (new_state) {
            api_->set_led_on(
                configured_led_,
                [this]() {
                    spdlog::info("[{}] LED turned ON - waiting for state update", get_name());
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to turn LED on: {}", err.message);
                    NOTIFY_ERROR("Failed to turn light on: {}", err.user_message());
                });
        } else {
            api_->set_led_off(
                configured_led_,
                [this]() {
                    spdlog::info("[{}] LED turned OFF - waiting for state update", get_name());
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to turn LED off: {}", err.message);
                    NOTIFY_ERROR("Failed to turn light off: {}", err.user_message());
                });
        }
    } else {
        spdlog::warn("[{}] API not available - cannot control LED", get_name());
        NOTIFY_ERROR("Cannot control light: printer not connected");
    }
}

void PrintStatusPanel::handle_pause_button() {
    if (current_state_ == PrintState::Printing) {
        spdlog::info("[{}] Pausing print...", get_name());

        if (api_) {
            api_->pause_print(
                [this]() {
                    spdlog::info("[{}] Pause command sent successfully", get_name());
                    // State will update via PrinterState observer when Moonraker confirms
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to pause print: {}", err.message);
                    NOTIFY_ERROR("Failed to pause print: {}", err.user_message());
                });
        } else {
            // Fall back to local state change for mock mode
            spdlog::warn("[{}] API not available - using local state change", get_name());
            set_state(PrintState::Paused);
        }
    } else if (current_state_ == PrintState::Paused) {
        spdlog::info("[{}] Resuming print...", get_name());

        if (api_) {
            api_->resume_print(
                [this]() {
                    spdlog::info("[{}] Resume command sent successfully", get_name());
                    // State will update via PrinterState observer when Moonraker confirms
                },
                [](const MoonrakerError& err) {
                    spdlog::error("Failed to resume print: {}", err.message);
                    NOTIFY_ERROR("Failed to resume print: {}", err.user_message());
                });
        } else {
            // Fall back to local state change for mock mode
            spdlog::warn("[{}] API not available - using local state change", get_name());
            set_state(PrintState::Printing);
        }
    }
}

void PrintStatusPanel::handle_tune_button() {
    spdlog::info("[{}] Tune button clicked (not yet implemented)", get_name());
    // TODO: Open tuning overlay with speed/flow/temp adjustments
}

void PrintStatusPanel::handle_cancel_button() {
    spdlog::info("[{}] Cancel button clicked", get_name());

    // TODO: Add confirmation dialog before canceling

    if (api_) {
        api_->cancel_print(
            [this]() {
                spdlog::info("[{}] Cancel command sent successfully", get_name());
                // State will update via PrinterState observer when Moonraker confirms
            },
            [](const MoonrakerError& err) {
                spdlog::error("Failed to cancel print: {}", err.message);
                NOTIFY_ERROR("Failed to cancel print: {}", err.user_message());
            });
    } else {
        spdlog::warn("[{}] API not available - cannot cancel print", get_name());
        NOTIFY_ERROR("Cannot cancel: not connected to printer");
    }
}

void PrintStatusPanel::handle_resize() {
    spdlog::debug("[{}] Handling resize event", get_name());
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void PrintStatusPanel::on_nozzle_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_nozzle_card_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_nozzle_card_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_bed_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_bed_card_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_bed_card_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_light_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_light_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_light_button();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_pause_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_pause_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_pause_button();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_tune_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_tune_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_tune_button();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_cancel_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_cancel_clicked");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_cancel_button();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::on_resize_static() {
    // Use global instance for resize callback (registered without user_data)
    if (g_print_status_panel) {
        g_print_status_panel->handle_resize();
    }
}

// ============================================================================
// PRINTERSTATE OBSERVER CALLBACKS
// ============================================================================

void PrintStatusPanel::extruder_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_temperature_changed();
    }
}

void PrintStatusPanel::extruder_target_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_temperature_changed();
    }
}

void PrintStatusPanel::bed_temp_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_temperature_changed();
    }
}

void PrintStatusPanel::bed_target_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_temperature_changed();
    }
}

void PrintStatusPanel::print_progress_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_print_progress_changed(lv_subject_get_int(subject));
    }
}

void PrintStatusPanel::print_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        // Read enum from integer subject (type-safe, no string parsing)
        auto state = static_cast<PrintJobState>(lv_subject_get_int(subject));
        self->on_print_state_changed(state);
    }
}

void PrintStatusPanel::print_filename_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_print_filename_changed(lv_subject_get_string(subject));
    }
}

void PrintStatusPanel::speed_factor_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_speed_factor_changed(lv_subject_get_int(subject));
    }
}

void PrintStatusPanel::flow_factor_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_flow_factor_changed(lv_subject_get_int(subject));
    }
}

void PrintStatusPanel::led_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_led_state_changed(lv_subject_get_int(subject));
    }
}

void PrintStatusPanel::print_layer_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<PrintStatusPanel*>(lv_observer_get_user_data(observer));
    if (self) {
        self->on_print_layer_changed(lv_subject_get_int(subject));
    }
}

// ============================================================================
// OBSERVER INSTANCE METHODS
// ============================================================================

void PrintStatusPanel::on_temperature_changed() {
    // Read all temperature values from PrinterState subjects
    int extruder_temp = lv_subject_get_int(printer_state_.get_extruder_temp_subject());
    int extruder_target = lv_subject_get_int(printer_state_.get_extruder_target_subject());
    int bed_temp = lv_subject_get_int(printer_state_.get_bed_temp_subject());
    int bed_target = lv_subject_get_int(printer_state_.get_bed_target_subject());

    // Update internal state and display
    set_temperatures(extruder_temp, extruder_target, bed_temp, bed_target);

    spdlog::trace("[{}] Temperatures updated: nozzle {}/{}°C, bed {}/{}°C", get_name(),
                  extruder_temp, extruder_target, bed_temp, bed_target);
}

void PrintStatusPanel::on_print_progress_changed(int progress) {
    // Update progress display without calling update_all_displays()
    // to avoid redundant updates when multiple subjects change
    current_progress_ = progress;
    if (current_progress_ < 0)
        current_progress_ = 0;
    if (current_progress_ > 100)
        current_progress_ = 100;

    // Update progress text
    std::snprintf(progress_text_buf_, sizeof(progress_text_buf_), "%d%%", current_progress_);
    lv_subject_copy_string(&progress_text_subject_, progress_text_buf_);

    // Update progress bar widget directly
    if (progress_bar_) {
        lv_bar_set_value(progress_bar_, current_progress_, LV_ANIM_OFF);
    }

    spdlog::trace("[{}] Progress updated: {}%", get_name(), current_progress_);
}

void PrintStatusPanel::on_print_state_changed(PrintJobState job_state) {
    // Map PrintJobState (from PrinterState) to PrintState (UI-specific)
    // Note: PrintState has a Preparing state that doesn't exist in PrintJobState -
    // that's managed locally by set_preparing()/end_preparing()
    PrintState new_state = PrintState::Idle;

    switch (job_state) {
    case PrintJobState::STANDBY:
        new_state = PrintState::Idle;
        break;
    case PrintJobState::PRINTING:
        new_state = PrintState::Printing;
        break;
    case PrintJobState::PAUSED:
        new_state = PrintState::Paused;
        break;
    case PrintJobState::COMPLETE:
        new_state = PrintState::Complete;
        break;
    case PrintJobState::CANCELLED:
        new_state = PrintState::Cancelled;
        break;
    case PrintJobState::ERROR:
        new_state = PrintState::Error;
        break;
    }

    // Only update if state actually changed
    if (new_state != current_state_) {
        set_state(new_state);
        spdlog::info("[{}] Print state changed: {} -> {}", get_name(),
                     print_job_state_to_string(job_state), static_cast<int>(new_state));

        // Toggle G-code viewer visibility based on print state
        // Show viewer during printing/paused, hide during idle/complete
        bool show_viewer = (new_state == PrintState::Printing || new_state == PrintState::Paused);
        show_gcode_viewer(show_viewer);
    }
}

void PrintStatusPanel::on_print_filename_changed(const char* filename) {
    if (filename && filename[0] != '\0') {
        set_filename(filename);
        spdlog::debug("[{}] Filename updated: {}", get_name(), filename);
    }
}

void PrintStatusPanel::on_speed_factor_changed(int speed) {
    speed_percent_ = speed;
    if (subjects_initialized_) {
        std::snprintf(speed_buf_, sizeof(speed_buf_), "%d%%", speed_percent_);
        lv_subject_copy_string(&speed_subject_, speed_buf_);
    }
    spdlog::trace("[{}] Speed factor updated: {}%", get_name(), speed);
}

void PrintStatusPanel::on_flow_factor_changed(int flow) {
    flow_percent_ = flow;
    if (subjects_initialized_) {
        std::snprintf(flow_buf_, sizeof(flow_buf_), "%d%%", flow_percent_);
        lv_subject_copy_string(&flow_subject_, flow_buf_);
    }
    spdlog::trace("[{}] Flow factor updated: {}%", get_name(), flow);
}

void PrintStatusPanel::on_led_state_changed(int state) {
    led_on_ = (state != 0);
    spdlog::debug("[{}] LED state changed: {} (from PrinterState)", get_name(),
                  led_on_ ? "ON" : "OFF");
}

void PrintStatusPanel::on_print_layer_changed(int current_layer) {
    // Update internal layer state
    current_layer_ = current_layer;
    int total_layers = lv_subject_get_int(printer_state_.get_print_layer_total_subject());
    total_layers_ = total_layers;

    // Update the layer text display
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), "Layer %d / %d", current_layer_,
                  total_layers_);
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);

    // Update G-code viewer ghost layer if viewer is active and visible
    if (gcode_viewer_ && !lv_obj_has_flag(gcode_viewer_, LV_OBJ_FLAG_HIDDEN)) {
        ui_gcode_viewer_set_print_progress(gcode_viewer_, current_layer);
        spdlog::trace("[{}] G-code viewer ghost layer updated to {}", get_name(), current_layer);
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void PrintStatusPanel::set_filename(const char* filename) {
    std::snprintf(filename_buf_, sizeof(filename_buf_), "%s", filename);
    lv_subject_copy_string(&filename_subject_, filename_buf_);
}

void PrintStatusPanel::set_progress(int percent) {
    current_progress_ = percent;
    if (current_progress_ < 0)
        current_progress_ = 0;
    if (current_progress_ > 100)
        current_progress_ = 100;
    update_all_displays();
}

void PrintStatusPanel::set_layer(int current, int total) {
    current_layer_ = current;
    total_layers_ = total;
    update_all_displays();
}

void PrintStatusPanel::set_times(int elapsed_secs, int remaining_secs) {
    elapsed_seconds_ = elapsed_secs;
    remaining_seconds_ = remaining_secs;
    update_all_displays();
}

void PrintStatusPanel::set_temperatures(int nozzle_cur, int nozzle_tgt, int bed_cur, int bed_tgt) {
    nozzle_current_ = nozzle_cur;
    nozzle_target_ = nozzle_tgt;
    bed_current_ = bed_cur;
    bed_target_ = bed_tgt;
    update_all_displays();
}

void PrintStatusPanel::set_speeds(int speed_pct, int flow_pct) {
    speed_percent_ = speed_pct;
    flow_percent_ = flow_pct;
    update_all_displays();
}

void PrintStatusPanel::set_state(PrintState state) {
    current_state_ = state;
    update_all_displays();
    spdlog::debug("[{}] State changed to: {}", get_name(), static_cast<int>(state));
}

// ============================================================================
// PRE-PRINT PREPARATION STATE
// ============================================================================

void PrintStatusPanel::set_preparing(const std::string& operation_name, int current_step,
                                      int total_steps) {
    current_state_ = PrintState::Preparing;

    // Update operation name with step info: "Homing (1/3)"
    snprintf(preparing_operation_buf_, sizeof(preparing_operation_buf_), "%s (%d/%d)",
             operation_name.c_str(), current_step, total_steps);
    lv_subject_set_pointer(&preparing_operation_subject_, preparing_operation_buf_);

    // Calculate overall progress based on step position
    // Each step contributes equally to 100%
    int progress = (current_step > 0 && total_steps > 0)
                       ? ((current_step - 1) * 100) / total_steps
                       : 0;
    lv_subject_set_int(&preparing_progress_subject_, progress);

    // Make preparing UI visible
    lv_subject_set_int(&preparing_visible_subject_, 1);

    spdlog::info("[{}] Preparing: {} (step {}/{})", get_name(), operation_name, current_step,
                 total_steps);
}

void PrintStatusPanel::set_preparing_progress(float progress) {
    // Clamp to valid range
    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;

    int pct = static_cast<int>(progress * 100.0f);
    lv_subject_set_int(&preparing_progress_subject_, pct);

    spdlog::trace("[{}] Preparing progress: {}%", get_name(), pct);
}

void PrintStatusPanel::end_preparing(bool success) {
    // Hide preparing UI
    lv_subject_set_int(&preparing_visible_subject_, 0);
    lv_subject_set_int(&preparing_progress_subject_, 0);

    if (success) {
        // Transition to Printing state
        set_state(PrintState::Printing);
        spdlog::info("[{}] Preparation complete, starting print", get_name());
    } else {
        // Transition back to Idle
        set_state(PrintState::Idle);
        spdlog::warn("[{}] Preparation cancelled or failed", get_name());
    }
}

// ============================================================================
// EXCLUDE OBJECT FEATURE
// ============================================================================

constexpr uint32_t EXCLUDE_UNDO_WINDOW_MS = 5000; // 5 second undo window

void PrintStatusPanel::on_object_long_pressed(lv_obj_t* viewer, const char* object_name,
                                               void* user_data) {
    (void)viewer;
    auto* self = static_cast<PrintStatusPanel*>(user_data);
    if (self && object_name && object_name[0] != '\0') {
        self->handle_object_long_press(object_name);
    }
}

void PrintStatusPanel::handle_object_long_press(const char* object_name) {
    if (!object_name || object_name[0] == '\0') {
        spdlog::debug("[{}] Long-press on empty area (no object)", get_name());
        return;
    }

    // Check if already excluded
    if (excluded_objects_.count(object_name) > 0) {
        spdlog::info("[{}] Object '{}' already excluded - ignoring", get_name(), object_name);
        return;
    }

    // Check if there's already a pending exclusion
    if (!pending_exclude_object_.empty()) {
        spdlog::warn("[{}] Already have pending exclusion for '{}' - ignoring new request",
                     get_name(), pending_exclude_object_);
        return;
    }

    spdlog::info("[{}] Long-press on object: '{}' - showing confirmation", get_name(), object_name);

    // Store the object name for when confirmation happens
    pending_exclude_object_ = object_name;

    // Create confirmation dialog
    std::string title = "Exclude Object?";
    std::string message = "Stop printing \"" + std::string(object_name) +
                          "\"?\n\nThis cannot be undone after 5 seconds.";

    const char* attrs[] = {
        "title", title.c_str(),
        "message", message.c_str(),
        nullptr
    };

    lv_obj_t* screen = lv_screen_active();
    lv_xml_create(screen, "confirmation_dialog", attrs);

    // Find the created dialog (should be last child of screen)
    uint32_t child_cnt = lv_obj_get_child_count(screen);
    exclude_confirm_dialog_ = (child_cnt > 0) ? lv_obj_get_child(screen, child_cnt - 1) : nullptr;

    if (!exclude_confirm_dialog_) {
        spdlog::error("[{}] Failed to create exclude confirmation dialog", get_name());
        pending_exclude_object_.clear();
        return;
    }

    // Update button text - "Exclude" instead of default "Delete"
    lv_obj_t* confirm_btn = lv_obj_find_by_name(exclude_confirm_dialog_, "dialog_confirm_btn");
    if (confirm_btn) {
        lv_obj_t* btn_label = lv_obj_get_child(confirm_btn, 0);
        if (btn_label) {
            lv_label_set_text(btn_label, "Exclude");
        }
    }

    // Wire up button callbacks
    lv_obj_t* cancel_btn = lv_obj_find_by_name(exclude_confirm_dialog_, "dialog_cancel_btn");
    if (cancel_btn) {
        lv_obj_add_event_cb(cancel_btn, on_exclude_cancel_clicked, LV_EVENT_CLICKED, this);
    }
    if (confirm_btn) {
        lv_obj_add_event_cb(confirm_btn, on_exclude_confirm_clicked, LV_EVENT_CLICKED, this);
    }
}

void PrintStatusPanel::on_exclude_confirm_clicked(lv_event_t* e) {
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_exclude_confirmed();
    }
}

void PrintStatusPanel::on_exclude_cancel_clicked(lv_event_t* e) {
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_exclude_cancelled();
    }
}

void PrintStatusPanel::handle_exclude_confirmed() {
    spdlog::info("[{}] Exclusion confirmed for '{}'", get_name(), pending_exclude_object_);

    // Close the dialog
    if (exclude_confirm_dialog_) {
        lv_obj_delete(exclude_confirm_dialog_);
        exclude_confirm_dialog_ = nullptr;
    }

    if (pending_exclude_object_.empty()) {
        spdlog::error("[{}] No pending object for exclusion", get_name());
        return;
    }

    // Immediately update visual state in G-code viewer (red/semi-transparent)
    if (gcode_viewer_) {
        // For immediate visual feedback, we add to a "visually excluded" set
        // but don't send to Klipper yet - that happens after undo timer
        std::unordered_set<std::string> visual_excluded = excluded_objects_;
        visual_excluded.insert(pending_exclude_object_);
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, visual_excluded);
        spdlog::debug("[{}] Updated viewer with visual exclusion", get_name());
    }

    // Start undo timer - when it fires, we send EXCLUDE_OBJECT to Klipper
    if (exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
    }
    exclude_undo_timer_ = lv_timer_create(exclude_undo_timer_cb, EXCLUDE_UNDO_WINDOW_MS, this);
    lv_timer_set_repeat_count(exclude_undo_timer_, 1);

    // Show toast with "Undo" action button
    std::string toast_msg = "Excluding \"" + pending_exclude_object_ + "\"...";
    ui_toast_show_with_action(
        ToastSeverity::WARNING,
        toast_msg.c_str(),
        "Undo",
        [](void* user_data) {
            auto* self = static_cast<PrintStatusPanel*>(user_data);
            if (self) {
                self->handle_exclude_undo();
            }
        },
        this,
        EXCLUDE_UNDO_WINDOW_MS
    );

    spdlog::info("[{}] Started {}ms undo window for '{}'", get_name(), EXCLUDE_UNDO_WINDOW_MS,
                 pending_exclude_object_);
}

void PrintStatusPanel::handle_exclude_cancelled() {
    spdlog::info("[{}] Exclusion cancelled for '{}'", get_name(), pending_exclude_object_);

    // Close the dialog
    if (exclude_confirm_dialog_) {
        lv_obj_delete(exclude_confirm_dialog_);
        exclude_confirm_dialog_ = nullptr;
    }

    // Clear pending state
    pending_exclude_object_.clear();

    // Clear selection in viewer
    if (gcode_viewer_) {
        std::unordered_set<std::string> empty_set;
        ui_gcode_viewer_set_highlighted_objects(gcode_viewer_, empty_set);
    }
}

void PrintStatusPanel::handle_exclude_undo() {
    if (pending_exclude_object_.empty()) {
        spdlog::warn("[{}] Undo called but no pending exclusion", get_name());
        return;
    }

    spdlog::info("[{}] Undo pressed - cancelling exclusion of '{}'", get_name(),
                 pending_exclude_object_);

    // Cancel the timer
    if (exclude_undo_timer_) {
        lv_timer_delete(exclude_undo_timer_);
        exclude_undo_timer_ = nullptr;
    }

    // Restore visual state - remove from visual exclusion
    if (gcode_viewer_) {
        ui_gcode_viewer_set_excluded_objects(gcode_viewer_, excluded_objects_);
    }

    // Clear pending
    pending_exclude_object_.clear();

    // Show confirmation that undo succeeded
    ui_toast_show(ToastSeverity::SUCCESS, "Exclusion cancelled", 2000);
}

void PrintStatusPanel::exclude_undo_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<PrintStatusPanel*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }

    self->exclude_undo_timer_ = nullptr; // Timer auto-deletes after single shot

    if (self->pending_exclude_object_.empty()) {
        spdlog::warn("[PrintStatusPanel] Undo timer fired but no pending object");
        return;
    }

    std::string object_name = self->pending_exclude_object_;
    self->pending_exclude_object_.clear();

    spdlog::info("[PrintStatusPanel] Undo window expired - sending EXCLUDE_OBJECT for '{}'",
                 object_name);

    // Actually send the command to Klipper via MoonrakerAPI
    if (self->api_) {
        self->api_->exclude_object(
            object_name,
            [self, object_name]() {
                spdlog::info("[PrintStatusPanel] EXCLUDE_OBJECT '{}' sent successfully", object_name);
                // Move to confirmed excluded set
                self->excluded_objects_.insert(object_name);
            },
            [object_name](const MoonrakerError& err) {
                spdlog::error("[PrintStatusPanel] Failed to exclude '{}': {}", object_name,
                              err.message);
                // TODO: Show error toast and revert visual state
            });
    } else {
        spdlog::warn("[PrintStatusPanel] No API available - simulating exclusion");
        self->excluded_objects_.insert(object_name);
    }
}

