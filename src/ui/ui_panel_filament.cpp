// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_filament.h"

#include "ui_ams_edit_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_manual_pull_prompt.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_spool_canvas.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "filament_database.h"
#include "filament_op_dispatch.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "material_settings_manager.h"
#include "observer_factory.h"
#include "post_op_cooldown_manager.h"
#include "preset_materials.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "toolhead_homing.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>

using namespace helix;

// Preset slot count (0=PLA-position, 1=PETG-position, 2=ABS-position, 3=TPU-position).
// The material each slot represents is runtime-reassignable; slot identity comes
// from helix::presets (backed by MaterialSettingsManager), never a local copy.
static constexpr int PRESET_COUNT = 4;

namespace helix::filament_presets {
bool validate_reassignment(int slot, const std::string& material) {
    if (slot < 0 || slot >= 4 || material.empty()) {
        return false;
    }
    return filament::find_material(material).has_value();
}
} // namespace helix::filament_presets

using helix::ui::observe_int_async;
using helix::ui::observe_int_sync;
using helix::ui::temperature::deci_to_degrees;
using helix::ui::temperature::format_target_or_off;
using helix::ui::temperature::get_heating_state_color;

// The shared MacroParamModal and the tier-3 gcode fallbacks now live in
// filament_op_router.h so AmsOperationSidebar and FilamentRunoutHandler reach
// the same instance and the same sequences.
using helix::ui::filament_load_fallback_gcode;
using helix::ui::filament_unload_fallback_gcode;
using helix::ui::get_filament_param_modal;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

FilamentPanel::FilamentPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Initialize buffer contents with default values
    std::snprintf(temp_display_buf_, sizeof(temp_display_buf_), "%d / %d°C", nozzle_current_,
                  nozzle_target_);
    std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Select material to begin"));
    std::snprintf(warning_temps_buf_, sizeof(warning_temps_buf_),
                  lv_tr("Current: %d°C | Target: %d°C"), nozzle_current_, nozzle_target_);
    std::snprintf(safety_warning_text_buf_, sizeof(safety_warning_text_buf_),
                  lv_tr("Heat to at least %d°C for filament operations"), min_extrude_temp_);
    format_target_or_off(0, material_nozzle_buf_, sizeof(material_nozzle_buf_));
    format_target_or_off(0, material_bed_buf_, sizeof(material_bed_buf_));
    std::snprintf(nozzle_current_buf_, sizeof(nozzle_current_buf_), "%d°C", nozzle_current_);
    format_target_or_off(0, nozzle_target_buf_, sizeof(nozzle_target_buf_));
    std::snprintf(bed_current_buf_, sizeof(bed_current_buf_), "%d°C", bed_current_);
    format_target_or_off(0, bed_target_buf_, sizeof(bed_target_buf_));

    // Register XML event callbacks
    register_xml_callbacks({
        {"filament_manage_slots_cb", on_manage_slots_clicked},
        {"on_filament_load", on_load_clicked},
        {"on_filament_unload", on_unload_clicked},
        {"on_filament_extrude", on_extrude_clicked},
        {"on_filament_purge", on_purge_clicked},
        {"on_filament_retract", on_retract_clicked},
        // Material preset buttons
        {"on_filament_preset_pla", on_preset_pla_clicked},
        {"on_filament_preset_petg", on_preset_petg_clicked},
        {"on_filament_preset_abs", on_preset_abs_clicked},
        {"on_filament_preset_tpu", on_preset_tpu_clicked},
        {"on_filament_preset_spool", on_preset_spool_clicked},
        // Material preset long-press (opens material picker)
        {"on_filament_preset_pla_hold", on_preset_pla_hold},
        {"on_filament_preset_petg_hold", on_preset_petg_hold},
        {"on_filament_preset_abs_hold", on_preset_abs_hold},
        {"on_filament_preset_tpu_hold", on_preset_tpu_hold},
        // Temperature tap targets
        {"on_filament_nozzle_temp_tap", on_nozzle_temp_tap_clicked},
        {"on_filament_bed_temp_tap", on_bed_temp_tap_clicked},
        {"on_filament_nozzle_target_tap", on_nozzle_target_tap_clicked},
        {"on_filament_bed_target_tap", on_bed_target_tap_clicked},
        {"on_filament_chamber_target_tap", on_filament_chamber_target_tap},
        // Extrude length buttons
        {"on_filament_extrude_length_5mm", on_extrude_length_5mm_clicked},
        {"on_filament_extrude_length_10mm", on_extrude_length_10mm_clicked},
        {"on_filament_extrude_length_25mm", on_extrude_length_25mm_clicked},
        // Cooldown button
        {"on_filament_cooldown", on_cooldown_clicked},
        // Extruder selector dropdown
        {"on_extruder_dropdown_changed", on_extruder_dropdown_changed},
        // External spool edit
        {"on_external_spool_edit", on_external_spool_edit_clicked},
    });

    // Subscribe to PrinterState temperatures using bundle pattern
    // NOTE: Observers must defer UI updates via ui_queue_update() to avoid render-phase assertions
    // [L029]
    temp_observers_.setup_async(
        this, printer_state_,
        [](FilamentPanel* self, int raw) { self->nozzle_current_ = deci_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->nozzle_target_ = deci_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->bed_current_ = deci_to_degrees(raw); },
        [](FilamentPanel* self, int raw) { self->bed_target_ = deci_to_degrees(raw); },
        [](FilamentPanel* self) { self->update_all_temps(); });

    // Subscribe to chamber temperature (optional - only if printer has chamber)
    // Note: We check are_subjects_initialized() because observers may fire immediately
    // upon registration, but subjects aren't initialized until init_subjects() is called.
    chamber_temp_observer_ = observe_int_sync<FilamentPanel>(
        printer_state_.get_chamber_temp_subject(), this,
        [](FilamentPanel* self, int raw) {
            self->chamber_current_ = deci_to_degrees(raw);
            if (self->are_subjects_initialized()) {
                self->update_chamber_temp_display();
                self->update_status();
            }
        },
        printer_state_.get_subjects_lifetime());
    chamber_target_observer_ = observe_int_sync<FilamentPanel>(
        printer_state_.get_chamber_target_subject(), this,
        [](FilamentPanel* self, int raw) {
            self->chamber_target_ = raw; // Store decidegrees (matches PrinterState format)
            if (self->are_subjects_initialized()) {
                self->update_chamber_temp_display();
            }
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to active tool changes for dynamic nozzle label + dropdown sync.
    // Also rebind TemperatureService to the new tool's extruder — otherwise
    // the mini graph stays glued to whatever extruder was active when
    // TemperatureService::setup_panel last ran (typically T0, since the
    // temperature overlay panel is rarely created on startup). #9 — without
    // this re-bind the Snapmaker U1 user sees T0's cold-baseline plot while
    // the actively-heating T1 ramps invisibly.
    active_tool_observer_ = observe_int_sync<FilamentPanel>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](FilamentPanel* self, int tool_idx) {
            self->update_nozzle_label();
            if (self->extruder_dropdown_ && tool_idx >= 0) {
                lv_dropdown_set_selected(self->extruder_dropdown_, static_cast<uint32_t>(tool_idx));
            }
            if (self->temp_control_panel_) {
                const auto* tool = helix::ToolState::instance().active_tool();
                if (tool && tool->extruder_name) {
                    self->temp_control_panel_->switch_active_extruder(*tool->extruder_name);
                }
            }
            // Re-evaluate Load/Unload/Purge gating for the newly-selected tool.
            self->update_filament_op_buttons();
        },
        helix::ToolState::instance().get_subjects_lifetime());
    update_nozzle_label();

    // Re-evaluate Load/Unload/Purge gating whenever live AMS load state changes
    // (Task 5): the aggregate filament_loaded flag and the active-slot index.
    // Both are static AmsState subjects — no SubjectLifetime token needed.
    ams_loaded_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_filament_loaded_subject(), this,
        [](FilamentPanel* self, int) { self->update_filament_op_buttons(); },
        AmsState::instance().get_subjects_lifetime());
    ams_current_slot_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_current_slot_subject(), this,
        [](FilamentPanel* self, int) { self->update_filament_op_buttons(); },
        AmsState::instance().get_subjects_lifetime());

    // The same gating depends on print state: a runout pause arrives while the
    // panel is already open, so without this the buttons keep the pre-pause
    // enablement until some unrelated AMS signal happens to fire.
    //
    // print_lifecycle, not print_active and no longer print_state_enum. It has to
    // distinguish PRINTING -> PAUSED (a pause UNGATES the buttons on every backend
    // but AD5X, and print_active is 1 across both so it never fires there) AND see
    // Idle -> Preparing, which the raw enum does not move on at all — the gate now
    // refuses during a host-side pre-print block. The lifetime token is mandatory —
    // PrinterState is a separate singleton whose subjects tests tear down while
    // this guard is alive (#705).
    print_active_observer_ = observe_int_sync<FilamentPanel>(
        printer_state_.get_print_lifecycle_subject(), this,
        [](FilamentPanel* self, int) { self->update_filament_op_buttons(); },
        printer_state_.get_static_print_subjects_lifetime());

    // Note: Chamber temperature display is initialized by observer callbacks
    // and refresh_all_displays() on panel activation.
    // We don't call update_chamber_temp_display() here because subjects
    // aren't initialized yet (init_subjects() is called after construction).
}

FilamentPanel::~FilamentPanel() {
    // Also cancels op_revert_timer_ — see deinit_subjects(), which does it first
    // so the timer can't write the subjects it targets after they are gone.
    deinit_subjects();

    // Clean up warning dialogs if open (prevents memory leak and use-after-free)
    if (lv_is_initialized()) {
        if (load_warning_dialog_) {
            helix::ui::modal_hide(load_warning_dialog_);
            load_warning_dialog_ = nullptr;
        }
        if (unload_warning_dialog_) {
            helix::ui::modal_hide(unload_warning_dialog_);
            unload_warning_dialog_ = nullptr;
        }
    }
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void FilamentPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize subjects with default values
        UI_MANAGED_SUBJECT_STRING(temp_display_subject_, temp_display_buf_, temp_display_buf_,
                                  "filament_temp_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, status_buf_, "filament_status",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(material_selected_subject_, -1, "filament_material_selected",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(extrusion_allowed_subject_, 0, "filament_extrusion_allowed",
                               subjects_); // false (cold at start)
        UI_MANAGED_SUBJECT_INT(safety_warning_visible_subject_, 1,
                               "filament_safety_warning_visible",
                               subjects_); // true (cold at start)
        UI_MANAGED_SUBJECT_STRING(warning_temps_subject_, warning_temps_buf_, warning_temps_buf_,
                                  "filament_warning_temps", subjects_);
        UI_MANAGED_SUBJECT_STRING(safety_warning_text_subject_, safety_warning_text_buf_,
                                  safety_warning_text_buf_, "filament_safety_warning_text",
                                  subjects_);

        // Material temperature display subjects (for right side preset displays)
        UI_MANAGED_SUBJECT_STRING(material_nozzle_temp_subject_, material_nozzle_buf_,
                                  material_nozzle_buf_, "filament_material_nozzle_temp", subjects_);
        UI_MANAGED_SUBJECT_STRING(material_bed_temp_subject_, material_bed_buf_, material_bed_buf_,
                                  "filament_material_bed_temp", subjects_);

        // Nozzle label (dynamic for multi-tool)
        UI_MANAGED_SUBJECT_STRING(nozzle_label_subject_, nozzle_label_buf_, lv_tr("Nozzle"),
                                  "filament_nozzle_label", subjects_);

        // Left card temperature subjects (current and target for nozzle/bed)
        UI_MANAGED_SUBJECT_STRING(nozzle_current_subject_, nozzle_current_buf_, nozzle_current_buf_,
                                  "filament_nozzle_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(nozzle_target_subject_, nozzle_target_buf_, nozzle_target_buf_,
                                  "filament_nozzle_target", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_current_subject_, bed_current_buf_, bed_current_buf_,
                                  "filament_bed_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(bed_target_subject_, bed_target_buf_, bed_target_buf_,
                                  "filament_bed_target", subjects_);
        UI_MANAGED_SUBJECT_STRING(chamber_current_subject_, chamber_current_buf_,
                                  chamber_current_buf_, "filament_chamber_current", subjects_);
        UI_MANAGED_SUBJECT_STRING(chamber_target_subject_, chamber_target_buf_, chamber_target_buf_,
                                  "filament_chamber_target", subjects_);

        // Operation in progress subject (for disabling buttons during filament ops)
        operation_guard_.init_subject("filament_operation_in_progress", subjects_);

        // LIVE load-state gating subjects (Task 5). Default: Load enabled, Unload
        // disabled (cold start = nothing loaded). Recomputed by
        // update_filament_op_buttons() from the selected tool's live load state.
        UI_MANAGED_SUBJECT_INT(load_disabled_subject_, 0, "filament_load_disabled", subjects_);
        UI_MANAGED_SUBJECT_INT(unload_disabled_subject_, 1, "filament_unload_disabled", subjects_);

        // Cooldown button visibility (1 when nozzle or bed target > 0)
        UI_MANAGED_SUBJECT_INT(nozzle_heating_subject_, 0, "filament_nozzle_heating", subjects_);

        // Extrude length button active states (boolean: 0=inactive, 1=active)
        // Using separate subjects because bind_style doesn't work well with multiple ref_values
        UI_MANAGED_SUBJECT_INT(extrude_length_5mm_active_subject_, 0,
                               "filament_extrude_length_5mm_active", subjects_);
        UI_MANAGED_SUBJECT_INT(extrude_length_10mm_active_subject_, 1,
                               "filament_extrude_length_10mm_active", subjects_);
        UI_MANAGED_SUBJECT_INT(extrude_length_25mm_active_subject_, 0,
                               "filament_extrude_length_25mm_active", subjects_);

        // Per-op button feedback state (0=idle, 1=busy spinner, 2=done check).
        // Bound to each op button's bind_op_state in filament_panel.xml.
        UI_MANAGED_SUBJECT_INT(op_load_state_subject_, 0, "filament_op_load_state", subjects_);
        UI_MANAGED_SUBJECT_INT(op_unload_state_subject_, 0, "filament_op_unload_state", subjects_);
        UI_MANAGED_SUBJECT_INT(op_purge_state_subject_, 0, "filament_op_purge_state", subjects_);
        UI_MANAGED_SUBJECT_INT(op_extrude_state_subject_, 0, "filament_op_extrude_state",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(op_retract_state_subject_, 0, "filament_op_retract_state",
                               subjects_);

        // Card title subject (dynamic: "Multi-Filament" or "External Spool")
        std::strncpy(card_title_buf_, lv_tr("Multi-Filament"), sizeof(card_title_buf_) - 1);
        UI_MANAGED_SUBJECT_STRING(card_title_subject_, card_title_buf_, card_title_buf_,
                                  "filament_card_title", subjects_);

        spdlog::debug("[{}] temp={}/{}°C, material={}", get_name(), nozzle_current_, nozzle_target_,
                      selected_material_);
    });
}

