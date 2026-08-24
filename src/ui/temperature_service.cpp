// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_service.h"

#include "ui_breakpoint.h"
#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "app_constants.h"
#include "app_globals.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "temp_graph_controller.h"
#include "temperature_controller.h"
#include "temperature_history_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

using namespace helix;
using helix::ui::observe_int_sync;
using helix::ui::temperature::deci_to_degrees_f;

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

namespace {

/// Preset button object name for index i: 0 is "Off", i>=1 is user preset slot
/// i-1. Names are SLOT-indexed ("preset_m0"), not material-indexed
/// ("preset_pla"), so reassigning a slot's material never requires renaming an
/// XML object.
const char* preset_button_name(int i) {
    // Intentionally sized to PRESET_COUNT, not TEMP_PANEL_VISIBLE_PRESETS: the
    // trailing "preset_m3" entry is unused by the temp panels and that is fine.
    // Over-sizing a name table is harmless; under-sizing it is not.
    static const auto names = [] {
        std::array<std::string, 1 + helix::presets::PRESET_COUNT> n;
        n[0] = "preset_off";
        for (int s = 0; s < helix::presets::PRESET_COUNT; ++s) {
            n[s + 1] = "preset_m" + std::to_string(s);
        }
        return n;
    }();
    return names[static_cast<size_t>(i)].c_str();
}

/// Target temperature behind preset button i for a given heater.
int preset_button_value(const helix::HeaterPresets& p, int i) {
    return i == 0 ? p.off : p.material[static_cast<size_t>(i - 1)];
}

} // namespace

// ============================================================================
// Constructor
// ============================================================================

TemperatureService::TemperatureService(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
    // Preset temperatures are derived per user preset slot, not per hardcoded
    // material. TemperatureController owns the derivation (nozzle/bed from the
    // filament database, chamber from its documented enclosure ladder) so the
    // service and the controller can never drift apart — they used to maintain
    // two independent copies of the same three lookups.
    const HeaterPresets nozzle_presets = compute_heater_presets(HeaterType::Nozzle);
    const HeaterPresets bed_presets = compute_heater_presets(HeaterType::Bed);
    const HeaterPresets chamber_presets = compute_heater_presets(HeaterType::Chamber);

    // ── Nozzle config ───────────────────────────────────────────────────
    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];
    nozzle.config = {.type = HeaterType::Nozzle,
                     .name = "Nozzle",
                     .title = "Nozzle Temperature",
                     .color = helix::TEMP_GRAPH_SERIES_COLORS[0], // nozzle
                     .temp_range_max = 320.0f,
                     .y_axis_increment = 80,
                     .presets = nozzle_presets,
                     .keypad_range = {0.0f, 350.0f}};
    nozzle.cooling_threshold_deci = 400; // 40°C
    nozzle.klipper_name = "extruder";    // Updated dynamically for multi-extruder
    nozzle.min_temp = AppConstants::Temperature::DEFAULT_MIN_TEMP;
    nozzle.max_temp = AppConstants::Temperature::DEFAULT_NOZZLE_MAX;

    // ── Bed config ──────────────────────────────────────────────────────
    auto& bed = heaters_[idx(HeaterType::Bed)];
    bed.config = {.type = HeaterType::Bed,
                  .name = "Bed",
                  .title = "Heatbed Temperature",
                  .color = helix::TEMP_GRAPH_SERIES_COLORS[1], // bed
                  .temp_range_max = 140.0f,
                  .y_axis_increment = 35,
                  .presets = bed_presets,
                  .keypad_range = {0.0f, 150.0f}};
    bed.cooling_threshold_deci = 350; // 35°C
    bed.klipper_name = "heater_bed";
    bed.min_temp = AppConstants::Temperature::DEFAULT_MIN_TEMP;
    bed.max_temp = AppConstants::Temperature::DEFAULT_BED_MAX;

    // ── Chamber config ──────────────────────────────────────────────────
    auto& chamber = heaters_[idx(HeaterType::Chamber)];
    chamber.config = {.type = HeaterType::Chamber,
                      .name = "Chamber",
                      .title = "Chamber Temperature",
                      .color = helix::TEMP_GRAPH_SERIES_COLORS[2], // chamber
                      .temp_range_max = 80.0f,
                      .y_axis_increment = 20,
                      .presets = chamber_presets,
                      .keypad_range = {0.0f, 80.0f}};
    chamber.cooling_threshold_deci = 300;            // 30°C
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
    nozzle.temp_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_active_extruder_temp_subject(), this,
        [](TemperatureService* self, int temp) { self->on_temp_changed(HeaterType::Nozzle, temp); },
        printer_state_.get_subjects_lifetime());
    nozzle.target_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_active_extruder_target_subject(), this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Nozzle, target);
        },
        printer_state_.get_subjects_lifetime());
    bed.temp_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_bed_temp_subject(bed.temp_lifetime), this,
        [](TemperatureService* self, int temp) { self->on_temp_changed(HeaterType::Bed, temp); },
        bed.temp_lifetime);
    bed.target_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_bed_target_subject(bed.target_lifetime), this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Bed, target);
        },
        bed.target_lifetime);
    chamber.temp_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_chamber_temp_subject(chamber.temp_lifetime), this,
        [](TemperatureService* self, int temp) {
            self->on_temp_changed(HeaterType::Chamber, temp);
        },
        chamber.temp_lifetime);
    chamber.target_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_chamber_target_subject(chamber.target_lifetime), this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Chamber, target);
        },
        chamber.target_lifetime);
    // M141 cooling mode parks the ≤40°C setpoint on the cooling-fan target while
    // the heater target stays 0. Observe it too so the effective chamber setpoint
    // reflects "Maintaining" sets. recompute_chamber_target() reads BOTH subjects.
    chamber.fan_target_observer = observe_int_sync<TemperatureService>(
        printer_state_.get_chamber_fan_target_subject(chamber.fan_target_lifetime), this,
        [](TemperatureService* self, int /*fan_target*/) { self->recompute_chamber_target(); },
        chamber.fan_target_lifetime);

    // Register XML event callbacks (BEFORE any lv_xml_create calls)
    // Generic callbacks (used by chamber + can be used by nozzle/bed after XML update)
    register_xml_callbacks({
        {"on_heater_preset_clicked", on_heater_preset_clicked},
        {"on_heater_confirm_clicked", on_heater_confirm_clicked},
        {"on_heater_custom_clicked", on_heater_custom_clicked},
    });

    // Legacy callbacks (still needed for existing nozzle/bed XML until they're updated)
    register_xml_callbacks({
        {"on_nozzle_custom_clicked", on_nozzle_custom_clicked},
        {"on_bed_custom_clicked", on_bed_custom_clicked},
    });

    spdlog::debug("[TempPanel] Constructed - subscribed to PrinterState temperature subjects");
}

