// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h"
#include "ui_observer_guard.h"
#include "ui_temp_graph.h"
#include "ui_temperature_utils.h" // kHeaterStatusBufBytes
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "temp_graph_controller.h"
#include "temperature_controller.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class PrinterState;
class TemperatureController;
class IMoonrakerAPI;
} // namespace helix
class TemperatureService;

// ─────────────────────────────────────────────────────────────────────────────
// Per-heater state (replaces duplicated nozzle_*/bed_* fields)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Encapsulates all per-heater state for one temperature panel.
 *
 * One instance per heater type (nozzle, bed, chamber). Holds config,
 * temperature state, LVGL subjects, graph data, and observer handles.
 */
struct HeaterState {
    heater_config_t config{};

    // Temperature state (decidegrees)
    int current = 25;
    int target = 0;
    int min_temp = 0;
    int max_temp = 0;

    // Status thresholds
    int cooling_threshold_deci = 0; ///< Above this when target=0 → "Cooling down"

    // Chamber-specific: read-only when sensor-only (no heater present)
    bool read_only = false;

    // Klipper object name for set_temperature() API calls
    std::string klipper_name;

    // LVGL subjects for XML data binding
    lv_subject_t display_subject{};
    lv_subject_t status_subject{};       ///< duty text ("" = none; "Monitoring" read-only)
    lv_subject_t status_state_subject{}; ///< HeaterStatusState int (glyph pick)
    lv_subject_t heating_subject{};      ///< 0=off, 1=on (for icon visibility)

    // Subject string buffers
    std::array<char, 32> display_buf{};
    std::array<char, helix::ui::temperature::kHeaterStatusBufBytes> status_buf{};

    int64_t last_graph_update_ms = 0;

    // External graphs registered for this heater's temperature updates
    struct RegisteredGraph {
        ui_temp_graph_t* graph;
        SeriesId series_id;
    };
    std::vector<RegisteredGraph> temp_graphs;

    // Observer handles (RAII cleanup)
    // Lifetimes MUST be declared before observers (destroyed after, so observers
    // can still check alive token during destruction)
    SubjectLifetime temp_lifetime;
    SubjectLifetime target_lifetime;
    ObserverGuard temp_observer;
    ObserverGuard target_observer;

    // Chamber-specific: the M141 cooling-fan target is a second setpoint source.
    // The effective chamber setpoint is heater-or-fan (see chamber_effective_setpoint).
    SubjectLifetime fan_target_lifetime;
    ObserverGuard fan_target_observer;
    // Control-mode word for the chamber status line ("Heating"/"Maintaining"/"Off").
    const char* chamber_mode = "Off";
};

// ─────────────────────────────────────────────────────────────────────────────
// TemperatureService
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Temperature Control Panel - manages nozzle, bed, and chamber temperature UI
 *
 * Handles all heater types through a HeaterState array: per-heater display and
 * status subjects fed from PrinterState, externally registered temperature
 * graphs, and the filament panel's mini combined graph.
 */
class TemperatureService {
  public:
    TemperatureService(helix::PrinterState& printer_state, helix::IMoonrakerAPI* api);
    ~TemperatureService();

    // Non-copyable, non-movable (has reference member and LVGL subject state)
    TemperatureService(const TemperatureService&) = delete;
    TemperatureService& operator=(const TemperatureService&) = delete;
    TemperatureService(TemperatureService&&) = delete;
    TemperatureService& operator=(TemperatureService&&) = delete;

    /// Switch the active extruder. Rebinds heater observers and rebuilds the
    /// mini combined graph against the new extruder. Idempotent — no-op when `name` already matches
    /// the current active extruder. Must be called from the LVGL/UI thread.
    void switch_active_extruder(const std::string& name) {
        select_extruder(name);
    }

    void init_subjects();
    void deinit_subjects();

    // ── Setters (decidegrees, used by tests and PrinterState observers) ──
    void set_heater(helix::HeaterType type, int current, int target);
    void set_heater_limits(helix::HeaterType type, int min_temp, int max_temp);

