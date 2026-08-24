// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h"
#include "ui_heater_icon_binder.h"
#include "ui_observer_guard.h"
#include "ui_temp_graph.h"

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "subject_managed_panel.h"
#include "temp_graph_controller.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class PrinterState;
class TemperatureController;
} // namespace helix
class IMoonrakerAPI;
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
    int pending = -1; // -1 = no pending selection (user picked but not confirmed)
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
    lv_subject_t status_subject{};
    lv_subject_t heating_subject{}; ///< 0=off, 1=on (for icon visibility)

    // Subject string buffers
    std::array<char, 32> display_buf{};
    std::array<char, 64> status_buf{};

    // Panel widget (the overlay lv_obj)
    lv_obj_t* panel = nullptr;

    // Heating icon binder (gradient color + pulse while heating). Bound from
    // this heater's own overlay panel root, so it cannot pick up another
    // heater's same-named icon. Owns its own temperature observers.
    helix::ui::HeaterIconBinder icon_binder;

    // Graph widget
    ui_temp_graph_t* graph = nullptr;
    int series_id = -1;
    int64_t last_graph_update_ms = 0;

    // External graphs registered for this heater's temperature updates
    struct RegisteredGraph {
        ui_temp_graph_t* graph;
        int series_id;
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
// Generic lifecycle wrapper (replaces NozzleTempPanelLifecycle + BedTempPanelLifecycle)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Generic lifecycle wrapper for heater temperature panels
 *
 * Thin wrapper that implements IPanelLifecycle and delegates to TemperatureService
 * for the specified heater type. One instance per heater type.
 */
class HeaterTempPanelLifecycle : public IPanelLifecycle {
  public:
    HeaterTempPanelLifecycle(TemperatureService* panel, helix::HeaterType type, const char* name)
        : panel_(panel), type_(type), name_(name) {}

    const char* get_name() const override {
        return name_;
    }
    void on_activate() override;
    void on_deactivate() override;

    helix::HeaterType type() const {
        return type_;
    }

  private:
    TemperatureService* panel_;
    helix::HeaterType type_;
    const char* name_;
};

// Keep backward-compat type aliases for existing code
using NozzleTempPanelLifecycle = HeaterTempPanelLifecycle;
using BedTempPanelLifecycle = HeaterTempPanelLifecycle;

// ─────────────────────────────────────────────────────────────────────────────
// Preset button user_data (for generic preset callback)
// ─────────────────────────────────────────────────────────────────────────────

struct PresetButtonData {
    TemperatureService* panel;
    helix::HeaterType heater_type;
    int preset_value; ///< Target temperature in degrees (0 = off)
};

// ─────────────────────────────────────────────────────────────────────────────
// TemperatureService
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Temperature Control Panel - manages nozzle, bed, and chamber temperature UI
 *
 * Unified panel that handles all heater types through a HeaterState array.
 * Each heater has its own overlay panel, graph, presets, and lifecycle.
 */
class TemperatureService {
  public:
    TemperatureService(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~TemperatureService();

    // Non-copyable, non-movable (has reference member and LVGL subject state)
    TemperatureService(const TemperatureService&) = delete;
    TemperatureService& operator=(const TemperatureService&) = delete;
    TemperatureService(TemperatureService&&) = delete;
    TemperatureService& operator=(TemperatureService&&) = delete;

    // ── Generic heater API ──────────────────────────────────────────────
    void setup_panel(helix::HeaterType type, lv_obj_t* panel, lv_obj_t* parent_screen);
    void on_panel_activate(helix::HeaterType type);
    void on_panel_deactivate(helix::HeaterType type);
    HeaterTempPanelLifecycle* get_lifecycle(helix::HeaterType type);

    // ── Backward-compat wrappers ────────────────────────────────────────
    void setup_nozzle_panel(lv_obj_t* panel, lv_obj_t* parent_screen) {
        setup_panel(helix::HeaterType::Nozzle, panel, parent_screen);
    }
    void setup_bed_panel(lv_obj_t* panel, lv_obj_t* parent_screen) {
        setup_panel(helix::HeaterType::Bed, panel, parent_screen);
    }
    void setup_chamber_panel(lv_obj_t* panel, lv_obj_t* parent_screen) {
        setup_panel(helix::HeaterType::Chamber, panel, parent_screen);
    }
    NozzleTempPanelLifecycle* get_nozzle_lifecycle() {
        return get_lifecycle(helix::HeaterType::Nozzle);
    }
    BedTempPanelLifecycle* get_bed_lifecycle() {
        return get_lifecycle(helix::HeaterType::Bed);
    }
    HeaterTempPanelLifecycle* get_chamber_lifecycle() {
        return get_lifecycle(helix::HeaterType::Chamber);
    }

    /// Switch the active extruder. Rebinds heater observers, replays graph
    /// history, and rebuilds the mini combined graph against the new
    /// extruder. Idempotent — no-op when `name` already matches the
    /// current active extruder. Must be called from the LVGL/UI thread.
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

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    void set_controller(helix::TemperatureController* controller) {
        controller_ = controller;
    }
    helix::TemperatureController* controller() {
        return controller_;
    }

    // ── Mini combined graph (filament panel) ────────────────────────────
    void setup_mini_combined_graph(lv_obj_t* container);

    // ── External graph registration ─────────────────────────────────────
    void register_heater_graph(ui_temp_graph_t* graph, int series_id, const std::string& heater);
    void unregister_heater_graph(ui_temp_graph_t* graph);

    // ── XML event callbacks (public static for XML registration) ────────
    static void on_heater_preset_clicked(lv_event_t* e);
    static void on_heater_confirm_clicked(lv_event_t* e);
    static void on_heater_custom_clicked(lv_event_t* e);

    // The eight per-material preset callbacks (on_nozzle_preset_pla_clicked and
    // friends) are gone: they were byte-identical bodies that all forwarded to
    // send_temperature(type, data->preset_value). All three temp panels now use
    // the single index-parameterized on_heater_preset_clicked, with the slot's
    // temperature carried in the button's PresetButtonData.
    static void on_nozzle_custom_clicked(lv_event_t* e);
    static void on_bed_custom_clicked(lv_event_t* e);

    // ── Access to HeaterState for lazy overlay helper ────────────────────
    HeaterState& heater(helix::HeaterType type) {
        return heaters_[static_cast<int>(type)];
    }
    const char* xml_component_name(helix::HeaterType type) const;

  private:
    // ── Generic instance methods ────────────────────────────────────────
    void on_temp_changed(helix::HeaterType type, int temp_deci);
    void on_target_changed(helix::HeaterType type, int target_deci);
    // Chamber: combine the heater-target and cooling-fan-target subjects into a
    // single effective setpoint + control-mode word, then refresh display/status.
    void recompute_chamber_target();
    void update_display(helix::HeaterType type);
    void update_status(helix::HeaterType type);
    void send_temperature(helix::HeaterType type, int target);
    void update_graphs(helix::HeaterType type, float temp_deg, int64_t now_ms);
    void replay_history_to_graph(helix::HeaterType type);

    // Show/hide preset buttons based on the heater's configured max_temp so a
    // preset above the chamber's ceiling is never offered. Safe no-op when the
    // panel isn't built or max_temp is unknown (0). Main thread only.
    void apply_preset_limits(helix::HeaterType type);

    // ── Graph helpers ───────────────────────────────────────────────────
    ui_temp_graph_t* create_temp_graph(lv_obj_t* chart_area, const heater_config_t* config,
                                       int target_temp, int* series_id_out);
    void replay_history_from_manager(ui_temp_graph_t* graph, int series_id,
                                     const std::string& heater_name);

    // Keypad callback
    static void keypad_value_cb(float value, void* user_data);

    helix::PrinterState& printer_state_;
    IMoonrakerAPI* api_;
    helix::TemperatureController* controller_ = nullptr;

    // ── Per-heater state (indexed by HeaterType) ────────────────────────
    std::array<HeaterState, helix::HEATER_TYPE_COUNT> heaters_;

    // ── Multi-extruder support (nozzle-specific) ────────────────────────
    std::string active_extruder_name_ = "extruder";
    ObserverGuard extruder_version_observer_;
    ObserverGuard active_tool_observer_;

    void select_extruder(const std::string& name);
    void rebuild_extruder_segments();
    void rebuild_extruder_segments_impl();

    // ── Mini combined graph (filament panel) ────────────────────────────
    // Container ptr is retained so select_extruder() can recreate the
    // controller against the new active extruder. Owned by the filament
    // panel's XML — its lifetime exceeds ours under normal teardown, but
    // we guard with lv_obj_is_valid() before reuse just in case.
    lv_obj_t* mini_graph_container_ = nullptr;
    std::unique_ptr<helix::TempGraphController> mini_graph_controller_;

    // ── Graph update throttling ─────────────────────────────────────────
    static constexpr int64_t GRAPH_SAMPLE_INTERVAL_MS = 1000;

    // ── Subject management ──────────────────────────────────────────────
    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    /// Expires the deferred segment rebuild. Declared after `subjects_` so
    /// reverse-order member destruction invalidates it before the subjects it
    /// protects; also invalidated by deinit_subjects(). The in-lambda
    /// `subjects_initialized_` test is not a substitute — reading that flag is
    /// itself a member access on a possibly-freed `this` (#1165, #1146).
    helix::AsyncLifetimeGuard async_lifetime_;

    // ── Lifecycle wrappers (owned by this object) ───────────────────────
    HeaterTempPanelLifecycle nozzle_lifecycle_{this, helix::HeaterType::Nozzle,
                                               "Nozzle Temperature"};
    HeaterTempPanelLifecycle bed_lifecycle_{this, helix::HeaterType::Bed, "Bed Temperature"};
    HeaterTempPanelLifecycle chamber_lifecycle_{this, helix::HeaterType::Chamber,
                                                "Chamber Temperature"};

    // ── Spool preset helpers ────────────────────────────────────────────
    void setup_spool_preset(helix::HeaterType type, lv_obj_t* overlay_content);

    // ── Static preset button data (LVGL holds raw pointers) ─────────────

    /// Preset material slots the nozzle/bed/chamber temp panels have room to
    /// DISPLAY. Deliberately 3, not helix::presets::PRESET_COUNT (4).
    ///
    /// This is a LAYOUT CONSTRAINT, NOT AN OVERSIGHT. Those panels render their
    /// presets as width="48%" buttons in a row_wrap grid, so "Off" + 3 slots
    /// fills exactly two rows. A 4th slot pushes the grid to a third row, which
    /// does not fit in right_column at the smaller breakpoints — it eats the
    /// flex spacer above it and overflows.
    ///
    /// Slot 3 is NOT disabled. It stays fully live in the filament panel, in the
    /// PID calibration panel, and in the underlying preset data —
    /// TemperatureController::compute_heater_presets() still computes a target
    /// for all PRESET_COUNT slots. This is display truncation in these three
    /// panels only.
    ///
    /// The visible count differs per screen ON PURPOSE, because each screen has
    /// a different amount of room:
    ///   - temp graph overlay -> 3 slots (TEMP_GRAPH_VISIBLE_PRESETS)
    ///   - nozzle/bed/chamber -> 3 slots (here)
    ///   - filament panel     -> 4 slots
    ///   - PID panel          -> 4 slots
    /// If you want a 4th slot visible here, FIND THE SPACE FIRST. Do not "fix"
    /// the inconsistency by trimming the filament or PID panels to match.
    ///
    /// Matching prose lives in ui_xml/{nozzle,bed,chamber}_temp_panel.xml at the
    /// spot the 4th button would occupy; this constant is what makes the
    /// invariant compiler-enforced rather than comment-enforced.
    static constexpr int TEMP_PANEL_VISIBLE_PRESETS = 3;
    static_assert(TEMP_PANEL_VISIBLE_PRESETS <= helix::presets::PRESET_COUNT,
                  "cannot surface more preset slots than exist");

    /// "Off" + the visible material presets. This is the number of preset
    /// buttons that actually EXIST in the temp panel XML, which is what every
    /// use of this constant means: the preset_data_ sizing, the per-heater
    /// base_idx stride, and both preset loops in temperature_service.cpp.
    static constexpr int PRESETS_PER_HEATER = 1 + TEMP_PANEL_VISIBLE_PRESETS;
    std::array<PresetButtonData, helix::HEATER_TYPE_COUNT * PRESETS_PER_HEATER> preset_data_{};

    // Spool preset data (one per heater type: nozzle, bed)
    std::array<PresetButtonData, helix::HEATER_TYPE_COUNT> spool_preset_data_{};
    std::array<std::array<char, 48>, helix::HEATER_TYPE_COUNT> spool_preset_label_bufs_{};
};