TemperatureService::~TemperatureService() {
    deinit_subjects();
}

// ============================================================================
// Generic temperature/target change handlers
// ============================================================================

void TemperatureService::on_temp_changed(HeaterType type, int temp_deci) {
    auto& h = heaters_[idx(type)];

    // Filter garbage data at the source
    int max_valid = (type == HeaterType::Nozzle) ? 4000 : (type == HeaterType::Bed) ? 2000 : 1500;
    if (temp_deci <= 0 || temp_deci > max_valid) {
        return;
    }

    h.current = temp_deci;
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

    float temp_deg = deci_to_degrees_f(temp_deci);
    update_graphs(type, temp_deg, now_ms);
}

void TemperatureService::on_target_changed(HeaterType type, int target_deci) {
    // Chamber's effective setpoint is heater-OR-fan (M141 splits the setpoint
    // across two Klipper objects). Route through the combine path rather than
    // taking the heater readback as gospel. Nozzle/Bed keep the direct write.
    if (type == HeaterType::Chamber) {
        recompute_chamber_target();
        return;
    }

    auto& h = heaters_[idx(type)];
    h.target = target_deci;
    update_display(type);
    update_status(type);

    if (!subjects_initialized_) {
        return;
    }

    float target_deg = deci_to_degrees_f(target_deci);

    if (ui_temp_graph_is_valid(h.graph) && h.series_id >= 0) {
        // Always pass show=true: the series HAS a target capability (it's a heater).
        // The buffer's 0-sentinel handles "off period" gaps via the segmenter, so
        // we never want to flip show_target off here — that would erase the whole
        // historical trace.
        ui_temp_graph_set_series_target(h.graph, h.series_id, target_deg, true);
        spdlog::trace("[TempPanel] {} target line: {:.1f}°C", heater_label(type), target_deg);
    }
}

