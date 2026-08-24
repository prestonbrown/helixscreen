// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h"
#include "ui_heater_icon_binder.h"
#include "ui_temp_graph.h"

#include "overlay_base.h"
#include "temp_graph_controller.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class PrinterState;
} // namespace helix
class TemperatureService;

/**
 * @brief Unified temperature graph overlay
 *
 * Replaces the 3 separate nozzle/bed/chamber overlays with a single overlay
 * that graphs ALL temperature sensors with toggle chips and optional controls.
 *
 * Graph lifecycle (creation, observers, history backfill, auto-range) is
 * delegated to TempGraphController. The overlay owns UI-specific concerns:
 * toggle chips, mode system, control strips, keypad, extruder selector.
 *
 * ## Modes
 * - GraphOnly: Full-height graph, no heater controls (opened from mini graph tap)
 * - Nozzle: Graph + nozzle preset controls (opened from nozzle temp click)
 * - Bed: Graph + bed preset controls
 * - Chamber: Graph + chamber preset controls (hidden if sensor-only)
 */
class TempGraphOverlay : public OverlayBase {
  public:
    enum class Mode { GraphOnly, Nozzle, Bed, Chamber };

    TempGraphOverlay();
    ~TempGraphOverlay() override;

    // OverlayBase interface
    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Temperature Graph";
    }
    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    /**
     * @brief Open the overlay in a specific mode
     *
     * Sets the mode and pushes the overlay via NavigationManager.
     * Must be called after init_subjects/create on first use.
     *
     * @param mode The display mode (determines which controls are shown)
     * @param parent_screen Parent screen for lazy creation
     */
    void open(Mode mode, lv_obj_t* parent_screen);

    // Static event callbacks (for XML registration)
    static void on_temp_graph_preset_clicked(lv_event_t* e);
    static void on_temp_graph_custom_clicked(lv_event_t* e);

  private:
    /**
     * @brief Per-series display metadata for chips and UI
     *
     * Observer/lifetime state lives in the TempGraphController. This struct
     * only tracks what the overlay needs for chip toggles and control strips.
     */
    struct SeriesInfo {
        std::string display_name; ///< UI label (e.g., "Nozzle", "Bed", "MCU")
        std::string heater_name;  ///< History manager key (e.g., "extruder", "heater_bed")
        std::string klipper_name; ///< Full Klipper object name for API calls
        lv_color_t color{};       ///< Series line color
        int series_id = -1;       ///< Graph series ID (mapped from controller)
        bool visible = true;      ///< Current visibility state
        bool has_target = false;  ///< Whether this heater has a controllable target
        bool is_dynamic = false;  ///< Dynamic sensor (needs SubjectLifetime)
        lv_obj_t* chip = nullptr; ///< Toggle chip widget
    };

    // Series management
    void discover_series();
    void apply_default_visibility();
    void create_chips();

    // Publish currently-visible klipper names to the global snapshot consumed
    // by widgets that opt into "follow my graph selection" mode.
    void publish_visibility_snapshot() const;

    // Chip interaction
    void toggle_series_visibility(size_t series_idx);
    void update_chip_style(size_t series_idx);
    static void on_chip_clicked(lv_event_t* e);

    // Control strip
    void configure_control_strip();

    // Keypad callback
    static void keypad_value_cb(float value, void* user_data);

    // Extruder selector
    void rebuild_extruder_selector();
    static void on_extruder_selected(lv_event_t* e);

    // Preset helpers
    struct PresetData {
        TempGraphOverlay* overlay;
        int preset_value;
    };
    /// How many of the user's preset material slots this overlay surfaces.
    ///
    /// DELIBERATELY 3, not helix::presets::PRESET_COUNT. This overlay's preset
    /// strip has no room for a fourth button — it is a layout constraint, not an
    /// oversight, and not something a DRY pass should "fix". The temp PANELS
    /// (nozzle/bed/chamber) do show all four slots; this compact overlay shows
    /// the first three. If you widen it, you must first find the space.
    static constexpr int TEMP_GRAPH_VISIBLE_PRESETS = 3;
    static_assert(TEMP_GRAPH_VISIBLE_PRESETS <= helix::presets::PRESET_COUNT,
                  "cannot surface more preset slots than exist");

    /// "Off" + the visible material presets.
    static constexpr int MAX_PRESETS = 1 + TEMP_GRAPH_VISIBLE_PRESETS;
    std::array<PresetData, MAX_PRESETS> preset_data_{};

    // State
    Mode mode_ = Mode::GraphOnly;

    // Declarative mode subject (0=GraphOnly, 1=Nozzle, 2=Bed, 3=Chamber).
    // Drives strip visibility and graph_outer width from XML — see
    // temp_graph_overlay.xml's <subjects> block. Seeded with mode_ by
    // init_subjects(); synced on every open() call.
    lv_subject_t mode_subject_{};
    std::unique_ptr<helix::TempGraphController> controller_;
    lv_obj_t* chip_row_ = nullptr;
    lv_obj_t* graph_container_ = nullptr;
    lv_obj_t* nozzle_strip_ = nullptr;
    lv_obj_t* bed_strip_ = nullptr;
    lv_obj_t* chamber_strip_ = nullptr;
    lv_obj_t* extruder_selector_row_ = nullptr;
    std::vector<SeriesInfo> series_;

    // Thermal tint for the largest heater glyphs in the product (size="xl", one
    // per control strip). Bound in on_activate(), unbound in on_deactivate() —
    // cached_overlay_ persists across pushes, but the printer_state_ pointer and
    // subject lifetimes are only valid while active, same pattern as
    // PrintStatusWidget's binders (ui_panel_print_status.cpp).
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;

    // Dependencies (resolved on open)
    helix::PrinterState* printer_state_ = nullptr;
    TemperatureService* temp_control_panel_ = nullptr;

    // Active extruder name (for nozzle mode)
    std::string active_extruder_name_ = "extruder";

    // Subject management
    SubjectManager subjects_;

    // Cached panel for lazy creation
    lv_obj_t* cached_overlay_ = nullptr;

    // Color palette — uses shared TEMP_GRAPH_SERIES_COLORS from temp_graph_controller.h
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers cleanup with StaticPanelRegistry.
 */
TempGraphOverlay& get_global_temp_graph_overlay();

/**
 * @brief Snapshot of klipper sensor names last visible on the full-screen overlay.
 *
 * `nullopt` until the overlay opens once OR when the active printer differs
 * from the printer the snapshot was captured against. Updated whenever the
 * overlay applies default visibility or the user toggles a chip. Consumed by
 * widgets that opt into "follow the user's graph selection" mode.
 *
 * The printer-name guard prevents a snapshot from one printer's sensor set
 * from leaking into a different printer's home graph card after a switch.
 */
std::optional<std::vector<std::string>> get_temp_graph_visibility_snapshot();

namespace helix::test_access {
/// Test-only seeder for the visibility snapshot. Production code never calls this;
/// it exists so widget tests can drive the follow-mode code path without spinning
/// up a fully-wired overlay. Seeds against the current active printer name so
/// the snapshot reads back as set.
void set_temp_graph_visibility_snapshot(std::optional<std::vector<std::string>> snapshot);
} // namespace helix::test_access