void FilamentPanel::deinit_subjects() {
    // Cancel the op-state timer before the subjects it writes go away. The
    // operation guard owns a timer of its own whose handler writes the same
    // subjects, so it has to stop here too — deinit_subjects() runs from
    // StaticPanelRegistry::destroy_all(), well before lv_deinit().
    cancel_op_revert_timer();
    operation_guard_.end();
    backend_op_active_ = false;
    op_in_flight_.reset();
    op_showing_busy_.reset();

    // Cancel any pending preheat without notification (panel is being torn down)
    if (pending_preheat_op_ != PreheatOp::NONE) {
        pending_preheat_op_ = PreheatOp::NONE;
        pending_preheat_target_ = 0;
        // Same leak this abandonment path guards against in cancel_pending_preheat().
        if (AmsBackend* backend = AmsState::instance().get_backend()) {
            backend->clear_home_preconfirmed();
        }
        // Don't schedule delayed cooldown during teardown — just cool down immediately
        if (prior_nozzle_target_ == 0) {
            if (auto* c = get_temperature_controller()) {
                c->set_target(helix::HeaterType::Nozzle, 0.0, {.toast = false});
            }
        }
        prior_nozzle_target_ = 0;
    }
    external_spool_observer_.reset();
    ams_loaded_observer_.reset();
    ams_current_slot_observer_.reset();
    // Watches PrinterState's print_state_enum, a subject tests deinit between
    // cases — must be released alongside the AmsState guards above.
    print_active_observer_.reset();
    temp_observers_.clear();
    deinit_subjects_base(subjects_);
}

void FilamentPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Filament macros now resolved via StandardMacros singleton (auto-detected or user-configured)
    spdlog::debug("[{}] Setting up (events handled declaratively via XML)", get_name());

    // Find preset buttons (for visual state updates)
    const char* preset_names[] = {"preset_pla", "preset_petg", "preset_abs", "preset_tpu"};
    for (int i = 0; i < 4; i++) {
        preset_buttons_[i] = lv_obj_find_by_name(panel_, preset_names[i]);
    }

    // Action buttons (btn_load, btn_unload, btn_purge) - disabled state managed by XML bindings

    // Find safety warning card
    safety_warning_ = lv_obj_find_by_name(panel_, "safety_warning");

    // Find status icon for dynamic updates
    status_icon_ = lv_obj_find_by_name(panel_, "status_icon");

    // Find temperature labels for color updates
    nozzle_current_label_ = lv_obj_find_by_name(panel_, "nozzle_current_temp");
    bed_current_label_ = lv_obj_find_by_name(panel_, "bed_current_temp");
    chamber_current_label_ = lv_obj_find_by_name(panel_, "chamber_current_temp");

    // Find temp graph for dynamic sizing when bottom card changes
    temp_graph_card_ = lv_obj_find_by_name(panel_, "temp_graph_card");

    // Find spool card widgets (serves both Multi-Filament and External Spool modes)
    spool_card_ = lv_obj_find_by_name(panel_, "spool_card");
    spool_card_header_row_ = lv_obj_find_by_name(panel_, "spool_card_header_row");
    extruder_selector_group_ = lv_obj_find_by_name(panel_, "extruder_selector_group");
    extruder_dropdown_ = lv_obj_find_by_name(panel_, "extruder_dropdown");
    btn_manage_slots_ = lv_obj_find_by_name(panel_, "btn_manage_slots");
    ams_manage_row_ = lv_obj_find_by_name(panel_, "ams_manage_row");

    // Find external spool row widgets
    external_spool_row_ = lv_obj_find_by_name(panel_, "external_spool_row");
    external_spool_container_ = lv_obj_find_by_name(panel_, "external_spool_container");
    external_spool_material_label_ = lv_obj_find_by_name(panel_, "external_spool_material_label");
    external_spool_color_label_ = lv_obj_find_by_name(panel_, "external_spool_color_label");

    // Find spool preset widgets
    spool_preset_row_ = lv_obj_find_by_name(panel_, "spool_preset_row");
    spool_preset_button_ = lv_obj_find_by_name(panel_, "preset_spool");
    spool_preset_label_ = lv_obj_find_by_name(panel_, "spool_preset_label");
    spool_preset_temps_ = lv_obj_find_by_name(panel_, "spool_preset_temps");

    // Setup external spool display (creates canvas, wires observer)
    setup_external_spool_display();

    // Setup spool preset button (show if active material doesn't match standard presets)
    update_spool_preset();

    // Populate extruder dropdown and set card visibility
    populate_extruder_dropdown();
    update_multi_filament_card_visibility();

    // Seed Load/Unload/Purge gating from current live load state (Task 5).
    update_filament_op_buttons();

    // Rebuild dropdown if tool list changes
    tools_version_observer_ = observe_int_sync<FilamentPanel>(
        helix::ToolState::instance().get_tools_version_subject(), this,
        [](FilamentPanel* self, int) {
            self->populate_extruder_dropdown();
            self->update_multi_filament_card_visibility();
        },
        helix::ToolState::instance().get_subjects_lifetime());

    // Subscribe to AMS type to update card row visibility. Graph vs spool
    // sizing is owned by apply_left_column_sizing() (called from
    // update_multi_filament_card_visibility). At MICRO/TINY with no AMS the
    // spool card has almost no content, so the graph becomes the flex filler.
    ams_type_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_ams_type_subject(), this,
        [](FilamentPanel* self, int /*ams_type*/) {
            self->update_multi_filament_card_visibility();
        },
        AmsState::instance().get_subjects_lifetime());

    // End the operation guard when the AMS action reaches a terminal state. IDLE
    // means the backend finished; ERROR means it gave up — AFC's stuck-action
    // backstop resolves to ERROR and nothing else, so accepting only IDLE left the
    // guard armed and the button spinning until the 120s timeout (#1183).
    ams_action_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_ams_action_subject(), this,
        [](FilamentPanel* self, int action) {
            const bool idle = (action == static_cast<int>(AmsAction::IDLE));
            const bool failed = (action == static_cast<int>(AmsAction::ERROR));
            // AmsSystemInfo::is_busy() is exactly "action is neither IDLE nor
            // ERROR", so every edge of this subject changes the button gating —
            // including ops this panel did not start. Re-gate before the guard
            // bookkeeping below, which only cares about the terminal edges.
            self->update_filament_op_buttons();
            if ((!idle && !failed) || !self->operation_guard_.is_active()) {
                return;
            }
            spdlog::debug("[{}] AMS action reached {}, ending operation guard", self->get_name(),
                          idle ? "IDLE" : "ERROR");
            self->operation_guard_.end();
            // Complete on-button feedback for fire-and-forget AMS-backend ops.
            // Gated on backend_op_active_ so gcode/macro ops (which drive
            // op_succeeded from their own execute_gcode callback) are never
            // double-completed here.
            if (self->backend_op_active_ && self->op_in_flight_) {
                FilamentOp op = *self->op_in_flight_;
                self->backend_op_active_ = false;
                self->op_in_flight_.reset();
                if (failed) {
                    // Spinner off, no checkmark, no toast. The ERROR edge already
                    // owns a surface, but which one depends on the backend:
                    // AmsErrorBridge only presents when current_error() returns an
                    // event, which AD5X IFS and QIDI override and AFC does not.
                    // AFC's own error dialog is AmsPanel::show_loading_error_modal()
                    // (Retry/Close) off the same subject, plus the Resume/Unload/
                    // Recover modal GcodeErrorRouter builds when it classifies the
                    // `!!` line. A toast here would stack on whichever fired.
                    //
                    // The heater is deliberately left alone — ERROR can mean "we
                    // stopped waiting", not "the backend stopped", and cutting heat
                    // under a live AFC move would jam the toolhead (recovery from
                    // the dialog needs it hot anyway). This matches every other
                    // op_failed() path, none of which restore the heater.
                    self->op_failed(op);
                    return;
                }
                self->op_succeeded(op);
                // Backend (AFC/etc.) ops complete HERE, not via the gcode/macro
                // success callbacks that call restore_heater_after_preheat().
                // Without this the post-op cooldown is never scheduled and the
                // nozzle holds the material temp indefinitely after a swap.
                self->restore_heater_after_preheat();
            }
        },
        AmsState::instance().get_subjects_lifetime());

    // Load persisted preset-material assignments (default PLA/PETG/ABS/TPU if unset)
    helix::MaterialSettingsManager::instance().init(); // idempotent

    // Populate preset button temperature + name labels from filament database
    helix::presets::refresh_subjects();

    // Initialize visual state
    update_preset_buttons_visual();
    update_temp_display();
    update_left_card_temps();
    update_material_temp_display();
    update_status();
    update_status_icon_for_state();
    update_warning_text();
    update_safety_state();

    // Trigger initial extrude length selection (notifies bind_style observers)
    handle_extrude_length_select(extrude_length_);

    // Setup combined temperature graph if TemperatureService is available
    if (temp_control_panel_) {
        lv_obj_t* graph_container = lv_obj_find_by_name(panel_, "temp_graph_container");
        if (graph_container) {
            temp_control_panel_->setup_mini_combined_graph(graph_container);
            spdlog::debug("[{}] Temperature graph initialized", get_name());
        } else {
            spdlog::warn("[{}] temp_graph_container not found in XML", get_name());
        }
    }

    // Make the graph card clickable to open the unified temp graph overlay
    if (temp_graph_card_) {
        lv_obj_add_flag(temp_graph_card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            temp_graph_card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
                if (self) {
                    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly,
                                                         self->parent_screen_);
                }
            },
            LV_EVENT_CLICKED, this);
    }

    // AMS mini status widget is now created declaratively via XML <ams_mini_status/>

    spdlog::debug("[{}] Setup complete!", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void FilamentPanel::update_temp_display() {
    std::snprintf(temp_display_buf_, sizeof(temp_display_buf_), "%d / %d°C", nozzle_current_,
                  nozzle_target_);
    lv_subject_copy_string(&temp_display_subject_, temp_display_buf_);
}

void FilamentPanel::update_status_icon(const char* icon_name, const char* variant) {
    if (!status_icon_)
        return;

    // Update icon imperatively using ui_icon API
    ui_icon_set_source(status_icon_, icon_name);
    ui_icon_set_variant(status_icon_, variant);
}

void FilamentPanel::update_status() {
    const char* status_msg;

    // First check if nozzle is ready for extrusion (highest priority for filament operations)
    if (helix::ui::temperature::is_extrusion_safe(nozzle_current_, min_extrude_temp_)) {
        // Hot enough for any extruder move — load, unload and purge all sit on
        // this panel, so the wording names the state rather than one of them.
        status_msg = lv_tr("Ready for filament operations");
        update_status_icon("check", "success");
    } else if (nozzle_target_ >= min_extrude_temp_) {
        // Nozzle heating in progress — show current AND target so the user can
        // see ramp progress (#8: previously only target was visible, leaving
        // the user staring at a static "Heating to 230°C..." for several
        // minutes with no sign of life from the firmware).
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Heating: %d / %d°C"),
                      nozzle_current_, nozzle_target_);
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("flash", "warning");
        return; // Already updated, exit early
    } else if (chamber_target_ > 0 && chamber_current_ < deci_to_degrees(chamber_target_) - 5) {
        // Chamber is heating (show only if nozzle is cold)
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Chamber heating to %d°C..."),
                      deci_to_degrees(chamber_target_));
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("fire", "warning");
        return;
    } else if (chamber_target_ > 0 && chamber_current_ >= deci_to_degrees(chamber_target_) - 5 &&
               chamber_current_ <= deci_to_degrees(chamber_target_) + 2) {
        // Chamber at target (show only if nozzle is cold)
        std::snprintf(status_buf_, sizeof(status_buf_), lv_tr("Chamber at %d°C"),
                      deci_to_degrees(chamber_target_));
        lv_subject_copy_string(&status_subject_, status_buf_);
        update_status_icon("check", "success");
        return;
    } else {
        // Cold - needs material selection
        status_msg = lv_tr("Select material to begin");
        update_status_icon("cooldown", "secondary");
    }

    lv_subject_copy_string(&status_subject_, status_msg);
}

