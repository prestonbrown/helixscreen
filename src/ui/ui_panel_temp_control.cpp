// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_temp_control.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_temp_graph_scaling.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_constants.h"
#include "app_globals.h"
#include "filament_database.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "temperature_history_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>

using namespace helix;
using helix::ui::observe_int_sync;
using helix::ui::temperature::centi_to_degrees_f;

// ============================================================================
// Helper: heater type index
// ============================================================================
static int idx(HeaterType type) {
    return static_cast<int>(type);
}

static const char* heater_label(HeaterType type) {
    switch (type) {
    case HeaterType::Nozzle:
        return "Nozzle";
    case HeaterType::Bed:
        return "Bed";
    case HeaterType::Chamber:
        return "Chamber";
    }
    return "Unknown";
}

// ============================================================================
// Constructor
// ============================================================================

TempControlPanel::TempControlPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
    // Get recommended temperatures from filament database
    auto pla_info = filament::find_material("PLA");
    auto petg_info = filament::find_material("PETG");
    auto abs_info = filament::find_material("ABS");

    // Nozzle presets
    int nozzle_pla = pla_info ? pla_info->nozzle_recommended() : 210;
    int nozzle_petg = petg_info ? petg_info->nozzle_recommended() : 245;
    int nozzle_abs = abs_info ? abs_info->nozzle_recommended() : 255;

    // Bed presets
    int bed_pla = pla_info ? pla_info->bed_temp : 60;
    int bed_petg = petg_info ? petg_info->bed_temp : 80;
    int bed_abs = abs_info ? abs_info->bed_temp : 100;

    // ── Nozzle config ───────────────────────────────────────────────────
    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];
    nozzle.config = {.type = HeaterType::Nozzle,
                     .name = "Nozzle",
                     .title = "Nozzle Temperature",
                     .color = theme_manager_get_color("heating_color"),
                     .temp_range_max = 320.0f,
                     .y_axis_increment = 80,
                     .presets = {0, nozzle_pla, nozzle_petg, nozzle_abs},
                     .keypad_range = {0.0f, 350.0f}};
    nozzle.cooling_threshold_centi = 400; // 40°C
    nozzle.klipper_name = "extruder";     // Updated dynamically for multi-extruder
    nozzle.min_temp = AppConstants::Temperature::DEFAULT_MIN_TEMP;
    nozzle.max_temp = AppConstants::Temperature::DEFAULT_NOZZLE_MAX;

    // ── Bed config ──────────────────────────────────────────────────────
    auto& bed = heaters_[idx(HeaterType::Bed)];
    bed.config = {.type = HeaterType::Bed,
                  .name = "Bed",
                  .title = "Heatbed Temperature",
                  .color = theme_manager_get_color("cooling_color"),
                  .temp_range_max = 140.0f,
                  .y_axis_increment = 35,
                  .presets = {0, bed_pla, bed_petg, bed_abs},
                  .keypad_range = {0.0f, 150.0f}};
    bed.cooling_threshold_centi = 350; // 35°C
    bed.klipper_name = "heater_bed";
    bed.min_temp = AppConstants::Temperature::DEFAULT_MIN_TEMP;
    bed.max_temp = AppConstants::Temperature::DEFAULT_BED_MAX;

    // ── Chamber config ──────────────────────────────────────────────────
    auto& chamber = heaters_[idx(HeaterType::Chamber)];
    chamber.config = {.type = HeaterType::Chamber,
                      .name = "Chamber",
                      .title = "Chamber Temperature",
                      .color = lv_color_hex(0xA3BE8C), // nord14 Aurora green
                      .temp_range_max = 80.0f,
                      .y_axis_increment = 20,
                      .presets = {0, 0, 45, 55}, // Off, PLA=off, PETG=45, ABS=55
                      .keypad_range = {0.0f, 80.0f}};
    chamber.cooling_threshold_centi = 300;           // 30°C
    chamber.klipper_name = "heater_generic chamber"; // Updated from discovery
    chamber.read_only = true; // Default sensor-only; updated at runtime from capability subject
    chamber.min_temp = 0;
    chamber.max_temp = 80;

    // Zero all string buffers
    for (auto& h : heaters_) {
        h.display_buf.fill('\0');
        h.status_buf.fill('\0');
    }

    // Subscribe to temperature subjects with individual ObserverGuards.
    // Nozzle observers are separate so they can be rebound when switching
    // extruders in multi-extruder setups (bed/chamber observers stay constant).
    nozzle.temp_observer = observe_int_sync<TempControlPanel>(
        printer_state_.get_active_extruder_temp_subject(), this,
        [](TempControlPanel* self, int temp) { self->on_temp_changed(HeaterType::Nozzle, temp); });
    nozzle.target_observer =
        observe_int_sync<TempControlPanel>(printer_state_.get_active_extruder_target_subject(),
                                           this, [](TempControlPanel* self, int target) {
                                               self->on_target_changed(HeaterType::Nozzle, target);
                                           });
    bed.temp_observer = observe_int_sync<TempControlPanel>(
        printer_state_.get_bed_temp_subject(), this,
        [](TempControlPanel* self, int temp) { self->on_temp_changed(HeaterType::Bed, temp); });
    bed.target_observer = observe_int_sync<TempControlPanel>(
        printer_state_.get_bed_target_subject(), this, [](TempControlPanel* self, int target) {
            self->on_target_changed(HeaterType::Bed, target);
        });
    chamber.temp_observer = observe_int_sync<TempControlPanel>(
        printer_state_.get_chamber_temp_subject(), this,
        [](TempControlPanel* self, int temp) { self->on_temp_changed(HeaterType::Chamber, temp); });
    chamber.target_observer = observe_int_sync<TempControlPanel>(
        printer_state_.get_chamber_target_subject(), this, [](TempControlPanel* self, int target) {
            self->on_target_changed(HeaterType::Chamber, target);
        });

    // Register XML event callbacks (BEFORE any lv_xml_create calls)
    // Generic callbacks (used by chamber + can be used by nozzle/bed after XML update)
    register_xml_callbacks({
        {"on_heater_preset_clicked", on_heater_preset_clicked},
        {"on_heater_confirm_clicked", on_heater_confirm_clicked},
        {"on_heater_custom_clicked", on_heater_custom_clicked},
    });

    // Legacy callbacks (still needed for existing nozzle/bed XML until they're updated)
    register_xml_callbacks({
        {"on_nozzle_confirm_clicked", on_nozzle_confirm_clicked},
        {"on_nozzle_preset_off_clicked", on_nozzle_preset_off_clicked},
        {"on_nozzle_preset_pla_clicked", on_nozzle_preset_pla_clicked},
        {"on_nozzle_preset_petg_clicked", on_nozzle_preset_petg_clicked},
        {"on_nozzle_preset_abs_clicked", on_nozzle_preset_abs_clicked},
        {"on_nozzle_custom_clicked", on_nozzle_custom_clicked},
        {"on_bed_confirm_clicked", on_bed_confirm_clicked},
        {"on_bed_preset_off_clicked", on_bed_preset_off_clicked},
        {"on_bed_preset_pla_clicked", on_bed_preset_pla_clicked},
        {"on_bed_preset_petg_clicked", on_bed_preset_petg_clicked},
        {"on_bed_preset_abs_clicked", on_bed_preset_abs_clicked},
        {"on_bed_custom_clicked", on_bed_custom_clicked},
    });

    spdlog::debug("[TempPanel] Constructed - subscribed to PrinterState temperature subjects");
}