void TemperatureService::recompute_chamber_target() {
    auto& chamber = heaters_[idx(HeaterType::Chamber)];

    // Derive both the effective target and the control mode from the data-layer
    // subjects, which fold in the cooling-fan resting target so M141 S0 reads as
    // Off (effective 0) rather than a deliberate "Maintaining" set at the resting
    // temperature. The graph target line then matches the displayed value.
    chamber.target = lv_subject_get_int(printer_state_.get_chamber_effective_target_subject());
    chamber.chamber_mode =
        helix::ui::temperature::chamber_mode_word(static_cast<helix::ChamberMode>(
            lv_subject_get_int(printer_state_.get_chamber_mode_subject())));

    update_display(HeaterType::Chamber);
    update_status(HeaterType::Chamber);

    if (!subjects_initialized_) {
        return;
    }

    float target_deg = deci_to_degrees_f(chamber.target);
    if (ui_temp_graph_is_valid(chamber.graph) && chamber.series_id >= 0) {
        ui_temp_graph_set_series_target(chamber.graph, chamber.series_id, target_deg, true);
        spdlog::trace("[TempPanel] Chamber target line: {:.1f}°C ({})", target_deg,
                      chamber.chamber_mode);
    }
}

// ============================================================================
// Display + Status updates (generic)
// ============================================================================

void TemperatureService::update_display(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];
    int current_deg = deci_to_degrees_f(h.current);
    int target_deg = deci_to_degrees_f(h.target);
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

void TemperatureService::update_status(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];

    // Re-check read_only for chamber from live capability subject
    if (type == HeaterType::Chamber) {
        auto* cap_subj = printer_state_.get_printer_has_chamber_heater_subject();
        h.read_only = (lv_subject_get_int(cap_subj) == 0);
    }

    // Use heater_display() for consistent status strings and color across all panels
    auto result = helix::ui::temperature::heater_display(h.current, h.target);

    if (h.read_only) {
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", lv_tr("Monitoring"));
    } else if (type == HeaterType::Chamber) {
        // Delegate to the shared helper so the controls panel and the temp-graph
        // overlay always produce identical output (single source of truth).
        auto mode_int = lv_subject_get_int(printer_state_.get_chamber_mode_subject());
        auto status = helix::ui::temperature::chamber_status_text(
            h.current, h.target, static_cast<helix::ChamberMode>(mode_int));
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", status.c_str());
    } else {
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", result.status.c_str());
    }

    lv_subject_copy_string(&h.status_subject, h.status_buf.data());

    int heating_state = (h.target > 0) ? 1 : 0;
    lv_subject_set_int(&h.heating_subject, heating_state);

    spdlog::trace("[TempPanel] {} status: '{}' (heating={})", heater_label(type),
                  h.status_buf.data(), heating_state);
}

// ============================================================================
// Send temperature command (generic)
// ============================================================================

void TemperatureService::send_temperature(HeaterType type, int target) {
    const char* label = heater_label(type);

    // `target` is in degrees at this layer (presets and the keypad both pass
    // degrees). The controller resolves the klipper object name fresh, sends
    // the single set_target, and shows the standard error toast on failure.
    if (!controller_) {
        spdlog::warn("[TempPanel] Cannot set {} temp: no controller", label);
        return;
    }

    spdlog::debug("[TempPanel] Sending {} temperature: {}°C", label, target);
    controller_->set_target(type, static_cast<double>(target), {.toast = true});
}

// ============================================================================
// Configured max_temp clamping (chamber)
// ============================================================================

void TemperatureService::apply_preset_limits(HeaterType type) {
    auto& h = heaters_[idx(type)];
    if (!h.panel) {
        return;
    }
    lv_obj_t* overlay_content = lv_obj_find_by_name(h.panel, "overlay_content");
    if (!overlay_content) {
        return;
    }

    for (int i = 0; i < PRESETS_PER_HEATER; ++i) {
        const int preset_value = preset_button_value(h.config.presets, i);
        lv_obj_t* btn = lv_obj_find_by_name(overlay_content, preset_button_name(i));
        if (!btn) {
            continue;
        }
        // Set-or-clear so this is correct on reconnect to a printer with a
        // different ceiling, not just one-directional hiding. The controller
        // owns the configured-max ceiling (0 = unknown → all presets shown).
        bool visible = controller_ ? controller_->preset_visible(type, preset_value) : true;
        if (visible) {
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
            spdlog::debug("[TempPanel] {} preset {}°C hidden (> configured max)",
                          heater_label(type), preset_value);
        }
    }
}