void FilamentPanel::update_warning_text() {
    std::snprintf(warning_temps_buf_, sizeof(warning_temps_buf_),
                  lv_tr("Current: %d°C | Target: %d°C"), nozzle_current_, nozzle_target_);
    lv_subject_copy_string(&warning_temps_subject_, warning_temps_buf_);
}

void FilamentPanel::update_safety_state() {
    // Route through is_extrusion_allowed() so the cold-extrude override (#978)
    // both enables the buttons and hides the "heat first" warning.
    bool allowed = is_extrusion_allowed();

    // Hide the safety warning (and enable buttons) if we have a known spool material,
    // since the load/unload handlers will auto-preheat to the correct temperature.
    bool has_known_spool = has_active_spool_material();
    bool show_warning = !allowed && !has_known_spool;

    lv_subject_set_int(&extrusion_allowed_subject_, allowed ? 1 : 0);
    lv_subject_set_int(&safety_warning_visible_subject_, show_warning ? 1 : 0);

    spdlog::trace("[{}] Safety state updated: allowed={}, known_spool={} (temp={}°C)", get_name(),
                  allowed, has_known_spool, nozzle_current_);
}

void FilamentPanel::update_preset_buttons_visual() {
    for (int i = 0; i < 4; i++) {
        if (!preset_buttons_[i])
            continue;

        if (i == selected_material_) {
            lv_obj_add_state(preset_buttons_[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(preset_buttons_[i], LV_STATE_CHECKED);
        }
    }
    // Deselect spool preset when a standard preset is selected
    if (spool_preset_button_ && selected_material_ >= 0) {
        lv_obj_remove_state(spool_preset_button_, LV_STATE_CHECKED);
    }
}

void FilamentPanel::check_and_auto_select_preset() {
    // Check if both nozzle and bed targets match any preset. A branded slot is
    // matched against its exact product temps (not the generic type's) — otherwise
    // a branded preset's CHECKED highlight would get cleared the moment the live
    // temperature observer round-trips the target, since the branded target
    // generally won't equal the generic material's recommended temps.
    auto& mgr = helix::MaterialSettingsManager::instance();
    int matching_preset = -1;
    for (int i = 0; i < PRESET_COUNT; i++) {
        auto branded = mgr.get_preset_filament(i);
        if (branded && branded->is_branded()) {
            if (nozzle_target_ == branded->nozzle && bed_target_ == branded->bed) {
                matching_preset = i;
                break;
            }
            continue;
        }
        auto mat = filament::find_material(helix::presets::name(i));
        if (mat && nozzle_target_ == mat->nozzle_recommended() && bed_target_ == mat->bed_temp) {
            matching_preset = i;
            break;
        }
    }

    // Only update if selection changed
    if (matching_preset != selected_material_) {
        selected_material_ = matching_preset;
        lv_subject_set_int(&material_selected_subject_, selected_material_);
        update_preset_buttons_visual();

        if (matching_preset >= 0) {
            spdlog::debug("[{}] Auto-selected preset: {} (nozzle={}°C, bed={}°C)", get_name(),
                          helix::presets::name(matching_preset), nozzle_target_, bed_target_);
        } else {
            spdlog::debug("[{}] No matching preset for nozzle={}°C, bed={}°C", get_name(),
                          nozzle_target_, bed_target_);
        }
    }
}

void FilamentPanel::update_nozzle_label() {
    auto label = helix::ToolState::instance().nozzle_label();
    std::snprintf(nozzle_label_buf_, sizeof(nozzle_label_buf_), "%s", label.c_str());
    if (subjects_initialized_) {
        lv_subject_copy_string(&nozzle_label_subject_, nozzle_label_buf_);
    }
}

void FilamentPanel::update_all_temps() {
    // Unified update handler for temperature observer bundle.
    // Called on UI thread after any temperature value changes.
    if (!panel_)
        return;

    // Always update current-temp-dependent displays
    update_left_card_temps();
    update_temp_display();
    update_warning_text();
    update_safety_state();
    update_status();

    // Only update target-dependent displays when targets actually changed.
    // Current temps change frequently during heating (~1Hz × 4 subjects),
    // but preset matching and material display only depend on targets.
    bool targets_changed =
        (nozzle_target_ != prev_nozzle_target_ || bed_target_ != prev_bed_target_);
    if (targets_changed) {
        prev_nozzle_target_ = nozzle_target_;
        prev_bed_target_ = bed_target_;
        update_material_temp_display();
        check_and_auto_select_preset();
        lv_subject_set_int(&nozzle_heating_subject_,
                           (nozzle_target_ > 0 || bed_target_ > 0) ? 1 : 0);

        // Cancel pending cooldown if user manually changed heater target
        if (nozzle_target_ != 0) {
            PostOpCooldownManager::instance().cancel();
        }
    }

    // Check if pending preheat target has been reached
    check_pending_preheat();
}

// ============================================================================
// INSTANCE HANDLERS
// ============================================================================

void FilamentPanel::handle_preset_button(int material_id) {
    // Delegate state update and display refresh to the public API
    set_material(material_id);

    // A branded product attached to this slot (via the catalog picker) overrides the
    // generic material-DB temps set_material() just applied — heat to the exact
    // product's temps instead of the generic material's. PresetFilament::nozzle/bed
    // are whole-°C ints, same unit set_material() already uses.
    if (selected_material_ == material_id) {
        auto branded = helix::MaterialSettingsManager::instance().get_preset_filament(material_id);
        if (branded && branded->is_branded()) {
            nozzle_target_ = branded->nozzle;
            bed_target_ = branded->bed;
            update_temp_display();
            update_material_temp_display();
            update_status();
        }
    }

    // Send temperature commands to printer (nozzle, bed, and chamber if applicable)
    if (selected_material_ == material_id) {
        if (auto* c = get_temperature_controller()) {
            // Switching material: hold the previous filament's temp if hotter so
            // the old material still purges cleanly (keep_previous_hot).
            c->set_target(helix::HeaterType::Nozzle, static_cast<double>(nozzle_target_),
                          {.toast = true,
                           .keep_previous_hot = true,
                           .on_success = [target = nozzle_target_]() {
                               NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), target);
                           }});
            c->set_target(helix::HeaterType::Bed, static_cast<double>(bed_target_),
                          {.toast = true, .on_success = [target = bed_target_]() {
                               NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), target);
                           }});

            // Set chamber temperature if preset specifies one
            if (chamber_target_ > 0) {
                int target = deci_to_degrees(chamber_target_);
                c->set_target(helix::HeaterType::Chamber, static_cast<double>(target),
                              {.toast = true, .on_success = [target]() {
                                   NOTIFY_SUCCESS(lv_tr("Chamber target set to {}°C"), target);
                               }});
            }
        }
    }
}

void FilamentPanel::reassign_preset(int slot, const std::string& material) {
    if (!helix::filament_presets::validate_reassignment(slot, material)) {
        spdlog::warn("[{}] reassign_preset rejected: slot={}, material='{}'", get_name(), slot,
                     material);
        return;
    }
    helix::MaterialSettingsManager::instance().set_preset_material(slot, material);
    helix::presets::refresh_subjects();
    if (auto* tc = get_temperature_controller()) {
        tc->refresh_presets();
    }
    check_and_auto_select_preset(); // refresh CHECKED highlight vs current targets
    update_spool_preset();          // refresh 5th (dynamic spool) button visibility
    spdlog::info("[{}] Preset slot {} reassigned to {}", get_name(), slot, material);
}

void FilamentPanel::reset_presets_to_defaults() {
    helix::MaterialSettingsManager::instance().reset_preset_materials();
    helix::presets::refresh_subjects();
    if (auto* tc = get_temperature_controller()) {
        tc->refresh_presets();
    }
    check_and_auto_select_preset();
    update_spool_preset(); // refresh 5th (dynamic spool) button visibility
    spdlog::info("[{}] Presets reset to defaults", get_name());
}

void FilamentPanel::handle_preset_longpress(int slot) {
    if (slot < 0 || slot >= PRESET_COUNT || !preset_buttons_[slot]) {
        return;
    }
    lv_obj_t* screen = lv_obj_get_screen(preset_buttons_[slot]);
    // Reset-to-defaults affordance is preset-editing-only — gated purely on whether
    // this callback is set before show() (matches the retired MaterialPickerMenu's
    // reset_callback_ gate). The AMS slot-assignment picker never calls this, so its
    // instance keeps the row hidden.
    catalog_picker_.set_reset_callback(
        []() { get_global_filament_panel().reset_presets_to_defaults(); });
    // Fires synchronously on the main thread from the modal's Select button click —
    // same threading context the old material_picker_ callback ran in — so call
    // straight through get_global_filament_panel() (singleton, lives for process
    // lifetime) with no defer/lifetime-token needed, matching that prior pattern.
    catalog_picker_.show(screen, std::optional<std::string>(helix::presets::name(slot)),
                         [slot](const helix::printer::EffectiveFilament& ef) {
                             get_global_filament_panel().apply_preset_pick(slot, ef);
                         });

    // The long-press that opened the picker is still an active press. Without this, the
    // eventual release lands on whatever widget now sits under the finger — the Type
    // dropdown, which appears roughly where the preset button was — and auto-opens it.
    // Make the input device swallow events until the physical release.
    if (lv_indev_t* indev = lv_indev_active()) {
        lv_indev_wait_release(indev);
    }
}

void FilamentPanel::apply_preset_pick(int slot, const helix::printer::EffectiveFilament& ef) {
    // reassign_preset() persists the plain type via set_preset_material(), which
    // ALSO clears any stale branding on the slot (MaterialSettingsManager treats a
    // plain type-swap as reverting to generic). So the branded attach below MUST
    // come after reassign_preset(), not before, or set_preset_material() would wipe
    // out the branding we're trying to set.
    reassign_preset(slot, ef.type);
    helix::MaterialSettingsManager::instance().set_preset_filament(slot, ef);
    // reassign_preset() already refreshed labels/temps/highlight for the generic
    // type; refresh again now that the branded product is attached so the button
    // shows the branded name/temps instead.
    helix::presets::refresh_subjects();
    check_and_auto_select_preset();
    spdlog::info("[{}] Preset slot {} attached to branded filament '{}' ({}/{}°C)", get_name(),
                 slot, ef.id, ef.nozzle_recommended, ef.bed_temp);
}