TempControlPanel::~TempControlPanel() {
    deinit_subjects();
}

// ============================================================================
// Generic temperature/target change handlers
// ============================================================================

void TempControlPanel::on_temp_changed(HeaterType type, int temp_centi) {
    auto& h = heaters_[idx(type)];

    // Filter garbage data at the source
    int max_valid = (type == HeaterType::Nozzle) ? 4000 : (type == HeaterType::Bed) ? 2000 : 1500;
    if (temp_centi <= 0 || temp_centi > max_valid) {
        return;
    }

    h.current = temp_centi;
    update_display(type);
    update_status(type);

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    if (!subjects_initialized_) {
        return;
    }

    // Throttle live graph updates to 1 Hz
    if (now_ms - h.last_graph_update_ms < GRAPH_SAMPLE_INTERVAL_MS) {
        return;
    }
    h.last_graph_update_ms = now_ms;

    float temp_deg = centi_to_degrees_f(temp_centi);
    update_graphs(type, temp_deg, now_ms);

    // Update mini graph Y-axis scaling (only for nozzle/bed)
    if (type == HeaterType::Nozzle || type == HeaterType::Bed) {
        float nozzle_deg = centi_to_degrees_f(heaters_[idx(HeaterType::Nozzle)].current);
        float bed_deg = centi_to_degrees_f(heaters_[idx(HeaterType::Bed)].current);
        update_mini_graph_y_axis(nozzle_deg, bed_deg);
    }
}

void TempControlPanel::on_target_changed(HeaterType type, int target_centi) {
    auto& h = heaters_[idx(type)];
    h.target = target_centi;
    update_display(type);
    update_status(type);

    if (!subjects_initialized_) {
        return;
    }

    float target_deg = centi_to_degrees_f(target_centi);
    bool show_target = (target_centi > 0);

    if (ui_temp_graph_is_valid(h.graph) && h.series_id >= 0) {
        ui_temp_graph_set_series_target(h.graph, h.series_id, target_deg, show_target);
        spdlog::trace("[TempPanel] {} target line: {:.1f}°C (visible={})", heater_label(type),
                      target_deg, show_target);
    }

    // Also update mini combined graph target line (nozzle/bed only)
    if (type == HeaterType::Nozzle && ui_temp_graph_is_valid(mini_graph_) &&
        mini_nozzle_series_id_ >= 0) {
        ui_temp_graph_set_series_target(mini_graph_, mini_nozzle_series_id_, target_deg,
                                        show_target);
    } else if (type == HeaterType::Bed && ui_temp_graph_is_valid(mini_graph_) &&
               mini_bed_series_id_ >= 0) {
        ui_temp_graph_set_series_target(mini_graph_, mini_bed_series_id_, target_deg, show_target);
    }
}

// ============================================================================
// Display + Status updates (generic)
// ============================================================================

void TempControlPanel::update_display(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];
    int current_deg = centi_to_degrees_f(h.current);
    int target_deg = centi_to_degrees_f(h.target);
    int display_target = (h.pending >= 0) ? h.pending : target_deg;

    if (h.pending >= 0) {
        if (h.pending > 0) {
            snprintf(h.display_buf.data(), h.display_buf.size(), "%d / %d*", current_deg,
                     h.pending);
        } else {
            snprintf(h.display_buf.data(), h.display_buf.size(), "%d / —*", current_deg);
        }
    } else if (display_target > 0) {
        snprintf(h.display_buf.data(), h.display_buf.size(), "%d / %d", current_deg,
                 display_target);
    } else {
        snprintf(h.display_buf.data(), h.display_buf.size(), "%d / —", current_deg);
    }
    lv_subject_copy_string(&h.display_subject, h.display_buf.data());
}