// ============================================================================
// Graph updates (generic)
// ============================================================================

void TemperatureService::update_graphs(HeaterType type, float temp_deg, int64_t now_ms) {
    auto& h = heaters_[idx(type)];

    for (const auto& reg : h.temp_graphs) {
        if (ui_temp_graph_is_valid(reg.graph) && reg.series_id >= 0) {
            ui_temp_graph_update_series_with_time(reg.graph, reg.series_id, temp_deg, now_ms);
        }
    }
}

void TemperatureService::replay_history_to_graph(HeaterType type) {
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

void TemperatureService::init_subjects() {
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
        int current_deg = deci_to_degrees_f(h.current);
        int target_deg = deci_to_degrees_f(h.target);
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

void TemperatureService::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Set flag BEFORE deinit to prevent deferred callbacks from accessing
    // torn-down subjects during cleanup
    subjects_initialized_ = false;
    // Expire the queued rebuild as well — the flag above stops it dereferencing
    // torn-down subjects, but only while `this` is still alive to be read (#1165).
    async_lifetime_.invalidate();
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

HeaterTempPanelLifecycle* TemperatureService::get_lifecycle(HeaterType type) {
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

void TemperatureService::on_panel_activate(HeaterType type) {
    auto& h = heaters_[idx(type)];
    spdlog::debug("[TempPanel] {} panel activated", heater_label(type));

    update_display(type);
    update_status(type);

    // Keep the chamber limits current: setup_panel may have run before
    // discovery resolved the chamber heater, leaving the ceiling unknown.
    // The controller re-syncs the name + (re-)fetches the configured max if
    // still unknown; re-clamp presets with whatever is known now.
    if (type == HeaterType::Chamber && !h.read_only) {
        if (controller_) {
            controller_->ensure_limits(HeaterType::Chamber);
        }
        apply_preset_limits(type);
    }

    if (h.graph) {
        replay_history_to_graph(type);
    }
}

void TemperatureService::on_panel_deactivate(HeaterType type) {
    auto& h = heaters_[idx(type)];
    spdlog::debug("[TempPanel] {} panel deactivated", heater_label(type));
    h.pending = -1;
}

// ============================================================================
// XML component name mapping
// ============================================================================

const char* TemperatureService::xml_component_name(HeaterType type) const {
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

ui_temp_graph_t* TemperatureService::create_temp_graph(lv_obj_t* chart_area,
                                                       const heater_config_t* config,
                                                       int target_temp, int* series_id_out) {
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
        // Always pass show=true at create time: the series HAS a target capability.
        // History rendering will start empty (no positive samples yet) and become
        // populated as the user sets targets and samples accumulate.
        ui_temp_graph_set_series_target(graph, series_id, static_cast<float>(target_temp), true);
        spdlog::debug("[TempPanel] {} graph created (awaiting live data)", config->name);
    }

    return graph;
}

// ============================================================================
// Generic panel setup
// ============================================================================

void TemperatureService::setup_panel(HeaterType type, lv_obj_t* panel, lv_obj_t* parent_screen) {
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
        // Effective setpoint + mode come from the data-layer subjects, which fold in
        // the cooling-fan resting target (M141 S0 → fan at resting → Off). Seed both
        // h.target and the mode word so the initial display matches live updates.
        h.target = lv_subject_get_int(printer_state_.get_chamber_effective_target_subject());
        h.chamber_mode = helix::ui::temperature::chamber_mode_word(static_cast<helix::ChamberMode>(
            lv_subject_get_int(printer_state_.get_chamber_mode_subject())));

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

    // Wire action button (confirm) via C++ event handler — do NOT use
    // lv_obj_set_user_data() on ui_button components (overwrites UiButtonData*)
    lv_obj_t* overlay_header = lv_obj_find_by_name(panel, "overlay_header");
    if (overlay_header) {
        lv_obj_t* action_button = lv_obj_find_by_name(overlay_header, "action_button");
        if (action_button) {
            lv_obj_add_event_cb(action_button, on_heater_confirm_clicked, LV_EVENT_CLICKED, this);
        }
    }

    // Wire preset buttons via C++ event handlers with per-button PresetButtonData.
    // Each button gets its own lv_obj_add_event_cb so the callback can read
    // the data via lv_event_get_user_data(e) without touching obj user_data.
    int base_idx = idx(type) * PRESETS_PER_HEATER;
    for (int i = 0; i < PRESETS_PER_HEATER; ++i) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay_content, preset_button_name(i));
        if (btn) {
            preset_data_[base_idx + i] = {this, type, preset_button_value(h.config.presets, i)};
            lv_obj_add_event_cb(btn, on_heater_preset_clicked, LV_EVENT_CLICKED,
                                &preset_data_[base_idx + i]);
        }
    }

    // Wire custom button via C++ event handler
    lv_obj_t* btn_custom = lv_obj_find_by_name(overlay_content, "btn_custom");
    if (btn_custom) {
        lv_obj_add_event_cb(btn_custom, on_heater_custom_clicked, LV_EVENT_CLICKED, this);
    }

    // Dynamic spool preset (shown when active material doesn't match standard presets)
    setup_spool_preset(type, overlay_content);

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

    // Chamber: clamp keypad range + presets to the heater's Klipper-configured
    // max_temp (e.g. a 60°C chamber must not offer 80°C). Apply with whatever is
    // known now; the controller fetches the configfile ceiling asynchronously.
    if (type == HeaterType::Chamber && !h.read_only) {
        apply_preset_limits(type);
        if (controller_) {
            controller_->ensure_limits(HeaterType::Chamber);
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

    // Bind heating icon animator. The binder owns its own temperature
    // observers and finds the icon by the conventional glyph name under this
    // heater's own overlay panel root, so it cannot pick up another heater's
    // same-named icon.
    if (h.icon_binder.bind(panel, printer_state_, type)) {
        spdlog::debug("[TempPanel] {} heating icon binder attached", heater_label(type));
    }

    // Nozzle-specific: multi-extruder support
    if (type == HeaterType::Nozzle) {
        if (printer_state_.extruder_count() > 1) {
            rebuild_extruder_segments();
        }

        extruder_version_observer_ = observe_int_sync<TemperatureService>(
            printer_state_.get_extruder_version_subject(), this,
            [](TemperatureService* self, int /*version*/) {
                spdlog::debug("[TempPanel] Extruder list changed, rebuilding selector");
                self->rebuild_extruder_segments();
            },
            printer_state_.get_subjects_lifetime());

        auto& tool_state = helix::ToolState::instance();
        if (tool_state.is_multi_tool()) {
            active_tool_observer_ = observe_int_sync<TemperatureService>(
                tool_state.get_active_tool_subject(), this,
                [](TemperatureService* self, int /*tool_idx*/) {
                    auto& ts = helix::ToolState::instance();
                    const auto* tool = ts.active_tool();
                    if (tool && tool->extruder_name) {
                        self->select_extruder(*tool->extruder_name);
                    }
                },
                tool_state.get_subjects_lifetime());
        }
    }

    spdlog::debug("[TempPanel] {} panel setup complete!", heater_label(type));
}

// ============================================================================
// Setters (backward-compat)
// ============================================================================

void TemperatureService::set_heater(HeaterType type, int current, int target) {
    auto& h = heaters_[idx(type)];
    helix::ui::temperature::validate_and_clamp_pair(
        current, target, helix::ui::temperature::degrees_to_deci(h.min_temp),
        helix::ui::temperature::degrees_to_deci(h.max_temp), heater_label(type));
    h.current = current;
    h.target = target;
    update_display(type);
}

void TemperatureService::set_heater_limits(HeaterType type, int min_temp, int max_temp) {
    auto& h = heaters_[idx(type)];
    h.min_temp = min_temp;
    h.max_temp = max_temp;
    spdlog::debug("[TempPanel] {} limits updated: {}-{}°C", heater_label(type), min_temp, max_temp);
}

// ============================================================================
// XML event callbacks — GENERIC
// ============================================================================

void TemperatureService::on_heater_preset_clicked(lv_event_t* e) {
    auto* data = static_cast<PresetButtonData*>(lv_event_get_user_data(e));
    if (!data || !data->panel)
        return;

    spdlog::debug("[TempPanel] {} preset clicked: setting to {}°C", heater_label(data->heater_type),
                  data->preset_value);
    data->panel->send_temperature(data->heater_type, data->preset_value);
}

void TemperatureService::on_heater_confirm_clicked(lv_event_t* e) {
    auto* self = static_cast<TemperatureService*>(lv_event_get_user_data(e));
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

    // Route through the controller so the klipper object name is resolved fresh
    // (chamber → live discovery name, never the stale default that triggers a
    // key69 HEATER=chamber error). Standard error toast via {.toast=true}; the
    // success toast keeps the off/target distinction the confirm button shows.
    const char* label = heater_label(type);
    if (auto* c = self->controller_) {
        c->set_target(type, static_cast<double>(temp_target),
                      {.toast = true, .on_success = [label, temp_target]() {
                           if (temp_target == 0) {
                               NOTIFY_SUCCESS(lv_tr("{} heater turned off"), label);
                           } else {
                               NOTIFY_SUCCESS(lv_tr("{} target set to {}°C"), label, temp_target);
                           }
                       }});
    }

    NavigationManager::instance().go_back();
}

// Struct for keypad callback
struct KeypadCallbackData {
    TemperatureService* panel;
    helix::HeaterType type;
};

void TemperatureService::keypad_value_cb(float value, void* user_data) {
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

void TemperatureService::on_heater_custom_clicked(lv_event_t* e) {
    auto* self = static_cast<TemperatureService*>(lv_event_get_user_data(e));
    if (!self)
        return;

    // Determine heater type from which panel owns this button
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
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

    // Ensure the chamber's configured ceiling is being fetched before reading
    // the keypad range (no-op once known / non-chamber). The controller owns
    // the effective ceiling (configured max clamped to the heater default).
    float max_value = h.config.keypad_range.max;
    if (self->controller_) {
        self->controller_->ensure_limits(type);
        max_value = self->controller_->keypad_range(type).max;
    }

    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(h.target)),
        .min_value = h.config.keypad_range.min,
        .max_value = max_value,
        .title_label = h.config.title,
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = keypad_value_cb,
        .user_data = &s_keypad_data[idx(type)]};

    ui_keypad_show(&keypad_config);
}

// ============================================================================
// Spool preset (dynamic button for active spool material)
// ============================================================================

void TemperatureService::setup_spool_preset(helix::HeaterType type, lv_obj_t* overlay_content) {
    // Only nozzle and bed get spool presets (chamber doesn't make sense)
    if (type != HeaterType::Nozzle && type != HeaterType::Bed)
        return;

    lv_obj_t* btn = lv_obj_find_by_name(overlay_content, "preset_spool");
    if (!btn)
        return;

    auto active = helix::get_active_material();
    if (!active) {
        spdlog::debug("[TempPanel] {} spool preset: no active material", heater_label(type));
        return;
    }

    // Check if material matches a standard preset (case-insensitive).
    // Include TPU even though temp panels don't have a TPU button — showing a spool
    // preset for a well-known material would be confusing.
    std::string mat_upper = active->material_name;
    std::transform(mat_upper.begin(), mat_upper.end(), mat_upper.begin(), ::toupper);
    if (mat_upper == "PLA" || mat_upper == "PETG" || mat_upper == "ABS" || mat_upper == "TPU") {
        spdlog::debug("[TempPanel] {} spool preset: material '{}' matches standard preset",
                      heater_label(type), active->material_name);
        return;
    }

    // Determine the temperature for this heater type
    int temp = 0;
    if (type == HeaterType::Nozzle) {
        temp = active->material_info.nozzle_recommended();
    } else {
        temp = active->material_info.bed_temp;
    }

    if (temp <= 0) {
        spdlog::debug("[TempPanel] {} spool preset: material '{}' has no temp for this heater",
                      heater_label(type), active->material_name);
        return;
    }

    // Wire preset click handler via C++ event_cb with per-button data.
    // Do NOT use lv_obj_set_user_data — that overwrites UiButtonData* allocated by ui_button.
    auto& data = spool_preset_data_[idx(type)];
    data = {this, type, temp};
    lv_obj_add_event_cb(btn, on_heater_preset_clicked, LV_EVENT_CLICKED, &data);

    // Format button label: "MaterialName (Temp°C)".
    // Using lv_label_set_text directly: text is dynamic (material name + computed temp).
    auto& buf = spool_preset_label_bufs_[idx(type)];
    snprintf(buf.data(), buf.size(), "%s (%d°C)", active->display_name.c_str(), temp);

    lv_obj_t* label = lv_obj_find_by_name(btn, "spool_preset_label");
    if (label) {
        lv_label_set_text(label, buf.data());
    }

    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    spdlog::debug("[TempPanel] {} spool preset: '{}' at {}°C", heater_label(type),
                  active->display_name, temp);
}

void TemperatureService::on_nozzle_custom_clicked(lv_event_t* e) {
    auto* self = static_cast<TemperatureService*>(lv_event_get_user_data(e));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Nozzle)];
    s_keypad_data[idx(HeaterType::Nozzle)] = {self, HeaterType::Nozzle};

    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(h.target)),
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