void FilamentPanel::handle_nozzle_temp_tap() {
    spdlog::debug("[{}] Opening custom nozzle temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value =
                                     static_cast<float>(nozzle_target_ > 0 ? nozzle_target_ : 200),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(nozzle_max_temp_),
                                 .title_label = lv_tr("Nozzle Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback = custom_nozzle_keypad_cb,
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_bed_temp_tap() {
    spdlog::debug("[{}] Opening custom bed temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value =
                                     static_cast<float>(bed_target_ > 0 ? bed_target_ : 60),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(bed_max_temp_),
                                 .title_label = lv_tr("Bed Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback = custom_bed_keypad_cb,
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_chamber_temp_tap() {
    spdlog::debug("[{}] Opening custom chamber temperature keypad", get_name());

    ui_keypad_config_t config = {.initial_value = static_cast<float>(
                                     chamber_target_ > 0 ? deci_to_degrees(chamber_target_) : 50),
                                 .min_value = 0.0f,
                                 .max_value = static_cast<float>(chamber_max_temp_),
                                 .title_label = lv_tr("Chamber Temperature"),
                                 .unit_label = "°C",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback =
                                     [](float value, void* user_data) {
                                         auto* self = static_cast<FilamentPanel*>(user_data);
                                         if (self) {
                                             self->handle_custom_chamber_confirmed(value);
                                         }
                                     },
                                 .user_data = this};

    ui_keypad_show(&config);
}

void FilamentPanel::handle_custom_chamber_confirmed(float value) {
    spdlog::info("[{}] Custom chamber temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    chamber_target_ = helix::units::to_decidegrees(value);
    update_chamber_temp_display();

    int target = static_cast<int>(value);
    if (auto* c = get_temperature_controller()) {
        c->set_target(helix::HeaterType::Chamber, static_cast<double>(target),
                      {.toast = true, .on_success = [target]() {
                           NOTIFY_SUCCESS(lv_tr("Chamber target set to {}°C"), target);
                       }});
    }
}

void FilamentPanel::handle_custom_nozzle_confirmed(float value) {
    spdlog::info("[{}] Custom nozzle temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    nozzle_target_ = static_cast<int>(value);
    // Deselect any preset since user set custom temp
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_temp_display();
    update_material_temp_display();
    update_status();

    // Send temperature command to printer
    if (auto* c = get_temperature_controller()) {
        c->set_target(helix::HeaterType::Nozzle, static_cast<double>(nozzle_target_),
                      {.toast = true, .on_success = [target = nozzle_target_]() {
                           NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), target);
                       }});
    }
}

void FilamentPanel::handle_custom_bed_confirmed(float value) {
    spdlog::info("[{}] Custom bed temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    bed_target_ = static_cast<int>(value);
    // Deselect any preset since user set custom temp
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_material_temp_display();

    // Send temperature command to printer
    if (auto* c = get_temperature_controller()) {
        c->set_target(helix::HeaterType::Bed, static_cast<double>(bed_target_),
                      {.toast = true, .on_success = [target = bed_target_]() {
                           NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), target);
                       }});
    }
}

void FilamentPanel::update_material_temp_display() {
    // Use centralized formatting with em dash for heater-off state
    format_target_or_off(nozzle_target_, material_nozzle_buf_, sizeof(material_nozzle_buf_));
    format_target_or_off(bed_target_, material_bed_buf_, sizeof(material_bed_buf_));
    lv_subject_copy_string(&material_nozzle_temp_subject_, material_nozzle_buf_);
    lv_subject_copy_string(&material_bed_temp_subject_, material_bed_buf_);
}

void FilamentPanel::update_chamber_temp_display() {
    // chamber_current_ is already in degrees (observer converts), chamber_target_ is decidegrees
    std::snprintf(chamber_current_buf_, sizeof(chamber_current_buf_), "%d°C", chamber_current_);
    format_target_or_off(deci_to_degrees(chamber_target_), chamber_target_buf_,
                         sizeof(chamber_target_buf_));
    lv_subject_copy_string(&chamber_current_subject_, chamber_current_buf_);
    lv_subject_copy_string(&chamber_target_subject_, chamber_target_buf_);

    // Apply 4-state heating color (matches nozzle/bed)
    if (chamber_current_label_) {
        int target_deg = deci_to_degrees(chamber_target_);
        lv_color_t color = get_heating_state_color(chamber_current_, target_deg);
        lv_obj_set_style_text_color(chamber_current_label_, color, LV_PART_MAIN);
    }
}

void FilamentPanel::update_left_card_temps() {
    // Update current temps
    std::snprintf(nozzle_current_buf_, sizeof(nozzle_current_buf_), "%d°C", nozzle_current_);
    std::snprintf(bed_current_buf_, sizeof(bed_current_buf_), "%d°C", bed_current_);
    lv_subject_copy_string(&nozzle_current_subject_, nozzle_current_buf_);
    lv_subject_copy_string(&bed_current_subject_, bed_current_buf_);

    // Update target temps using centralized formatting with em dash for heater-off state
    format_target_or_off(nozzle_target_, nozzle_target_buf_, sizeof(nozzle_target_buf_));
    format_target_or_off(bed_target_, bed_target_buf_, sizeof(bed_target_buf_));
    lv_subject_copy_string(&nozzle_target_subject_, nozzle_target_buf_);
    lv_subject_copy_string(&bed_target_subject_, bed_target_buf_);

    // Update temperature label colors using 4-state heating logic
    // (matches temp_display widget: gray=off, red=heating, green=at-temp, blue=cooling)
    if (nozzle_current_label_) {
        lv_color_t nozzle_color = get_heating_state_color(nozzle_current_, nozzle_target_);
        lv_obj_set_style_text_color(nozzle_current_label_, nozzle_color, LV_PART_MAIN);
    }
    if (bed_current_label_) {
        lv_color_t bed_color = get_heating_state_color(bed_current_, bed_target_);
        lv_obj_set_style_text_color(bed_current_label_, bed_color, LV_PART_MAIN);
    }
}

void FilamentPanel::update_status_icon_for_state() {
    // Determine icon and color based on current state
    if (nozzle_target_ == 0 && bed_target_ == 0) {
        // Idle - no target set
        update_status_icon("info", "secondary");
    } else if (nozzle_current_ < nozzle_target_ - 5 || bed_current_ < bed_target_ - 5) {
        // Heating
        update_status_icon("fire", "warning");
    } else if (nozzle_current_ > nozzle_target_ + 5 && nozzle_target_ > 0) {
        // Cooling down
        update_status_icon("cooldown", "info");
    } else {
        // At temperature
        update_status_icon("check", "success");
    }
}

// set_operation_in_progress removed — replaced by OperationTimeoutGuard

void FilamentPanel::handle_extrude_length_select(int amount) {
    extrude_length_ = amount;
    // Update boolean subjects for each button (only one active at a time)
    lv_subject_set_int(&extrude_length_5mm_active_subject_, amount == 5 ? 1 : 0);
    lv_subject_set_int(&extrude_length_10mm_active_subject_, amount == 10 ? 1 : 0);
    lv_subject_set_int(&extrude_length_25mm_active_subject_, amount == 25 ? 1 : 0);
    spdlog::debug("[{}] Extrude length set to {}mm", get_name(), amount);
}

// ============================================================================
// ON-BUTTON OPERATION FEEDBACK
//
// Each op button shows its own progress (animated spinner → checkmark) via an
// int subject bound to ui_button bind_op_state, replacing the old stacked
// start/complete toasts. Errors/timeouts keep their toast. All setters below run
// on the main thread (op_started from the click handler; op_succeeded/op_failed
// inside the helix::ui::async_call bodies already used to marshal off the
// WebSocket background thread). FilamentPanel is a global singleton, so `this` is
// always valid — no AsyncLifetimeGuard needed here [L012].
// ============================================================================

constexpr uint32_t OP_DONE_REVERT_MS = 1500;     ///< how long the "done" checkmark shows
constexpr uint32_t MIN_SPINNER_VISIBLE_MS = 500; ///< floor so instant ops still flash a spinner

lv_subject_t* FilamentPanel::op_state_subject(FilamentOp op) {
    switch (op) {
    case FilamentOp::Load:
        return &op_load_state_subject_;
    case FilamentOp::Unload:
        return &op_unload_state_subject_;
    case FilamentOp::Purge:
        return &op_purge_state_subject_;
    case FilamentOp::Extrude:
        return &op_extrude_state_subject_;
    case FilamentOp::Retract:
        return &op_retract_state_subject_;
    }
    return &op_load_state_subject_;
}

void FilamentPanel::set_op_state(FilamentOp op, int state) {
    lv_subject_set_int(op_state_subject(op), state);
}

void FilamentPanel::cancel_op_revert_timer() {
    // Single handle covers both phases (min-spinner delay AND done→idle revert),
    // so this cancels whichever is pending.
    if (op_revert_timer_) {
        lv_timer_delete(op_revert_timer_);
        op_revert_timer_ = nullptr;
    }
}

// Schedule the single shared op timer. `this` is a singleton so the raw user_data
// capture is safe; the timer is cancelled on a new op / teardown via
// cancel_op_revert_timer(). repeat_count==1 → LVGL auto-deletes after firing, so
// the callbacks must NOT delete it (double-free); they just null the handle.
void FilamentPanel::schedule_op_timer(uint32_t delay_ms, lv_timer_cb_t cb) {
    cancel_op_revert_timer();
    op_revert_timer_ = lv_timer_create(cb, delay_ms, this);
    lv_timer_set_repeat_count(op_revert_timer_, 1);
}

// Enter the "done" checkmark state and arm the timer that reverts it to idle.
// Shared by both op_succeeded branches (immediate vs after the min-spinner floor).
void FilamentPanel::enter_op_done_state(FilamentOp op) {
    set_op_state(op, 2); // done → checkmark
    op_revert_target_ = op;
    schedule_op_timer(OP_DONE_REVERT_MS, [](lv_timer_t* t) {
        auto* self = static_cast<FilamentPanel*>(lv_timer_get_user_data(t));
        self->op_revert_timer_ = nullptr;
        self->set_op_state(self->op_revert_target_, 0);
    });
}

void FilamentPanel::op_started(FilamentOp op) {
    // A new op cancels any pending timer (min-delay or revert) and resets all op
    // buttons to idle so only the active one shows the spinner.
    cancel_op_revert_timer();
    for (FilamentOp o : {FilamentOp::Load, FilamentOp::Unload, FilamentOp::Purge,
                         FilamentOp::Extrude, FilamentOp::Retract}) {
        if (o != op) {
            lv_subject_set_int(op_state_subject(o), 0);
        }
    }
    op_aborted_.reset(); // a fresh op is never pre-aborted
    op_busy_started_tick_ = lv_tick_get();
    op_showing_busy_ = op;
    set_op_state(op, 1); // busy → spinner
}

void FilamentPanel::op_succeeded(FilamentOp op) {
    // This op was already torn down out-of-band (see fail_op_on_unknown_command)
    // and the RPC's `ok` is only now catching up. Swallow it once, or the
    // checkmark lands on top of the error toast.
    if (op_aborted_ && *op_aborted_ == op) {
        op_aborted_.reset();
        if (op == FilamentOp::Unload) {
            helix::ui::disarm_manual_pull_prompt();
        }
        return;
    }
    if (op == FilamentOp::Unload) {
        // No-op unless execute_unload armed it, and unless the toolhead sensor
        // stayed silent — a printer with one has already prompted, at the earlier
        // and truer moment the filament actually cleared the gears.
        helix::ui::manual_pull_unload_finished();
    }
    op_showing_busy_.reset();
    // Instant-completing ops (mock gcode fires success synchronously; fast real
    // ops too) would flip spinner→check within a frame. Hold the spinner for a
    // minimum visible duration before showing the checkmark.
    uint32_t elapsed = lv_tick_elaps(op_busy_started_tick_);
    if (elapsed >= MIN_SPINNER_VISIBLE_MS) {
        enter_op_done_state(op);
        return;
    }
    op_revert_target_ = op; // reused by the deferred-done timer to know the op
    schedule_op_timer(MIN_SPINNER_VISIBLE_MS - elapsed, [](lv_timer_t* t) {
        auto* self = static_cast<FilamentPanel*>(lv_timer_get_user_data(t));
        self->op_revert_timer_ = nullptr;
        self->enter_op_done_state(self->op_revert_target_);
    });
}

void FilamentPanel::op_failed(FilamentOp op) {
    if (op == FilamentOp::Unload) {
        // A refused or timed-out unload must not leave the prompt armed to fire
        // on some later unrelated sensor edge.
        helix::ui::disarm_manual_pull_prompt();
    }
    op_showing_busy_.reset();
    cancel_op_revert_timer(); // also clears any pending min-spinner delay
    set_op_state(op, 0);      // back to idle; the error/timeout toast still fires
}

// Every filament operation arms the guard the same way. Routing all callsites
// through one entry point is what keeps the timeout handler from silently
// diverging per path — it used to be an inlined capture-nothing lambda copied
// eight times, and none of the copies cleared any of the op state.
void FilamentPanel::begin_operation_guard() {
    // Capturing `this` is safe: operation_guard_ is a member and ~OperationTimeoutGuard
    // cancels the timer, so the callback cannot outlive the panel.
    operation_guard_.begin(OPERATION_TIMEOUT_MS, [this] { handle_operation_timeout(); });
}

// Runs on the main thread from the guard's one-shot timer. OperationTimeoutGuard
// clears only its own filament_operation_in_progress subject, so a stalled op left
// its button spinning at state 1 for the rest of the session and left the in-flight
// bookkeeping set, which blocked the next op from ever completing (#1183).
void FilamentPanel::handle_operation_timeout() {
    NOTIFY_WARNING(lv_tr("Filament operation timed out"));
    backend_op_active_ = false;
    op_in_flight_.reset();
    if (op_showing_busy_) {
        op_failed(*op_showing_busy_);
    }
}

// Klipper aborts a macro body at an unknown command and reports it through
// respond_info (a `//` line, not `!!`), while Moonraker still answers `ok` for
// the script — so every success callback below fires for a macro that did
// nothing. The only correlation available is "an operation is visibly running":
// Klipper does not tie a response line to the RPC that provoked it, so an
// unknown-command line raised by some other client while a filament op happens to
// be spinning will fail that op. Failing a live op on a stale line is the lesser
// harm — a green checkmark for a macro that never ran is what sends users
// hunting the wrong problem.
void FilamentPanel::fail_op_on_unknown_command(const std::string& command) {
    if (!op_showing_busy_) {
        return; // nothing on screen to invalidate
    }
    const FilamentOp op = *op_showing_busy_;

    op_aborted_ = op; // swallow the `ok` that is still coming
    operation_guard_.end();
    backend_op_active_ = false;
    op_in_flight_.reset();
    op_failed(op);

    // The command name IS the actionable part — it names the macro the user has
    // to define (or remove) in their config. A bare "operation failed" leaves
    // them with nothing to act on.
    NOTIFY_ERROR(lv_tr("Macro stopped: printer has no '{}' command"), command);
}

void FilamentPanel::handle_load_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    // Cancel existing preheat if user taps again
    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    snapshot_prior_heater_target();

    if (!is_extrusion_allowed()) {
        // Ask "home printer first?" BEFORE the preheat, not after: the
        // physical G28 still fires later, inside
        // AmsSubscriptionBackend::ensure_homed_then() right before the tier-1
        // dispatch (unchanged) -- only the confirmation moves earlier, so a
        // decline never wastes a preheat cycle (#1235-adjacent).
        AmsBackend* delegating_backend = AmsState::instance().get_backend();
        if (!helix::toolhead_is_homed(printer_state_) &&
            !(delegating_backend && delegating_backend->delegates_homing_to_printer())) {
            spdlog::info("[{}] Toolhead not homed -- asking before starting preheat for load",
                         get_name());
            // FilamentPanel is an immortal singleton [L012] -- capturing
            // [this] directly is safe with no AsyncLifetimeGuard token.
            helix::ui::request_home_confirmation(
                [this]() {
                    if (AmsBackend* backend = AmsState::instance().get_backend()) {
                        backend->arm_home_preconfirmed();
                    }
                    start_preheat_for_op(PreheatOp::LOAD);
                },
                [this]() {
                    spdlog::info("[{}] User declined pre-load home; no heat commanded", get_name());
                });
            return;
        }
        start_preheat_for_op(PreheatOp::LOAD);
        return;
    }

    // Check if toolhead sensor shows filament already present
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    if (sensor_mgr.is_master_enabled() &&
        sensor_mgr.is_sensor_available(helix::FilamentSensorRole::TOOLHEAD) &&
        sensor_mgr.is_filament_detected(helix::FilamentSensorRole::TOOLHEAD)) {
        // Filament appears to already be loaded - show warning
        spdlog::info("[{}] Toolhead sensor shows filament present - showing load warning",
                     get_name());
        show_load_warning();
        return;
    }

    // No sensor or no filament detected - proceed directly
    execute_load();
}

void FilamentPanel::handle_unload_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    // Cancel existing preheat if user taps again
    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    snapshot_prior_heater_target();

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::UNLOAD);
        return;
    }

    // Check if toolhead sensor shows no filament (nothing to unload)
    auto& sensor_mgr = helix::FilamentSensorManager::instance();
    if (sensor_mgr.is_master_enabled() &&
        sensor_mgr.is_sensor_available(helix::FilamentSensorRole::TOOLHEAD) &&
        !sensor_mgr.is_filament_detected(helix::FilamentSensorRole::TOOLHEAD)) {
        // No filament detected - show warning
        spdlog::info("[{}] Toolhead sensor shows no filament - showing unload warning", get_name());
        show_unload_warning();
        return;
    }

    // Sensor not available or filament detected - proceed directly
    execute_unload();
}

void FilamentPanel::handle_extrude_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    snapshot_prior_heater_target();

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::EXTRUDE);
        return;
    }

    execute_extrude();
}