void TempControlPanel::update_status(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];
    constexpr int TEMP_TOLERANCE_CENTI = 20; // 2°C

    int target_deg = h.target / 10;

    if (h.read_only) {
        // Sensor-only heaters (e.g., chamber without active heater) just show "Monitoring"
        snprintf(h.status_buf.data(), h.status_buf.size(), "Monitoring");
    } else if (h.target > 0 && h.current < h.target - TEMP_TOLERANCE_CENTI) {
        snprintf(h.status_buf.data(), h.status_buf.size(), lv_tr("Heating to %d°C..."), target_deg);
    } else if (h.target > 0 && h.current >= h.target - TEMP_TOLERANCE_CENTI) {
        snprintf(h.status_buf.data(), h.status_buf.size(), "At target temperature");
    } else if (h.target == 0 && h.current > h.cooling_threshold_centi) {
        snprintf(h.status_buf.data(), h.status_buf.size(), "Cooling down");
    } else {
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", lv_tr("Idle"));
    }

    lv_subject_copy_string(&h.status_subject, h.status_buf.data());

    int heating_state = (h.target > 0) ? 1 : 0;
    lv_subject_set_int(&h.heating_subject, heating_state);

    h.animator.update(h.current, h.target);

    spdlog::trace("[TempPanel] {} status: '{}' (heating={})", heater_label(type),
                  h.status_buf.data(), heating_state);
}

// ============================================================================
// Send temperature command (generic)
// ============================================================================

void TempControlPanel::send_temperature(HeaterType type, int target) {
    auto& h = heaters_[idx(type)];
    const char* label = heater_label(type);

    // For nozzle, use the active extruder name (multi-extruder support)
    const std::string& klipper_name =
        (type == HeaterType::Nozzle) ? active_extruder_name_ : h.klipper_name;

    spdlog::debug("[TempPanel] Sending {} temperature: {}°C to {}", label, target, klipper_name);

    if (!api_) {
        spdlog::warn("[TempPanel] Cannot set {} temp: no API connection", label);
        return;
    }

    api_->set_temperature(
        klipper_name, static_cast<double>(target),
        []() {
            // No toast on success - immediate visual feedback is sufficient
        },
        [label](const MoonrakerError& error) {
            NOTIFY_ERROR("Failed to set {} temp: {}", label, error.user_message());
        });
}

// ============================================================================
// Graph updates (generic)
// ============================================================================

void TempControlPanel::update_graphs(HeaterType type, float temp_deg, int64_t now_ms) {
    auto& h = heaters_[idx(type)];

    for (const auto& reg : h.temp_graphs) {
        if (ui_temp_graph_is_valid(reg.graph) && reg.series_id >= 0) {
            ui_temp_graph_update_series_with_time(reg.graph, reg.series_id, temp_deg, now_ms);
        }
    }
}

void TempControlPanel::replay_history_to_graph(HeaterType type) {
    auto& h = heaters_[idx(type)];
    if (!h.graph || h.series_id < 0) {
        return;
    }

    // For nozzle, use the active extruder name for history lookup
    const std::string& heater_name =
        (type == HeaterType::Nozzle) ? active_extruder_name_ : h.klipper_name;
    replay_history_from_manager(h.graph, h.series_id, heater_name);
}

// ============================================================================
// Subject init/deinit
// ============================================================================

void TempControlPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[TempPanel] init_subjects() called twice - ignoring");
        return;
    }

    // Initialize display + status + heating subjects for each heater
    const char* display_names[] = {"nozzle_temp_display", "bed_temp_display",
                                   "chamber_temp_display"};
    const char* status_names[] = {"nozzle_status", "bed_status", "chamber_status"};
    const char* heating_names[] = {"nozzle_heating", "bed_heating", "chamber_heating"};

    for (int i = 0; i < helix::HEATER_TYPE_COUNT; ++i) {
        auto& h = heaters_[i];

        // Format initial display string
        int current_deg = centi_to_degrees_f(h.current);
        int target_deg = centi_to_degrees_f(h.target);
        snprintf(h.display_buf.data(), h.display_buf.size(), "%d / %d°C", current_deg, target_deg);

        // Initialize subjects
        UI_MANAGED_SUBJECT_STRING_N(h.display_subject, h.display_buf.data(), h.display_buf.size(),
                                    h.display_buf.data(), display_names[i], subjects_);
        UI_MANAGED_SUBJECT_STRING_N(h.status_subject, h.status_buf.data(), h.status_buf.size(),
                                    lv_tr("Idle"), status_names[i], subjects_);
        UI_MANAGED_SUBJECT_INT(h.heating_subject, 0, heating_names[i], subjects_);
    }

    subjects_initialized_ = true;
    spdlog::debug("[TempPanel] Subjects initialized for {} heater types", helix::HEATER_TYPE_COUNT);
}

void TempControlPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Set flag BEFORE deinit to prevent deferred callbacks from accessing
    // torn-down subjects during cleanup
    subjects_initialized_ = false;
    subjects_.deinit_all();
    spdlog::debug("[TempPanel] Subjects deinitialized");
}

// ============================================================================
// Lifecycle hooks
// ============================================================================

void HeaterTempPanelLifecycle::on_activate() {
    if (panel_) {
        panel_->on_panel_activate(type_);
    }
}

void HeaterTempPanelLifecycle::on_deactivate() {
    if (panel_) {
        panel_->on_panel_deactivate(type_);
    }
}

HeaterTempPanelLifecycle* TempControlPanel::get_lifecycle(HeaterType type) {
    switch (type) {
    case HeaterType::Nozzle:
        return &nozzle_lifecycle_;
    case HeaterType::Bed:
        return &bed_lifecycle_;
    case HeaterType::Chamber:
        return &chamber_lifecycle_;
    }
    return nullptr;
}

void TempControlPanel::on_panel_activate(HeaterType type) {
    auto& h = heaters_[idx(type)];
    spdlog::debug("[TempPanel] {} panel activated", heater_label(type));

    update_display(type);
    update_status(type);

    if (h.graph) {
        replay_history_to_graph(type);
    }
}

void TempControlPanel::on_panel_deactivate(HeaterType type) {
    auto& h = heaters_[idx(type)];
    spdlog::debug("[TempPanel] {} panel deactivated", heater_label(type));
    h.pending = -1;
}

// ============================================================================
// XML component name mapping
// ============================================================================

const char* TempControlPanel::xml_component_name(HeaterType type) const {
    switch (type) {
    case HeaterType::Nozzle:
        return "nozzle_temp_panel";
    case HeaterType::Bed:
        return "bed_temp_panel";
    case HeaterType::Chamber:
        return "chamber_temp_panel";
    }
    return nullptr;
}

