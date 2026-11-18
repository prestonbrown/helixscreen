// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_icon_binder.h"
#include "ui_temp_graph.h"

#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "heater_type.h"
#include "overlay_base.h"
#include "temp_graph_controller.h"
#include "temp_graph_series_policy.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
} // namespace helix
class TemperatureService;

/**
 * @brief Unified temperature graph overlay
 *
 * One overlay that graphs ALL temperature sensors, with toggle chips and
 * optional nozzle/bed/chamber controls.
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

    ~TempGraphOverlay() override;

    // OverlayBase interface
    void init_subjects() override;
    void register_callbacks() override;
    const char* xml_component() const override {
        return "temp_graph_overlay";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Temperature Graph";
    }
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;
    void on_ui_destroyed() override;

    /**
     * @brief Open the overlay in a specific mode
     *
     * Sets the mode and shows the overlay (OverlayBase::show).
     *
     * @param mode The display mode (determines which controls are shown)
     * @param parent_screen Parent screen for lazy creation
     * @param nozzle_extruder In Nozzle mode, the extruder the card opens on
     *        (the tool the caller is showing), which becomes the shared target
     *        tool (TargetTool). Empty keeps the target.
     */
    void open(Mode mode, lv_obj_t* parent_screen, const std::string& nozzle_extruder = {});

    /**
     * @brief Build the overlay in a specific mode inside a home page
     *
     * The full-page form of a heater tile (prestonbrown/helixscreen#1931):
     * OverlayBase::embed() with the mode open() would set.
     */
    bool embed_in(Mode mode, lv_obj_t* container);

    /// The header carries only the title, which a home page repeats.
    bool embed_keeps_header() const override {
        return false;
    }

  protected:
    void before_show() override;

  private:
    static void on_temp_graph_preset_clicked(lv_event_t* e);
    static void on_temp_graph_custom_clicked(lv_event_t* e);

    friend class TempGraphOverlayTestAccess;

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
        SeriesId series_id = SeriesId::None; ///< Graph series ID (mapped from controller)
        bool visible = true;                 ///< Current visibility state
        bool has_target = false;             ///< Whether this heater has a controllable target
        bool is_dynamic = false;             ///< Dynamic sensor (needs SubjectLifetime)
        bool is_sensor = false; ///< From TemperatureSensorManager: @ref group sets its default
        helix::TempGraphSensorGroup group = helix::TempGraphSensorGroup::Auxiliary;
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
    /// Show or hide one series' lines and dim its chip to match.
    void show_series(size_t series_idx);
    void update_chip_style(size_t series_idx);
    static void on_chip_clicked(lv_event_t* e);

    // Control strip
    void configure_control_strip();

    // Keypad callback
    static void keypad_value_cb(float value, void* user_data);

    /// Context captured when the custom-entry keypad opens. A nozzle captures
    /// the klipper name of the extruder the card was displaying, so the send
    /// still reaches it after the keypad's stacked push deactivates this
    /// overlay. A member (not function-static) so the overlay owns its life:
    /// the overlay is a global singleton that outlives the keypad.
    struct KeypadCtx {
        TempGraphOverlay* overlay = nullptr;
        helix::HeaterType type{};
        std::string klipper_name; ///< Nozzle only; empty for bed/chamber
    };
    KeypadCtx keypad_ctx_;

    // Preset helpers
    struct PresetData {
        TempGraphOverlay* overlay;
        int preset_value;
    };
    /// How many of the user's preset material slots this overlay surfaces.
    ///
    /// DELIBERATELY 3, not helix::presets::kPresetCount. This overlay's preset
    /// strip has no room for a fourth button — it is a layout constraint, not an
    /// oversight, and not something a DRY pass should "fix". The temp PANELS
    /// (nozzle/bed/chamber) do show all four slots; this compact overlay shows
    /// the first three. If you widen it, you must first find the space.
    static constexpr int kTempGraphVisiblePresets = 3;
    static_assert(kTempGraphVisiblePresets <= helix::presets::kPresetCount,
                  "cannot surface more preset slots than exist");

    /// "Off" + the visible material presets.
    static constexpr int kMaxPresets = 1 + kTempGraphVisiblePresets;
    std::array<PresetData, kMaxPresets> preset_data_{};

    // State
    Mode mode_ = Mode::GraphOnly;

    // Declarative mode subject (0=GraphOnly, 1=Nozzle, 2=Bed, 3=Chamber).
    // Drives strip visibility and graph_outer width from XML — see
    // temp_graph_overlay.xml's <subjects> block. Seeded with mode_ by
    // init_subjects(); synced on every open() call.
    helix::Subject mode_subject_{};
    std::unique_ptr<helix::TempGraphController> controller_;
    lv_obj_t* chip_row_ = nullptr;
    lv_obj_t* graph_container_ = nullptr;
    lv_obj_t* nozzle_strip_ = nullptr;
    lv_obj_t* bed_strip_ = nullptr;
    lv_obj_t* chamber_strip_ = nullptr;
    std::vector<SeriesInfo> series_;

    // Thermal tint for the largest heater glyphs in the product (size="xl", one
    // per control strip). Bound in on_activate(), unbound in on_deactivating():
    // the widget tree survives a stacked push, but the printer_state_ pointer and
    // subject lifetimes are only valid while active, same pattern as
    // PrintStatusWidget's binders (print_status_widget.h).
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;
    helix::ui::HeaterIconBinder chamber_hero_icon_binder_;

    // Dependencies (resolved on open)
    helix::PrinterState* printer_state_ = nullptr;
    TemperatureService* temp_control_panel_ = nullptr;

    /// Per chamber preset (Off, 1-3): 1 while the configured ceiling allows it.
    helix::Subject chamber_preset_visible_subjects_[4]{};
    /// Per chamber preset 1-3: its temperature as the user reads it ("40°C").
    helix::Subject chamber_preset_text_subjects_[3]{};
    char chamber_preset_text_bufs_[3][16]{};

    // Subject management
    SubjectManager subjects_;

    // Color palette — uses shared kTempGraphSeriesColors from temp_graph_controller.h
};

namespace helix::ui {
/// Register the heater rows' tap callbacks (heater_row.xml): each opens the
/// graph on its heater, the nozzle on the target tool. Before any XML that
/// holds heater_rows is built.
void register_heater_row_callbacks();
} // namespace helix::ui

/// Lazy singleton shared by every entry point that opens the graph.
inline TempGraphOverlay& get_global_temp_graph_overlay() {
    return helix::lazy_global<TempGraphOverlay>("TempGraphOverlay");
}

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