void FilamentPanel::execute_extrude() {
    spdlog::info("[{}] Extruding {}mm", get_name(), extrude_length_);

    if (!api_) {
        return;
    }

    // Inline G-code: M83 = relative extrusion, G1 E{amount} F{speed}
    begin_operation_guard();
    int speed_mm_min = helix::SettingsManager::instance().get_extrude_speed() * 60;
    spdlog::info("[{}] Extruding {}mm at F{}", get_name(), extrude_length_, speed_mm_min);
    std::string gcode = fmt::format("M83\nG1 E{} F{}", extrude_length_, speed_mm_min);
    op_started(FilamentOp::Extrude); // on-button spinner replaces the start toast

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_succeeded(FilamentOp::Extrude); // checkmark, then auto-revert
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_failed(FilamentOp::Extrude);
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Extrude may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Extrude failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::handle_purge_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    snapshot_prior_heater_target();

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::PURGE);
        return;
    }

    execute_purge();
}

void FilamentPanel::execute_purge() {
    spdlog::info("[{}] Purging", get_name());

    if (!api_) {
        return;
    }

    // Try StandardMacros Purge slot first (PURGE, PURGE_LINE, PRIME_LINE, etc.)
    const auto& info = StandardMacros::instance().get(StandardMacroSlot::Purge);
    if (!info.is_empty()) {
        std::string macro_name = info.get_macro();
        auto cached = MacroParamCache::instance().get(macro_name);

        // Pre-fill PURGE_TEMP from active material if available
        std::string purge_temp_default;
        auto active = helix::get_active_material();
        if (active) {
            int recommended = active->material_info.nozzle_recommended();
            if (recommended > 0) {
                purge_temp_default = std::to_string(recommended);
                spdlog::info("[{}] Active material '{}' recommends PURGE_TEMP={}", get_name(),
                             active->display_name, recommended);
            }
        }

        if (cached.knowledge == MacroParamKnowledge::KNOWN_PARAMS) {
            // Override PURGE_TEMP default with active material temp
            auto params = cached.params;
            if (!purge_temp_default.empty()) {
                for (auto& p : params) {
                    if (p.name == "PURGE_TEMP") {
                        p.default_value = purge_temp_default;
                        break;
                    }
                }
            }
            spdlog::info("[{}] Purge macro '{}' has params, showing modal", get_name(), macro_name);
            get_filament_param_modal().show_for_macro(
                lv_screen_active(), macro_name, params,
                [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Purg", result);
                });
            return;
        }

        if (cached.knowledge == MacroParamKnowledge::UNKNOWN) {
            spdlog::info("[{}] Purge macro '{}' params unknown, showing raw input", get_name(),
                         macro_name);
            get_filament_param_modal().show_for_unknown_params(
                lv_screen_active(), macro_name, [this, macro_name](const MacroParamResult& result) {
                    run_filament_macro(macro_name, "Purg", result);
                });
            return;
        }

        // KNOWN_NO_PARAMS — auto-pass PURGE_TEMP and execute directly
        MacroParamResult result;
        if (!purge_temp_default.empty()) {
            result.params["PURGE_TEMP"] = purge_temp_default;
        }
        run_filament_macro(macro_name, "Purg", result);
        return;
    }

    // Fallback: extrude a fixed 50mm at 10mm/s (M83 = relative extrusion)
    constexpr int PURGE_FALLBACK_MM = 50;
    constexpr int PURGE_FALLBACK_SPEED_MM_MIN = 10 * 60; // 10 mm/s → 600 mm/min
    begin_operation_guard();
    spdlog::info("[{}] Purge fallback: extruding {}mm at F{}", get_name(), PURGE_FALLBACK_MM,
                 PURGE_FALLBACK_SPEED_MM_MIN);
    std::string gcode =
        fmt::format("M83\nG1 E{} F{}", PURGE_FALLBACK_MM, PURGE_FALLBACK_SPEED_MM_MIN);
    op_started(FilamentOp::Purge); // on-button spinner replaces the start toast

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_succeeded(FilamentOp::Purge);
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_failed(FilamentOp::Purge);
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Purge may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Purge failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::handle_retract_button() {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }

    if (pending_preheat_op_ != PreheatOp::NONE) {
        cancel_pending_preheat();
        return;
    }

    snapshot_prior_heater_target();

    if (!is_extrusion_allowed()) {
        start_preheat_for_op(PreheatOp::RETRACT);
        return;
    }

    execute_retract();
}