// ============================================================================
// Graph creation helper
// ============================================================================

ui_temp_graph_t* TempControlPanel::create_temp_graph(lv_obj_t* chart_area,
                                                     const heater_config_t* config, int target_temp,
                                                     int* series_id_out) {
    if (!chart_area)
        return nullptr;

    ui_temp_graph_t* graph = ui_temp_graph_create(chart_area);
    if (!graph)
        return nullptr;

    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
    ui_temp_graph_set_temp_range(graph, 0.0f, config->temp_range_max);

    int series_id = ui_temp_graph_add_series(graph, config->name, config->color);
    if (series_id_out) {
        *series_id_out = series_id;
    }

    if (series_id >= 0) {
        bool show_target = (target_temp > 0);
        ui_temp_graph_set_series_target(graph, series_id, static_cast<float>(target_temp),
                                        show_target);
        spdlog::debug("[TempPanel] {} graph created (awaiting live data)", config->name);
    }

    return graph;
}

// ============================================================================
// Generic panel setup
// ============================================================================

void TempControlPanel::setup_panel(HeaterType type, lv_obj_t* panel, lv_obj_t* parent_screen) {
    auto& h = heaters_[idx(type)];
    h.panel = panel;

    // Read current values from PrinterState subjects
    if (type == HeaterType::Nozzle) {
        h.current = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        h.target = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
    } else if (type == HeaterType::Bed) {
        h.current = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        h.target = lv_subject_get_int(printer_state_.get_bed_target_subject());
    } else if (type == HeaterType::Chamber) {
        h.current = lv_subject_get_int(printer_state_.get_chamber_temp_subject());
        h.target = lv_subject_get_int(printer_state_.get_chamber_target_subject());

        // Update read_only from capability subject
        auto* cap_subj = printer_state_.get_printer_has_chamber_heater_subject();
        h.read_only = (lv_subject_get_int(cap_subj) == 0);

        // Update klipper_name from temperature state
        const auto& heater_name = printer_state_.temperature_state().chamber_heater_name();
        if (!heater_name.empty()) {
            h.klipper_name = heater_name;
        }
    }

    spdlog::debug("[TempPanel] {} initial state: current={}, target={} (read_only={})",
                  heater_label(type), h.current, h.target, h.read_only);

    update_display(type);

    // Standard overlay panel setup
    ui_overlay_panel_setup_standard(panel, parent_screen, "overlay_header", "overlay_content");

    lv_obj_t* overlay_content = lv_obj_find_by_name(panel, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[TempPanel] {}: overlay_content not found!", heater_label(type));
        return;
    }

    // Set user_data on action button (confirm)
    lv_obj_t* overlay_header = lv_obj_find_by_name(panel, "overlay_header");
    if (overlay_header) {
        lv_obj_t* action_button = lv_obj_find_by_name(overlay_header, "action_button");
        if (action_button) {
            lv_obj_set_user_data(action_button, this);
        }
    }

    // Set up preset buttons with PresetButtonData user_data
    const char* preset_names[] = {"preset_off", "preset_pla", "preset_petg", "preset_abs"};
    int preset_values[] = {
        h.config.presets.off,
        h.config.presets.pla,
        h.config.presets.petg,
        h.config.presets.abs,
    };

    int base_idx = idx(type) * PRESETS_PER_HEATER;
    for (int i = 0; i < PRESETS_PER_HEATER; ++i) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay_content, preset_names[i]);
        if (btn) {
            preset_data_[base_idx + i] = {this, type, preset_values[i]};
            lv_obj_set_user_data(btn, &preset_data_[base_idx + i]);
        }
    }

    // Custom button user_data
    lv_obj_t* btn_custom = lv_obj_find_by_name(overlay_content, "btn_custom");
    if (btn_custom) {
        lv_obj_set_user_data(btn_custom, this);
    }

    // Hide presets + custom for read-only chambers (sensor-only, no heater)
    if (h.read_only) {
        lv_obj_t* preset_grid = lv_obj_find_by_name(overlay_content, "preset_grid");
        if (preset_grid) {
            lv_obj_add_flag(preset_grid, LV_OBJ_FLAG_HIDDEN);
        }
        if (btn_custom) {
            lv_obj_add_flag(btn_custom, LV_OBJ_FLAG_HIDDEN);
        }
        if (overlay_header) {
            lv_obj_t* action_button = lv_obj_find_by_name(overlay_header, "action_button");
            if (action_button) {
                lv_obj_add_flag(action_button, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Load theme-aware graph color
    const char* component_name = xml_component_name(type);
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    if (scope) {
        bool use_dark_mode = theme_manager_is_dark_mode();
        const char* color_key = nullptr;
        if (type == HeaterType::Nozzle) {
            color_key = use_dark_mode ? "temp_graph_nozzle_dark" : "temp_graph_nozzle_light";
        } else if (type == HeaterType::Bed) {
            color_key = use_dark_mode ? "temp_graph_bed_dark" : "temp_graph_bed_light";
        } else if (type == HeaterType::Chamber) {
            color_key = use_dark_mode ? "temp_graph_chamber_dark" : "temp_graph_chamber_light";
        }
        if (color_key) {
            const char* color_str = lv_xml_get_const(scope, color_key);
            if (color_str) {
                h.config.color = theme_manager_parse_hex_color(color_str);
                spdlog::debug("[TempPanel] {} graph color: {} ({})", heater_label(type), color_str,
                              use_dark_mode ? "dark" : "light");
            }
        }
    }

    spdlog::debug("[TempPanel] Setting up {} panel...", heater_label(type));

    // Create temperature graph
    lv_obj_t* chart_area = lv_obj_find_by_name(overlay_content, "chart_area");
    if (chart_area) {
        h.graph = create_temp_graph(chart_area, &h.config, h.target, &h.series_id);
        if (h.graph) {
            ui_temp_graph_set_y_axis(h.graph, static_cast<float>(h.config.y_axis_increment), true);
            h.temp_graphs.push_back({h.graph, h.series_id});
            spdlog::debug("[TempPanel] Registered {} graph for temp updates", heater_label(type));
        }
    }

    replay_history_to_graph(type);

    // Attach heating icon animator
    const char* icon_name = nullptr;
    if (type == HeaterType::Nozzle) {
        icon_name = "nozzle_icon_glyph";
    } else if (type == HeaterType::Bed) {
        icon_name = "bed_icon";
    } else if (type == HeaterType::Chamber) {
        icon_name = "chamber_icon";
    }

    if (icon_name) {
        lv_obj_t* heater_icon = lv_obj_find_by_name(panel, icon_name);
        if (heater_icon) {
            h.animator.attach(heater_icon);
            h.animator.update(h.current, h.target);
            spdlog::debug("[TempPanel] {} heating animator attached", heater_label(type));
        }
    }

    // Nozzle-specific: multi-extruder support
    if (type == HeaterType::Nozzle) {
        if (printer_state_.extruder_count() > 1) {
            rebuild_extruder_segments();
        }

        extruder_version_observer_ = observe_int_sync<TempControlPanel>(
            printer_state_.get_extruder_version_subject(), this,
            [](TempControlPanel* self, int /*version*/) {
                spdlog::debug("[TempPanel] Extruder list changed, rebuilding selector");
                self->rebuild_extruder_segments();
            });

        auto& tool_state = helix::ToolState::instance();
        if (tool_state.is_multi_tool()) {
            active_tool_observer_ = observe_int_sync<TempControlPanel>(
                tool_state.get_active_tool_subject(), this,
                [](TempControlPanel* self, int /*tool_idx*/) {
                    auto& ts = helix::ToolState::instance();
                    const auto* tool = ts.active_tool();
                    if (tool && tool->extruder_name) {
                        self->select_extruder(*tool->extruder_name);
                    }
                });
        }
    }

    spdlog::debug("[TempPanel] {} panel setup complete!", heater_label(type));
}

// ============================================================================
// Setters (backward-compat)
// ============================================================================

void TempControlPanel::set_heater(HeaterType type, int current, int target) {
    auto& h = heaters_[idx(type)];
    helix::ui::temperature::validate_and_clamp_pair(current, target, h.min_temp, h.max_temp,
                                                    heater_label(type));
    h.current = current;
    h.target = target;
    update_display(type);
}

void TempControlPanel::set_heater_limits(HeaterType type, int min_temp, int max_temp) {
    auto& h = heaters_[idx(type)];
    h.min_temp = min_temp;
    h.max_temp = max_temp;
    spdlog::debug("[TempPanel] {} limits updated: {}-{}°C", heater_label(type), min_temp, max_temp);
}

// ============================================================================
// XML event callbacks — GENERIC
// ============================================================================

void TempControlPanel::on_heater_preset_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (!data || !data->panel)
        return;

    spdlog::debug("[TempPanel] {} preset clicked: setting to {}°C", heater_label(data->heater_type),
                  data->preset_value);
    data->panel->send_temperature(data->heater_type, data->preset_value);
}

void TempControlPanel::on_heater_confirm_clicked(lv_event_t* e) {
    auto* self = static_cast<TempControlPanel*>(lv_event_get_user_data(e));
    if (!self)
        return;

    // Determine heater type from the panel that owns this button
    // Walk up to find which heater's panel this is
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    HeaterType type = HeaterType::Nozzle; // fallback

    for (int i = 0; i < helix::HEATER_TYPE_COUNT; ++i) {
        auto& h = self->heaters_[i];
        if (h.panel && lv_obj_find_by_name(h.panel, "overlay_header")) {
            lv_obj_t* header = lv_obj_find_by_name(h.panel, "overlay_header");
            lv_obj_t* action_btn = lv_obj_find_by_name(header, "action_button");
            if (action_btn == target) {
                type = static_cast<HeaterType>(i);
                break;
            }
        }
    }

    auto& h = self->heaters_[idx(type)];
    int temp_target = (h.pending >= 0) ? h.pending : h.target;

    spdlog::debug("[TempPanel] {} temperature confirmed: {}°C (pending={})", heater_label(type),
                  temp_target, h.pending);

    h.pending = -1;

    if (self->api_) {
        const std::string& klipper_name =
            (type == HeaterType::Nozzle) ? self->active_extruder_name_ : h.klipper_name;
        const char* label = heater_label(type);

        self->api_->set_temperature(
            klipper_name, static_cast<double>(temp_target),
            [label, temp_target]() {
                if (temp_target == 0) {
                    NOTIFY_SUCCESS("{} heater turned off", label);
                } else {
                    NOTIFY_SUCCESS("{} target set to {}°C", label, temp_target);
                }
            },
            [label](const MoonrakerError& error) {
                NOTIFY_ERROR("Failed to set {} temp: {}", label, error.user_message());
            });
    }

    NavigationManager::instance().go_back();
}

// Struct for keypad callback
struct KeypadCallbackData {
    TempControlPanel* panel;
    helix::HeaterType type;
};

void TempControlPanel::keypad_value_cb(float value, void* user_data) {
    auto* data = static_cast<KeypadCallbackData*>(user_data);
    if (!data || !data->panel)
        return;

    int temp = static_cast<int>(value);
    spdlog::debug("[TempPanel] {} custom temperature: {}°C via keypad", heater_label(data->type),
                  temp);
    data->panel->send_temperature(data->type, temp);
}

// Static storage for keypad callback data (needed because LVGL holds raw pointers)
static KeypadCallbackData s_keypad_data[helix::HEATER_TYPE_COUNT];

void TempControlPanel::on_heater_custom_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (!self)
        return;

    // Determine heater type from which panel owns this button
    HeaterType type = HeaterType::Nozzle; // fallback
    for (int i = 0; i < helix::HEATER_TYPE_COUNT; ++i) {
        auto& h = self->heaters_[i];
        if (h.panel) {
            lv_obj_t* content = lv_obj_find_by_name(h.panel, "overlay_content");
            if (content) {
                lv_obj_t* custom_btn = lv_obj_find_by_name(content, "btn_custom");
                if (custom_btn == btn) {
                    type = static_cast<HeaterType>(i);
                    break;
                }
            }
        }
    }

    auto& h = self->heaters_[idx(type)];
    s_keypad_data[idx(type)] = {self, type};

    ui_keypad_config_t keypad_config = {.initial_value = static_cast<float>(h.target),
                                        .min_value = h.config.keypad_range.min,
                                        .max_value = h.config.keypad_range.max,
                                        .title_label = h.config.title,
                                        .unit_label = "°C",
                                        .allow_decimal = false,
                                        .allow_negative = false,
                                        .callback = keypad_value_cb,
                                        .user_data = &s_keypad_data[idx(type)]};

    ui_keypad_show(&keypad_config);
}

// ============================================================================
// XML event callbacks — LEGACY (delegate to generic)
// ============================================================================

// Legacy nozzle callbacks: still use old user_data pattern (this pointer on button)
void TempControlPanel::on_nozzle_confirm_clicked(lv_event_t* e) {
    auto* self = static_cast<TempControlPanel*>(lv_event_get_user_data(e));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Nozzle)];
    int target = (h.pending >= 0) ? h.pending : h.target;

    spdlog::debug("[TempPanel] Nozzle temperature confirmed: {}°C (pending={})", target, h.pending);
    h.pending = -1;

    if (self->api_) {
        self->api_->set_temperature(
            self->active_extruder_name_, static_cast<double>(target),
            [target]() {
                if (target == 0) {
                    NOTIFY_SUCCESS("Nozzle heater turned off");
                } else {
                    NOTIFY_SUCCESS("Nozzle target set to {}°C", target);
                }
            },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR("Failed to set nozzle temp: {}", error.user_message());
            });
    }

    NavigationManager::instance().go_back();
}