    // Backward-compat
    void set_nozzle(int current, int target) {
        set_heater(helix::HeaterType::Nozzle, current, target);
    }
    void set_bed(int current, int target) {
        set_heater(helix::HeaterType::Bed, current, target);
    }
    void set_nozzle_limits(int min_temp, int max_temp) {
        set_heater_limits(helix::HeaterType::Nozzle, min_temp, max_temp);
    }
    void set_bed_limits(int min_temp, int max_temp) {
        set_heater_limits(helix::HeaterType::Bed, min_temp, max_temp);
    }

    // Getters (decidegrees)
    int get_nozzle_target() const {
        return heaters_[static_cast<int>(helix::HeaterType::Nozzle)].target;
    }
    int get_bed_target() const {
        return heaters_[static_cast<int>(helix::HeaterType::Bed)].target;
    }
    int get_nozzle_current() const {
        return heaters_[static_cast<int>(helix::HeaterType::Nozzle)].current;
    }
    int get_bed_current() const {
        return heaters_[static_cast<int>(helix::HeaterType::Bed)].current;
    }

    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    void set_controller(helix::TemperatureController* controller) {
        controller_ = controller;
    }
    helix::TemperatureController* controller() {
        return controller_;
    }

    /// Effective ceiling (°C) for this service's custom-temperature keypad:
    /// the shared keypad-ceiling authority when a controller is wired, the
    /// heater's static config range otherwise. TempGraphOverlay's keypad asks
    /// this (#1619).
    float custom_keypad_max(helix::HeaterType type, float fallback_deg) {
        return helix::keypad_ceiling(controller_, type, fallback_deg);
    }

    // ── Mini combined graph (filament panel) ────────────────────────────
    void setup_mini_combined_graph(lv_obj_t* container);

    // ── External graph registration ─────────────────────────────────────
    void register_heater_graph(ui_temp_graph_t* graph, SeriesId series_id,
                               const std::string& heater);
    void unregister_heater_graph(ui_temp_graph_t* graph);

    // ── XML event callbacks (public static for XML registration) ────────
    // Chamber-heater diagnostics card (issue #1290): both delegate to the
    // globally-registered TemperatureController — never the api directly.
    static void on_chamber_fault_reset_clicked(lv_event_t* e);
    static void on_chamber_retry_target_clicked(lv_event_t* e);
    static void on_chamber_filter_fan_clicked(lv_event_t* e);
    static void on_chamber_dryer_start_clicked(lv_event_t* e);
    static void on_chamber_dryer_stop_clicked(lv_event_t* e);

    // ── Access to HeaterState for lazy overlay helper ────────────────────
    HeaterState& heater(helix::HeaterType type) {
        return heaters_[static_cast<int>(type)];
    }

  private:
    friend struct TemperatureServiceTestAccess;

    // ── Generic instance methods ────────────────────────────────────────
    void on_temp_changed(helix::HeaterType type, int temp_deci);
    void on_target_changed(helix::HeaterType type, int target_deci);
    // Chamber: combine the heater-target and cooling-fan-target subjects into a
    // single effective setpoint + control-mode word, then refresh display/status.
    void recompute_chamber_target();
    void update_display(helix::HeaterType type);
    /// The heater's current/target text; init_subjects() and update_display()
    /// both render through it.
    static void format_display(const HeaterState& h, char* buf, size_t size);
    void update_status(helix::HeaterType type);
    void update_graphs(helix::HeaterType type, float temp_deg, int64_t now_ms);

    helix::PrinterState& printer_state_;
    helix::IMoonrakerAPI* api_;
    helix::TemperatureController* controller_ = nullptr;

    // ── Per-heater state (indexed by HeaterType) ────────────────────────
    std::array<HeaterState, helix::kHeaterTypeCount> heaters_;

    // ── Multi-extruder support (nozzle-specific) ────────────────────────
    std::string active_extruder_name_ = "extruder";

    void select_extruder(const std::string& name);

    // ── Mini combined graph (filament panel) ────────────────────────────
    // Container ptr is retained so select_extruder() can recreate the
    // controller against the new active extruder. Owned by the filament
    // panel's XML, and reads nullptr once that tree is gone.
    helix::ui::WidgetRef mini_graph_container_;
    std::unique_ptr<helix::TempGraphController> mini_graph_controller_;

    // ── Graph update throttling ─────────────────────────────────────────
    static constexpr int64_t kGraphSampleIntervalMs = 1000;

    // ── Subject management ──────────────────────────────────────────────
    SubjectManager subjects_;
    bool subjects_initialized_ = false;
};