void FilamentPanel::execute_retract() {
    spdlog::info("[{}] Retracting {}mm", get_name(), extrude_length_);

    if (!api_) {
        return;
    }

    // Inline G-code: M83 = relative extrusion, negative E = retract
    begin_operation_guard();
    int speed_mm_min = helix::SettingsManager::instance().get_extrude_speed() * 60;
    spdlog::info("[{}] Retracting {}mm at F{}", get_name(), extrude_length_, speed_mm_min);
    std::string gcode = fmt::format("M83\nG1 E-{} F{}", extrude_length_, speed_mm_min);
    op_started(FilamentOp::Retract); // on-button spinner replaces the start toast

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_succeeded(FilamentOp::Retract);
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_failed(FilamentOp::Retract);
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Retract may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Retract failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

// ============================================================================
// EXTRUDER DROPDOWN
// ============================================================================

// Left-column sizing: graph always grows to fill, spool card hugs its
// content. Works for every breakpoint and every AMS mode — previous
// "graph fixed per-breakpoint, spool fills" arrangement truncated the
// AMS card at MEDIUM/SMALL when the fixed graph height ate too much.
void FilamentPanel::apply_left_column_sizing(bool /*external_spool_mode*/) {
    if (!temp_graph_card_ || !spool_card_)
        return;

    lv_obj_set_height(temp_graph_card_, 0);
    lv_obj_set_flex_grow(temp_graph_card_, 1);
    lv_obj_set_height(spool_card_, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(spool_card_, 0);
}

void FilamentPanel::update_multi_filament_card_visibility() {
    if (!spool_card_)
        return;

    bool has_ams = (lv_subject_get_int(AmsState::instance().get_ams_type_subject()) != 0);
    bool multi_tool = helix::ToolState::instance().is_multi_tool();

    // Card is ALWAYS visible
    lv_obj_remove_flag(spool_card_, LV_OBJ_FLAG_HIDDEN);

    // AMS manage row visible when AMS or multi-tool
    if (ams_manage_row_) {
        if (has_ams || multi_tool) {
            lv_obj_remove_flag(ams_manage_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ams_manage_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool external_spool_mode = !has_ams && !multi_tool;

    // External spool row visible when no AMS and no multi-tool
    if (external_spool_row_) {
        if (external_spool_mode) {
            lv_obj_remove_flag(external_spool_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(external_spool_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Header row (icon + title) hidden in external spool mode
    if (spool_card_header_row_) {
        if (external_spool_mode) {
            lv_obj_add_flag(spool_card_header_row_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(spool_card_header_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Center the external spool row within the card when in external spool mode
    if (external_spool_mode) {
        lv_obj_set_style_flex_main_place(spool_card_, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_cross_place(spool_card_, LV_FLEX_ALIGN_CENTER, 0);
    } else {
        lv_obj_set_style_flex_main_place(spool_card_, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_flex_cross_place(spool_card_, LV_FLEX_ALIGN_START, 0);
    }

    // Swap which card is the flex filler. Without AMS at MICRO/TINY the spool
    // card has almost no content, so hand the freed space to the temp graph.
    apply_left_column_sizing(external_spool_mode);

    // Update card title dynamically (for AMS/multi-tool modes)
    const char* title = external_spool_mode ? lv_tr("External Spool") : lv_tr("Multi-Filament");
    std::strncpy(card_title_buf_, title, sizeof(card_title_buf_) - 1);
    card_title_buf_[sizeof(card_title_buf_) - 1] = '\0';
    lv_subject_copy_string(&card_title_subject_, card_title_buf_);

    spdlog::debug("[{}] Multi-filament card: ams={}, multi_tool={}, title={}", get_name(), has_ams,
                  multi_tool, title);
}

void FilamentPanel::setup_external_spool_display() {
    if (!external_spool_container_)
        return;

    // Create 48x48 spool canvas inside the container
    external_spool_canvas_ = ui_spool_canvas_create(external_spool_container_, 48);
    if (!external_spool_canvas_) {
        spdlog::warn("[{}] Failed to create external spool canvas", get_name());
        return;
    }
    // L071: Canvas absorbs clicks — pass through to parent row's event_cb
    lv_obj_remove_flag(external_spool_canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(external_spool_canvas_, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Set initial state from AmsState
    update_external_spool_from_state();

    // Observe external spool color changes to reactively update display
    external_spool_observer_ = observe_int_sync<FilamentPanel>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](FilamentPanel* self, int /*color_int*/) {
            self->update_external_spool_from_state();
            self->update_spool_preset();
        },
        AmsState::instance().get_subjects_lifetime());

    spdlog::debug("[{}] External spool display initialized", get_name());
}

void FilamentPanel::update_external_spool_from_state() {
    if (!external_spool_canvas_)
        return;

    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        ui_spool_canvas_set_color(external_spool_canvas_, lv_color_hex(ext->color_rgb));
        float fill =
            (ext->total_weight_g > 0) ? ext->remaining_weight_g / ext->total_weight_g : 1.0f;
        ui_spool_canvas_set_fill_level(external_spool_canvas_, fill);

        // Update labels with material info
        if (external_spool_material_label_) {
            std::string mat_text;
            if (!ext->brand.empty() && !ext->material.empty()) {
                mat_text = ext->brand + " " + ext->material;
            } else if (!ext->material.empty()) {
                mat_text = ext->material;
            } else {
                mat_text = lv_tr("Unknown");
            }
            // Append Spoolman spool ID when linked (e.g., "Prusament PLA #129")
            if (ext->spoolman_id > 0) {
                char id_buf[16];
                snprintf(id_buf, sizeof(id_buf), " #%d", ext->spoolman_id);
                mat_text += id_buf; // i18n: do not translate — Spoolman ID
            }
            lv_label_set_text(external_spool_material_label_, mat_text.c_str());
        }
        if (external_spool_color_label_) {
            // Build second line: color name + remaining weight in grams
            std::string detail;
            if (!ext->color_name.empty()) {
                detail = ext->color_name;
            }
            if (ext->remaining_weight_g > 0) {
                char weight_str[16];
                snprintf(weight_str, sizeof(weight_str), "%.0fg", ext->remaining_weight_g);
                if (!detail.empty()) {
                    detail += " · ";
                }
                detail += weight_str;
            }
            lv_label_set_text(external_spool_color_label_, detail.c_str());
        }
    } else {
        // No spool assigned - show muted empty spool
        ui_spool_canvas_set_color(external_spool_canvas_, lv_color_hex(0x505050));
        ui_spool_canvas_set_fill_level(external_spool_canvas_, 0.0f);

        if (external_spool_material_label_) {
            lv_label_set_text(external_spool_material_label_, lv_tr("No spool assigned"));
        }
        if (external_spool_color_label_) {
            lv_label_set_text(external_spool_color_label_, lv_tr("Tap to assign"));
        }
    }
    ui_spool_canvas_redraw(external_spool_canvas_);
}

void FilamentPanel::show_external_spool_edit_modal() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Cannot show slot editor - no parent screen", get_name());
        return;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    SlotInfo initial_info = ext.value_or(SlotInfo{});
    initial_info.slot_index = -2;
    initial_info.global_index = -2;

    helix::ui::get_ams_edit_overlay().show_for_slot(
        parent_screen_, -2, initial_info, api_,
        [](const helix::ui::AmsEditOverlay::EditResult& result) {
            if (result.saved) {
                AmsState::instance().commit_external_spool_edit(result.slot_info);
                NOTIFY_INFO(lv_tr("External spool updated"));
            }
        });
}

void FilamentPanel::on_external_spool_edit_clicked(lv_event_t* /*e*/) {
    get_global_filament_panel().show_external_spool_edit_modal();
}

void FilamentPanel::populate_extruder_dropdown() {
    if (!extruder_dropdown_)
        return;

    auto& ts = helix::ToolState::instance();
    bool has_ams = (lv_subject_get_int(AmsState::instance().get_ams_type_subject()) != 0);
    if (!ts.is_multi_tool() || !has_ams) {
        if (extruder_selector_group_)
            lv_obj_add_flag(extruder_selector_group_, LV_OBJ_FLAG_HIDDEN);
        if (btn_manage_slots_)
            lv_obj_remove_flag(btn_manage_slots_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Multi-tool with AMS: show dropdown group, hide Manage button
    if (extruder_selector_group_)
        lv_obj_remove_flag(extruder_selector_group_, LV_OBJ_FLAG_HIDDEN);
    if (btn_manage_slots_)
        lv_obj_add_flag(btn_manage_slots_, LV_OBJ_FLAG_HIDDEN);

    // Build options string ("T0\nT1\nT2")
    std::string options;
    for (const auto& tool : ts.tools()) {
        if (!options.empty())
            options += '\n';
        options += tool.name;
    }
    lv_dropdown_set_options(extruder_dropdown_, options.c_str());

    // Sync selection to active tool
    int active = ts.active_tool_index();
    if (active >= 0 && active < ts.tool_count()) {
        lv_dropdown_set_selected(extruder_dropdown_, static_cast<uint32_t>(active));
    }

    spdlog::debug("[{}] Extruder dropdown populated: {} tools, active=T{}", get_name(),
                  ts.tool_count(), active);
}

int FilamentPanel::selected_op_slot() const {
    // Map the selected tool (dropdown index == tool index) to a global slot.
    // resolve_op_button_slot() prefers an explicit tool→slot map, then falls
    // back by topology: tool index == slot index on a multi-tool toolchanger,
    // but current_slot on a single-extruder multi-lane AMS (AD5X IFS), where
    // the loaded lane is NOT the tool index (prestonbrown/helixscreen#1065).
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return -1;
    }
    AmsSystemInfo sys = backend->get_system_info();
    int selected_tool = helix::ToolState::instance().active_tool_index();
    if (extruder_dropdown_) {
        selected_tool = static_cast<int>(lv_dropdown_get_selected(extruder_dropdown_));
    }
    return helix::ui::resolve_op_button_slot(sys, selected_tool,
                                             helix::ToolState::instance().tool_count());
}

void FilamentPanel::update_filament_op_buttons() {
    // Recompute Load/Unload/Purge gating from the SELECTED tool's LIVE load
    // state (Task 5). Without an AMS backend (single-extruder / external-spool
    // mode) we have no per-slot load signal, so leave both enabled — the only
    // disablers there are the safety-warning / operation-in-progress XML binds.
    //
    // The no-backend path below keeps its "leave both enabled" behavior even
    // mid-print: those printers drive load/unload through plain macros rather
    // than the AMS precondition guard, so a manual mid-pause feed is legitimate
    // there and nothing would refuse it.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        lv_subject_set_int(&load_disabled_subject_, 0);
        lv_subject_set_int(&unload_disabled_subject_, 0);
        return;
    }

    // Single source of truth for the acted-on slot — the same resolution the
    // Load/Unload executors use, so gating can never diverge from the op.
    int slot = selected_op_slot();

    // Whether a print blocks the op is print_blocks_filament_op(), the mirror of
    // AmsSubscriptionBackend::refuse_if_printing(). PRINTING always blocks;
    // PAUSED blocks only on a backend whose filament macro homes itself (AD5X
    // IFS). Reading the raw print_active subject here would grey the buttons
    // through every runout pause on every other backend — i.e. exactly when the
    // user needs them.
    const auto lifecycle = printer_state_.get_print_lifecycle();
    const bool print_blocks_op =
        helix::ui::print_blocks_filament_op(lifecycle, backend->filament_ops_self_home());

    // check_preconditions() refuses on a busy AMS *before* it even looks at the
    // print state, and an op can be started from the AMS panel or by the printer
    // itself. The panel never read this, so its Load button stayed lit through
    // every load someone else kicked off. AmsSystemInfo::is_busy() is the same
    // predicate the backend guard uses.
    const AmsSystemInfo sys = backend->get_system_info();

    helix::ui::OpButtonState state;
    state.print_blocks_op = print_blocks_op;
    state.system_busy = sys.is_busy();
    // unload_target_is_loaded() with is_current_slot=false keeps the narrow
    // per-slot rule this gating has always used for lanes (the recovery arm
    // belongs to the runout dialog, not to a resting panel) while routing the
    // bypass sentinel to the toolhead-wide flag — the only signal that can
    // answer for a spool with no lane behind it.
    state.slot_is_loaded = helix::ui::unload_target_is_loaded(
        slot, backend->slot_is_actively_loaded(slot), backend->slot_has_filament_at_toolhead(slot),
        /*is_current_slot=*/false, sys.filament_loaded);
    if (slot >= 0) {
        // Bypass deliberately skipped: there is no lane whose presence sensor
        // could answer, and slot_presence()'s nullopt ("unanswerable") is what
        // keeps Load reachable so the user can feed the next external spool.
        state.slot_has_filament = helix::ui::slot_presence(backend->get_slot_info(slot));
    }
    // Unload/Purge act on whatever is at the toolhead for this slot, and the
    // panel's Unload is always the heated toolhead unload — the cold lane ops
    // (Eject / Recover) live on the AMS context menu, not here.
    state.unload_available = state.slot_is_loaded;
    state.unload_is_cold_lane_op = false;

    const auto gating = helix::ui::compute_op_button_gating(state);
    lv_subject_set_int(&load_disabled_subject_, gating.load_disabled ? 1 : 0);
    lv_subject_set_int(&unload_disabled_subject_, gating.unload_disabled ? 1 : 0);
    spdlog::debug("[FilamentPanel] Op buttons: slot={} loaded={} has_filament={} busy={} "
                  "print_blocks={} (lifecycle={}, self_homes={}) "
                  "(load_disabled={}, unload_disabled={})",
                  slot, state.slot_is_loaded,
                  state.slot_has_filament ? (*state.slot_has_filament ? "yes" : "no") : "unknown",
                  state.system_busy, print_blocks_op, static_cast<int>(lifecycle),
                  backend->filament_ops_self_home(), gating.load_disabled, gating.unload_disabled);
}

void FilamentPanel::handle_extruder_changed() {
    if (!extruder_dropdown_)
        return;

    int selected = static_cast<int>(lv_dropdown_get_selected(extruder_dropdown_));
    auto& ts = helix::ToolState::instance();

    // Re-evaluate button gating for the newly-selected tool immediately, even if
    // it's already the active tool (no tool change issued below).
    update_filament_op_buttons();

    if (selected == ts.active_tool_index())
        return;

    if (selected < 0 || selected >= static_cast<int>(ts.tools().size())) {
        spdlog::warn("[{}] Invalid extruder index {}", get_name(), selected);
        return;
    }

    // Divergent behavior by topology: on a shared-extruder AMS (AFC BoxTurtle =
    // HUB, AD5X IFS = LINEAR), selecting a tool in the dropdown must NOT trigger a
    // physical filament swap — that's a multi-minute cut/unload/load at the single
    // toolhead. The dropdown is selection-only; the explicit Load button performs
    // the swap (execute_load acts on selected_op_slot()). Only a true PARALLEL
    // toolchanger (each tool is its own toolhead) changes tool on select. With no
    // backend (plain multi-extruder / external spool) keep the gcode Tn fallback.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->get_topology() != PathTopology::PARALLEL) {
        spdlog::info("[{}] Tool T{} selected (AMS: selection only; Load performs the swap)",
                     get_name(), selected);
        return;
    }

    spdlog::info("[{}] User selected tool T{}", get_name(), selected);

    ts.request_tool_change(
        selected, api_, [selected]() { NOTIFY_SUCCESS(lv_tr("Switched to T{}"), selected); },
        [this](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            // Revert dropdown to actual active tool on UI thread
            helix::ui::async_call(
                [](void* ctx) {
                    auto* panel = static_cast<FilamentPanel*>(ctx);
                    if (panel->extruder_dropdown_) {
                        int active = helix::ToolState::instance().active_tool_index();
                        if (active >= 0) {
                            lv_dropdown_set_selected(panel->extruder_dropdown_,
                                                     static_cast<uint32_t>(active));
                        }
                    }
                },
                this);
        });
}

void FilamentPanel::on_extruder_dropdown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extruder_dropdown_changed");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extruder_changed();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void FilamentPanel::on_manage_slots_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_manage_slots_clicked");
    LV_UNUSED(e);

    spdlog::info("[FilamentPanel] Opening AMS panel overlay");
    navigate_to_ams_panel();

    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_load_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_load_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_unload_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_extrude_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extrude_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extrude_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_purge_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_purge_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_purge_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_retract_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_retract_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_retract_button();
    LVGL_SAFE_EVENT_CB_END();
}

// Material preset callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_preset_pla_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_pla_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(0);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_petg_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_petg_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(1);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_abs_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_abs_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(2);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_tpu_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_tpu_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_button(3);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_pla_hold(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_pla_hold");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_longpress(0);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_petg_hold(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_petg_hold");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_longpress(1);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_abs_hold(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_abs_hold");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_longpress(2);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_tpu_hold(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_tpu_hold");
    LV_UNUSED(e);
    get_global_filament_panel().handle_preset_longpress(3);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_preset_spool_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_preset_spool_clicked");
    get_global_filament_panel().handle_spool_preset_button();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::handle_spool_preset_button() {
    if (!cached_active_material_.has_value())
        return;

    const auto& mat = cached_active_material_->material_info;
    nozzle_target_ = mat.nozzle_recommended();
    bed_target_ = mat.bed_temp;

    // Deselect all fixed presets
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, -1);
    update_preset_buttons_visual();

    // Highlight spool preset
    if (spool_preset_button_) {
        lv_obj_add_state(spool_preset_button_, LV_STATE_CHECKED);
    }

    update_temp_display();
    update_material_temp_display();
    update_status();

    // Send temperature commands
    if (auto* c = get_temperature_controller()) {
        // Switching material via spool preset: hold the previous filament's temp
        // if hotter so the old material still purges cleanly (keep_previous_hot).
        c->set_target(
            helix::HeaterType::Nozzle, static_cast<double>(nozzle_target_),
            {.toast = true, .keep_previous_hot = true, .on_success = [t = nozzle_target_]() {
                 NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), t);
             }});
        c->set_target(helix::HeaterType::Bed, static_cast<double>(bed_target_),
                      {.toast = true, .on_success = [t = bed_target_]() {
                           NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), t);
                       }});
    }

    spdlog::info("[{}] Spool preset applied: {} (nozzle={}°C, bed={}°C)", get_name(),
                 cached_active_material_->display_name, nozzle_target_, bed_target_);
}

void FilamentPanel::update_spool_preset() {
    cached_active_material_ = helix::get_active_material();

    if (!spool_preset_button_)
        return;

    if (!cached_active_material_.has_value()) {
        lv_obj_add_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const auto& active = *cached_active_material_;

    // Check if material matches an existing preset — if so, don't show spool button
    for (int i = 0; i < PRESET_COUNT; i++) {
        std::string preset_lower(helix::presets::name(i));
        std::string mat_lower(active.material_name);
        std::transform(preset_lower.begin(), preset_lower.end(), preset_lower.begin(), ::tolower);
        std::transform(mat_lower.begin(), mat_lower.end(), mat_lower.begin(), ::tolower);
        if (preset_lower == mat_lower) {
            lv_obj_add_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    // Novel material — show spool preset button.
    // Using lv_label_set_text directly: text is dynamic (material name + computed temps)
    // so subject binding is not practical here.
    lv_obj_remove_flag(spool_preset_button_, LV_OBJ_FLAG_HIDDEN);

    if (spool_preset_label_) {
        lv_label_set_text(spool_preset_label_, active.material_name.c_str());
    }
    if (spool_preset_temps_) {
        auto text = fmt::format("{} / {}°C", active.material_info.nozzle_recommended(),
                                active.material_info.bed_temp);
        lv_label_set_text(spool_preset_temps_, text.c_str());
    }

    // Deselect spool button initially (user must tap)
    if (spool_preset_button_) {
        lv_obj_remove_state(spool_preset_button_, LV_STATE_CHECKED);
    }

    spdlog::debug("[{}] Spool preset shown: {} ({}°C / {}°C)", get_name(), active.display_name,
                  active.material_info.nozzle_recommended(), active.material_info.bed_temp);
}

// Temperature tap callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_nozzle_temp_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_nozzle_temp_tap_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_nozzle_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_bed_temp_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_bed_temp_tap_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_bed_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::custom_nozzle_keypad_cb(float value, void* user_data) {
    auto* self = static_cast<FilamentPanel*>(user_data);
    if (self) {
        self->handle_custom_nozzle_confirmed(value);
    }
}

void FilamentPanel::custom_bed_keypad_cb(float value, void* user_data) {
    auto* self = static_cast<FilamentPanel*>(user_data);
    if (self) {
        self->handle_custom_bed_confirmed(value);
    }
}

void FilamentPanel::on_nozzle_target_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_nozzle_target_tap_clicked");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_nozzle_target_tap_clicked TRIGGERED");
    get_global_filament_panel().handle_nozzle_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_bed_target_tap_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_bed_target_tap_clicked");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_bed_target_tap_clicked TRIGGERED");
    get_global_filament_panel().handle_bed_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_filament_chamber_target_tap(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_filament_chamber_target_tap");
    LV_UNUSED(e);
    spdlog::debug("[FilamentPanel] on_filament_chamber_target_tap TRIGGERED");
    get_global_filament_panel().handle_chamber_temp_tap();
    LVGL_SAFE_EVENT_CB_END();
}

// Extrude length callbacks (XML event_cb - use global singleton)
void FilamentPanel::on_extrude_length_5mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extrude_length_5mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extrude_length_select(5);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_extrude_length_10mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extrude_length_10mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extrude_length_select(10);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_extrude_length_25mm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_extrude_length_25mm_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_extrude_length_select(25);
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_cooldown_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_cooldown_clicked");
    LV_UNUSED(e);
    get_global_filament_panel().handle_cooldown();
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::handle_cooldown() {
    spdlog::info("[{}] Cooldown requested - turning off heaters", get_name());

    if (api_) {
        // Build default cooldown gcode, including chamber if printer has one
        std::string default_gcode = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                                    "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0";

        const auto& discovery = printer_state_.get_discovery();
        if (discovery.has_chamber_heater()) {
            char chamber_gcode[128];
            if (helix::ui::temperature::build_heater_off_gcode(
                    discovery.chamber_heater_name(), chamber_gcode, sizeof(chamber_gcode))) {
                default_gcode += "\n";
                default_gcode += chamber_gcode;
            }
        }

        // Use configured cooldown macro (user-overridable in settings.json)
        auto* cfg = helix::Config::get_instance();
        helix::MacroConfig default_cooldown{"Cool Down", default_gcode};
        auto cooldown = cfg ? cfg->get_macro("cooldown", default_cooldown) : default_cooldown;

        api_->execute_gcode(
            cooldown.gcode, []() { NOTIFY_SUCCESS(lv_tr("Heaters off")); },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to turn off heaters: {}"), error.user_message());
            });
    }

    // Clear material selection since we're cooling down
    selected_material_ = -1;
    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
}

// ============================================================================
// PUBLIC API
// ============================================================================

void FilamentPanel::set_temp(int current, int target) {
    // Validate temperature ranges
    helix::ui::temperature::validate_and_clamp_pair(
        current, target, helix::ui::temperature::degrees_to_deci(nozzle_min_temp_),
        helix::ui::temperature::degrees_to_deci(nozzle_max_temp_), "Filament");

    nozzle_current_ = current;
    nozzle_target_ = target;

    update_temp_display();
    update_status();
    update_warning_text();
    update_safety_state();
}

void FilamentPanel::get_temp(int* current, int* target) const {
    if (current)
        *current = nozzle_current_;
    if (target)
        *target = nozzle_target_;
}

void FilamentPanel::set_material(int material_id) {
    if (material_id < 0 || material_id >= PRESET_COUNT) {
        spdlog::error("[{}] Invalid material ID {} (valid: 0-{})", get_name(), material_id,
                      PRESET_COUNT - 1);
        return;
    }

    auto mat = filament::find_material(helix::presets::name(material_id));
    if (!mat) {
        spdlog::error("[{}] Material '{}' not found in database", get_name(),
                      helix::presets::name(material_id));
        return;
    }

    selected_material_ = material_id;
    nozzle_target_ = mat->nozzle_recommended();
    bed_target_ = mat->bed_temp;

    // Set chamber target from material preset (clear if material has no chamber requirement)
    if (printer_state_.get_discovery().has_chamber_heater()) {
        chamber_target_ = mat->chamber_temp_c > 0
                              ? helix::ui::temperature::degrees_to_deci(mat->chamber_temp_c)
                              : 0;
        update_chamber_temp_display();
    }

    lv_subject_set_int(&material_selected_subject_, selected_material_);
    update_preset_buttons_visual();
    update_temp_display();
    update_material_temp_display();
    update_status();

    spdlog::info("[{}] Material set: {} (nozzle={}°C, bed={}°C, chamber={}°C)", get_name(),
                 helix::presets::name(material_id), nozzle_target_, bed_target_,
                 mat->chamber_temp_c);
}

bool FilamentPanel::is_extrusion_allowed() const {
    // Opt-in override (#978): users whose load/unload macros heat the nozzle
    // themselves — or perform a deliberate cold pull — can bypass the
    // min_extrude_temp gate so the buttons stay active on a cold hotend.
    if (helix::SafetySettingsManager::instance().get_allow_cold_extrude()) {
        return true;
    }
    return helix::ui::temperature::is_extrusion_safe(nozzle_current_, min_extrude_temp_);
}

bool FilamentPanel::has_active_spool_material() const {
    // Check if there's a known spool with valid material info (external spool or AMS active slot)
    auto ext = AmsState::instance().get_external_spool_info();
    if (ext.has_value()) {
        auto active = helix::build_active_material(*ext);
        if (active.material_info.nozzle_min > 0) {
            return true;
        }
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo sys_info = backend->get_system_info();
        const SlotInfo* active_slot = sys_info.get_active_slot();
        if (active_slot) {
            auto active = helix::build_active_material(*active_slot);
            if (active.material_info.nozzle_min > 0) {
                return true;
            }
        }
    }

    return false;
}

int FilamentPanel::preheat_slot_for_op(PreheatOp op) const {
    switch (op) {
    case PreheatOp::LOAD:
    case PreheatOp::UNLOAD:
        // The slot the operation acts on — the same resolution execute_load() /
        // execute_unload() use, so the temperature can never belong to a
        // different lane than the op.
        return selected_op_slot();
    case PreheatOp::EXTRUDE:
    case PreheatOp::RETRACT:
    case PreheatOp::PURGE: {
        // Not slot-scoped: these push whatever is already in the melt zone, so
        // the loaded lane is the authority, not the dropdown selection.
        AmsBackend* backend = AmsState::instance().get_backend();
        return backend ? backend->get_current_slot() : -1;
    }
    default:
        return -1;
    }
}

FilamentPanel::PreheatTempResult FilamentPanel::resolve_preheat_temp(int target_slot) const {
    // Priorities 1 and 2 (target slot, then the external spool as the fallback
    // for a load with no lane of its own) are shared with
    // AmsOperationSidebar::get_load_temp_for_slot() via
    // resolve_load_preheat_material(). This used to consult the external spool
    // FIRST and unconditionally, then the *loaded* slot rather than the selected
    // one — so a PETG lane selected while PLA was loaded preheated to PLA, and
    // any printer with an external spool assigned preheated every load to that
    // spool. Both were silent and both cause jams.
    AmsBackend* backend = AmsState::instance().get_backend();
    SlotInfo slot;
    const SlotInfo* slot_ptr = nullptr;
    if (backend && target_slot >= 0) {
        slot = backend->get_slot_info(target_slot);
        slot_ptr = &slot;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    if (auto resolved = helix::ui::resolve_load_preheat_material(
            target_slot, slot_ptr, ext.has_value() ? &ext.value() : nullptr)) {
        return {resolved->temp_c, resolved->material_name};
    }

    // Priority 3: the panel's selected material preset. The sidebar has no
    // preset UI, so this tail is the panel's alone — it only runs when neither
    // the target lane nor an external spool names a material, i.e. exactly where
    // the sidebar would fall back to a blind default.
    if (selected_material_ >= 0 && selected_material_ < PRESET_COUNT) {
        auto mat = filament::find_material(helix::presets::name(selected_material_));
        if (mat) {
            return {helix::ui::load_preheat_temp(*mat), helix::presets::name(selected_material_)};
        }
    }

    // Priority 4: Fallback to min_extrude_temp_
    return {min_extrude_temp_, ""};
}

const char* FilamentPanel::preheat_op_name(PreheatOp op) {
    switch (op) {
    case PreheatOp::LOAD:
        return "load";
    case PreheatOp::UNLOAD:
        return "unload";
    case PreheatOp::EXTRUDE:
        return "extrude";
    case PreheatOp::RETRACT:
        return "retract";
    case PreheatOp::PURGE:
        return "purge";
    default:
        return "unknown";
    }
}

// Reads the live extruder target subject rather than the cached
// nozzle_target_ member — set_material() overwrites nozzle_target_ with
// the preset's preview temperature, so it's unreliable here.
int FilamentPanel::current_extruder_target() const {
    auto* subj = printer_state_.get_active_extruder_target_subject();
    return subj ? deci_to_degrees(lv_subject_get_int(subj)) : 0;
}

// Called at op-handler entry (not inside start_preheat_for_op) so the
// hot-nozzle path that skips preheat still records what the user had
// commanded; restore_heater_after_preheat() consults this to decide
// whether to schedule the post-op cooldown.
void FilamentPanel::snapshot_prior_heater_target() {
    prior_nozzle_target_ = current_extruder_target();
}

void FilamentPanel::start_preheat_for_op(PreheatOp op) {
    auto [target, material_name] = resolve_preheat_temp(preheat_slot_for_op(op));

    pending_preheat_op_ = op;
    pending_preheat_target_ = target;

    // Send the preheat target and let the controller floor it at the hotter of
    // the requested temp, the latched previous target, and the current actual
    // (keep_previous_hot). The controller's own max() avoids the 240→200→240 dip
    // that a naive lower would cause, while the prior_nozzle_target_ snapshot
    // guarantees the heater isn't turned off when the op completes.
    const int real_target = current_extruder_target();
    if (auto* c = get_temperature_controller()) {
        c->set_target(helix::HeaterType::Nozzle, static_cast<double>(target),
                      {.toast = false, .keep_previous_hot = true});
    }

    if (material_name.empty()) {
        NOTIFY_INFO(lv_tr("Heating to {}°C..."), target);
    } else {
        NOTIFY_INFO(lv_tr("Heating to {}°C for {}..."), target, material_name);
    }

    spdlog::info("[{}] Starting preheat to {}°C ({}) for {} (prior_target={}, real_target={})",
                 get_name(), target, material_name.empty() ? "fallback" : material_name,
                 preheat_op_name(op), prior_nozzle_target_, real_target);
}

void FilamentPanel::check_pending_preheat() {
    if (pending_preheat_op_ == PreheatOp::NONE) {
        return;
    }

    constexpr int TEMP_THRESHOLD = 5;
    if (nozzle_current_ < (pending_preheat_target_ - TEMP_THRESHOLD)) {
        return;
    }

    PreheatOp op = pending_preheat_op_;
    pending_preheat_op_ = PreheatOp::NONE;
    pending_preheat_target_ = 0;

    spdlog::info("[{}] Preheat complete, executing {}", get_name(), preheat_op_name(op));

    switch (op) {
    case PreheatOp::LOAD:
        execute_load();
        break;
    case PreheatOp::UNLOAD:
        execute_unload();
        break;
    case PreheatOp::EXTRUDE:
        execute_extrude();
        break;
    case PreheatOp::RETRACT:
        execute_retract();
        break;
    case PreheatOp::PURGE:
        execute_purge();
        break;
    default:
        break;
    }
}

void FilamentPanel::cancel_pending_preheat() {
    if (pending_preheat_op_ == PreheatOp::NONE) {
        return;
    }

    spdlog::info("[{}] Preheat cancelled", get_name());
    pending_preheat_op_ = PreheatOp::NONE;
    pending_preheat_target_ = 0;

    // A confirmed-then-abandoned load must not leave home consent armed for a
    // later, unrelated dispatch on this backend. Harmless no-op when nothing
    // was armed (e.g. cancelling an UNLOAD/EXTRUDE/RETRACT/PURGE preheat,
    // which never arms this).
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        backend->clear_home_preconfirmed();
    }

    // Cancel any pending cooldown timer
    PostOpCooldownManager::instance().cancel();

    // Restore heater to prior state immediately (no delay for cancel)
    if (prior_nozzle_target_ == 0) {
        if (auto* c = get_temperature_controller()) {
            c->set_target(helix::HeaterType::Nozzle, 0.0, {.toast = false});
        }
    }
    prior_nozzle_target_ = 0;

    NOTIFY_INFO(lv_tr("Preheat cancelled"));
}

void FilamentPanel::restore_heater_after_preheat() {
    // Schedule the post-op cooldown whenever the printer is idle (not printing or
    // paused), regardless of the pre-op nozzle target. The old prior_nozzle_target_
    // == 0 guard was meant to preserve a deliberate manual preheat, but it could not
    // tell a real user-set target from one left stale by a PRIOR filament op that
    // never cooled — so after a swap the nozzle held the material temp indefinitely
    // (AFC's auto-heat on load makes this the common case). A real print re-heats or
    // cancels the pending cooldown, so cooling 120s after an idle swap is safe.
    // The lifecycle, so a job that is starting also suppresses the cooldown —
    // the pre-start block is about to heat the nozzle, and the comment above
    // already gives "a real print re-heats or cancels the pending cooldown" as
    // the reason this is safe. Preparing is that case, one step earlier.
    const auto lifecycle = printer_state_.get_print_lifecycle();
    if (!job_holds_machine(lifecycle)) {
        PostOpCooldownManager::instance().schedule();
    }
    prior_nozzle_target_ = 0;
}

void FilamentPanel::set_limits(int min_temp, int max_temp, int min_extrude_temp) {
    nozzle_min_temp_ = min_temp;
    nozzle_max_temp_ = max_temp;

    // Update min_extrude_temp and safety warning text if changed
    if (min_extrude_temp_ != min_extrude_temp) {
        min_extrude_temp_ = min_extrude_temp;
        std::snprintf(safety_warning_text_buf_, sizeof(safety_warning_text_buf_),
                      lv_tr("Heat to at least %d°C for filament operations"), min_extrude_temp_);
        lv_subject_copy_string(&safety_warning_text_subject_, safety_warning_text_buf_);
        spdlog::info("[{}] Min extrusion temp updated: {}°C", get_name(), min_extrude_temp_);
    }

    spdlog::debug("[{}] Nozzle temperature limits updated: {}-{}°C", get_name(), min_temp,
                  max_temp);
}

// ============================================================================
// FILAMENT SENSOR WARNING HELPERS
// ============================================================================

void FilamentPanel::execute_load() {
    // The three-tier routing (AMS backend → configured macro → raw gcode) lives
    // in plan_load(), the shared answer for every dispatch surface — it also
    // carries the already-mounted guard and the load-vs-swap rule that only
    // AmsOperationSidebar used to apply. Everything the planner needs is read off
    // the backend here; the panel only owns what to *do* with the answer.
    AmsBackend* backend = AmsState::instance().get_backend();
    // Single source of truth: act on the dropdown-selected tool's slot, the same
    // one the button gating uses — never a divergent current_slot read. Resolved
    // before the caps because needs_unload_before_load() is answered per lane.
    const int target_slot = selected_op_slot();

    AmsSystemInfo sys;
    helix::ui::BackendCaps caps;
    if (backend) {
        sys = backend->get_system_info();
        caps.present = true;
        caps.requires_slot_selection_for_load = backend->requires_slot_selection_for_load();
        caps.needs_unload_before_load = backend->needs_unload_before_load(sys, target_slot);
        caps.is_tool_changer = backend->get_type() == AmsType::TOOL_CHANGER;
    }

    const auto& info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    const helix::ui::FilamentOpPlan plan = helix::ui::plan_load(
        sys, caps, target_slot, !info.is_empty(), info.get_source() == MacroSource::CONFIGURED);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        // Backend load is fire-and-forget: completion is signaled by
        // ams_action_observer_ when AmsAction reaches IDLE or ERROR. Start the
        // guard + on-button spinner here; backend_op_active_ gates the observer
        // so it only completes backend ops (never gcode/macro ops).
        begin_operation_guard();
        backend_op_active_ = true;
        op_in_flight_ = FilamentOp::Load;
        op_started(FilamentOp::Load);
        AmsError err;
        switch (plan.ams_call) {
        case helix::ui::AmsCall::ChangeTool:
            spdlog::info("[{}] Filament seated — swapping to selected slot via tool change T{}",
                         get_name(), plan.ams_arg);
            err = backend->change_tool(plan.ams_arg);
            break;
        case helix::ui::AmsCall::Load:
        default: // plan_load yields no other tier-1 call
            spdlog::info("[{}] Loading filament directly into selected slot {} (no redirect)",
                         get_name(), plan.ams_arg);
            err = backend->load_filament(plan.ams_arg);
            break;
        }
        if (!err.success()) {
            operation_guard_.end();
            backend_op_active_ = false;
            op_in_flight_.reset();
            op_failed(FilamentOp::Load);
            helix::ui::notify_ams_error(err);
        }
        return;
    }

    case helix::ui::FilamentTier::Refused:
        switch (plan.refusal) {
        case helix::ui::FilamentRefusal::AlreadyMounted:
            // SELECT_TOOL on the tool already on the carriage is a firmware no-op;
            // dispatching it left the Load button spinning for the full guard
            // timeout (bundle 9KRXZ62P). Refuse without arming guard or spinner —
            // but say so, the user did press the button.
            spdlog::info("[{}] Selected tool is already mounted — refusing load", get_name());
            NOTIFY_INFO(lv_tr("That tool is already loaded"));
            break;
        case helix::ui::FilamentRefusal::SelectSlot:
        default:
            spdlog::info("[{}] AMS backend active ({}), no active slot — redirecting to AMS panel",
                         get_name(), ams_type_to_string(backend->get_type()));
            NOTIFY_INFO(lv_tr("Select a filament slot to load"));
            navigate_to_ams_panel();
            break;
        }
        return;

    case helix::ui::FilamentTier::Macro: {
        std::string macro_name = info.get_macro();
        // FilamentPanel is a global singleton, so `this` survives any modal
        // dismissal the shared param modal outlives [L012]. Surfaces with a
        // bounded lifetime must guard this callback with a LifetimeToken.
        helix::ui::dispatch_filament_macro(macro_name, helix::ui::ParamPolicy::Prompt,
                                           [this, macro_name](const MacroParamResult& result) {
                                               run_filament_macro(macro_name, "Load", result);
                                           });
        return;
    }

    case helix::ui::FilamentTier::RawGcode:
        break;
    }

    // Fallback: the shared tier-3 load sequence (bowden fast move + slow melt-zone push).
    begin_operation_guard();
    std::string gcode = filament_load_fallback_gcode();
    spdlog::info("[{}] Load fallback: {}", get_name(), gcode);
    op_started(FilamentOp::Load); // on-button spinner replaces the start toast

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                    self->op_succeeded(FilamentOp::Load);
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_failed(FilamentOp::Load);
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Load may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Filament load failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::execute_unload() {
    // Filament is being pulled — nothing left to purge, so drop the swap-preheat
    // latch. The next load computes its hold-temp fresh instead of inheriting this
    // material's target.
    printer_state_.clear_nozzle_load_latch();

    // When an AMS backend is active, route unload through it so the backend's
    // tool change sequence runs (retract, cut, purge) instead of raw extrusion.
    // plan_unload() gates tier 1 on the backend merely existing — deliberately
    // asymmetric with plan_load(), because bypass unload stays on the backend:
    // AFC calls the user's unload_filament macro itself when bypass is enabled.
    AmsBackend* backend = AmsState::instance().get_backend();
    // Single source of truth: act on the dropdown-selected tool's slot, the same
    // one the button gating uses — never a divergent current_slot read.
    const int slot = selected_op_slot();
    helix::ui::BackendCaps caps;
    bool loaded = false;
    if (backend) {
        // Only `present` matters to plan_unload; the remaining caps answer the
        // load-vs-swap question, which unload does not ask.
        caps.present = true;
        const AmsSystemInfo sys = backend->get_system_info();
        loaded = helix::ui::unload_target_is_loaded(slot, backend->slot_is_actively_loaded(slot),
                                                    backend->slot_has_filament_at_toolhead(slot),
                                                    sys.current_slot == slot, sys.filament_loaded);
    }

    const auto& info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    const helix::ui::FilamentOpPlan plan = helix::ui::plan_unload(
        caps, slot, loaded, !info.is_empty(), info.get_source() == MacroSource::CONFIGURED);

    // Nothing reels a bypass spool (or a backend-less printer's spool) back down
    // a lane, so the user has to finish the job by hand. Armed before dispatch so
    // the toolhead sensor's clear edge is already being watched when the retract
    // starts; op_succeeded/op_failed close it out for every tier below.
    if (plan.tier != helix::ui::FilamentTier::Refused &&
        helix::ui::unload_needs_manual_pull(backend != nullptr, slot)) {
        helix::ui::arm_manual_pull_prompt();
    }

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        begin_operation_guard();
        spdlog::info("[{}] Unloading filament from selected slot {} via AMS backend ({})",
                     get_name(), plan.ams_arg, ams_type_to_string(backend->get_type()));
        // On-button spinner replaces the start toast. Completion is signaled by
        // ams_action_observer_ when AmsAction reaches IDLE or ERROR;
        // backend_op_active_ gates that observer to backend ops only.
        backend_op_active_ = true;
        op_in_flight_ = FilamentOp::Unload;
        op_started(FilamentOp::Unload);
        // Pass the panel's single-source selected_op_slot() explicitly rather than
        // letting the backend re-resolve current_slot: the callsite's intended slot
        // is authoritative, so the unload can never diverge from the gating or the
        // "is anything loaded?" guard above. Re-reading current_slot in the backend
        // was the U1 Filament-panel-unload wrong-tool bug.
        AmsError err = backend->unload_filament(plan.ams_arg);
        if (!err.success()) {
            operation_guard_.end();
            backend_op_active_ = false;
            op_in_flight_.reset();
            op_failed(FilamentOp::Unload);
            helix::ui::notify_ams_error(err);
        }
        // Guard ends via ams_action_observer_ (AmsAction IDLE/ERROR) or timeout.
        return;
    }

    case helix::ui::FilamentTier::Refused:
        // NothingLoaded is plan_unload's only refusal.
        NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
        return;

    case helix::ui::FilamentTier::Macro: {
        std::string macro_name = info.get_macro();
        // See execute_load(): [this] is safe here only because the panel is
        // immortal [L012].
        helix::ui::dispatch_filament_macro(macro_name, helix::ui::ParamPolicy::Prompt,
                                           [this, macro_name](const MacroParamResult& result) {
                                               run_filament_macro(macro_name, "Unload", result);
                                           });
        return;
    }

    case helix::ui::FilamentTier::RawGcode:
        break;
    }

    // Fallback: the shared tier-3 unload sequence (tip-shape then long retract).
    begin_operation_guard();
    std::string gcode = filament_unload_fallback_gcode();
    spdlog::info("[{}] Unload fallback: {}", get_name(), gcode);
    op_started(FilamentOp::Unload); // on-button spinner replaces the start toast

    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                    self->op_succeeded(FilamentOp::Unload);
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->op_failed(FilamentOp::Unload);
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Unload may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Filament unload failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::run_filament_macro(const std::string& macro_name, const std::string& op_label,
                                       const MacroParamResult& params) {
    if (!api_) {
        return;
    }

    begin_operation_guard();
    spdlog::info("[{}] Running '{}' ({})", get_name(), macro_name, op_label);

    // Map the op label ("Load"/"Unload"/"Purg") to the triggering button so the
    // on-button spinner/checkmark drives that button. Unknown labels (none today)
    // get no on-button feedback but still run. Only one filament op runs at a time
    // (operation_guard_), so a single in-flight op member is sufficient; the
    // main-thread success/error bodies read it back.
    std::optional<FilamentOp> op;
    if (op_label == "Load") {
        op = FilamentOp::Load;
    } else if (op_label == "Unload") {
        op = FilamentOp::Unload;
    } else if (op_label == "Purg") {
        op = FilamentOp::Purge;
    }
    op_in_flight_ = op;
    if (op) {
        op_started(*op); // on-button spinner replaces the start toast
    }

    std::string gcode = helix::build_macro_gcode(macro_name, params);
    // FilamentPanel is a global singleton, so `this` capture is safe [L012]
    api_->execute_gcode(
        gcode,
        [this]() {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    self->restore_heater_after_preheat();
                    if (self->op_in_flight_) {
                        self->op_succeeded(*self->op_in_flight_);
                    }
                },
                this);
        },
        [this](const MoonrakerError& error) {
            helix::ui::async_call(
                [](void* ud) {
                    auto* self = static_cast<FilamentPanel*>(ud);
                    self->operation_guard_.end();
                    if (self->op_in_flight_) {
                        self->op_failed(*self->op_in_flight_);
                    }
                },
                this);
            if (error.type == MoonrakerErrorType::TIMEOUT) {
                NOTIFY_WARNING(lv_tr("Macro may still be running — response timed out"));
            } else {
                NOTIFY_ERROR(lv_tr("Macro failed: {}"), error.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void FilamentPanel::show_load_warning() {
    // Close any existing dialog first
    if (load_warning_dialog_) {
        helix::ui::modal_hide(load_warning_dialog_);
        load_warning_dialog_ = nullptr;
    }

    load_warning_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Filament Detected"),
        lv_tr("The toolhead sensor indicates filament is already loaded. "
              "Proceed with load anyway?"),
        ModalSeverity::Warning, lv_tr("Proceed"), on_load_warning_proceed, on_load_warning_cancel,
        this);

    if (!load_warning_dialog_) {
        spdlog::error("[{}] Failed to create load warning dialog", get_name());
        return;
    }

    spdlog::debug("[{}] Load warning dialog shown", get_name());
}

void FilamentPanel::show_unload_warning() {
    // Close any existing dialog first
    if (unload_warning_dialog_) {
        helix::ui::modal_hide(unload_warning_dialog_);
        unload_warning_dialog_ = nullptr;
    }

    unload_warning_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("No Filament Detected"),
        lv_tr("The toolhead sensor indicates no filament is present. "
              "Proceed with unload anyway?"),
        ModalSeverity::Warning, lv_tr("Proceed"), on_unload_warning_proceed,
        on_unload_warning_cancel, this);

    if (!unload_warning_dialog_) {
        spdlog::error("[{}] Failed to create unload warning dialog", get_name());
        return;
    }

    spdlog::debug("[{}] Unload warning dialog shown", get_name());
}

void FilamentPanel::on_load_warning_proceed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_warning_proceed");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self) {
        // Hide dialog first
        if (self->load_warning_dialog_) {
            helix::ui::modal_hide(self->load_warning_dialog_);
            self->load_warning_dialog_ = nullptr;
        }
        // Execute load
        self->execute_load();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_load_warning_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_load_warning_cancel");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self && self->load_warning_dialog_) {
        helix::ui::modal_hide(self->load_warning_dialog_);
        self->load_warning_dialog_ = nullptr;
        spdlog::debug("[FilamentPanel] Load cancelled by user");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_warning_proceed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_warning_proceed");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self) {
        // Hide dialog first
        if (self->unload_warning_dialog_) {
            helix::ui::modal_hide(self->unload_warning_dialog_);
            self->unload_warning_dialog_ = nullptr;
        }
        // Execute unload
        self->execute_unload();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FilamentPanel::on_unload_warning_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FilamentPanel] on_unload_warning_cancel");
    auto* self = static_cast<FilamentPanel*>(lv_event_get_user_data(e));
    if (self && self->unload_warning_dialog_) {
        helix::ui::modal_hide(self->unload_warning_dialog_);
        self->unload_warning_dialog_ = nullptr;
        spdlog::debug("[FilamentPanel] Unload cancelled by user");
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<FilamentPanel> g_filament_panel;

FilamentPanel& get_global_filament_panel() {
    if (!g_filament_panel) {
        g_filament_panel = std::make_unique<FilamentPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("FilamentPanel",
                                                         []() { g_filament_panel.reset(); });
    }
    return *g_filament_panel;
}

void filament_panel_report_unknown_command(const std::string& command) {
    // Reads g_filament_panel directly rather than going through
    // get_global_filament_panel(): the caller is GcodeNarrationRouter on a
    // gcode-response line, and a console message must not be what causes a panel
    // to be constructed.
    if (g_filament_panel) {
        g_filament_panel->fail_op_on_unknown_command(command);
    }
}