void TempControlPanel::on_bed_confirm_clicked(lv_event_t* e) {
    auto* self = static_cast<TempControlPanel*>(lv_event_get_user_data(e));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Bed)];
    int target = (h.pending >= 0) ? h.pending : h.target;

    spdlog::debug("[TempPanel] Bed temperature confirmed: {}°C (pending={})", target, h.pending);
    h.pending = -1;

    if (self->api_) {
        self->api_->set_temperature(
            "heater_bed", static_cast<double>(target),
            [target]() {
                if (target == 0) {
                    NOTIFY_SUCCESS("Bed heater turned off");
                } else {
                    NOTIFY_SUCCESS("Bed target set to {}°C", target);
                }
            },
            [](const MoonrakerError& error) {
                NOTIFY_ERROR("Failed to set bed temp: {}", error.user_message());
            });
    }

    NavigationManager::instance().go_back();
}

// Legacy preset callbacks: use old user_data pattern (this pointer on button)
void TempControlPanel::on_nozzle_preset_off_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Nozzle, data->preset_value);
        return;
    }
    // Fallback for old user_data pattern
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Nozzle,
                               self->heaters_[idx(HeaterType::Nozzle)].config.presets.off);
}

void TempControlPanel::on_nozzle_preset_pla_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Nozzle, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Nozzle,
                               self->heaters_[idx(HeaterType::Nozzle)].config.presets.pla);
}