void TemperatureService::on_bed_custom_clicked(lv_event_t* e) {
    auto* self = static_cast<TemperatureService*>(lv_event_get_user_data(e));
    if (!self)
        return;

    auto& h = self->heaters_[idx(HeaterType::Bed)];
    s_keypad_data[idx(HeaterType::Bed)] = {self, HeaterType::Bed};

    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(h.target)),
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

void TemperatureService::select_extruder(const std::string& name) {
    if (name == active_extruder_name_) {
        return;
    }

    if (!subjects_initialized_) {
        return;
    }

    spdlog::info("[TempPanel] Switching extruder: {} -> {}", active_extruder_name_, name);
    active_extruder_name_ = name;

    // Sync the global active extruder subjects (extruder_temp/extruder_target)
    // so XML-bound elements (temp_display, nozzle_icon) update to the selected tool
    printer_state_.set_active_extruder(name);

    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];

    // Rebind nozzle observers to the selected extruder's subjects
    SubjectLifetime temp_lt, target_lt;
    auto* temp_subj = printer_state_.get_extruder_temp_subject(name, temp_lt);
    auto* target_subj = printer_state_.get_extruder_target_subject(name, target_lt);

    if (temp_subj) {
        nozzle.temp_observer = observe_int_sync<TemperatureService>(
            temp_subj, this,
            [](TemperatureService* self, int temp) {
                self->on_temp_changed(HeaterType::Nozzle, temp);
            },
            temp_lt);
        nozzle.current = lv_subject_get_int(temp_subj);
    }
    if (target_subj) {
        nozzle.target_observer = observe_int_sync<TemperatureService>(
            target_subj, this,
            [](TemperatureService* self, int target) {
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

    // Rebuild the filament panel's mini graph against the new extruder
    // (#9). Without this the controller stays bound to whichever extruder
    // was active at setup_mini_combined_graph() time — typically T0, since
    // the setup runs at panel construction — and the chart shows T0's cold
    // baseline while the user is actively heating T1.
    if (mini_graph_controller_ && mini_graph_container_ && lv_obj_is_valid(mini_graph_container_)) {
        mini_graph_controller_.reset();
        setup_mini_combined_graph(mini_graph_container_);
    }

    rebuild_extruder_segments();
}

void TemperatureService::rebuild_extruder_segments() {
    async_lifetime_.defer("TemperatureService::rebuild_extruder_segments", [this]() {
        if (!subjects_initialized_)
            return;
        rebuild_extruder_segments_impl();
    });
}

void TemperatureService::rebuild_extruder_segments_impl() {
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
    helix::ui::safe_clean_children(selector);

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
    lv_obj_set_style_pad_column(selector, theme_manager_get_spacing("space_sm"), 0);

    auto& tool_state = helix::ToolState::instance();

    for (const auto& ext_name : names) {
        auto ext_it = extruders.find(ext_name);
        if (ext_it == extruders.end())
            continue;
        const auto& info = ext_it->second;
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
                auto* self = static_cast<TemperatureService*>(lv_event_get_user_data(ev));
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

void TemperatureService::replay_history_from_manager(ui_temp_graph_t* graph, int series_id,
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
        float temp = helix::ui::temperature::deci_to_degrees_f(sample.temp_deci);
        ui_temp_graph_update_series_with_time(graph, series_id, temp, sample.timestamp_ms);
        replayed++;
    }

    spdlog::info("[TempPanel] Replayed {} {} samples from history manager", replayed, heater_name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mini Combined Graph (for FilamentPanel)
// ─────────────────────────────────────────────────────────────────────────────

void TemperatureService::setup_mini_combined_graph(lv_obj_t* container) {
    if (!container) {
        spdlog::warn("[TempPanel] setup_mini_combined_graph: null container");
        return;
    }
    // Remember the container so select_extruder() can rebuild the mini
    // graph against the new active extruder. Without this the graph is
    // pinned to whichever extruder was active at first setup.
    mini_graph_container_ = container;

    static constexpr int MINI_GRAPH_POINTS = 300;

    helix::TempGraphControllerConfig config;
    config.point_count = MINI_GRAPH_POINTS;
    config.axis_size = "xs";
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                        TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS |
                        TEMP_GRAPH_FEATURE_GRADIENTS | TEMP_GRAPH_FEATURE_TARGET_HISTORY;
    // At MICRO/TINY the graph is pinned to ~100px — the X-axis time labels eat
    // roughly a fifth of that for minimal value, so drop them to give the data
    // lines more room to breathe.
    if (auto* bp_subj = theme_manager_get_breakpoint_subject()) {
        UiBreakpoint bp = as_breakpoint(lv_subject_get_int(bp_subj));
        if (bp == UiBreakpoint::Micro || bp == UiBreakpoint::Tiny) {
            features &= ~TEMP_GRAPH_FEATURE_X_AXIS;
        }
    }
    config.initial_features = features;
    {
        helix::TempGraphSeriesSpec nozzle_spec;
        nozzle_spec.klipper_name = active_extruder_name_;
        nozzle_spec.color = heaters_[idx(HeaterType::Nozzle)].config.color;
        nozzle_spec.show_target = true;
        // Source the localized "Nozzle [N]" label from PrinterTemperatureState
        // so the mini-graph legend matches the rest of the temp UI instead of
        // showing the raw Klipper "extruder" / "extruderN" identifier.
        const auto& exts = printer_state_.temperature_state().extruders();
        auto it = exts.find(active_extruder_name_);
        if (it != exts.end() && !it->second.display_name.empty())
            nozzle_spec.display_name = it->second.display_name;
        helix::TempGraphSeriesSpec bed_spec;
        bed_spec.klipper_name = "heater_bed";
        bed_spec.display_name = lv_tr("Bed");
        bed_spec.color = heaters_[idx(HeaterType::Bed)].config.color;
        bed_spec.show_target = true;
        config.series = {std::move(nozzle_spec), std::move(bed_spec)};
    }

    // Add chamber series if printer has a chamber heater or sensor
    {
        const auto& chamber = heaters_[idx(HeaterType::Chamber)];
        auto* heater_subj = printer_state_.get_printer_has_chamber_heater_subject();
        bool has_heater = heater_subj && lv_subject_get_int(heater_subj) != 0;

        if (has_heater && !chamber.klipper_name.empty()) {
            helix::TempGraphSeriesSpec spec;
            spec.klipper_name = chamber.klipper_name;
            spec.display_name = lv_tr("Chamber");
            spec.color = chamber.config.color;
            spec.show_target = true;
            config.series.push_back(std::move(spec));
        } else if (printer_state_.get_discovery().has_chamber_sensor()) {
            // Sensor-only: show temp without target line
            helix::TempGraphSeriesSpec spec;
            spec.klipper_name = printer_state_.get_discovery().chamber_sensor_name();
            spec.display_name = lv_tr("Chamber");
            spec.color = chamber.config.color;
            spec.show_target = false;
            config.series.push_back(std::move(spec));
        }
    }

    mini_graph_controller_ =
        std::make_unique<helix::TempGraphController>(container, std::move(config));

    spdlog::debug("[TempPanel] Mini combined graph created with {} point capacity",
                  MINI_GRAPH_POINTS);
}

void TemperatureService::register_heater_graph(ui_temp_graph_t* graph, int series_id,
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

void TemperatureService::unregister_heater_graph(ui_temp_graph_t* graph) {
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