void TempControlPanel::on_nozzle_preset_petg_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Nozzle, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Nozzle,
                               self->heaters_[idx(HeaterType::Nozzle)].config.presets.petg);
}

void TempControlPanel::on_nozzle_preset_abs_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Nozzle, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Nozzle,
                               self->heaters_[idx(HeaterType::Nozzle)].config.presets.abs);
}

void TempControlPanel::on_bed_preset_off_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Bed, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Bed,
                               self->heaters_[idx(HeaterType::Bed)].config.presets.off);
}

void TempControlPanel::on_bed_preset_pla_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Bed, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Bed,
                               self->heaters_[idx(HeaterType::Bed)].config.presets.pla);
}

void TempControlPanel::on_bed_preset_petg_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Bed, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Bed,
                               self->heaters_[idx(HeaterType::Bed)].config.presets.petg);
}

void TempControlPanel::on_bed_preset_abs_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* data = static_cast<PresetButtonData*>(lv_obj_get_user_data(btn));
    if (data && data->panel) {
        data->panel->send_temperature(HeaterType::Bed, data->preset_value);
        return;
    }
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (self)
        self->send_temperature(HeaterType::Bed,
                               self->heaters_[idx(HeaterType::Bed)].config.presets.abs);
}

void TempControlPanel::on_nozzle_custom_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Nozzle)];
    s_keypad_data[idx(HeaterType::Nozzle)] = {self, HeaterType::Nozzle};

    ui_keypad_config_t keypad_config = {.initial_value = static_cast<float>(h.target),
                                        .min_value = h.config.keypad_range.min,
                                        .max_value = h.config.keypad_range.max,
                                        .title_label = "Nozzle Temp",
                                        .unit_label = "°C",
                                        .allow_decimal = false,
                                        .allow_negative = false,
                                        .callback = keypad_value_cb,
                                        .user_data = &s_keypad_data[idx(HeaterType::Nozzle)]};

    ui_keypad_show(&keypad_config);
}

void TempControlPanel::on_bed_custom_clicked(lv_event_t* e) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<TempControlPanel*>(lv_obj_get_user_data(btn));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Bed)];
    s_keypad_data[idx(HeaterType::Bed)] = {self, HeaterType::Bed};

    ui_keypad_config_t keypad_config = {.initial_value = static_cast<float>(h.target),
                                        .min_value = h.config.keypad_range.min,
                                        .max_value = h.config.keypad_range.max,
                                        .title_label = "Heat Bed Temp",
                                        .unit_label = "°C",
                                        .allow_decimal = false,
                                        .allow_negative = false,
                                        .callback = keypad_value_cb,
                                        .user_data = &s_keypad_data[idx(HeaterType::Bed)]};

    ui_keypad_show(&keypad_config);
}

// ============================================================================
// MULTI-EXTRUDER SUPPORT
// ============================================================================

void TempControlPanel::select_extruder(const std::string& name) {
    if (name == active_extruder_name_) {
        return;
    }

    if (!subjects_initialized_) {
        return;
    }

    spdlog::info("[TempPanel] Switching extruder: {} -> {}", active_extruder_name_, name);
    active_extruder_name_ = name;

    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];

    // Rebind nozzle observers to the selected extruder's subjects
    SubjectLifetime temp_lt, target_lt;
    auto* temp_subj = printer_state_.get_extruder_temp_subject(name, temp_lt);
    auto* target_subj = printer_state_.get_extruder_target_subject(name, target_lt);

    if (temp_subj) {
        nozzle.temp_observer = observe_int_sync<TempControlPanel>(
            temp_subj, this,
            [](TempControlPanel* self, int temp) {
                self->on_temp_changed(HeaterType::Nozzle, temp);
            },
            temp_lt);
        nozzle.current = lv_subject_get_int(temp_subj);
    }
    if (target_subj) {
        nozzle.target_observer = observe_int_sync<TempControlPanel>(
            target_subj, this,
            [](TempControlPanel* self, int target) {
                self->on_target_changed(HeaterType::Nozzle, target);
            },
            target_lt);
        nozzle.target = lv_subject_get_int(target_subj);
    }

    nozzle.pending = -1;
    update_display(HeaterType::Nozzle);
    update_status(HeaterType::Nozzle);

    // Replay graph history for the newly selected extruder
    if (nozzle.graph && nozzle.series_id >= 0) {
        ui_temp_graph_clear_series(nozzle.graph, nozzle.series_id);
        replay_history_to_graph(HeaterType::Nozzle);
    }

    rebuild_extruder_segments();
}

void TempControlPanel::rebuild_extruder_segments() {
    helix::ui::queue_update([this]() { rebuild_extruder_segments_impl(); });
}

void TempControlPanel::rebuild_extruder_segments_impl() {
    if (!subjects_initialized_) {
        return;
    }

    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];
    auto* selector = lv_obj_find_by_name(nozzle.panel, "extruder_selector");
    if (!selector) {
        return;
    }

    int count = printer_state_.extruder_count();
    if (count <= 1) {
        lv_obj_add_flag(selector, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(selector, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(selector);

    // Build sorted extruder list for deterministic button order
    const auto& extruders = printer_state_.temperature_state().extruders();
    std::vector<std::string> names;
    names.reserve(extruders.size());
    for (const auto& [ext_name, info] : extruders) {
        names.push_back(ext_name);
    }
    std::sort(names.begin(), names.end());

    // Reset active extruder if it no longer exists
    if (extruders.find(active_extruder_name_) == extruders.end() && !names.empty()) {
        select_extruder(names.front());
        return;
    }

    lv_obj_set_flex_flow(selector, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(selector, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(selector, 8, 0);

    auto& tool_state = helix::ToolState::instance();

    for (const auto& ext_name : names) {
        const auto& info = extruders.at(ext_name);
        lv_obj_t* btn = lv_button_create(selector);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);

        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        if (ext_name == active_extruder_name_) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btn, LV_STATE_CHECKED);
        }

        std::string tool_name = tool_state.tool_name_for_extruder(ext_name);
        const std::string& btn_label = tool_name.empty() ? info.display_name : tool_name;

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, btn_label.c_str());
        lv_obj_center(label);
        lv_obj_set_user_data(btn, this);

        // Dynamically created buttons use C++ event callbacks (exception to
        // the "no lv_obj_add_event_cb" rule -- same pattern as FanDial)
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* ev) {
                auto* self = static_cast<TempControlPanel*>(lv_event_get_user_data(ev));
                if (!self)
                    return;
                lv_obj_t* clicked_btn = static_cast<lv_obj_t*>(lv_event_get_target(ev));
                lv_obj_t* lbl = lv_obj_get_child(clicked_btn, 0);
                if (!lbl)
                    return;
                const char* display_text = lv_label_get_text(lbl);

                const auto& exts = self->printer_state_.temperature_state().extruders();
                for (const auto& [kname, kinfo] : exts) {
                    if (kinfo.display_name == display_text) {
                        self->select_extruder(kname);
                        return;
                    }
                }
                auto& ts = helix::ToolState::instance();
                for (const auto& [kname, kinfo] : exts) {
                    if (ts.tool_name_for_extruder(kname) == display_text) {
                        self->select_extruder(kname);
                        return;
                    }
                }
                spdlog::warn("[TempPanel] Could not find extruder for label '{}'", display_text);
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[TempPanel] Rebuilt extruder selector with {} buttons", names.size());
}

// ============================================================================
// Graph history replay helpers
// ============================================================================

void TempControlPanel::replay_history_from_manager(ui_temp_graph_t* graph, int series_id,
                                                   const std::string& heater_name) {
    auto* mgr = get_temperature_history_manager();
    if (mgr == nullptr || graph == nullptr || series_id < 0) {
        return;
    }

    auto samples = mgr->get_samples(heater_name);
    if (samples.empty()) {
        spdlog::debug("[TempPanel] No history samples from manager for {}", heater_name);
        return;
    }

    int replayed = 0;
    for (const auto& sample : samples) {
        float temp = static_cast<float>(sample.temp_centi) / 10.0f;
        ui_temp_graph_update_series_with_time(graph, series_id, temp, sample.timestamp_ms);
        replayed++;
    }

    spdlog::info("[TempPanel] Replayed {} {} samples from history manager", replayed, heater_name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mini Combined Graph (for FilamentPanel)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int MINI_GRAPH_POINTS = 300; // 5-minute window at 1Hz

void TempControlPanel::setup_mini_combined_graph(lv_obj_t* container) {
    if (!container) {
        spdlog::warn("[TempPanel] setup_mini_combined_graph: null container");
        return;
    }

    mini_graph_ = ui_temp_graph_create(container);
    if (!mini_graph_) {
        spdlog::error("[TempPanel] Failed to create mini combined graph");
        return;
    }

    lv_obj_t* chart = ui_temp_graph_get_chart(mini_graph_);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
    ui_temp_graph_set_temp_range(mini_graph_, 0.0f, 150.0f);
    ui_temp_graph_set_point_count(mini_graph_, MINI_GRAPH_POINTS);
    ui_temp_graph_set_y_axis(mini_graph_, 50.0f, true);
    ui_temp_graph_set_axis_size(mini_graph_, "xs");

    auto& bed = heaters_[idx(HeaterType::Bed)];
    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];

    // Add bed series FIRST (renders underneath)
    mini_bed_series_id_ = ui_temp_graph_add_series(mini_graph_, "Bed", bed.config.color);
    if (mini_bed_series_id_ >= 0) {
        ui_temp_graph_set_series_gradient(mini_graph_, mini_bed_series_id_, LV_OPA_0, LV_OPA_10);
        bed.temp_graphs.push_back({mini_graph_, mini_bed_series_id_});
    }

    // Add nozzle series SECOND (renders on top)
    mini_nozzle_series_id_ = ui_temp_graph_add_series(mini_graph_, "Nozzle", nozzle.config.color);
    if (mini_nozzle_series_id_ >= 0) {
        ui_temp_graph_set_series_gradient(mini_graph_, mini_nozzle_series_id_, LV_OPA_0, LV_OPA_20);
        nozzle.temp_graphs.push_back({mini_graph_, mini_nozzle_series_id_});
    }

    replay_history_to_mini_graph();

    if (mini_nozzle_series_id_ >= 0 && nozzle.target > 0) {
        float target_deg = static_cast<float>(nozzle.target) / 10.0f;
        ui_temp_graph_set_series_target(mini_graph_, mini_nozzle_series_id_, target_deg, true);
    }
    if (mini_bed_series_id_ >= 0 && bed.target > 0) {
        float target_deg = static_cast<float>(bed.target) / 10.0f;
        ui_temp_graph_set_series_target(mini_graph_, mini_bed_series_id_, target_deg, true);
    }

    spdlog::debug("[TempPanel] Mini combined graph created with {} point capacity",
                  MINI_GRAPH_POINTS);
}

void TempControlPanel::register_heater_graph(ui_temp_graph_t* graph, int series_id,
                                             const std::string& heater) {
    if (heater.rfind("extruder", 0) == 0) {
        heaters_[idx(HeaterType::Nozzle)].temp_graphs.push_back({graph, series_id});
    } else if (heater == "heater_bed") {
        heaters_[idx(HeaterType::Bed)].temp_graphs.push_back({graph, series_id});
    } else if (heater.find("chamber") != std::string::npos) {
        heaters_[idx(HeaterType::Chamber)].temp_graphs.push_back({graph, series_id});
    }
    spdlog::debug("[TempPanel] Registered external graph for {}", heater);
}

void TempControlPanel::unregister_heater_graph(ui_temp_graph_t* graph) {
    auto remove_from = [graph](std::vector<HeaterState::RegisteredGraph>& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [graph](const HeaterState::RegisteredGraph& rg) {
                                     return rg.graph == graph;
                                 }),
                  vec.end());
    };
    for (auto& h : heaters_) {
        remove_from(h.temp_graphs);
    }
    spdlog::debug("[TempPanel] Unregistered external graph");
}

void TempControlPanel::update_mini_graph_y_axis(float nozzle_deg, float bed_deg) {
    if (!mini_graph_) {
        return;
    }

    float new_y_max = calculate_mini_graph_y_max(mini_graph_y_max_, nozzle_deg, bed_deg);

    if (new_y_max != mini_graph_y_max_) {
        spdlog::debug("[TempPanel] Mini graph Y-axis {} to {}°C",
                      new_y_max > mini_graph_y_max_ ? "expanded" : "shrunk", new_y_max);
        mini_graph_y_max_ = new_y_max;
        ui_temp_graph_set_temp_range(mini_graph_, 0.0f, mini_graph_y_max_);
    }
}

void TempControlPanel::replay_history_to_mini_graph() {
    if (!mini_graph_) {
        return;
    }

    auto* mgr = get_temperature_history_manager();
    if (mgr == nullptr) {
        spdlog::debug("[TempPanel] Mini graph: no history manager available");
        return;
    }

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    int64_t cutoff_ms = now_ms - (MINI_GRAPH_POINTS * 1000);

    auto replay_heater = [&](const std::string& heater_name, int series_id) {
        if (series_id < 0)
            return;

        auto samples = mgr->get_samples_since(heater_name, cutoff_ms);
        if (samples.empty())
            return;

        int64_t last_graphed_time = 0;
        int replayed = 0;

        for (const auto& sample : samples) {
            if (last_graphed_time > 0 &&
                (sample.timestamp_ms - last_graphed_time) < GRAPH_SAMPLE_INTERVAL_MS) {
                continue;
            }
            float temp_deg = centi_to_degrees_f(sample.temp_centi);
            ui_temp_graph_update_series_with_time(mini_graph_, series_id, temp_deg,
                                                  sample.timestamp_ms);
            last_graphed_time = sample.timestamp_ms;
            replayed++;
        }

        if (replayed > 0) {
            spdlog::debug("[TempPanel] Mini graph: replayed {} {} samples", replayed, heater_name);
        }
    };

    replay_heater(active_extruder_name_, mini_nozzle_series_id_);
    replay_heater("heater_bed", mini_bed_series_id_);
}
