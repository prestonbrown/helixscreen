// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_environment_overlay.h
 * @brief AMS Environment overlay — full temperature/humidity detail and dryer controls
 *
 * Opened from the compact environment indicator on the AMS panel.
 * Shows large readouts, material comfort ranges, and dryer controls
 * (when the backend supports drying).
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "ui_observer_guard.h"

#include "ams_environment_zone.h"
#include "helix/xml/indexed_subject_pool.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
namespace helix {
class AmsBackend;
struct DryingPreset;
} // namespace helix

namespace helix::ui {

struct AmsEnvironmentOverlayTestAccess;

/**
 * @class AmsEnvironmentOverlay
 * @brief Full environment detail overlay with dryer controls
 *
 * Two layouts based on backend capability:
 * - Passive (no dryer): Readouts + material comfort ranges + storage advice
 * - Active (dryer): Readouts + preset/temp/duration controls + start/stop
 */
class AmsEnvironmentOverlay : public OverlayBase {
  public:
    AmsEnvironmentOverlay();
    ~AmsEnvironmentOverlay() override;

    // Non-copyable
    AmsEnvironmentOverlay(const AmsEnvironmentOverlay&) = delete;
    AmsEnvironmentOverlay& operator=(const AmsEnvironmentOverlay&) = delete;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    /// Subscribe to live AMS state changes and pull current data.
    void on_activate() override;

    /// Drop the live-update subscription.
    void on_deactivating(DeactivateReason reason) override;

  protected:
    /// Reclaim the three zone-tab pools so a torn-down overlay does not leave
    /// their subjects registered. Mirrors MacrosPanel::on_ui_destroyed().
    void on_ui_destroyed() override;

  public:
    const char* get_name() const override {
        return "AMS Environment";
    }

    // === Public API ===

    /**
     * @brief Show the overlay over a set of zones.
     *
     * @param zones         The zones this view covers, ordered as the selector shows them
     * @param selected      Index into @p zones to open on
     * @param with_selector Whether to offer the tab strip. False when one zone was drilled
     *                      into from the overview, where Back is the way between zones.
     */
    void show_zone(lv_obj_t* parent_screen, std::vector<helix::printer::EnvironmentZone> zones,
                   size_t selected, bool with_selector);

    /// Switch the shown zone without rebuilding the detail widgets.
    void select_zone(size_t index);

    [[nodiscard]] size_t selected_zone_index() const {
        return selected_zone_;
    }
    /// Zones the selector offers; 0 when no selector is shown.
    [[nodiscard]] size_t zone_count() const;

    /**
     * @brief Refresh overlay data from backend
     */
    void refresh();

    /// Materials shown in the comfort table when no AMS slot is loaded.
    /// Falls back to the user's configured quick-preset materials rather than a
    /// private hardcoded list, so this surface names the same materials as every
    /// other preset surface in the app.
    static std::vector<std::string> fallback_comfort_materials();

  private:
    friend struct AmsEnvironmentOverlayTestAccess;

    // === Internal Methods ===

    /// Update all subjects from backend state
    void update_from_backend();

    /// Build the material comfort ranges text from current humidity
    void update_comfort_text(float humidity_pct);

    /// Populate the preset dropdown from backend presets
    void populate_presets();

    /// Apply a preset to the spinboxes
    void apply_preset(int index);

    /// Auto-select preset based on loaded materials
    void auto_select_preset();

    // === Static Callbacks ===

    static void on_start_stop_clicked(lv_event_t* e);
    static void on_temp_input_clicked(lv_event_t* e);
    static void on_duration_input_clicked(lv_event_t* e);

    /// Which of the two dryer inputs a keypad session is filling.
    enum class DryerField { Temperature, Duration };

    /// Open the numeric keypad over one input, ranged by what the shown box accepts.
    void show_dryer_keypad(DryerField field);
    /// Write a confirmed keypad value back into its input.
    void apply_dryer_keypad_value(DryerField field, float value);
    static void on_dryer_keypad_confirmed(float value, void* user_data);

    /// Field the open keypad is filling. Read by the confirm callback, which the
    /// keypad hands only a value and this overlay.
    DryerField keypad_field_ = DryerField::Temperature;

    /// Set once the user types a temp or duration, cleared when the overlay is shown
    /// again. Presets are a starting point, so the auto-selection stands down for the
    /// rest of a showing in which the user set a value themselves.
    bool dryer_inputs_edited_ = false;

    static void on_preset_changed(lv_event_t* e);
    static void on_zone_tab_clicked(lv_event_t* e);
    static void on_all_zones_clicked(lv_event_t* e);

    /// Push the selected zone's values into the detail subjects.
    void publish_selected_zone();
    /// Fill the tab pools and publish the count, populate-before-count.
    void rebuild_tabs();
    /// Restore the dryer temp/duration inputs from the last remembered values.
    void restore_remembered_dryer_inputs();
    /// The unit the on-screen zone belongs to, for the dryer-control call sites that
    /// command a specific box. -1 when no zone is shown, or the shown zone spans
    /// several units and cannot be attributed to one (HappyHare can report this for a
    /// gate outside every known unit's slot range). A read that only shapes a display
    /// value may treat -1 as "no data for this unit"; a call that would command the
    /// backend must refuse rather than guess which box to act on.
    [[nodiscard]] int acting_unit_index() const;

    // === State ===

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Cached dryer presets
    std::vector<DryingPreset> cached_presets_;

    /// Zones this instance is currently showing.
    std::vector<helix::printer::EnvironmentZone> zones_;
    /// Index into zones_ the detail subjects currently reflect.
    size_t selected_zone_ = 0;
    /// Whether the tab strip is offered for the current zones_.
    bool with_selector_ = false;

    /// Live-update observers. Environment temperature/humidity and dryer state do
    /// NOT bump AmsState's slots_version (that tracks slot data only), so the
    /// overlay observes the per-unit environment indicator text subjects and the
    /// system dryer subjects directly and re-pulls via refresh() on change. All
    /// are singleton subjects, so no paired SubjectLifetime is needed.
    ObserverGuard env_temp_observer_;
    ObserverGuard env_humidity_observer_;
    ObserverGuard dryer_active_observer_;
    ObserverGuard dryer_temp_observer_;

    /// Widget pointers (found after create)
    lv_obj_t* preset_dropdown_ = nullptr;
    lv_obj_t* temp_input_ = nullptr;
    lv_obj_t* duration_input_ = nullptr;

    // === Subjects (managed by SubjectManager) ===

    SubjectManager subjects_;

    lv_subject_t temp_text_subject_;
    char temp_text_buf_[64] = {};

    lv_subject_t target_temp_text_subject_;
    char target_temp_text_buf_[32] = {};

    lv_subject_t humidity_text_subject_;
    char humidity_text_buf_[32] = {};

    /// 1 when the shown unit has a humidity sensor. Gates the humidity readout
    /// and the Material Comfort strip, both of which need a real reading to say
    /// anything true. Per-overlay rather than the shown unit's
    /// ams_env_ind_<i>_humidity_visible so it cannot answer for the wrong unit.
    lv_subject_t humidity_visible_subject_;

    lv_subject_t title_text_subject_;
    char title_text_buf_[96] = {};

    lv_subject_t dryer_visible_subject_;
    lv_subject_t no_dryer_visible_subject_;
    lv_subject_t drying_active_subject_;

    lv_subject_t drying_text_subject_;
    char drying_text_buf_[64] = {};

    lv_subject_t drying_progress_subject_;

    static constexpr int MAX_COMFORT_ROWS = 4;
    lv_subject_t comfort_visible_[MAX_COMFORT_ROWS] = {};
    lv_subject_t comfort_status_[MAX_COMFORT_ROWS] = {}; ///< 0=OK, 1=Marginal, 2=Too humid
    lv_subject_t comfort_text_[MAX_COMFORT_ROWS] = {};
    char comfort_text_buf_[MAX_COMFORT_ROWS][96] = {};

    lv_subject_t start_stop_text_subject_;
    char start_stop_text_buf_[32] = {};

    lv_subject_t preset_text_subject_;
    char preset_text_buf_[64] = {};

    /// Zone selector + per-zone ceiling. "env_zone_" prefix; the pools are
    /// distinct from Task 7's overview overlay, which uses "zone_ov_".
    lv_subject_t zone_count_subject_{};
    lv_subject_t zone_state_subject_{};
    lv_subject_t temp_range_subject_{};
    char temp_range_buf_[32] = {};

    /// Which lanes the shown zone covers. The header names the box; this says how
    /// much of the printer is inside it, which a single-zone view cannot infer.
    lv_subject_t slots_text_subject_{};
    char slots_text_buf_[32] = {};

    /// Cross-unit affordance: offered only when the printer has more zones than this
    /// view is showing and no overview already sits beneath this overlay.
    lv_subject_t all_zones_visible_subject_{};
    lv_subject_t all_zones_text_subject_{};
    char all_zones_text_buf_[64] = {};

    /// Names the zone actually running, for the banner shown while
    /// zone_state_subject_ reads Queued. Empty otherwise.
    lv_subject_t queued_banner_subject_{};
    char queued_banner_buf_[128] = {};

    helix::xml::IndexedSubjectPool tab_label_pool_{"env_zone_tab_label",
                                                   helix::xml::IndexedSubjectPool::Type::String};
    helix::xml::IndexedSubjectPool tab_state_pool_{"env_zone_tab_state",
                                                   helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool tab_active_pool_{"env_zone_tab_active",
                                                    helix::xml::IndexedSubjectPool::Type::Int};
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup.
 *
 * @return Reference to singleton AmsEnvironmentOverlay
 */
AmsEnvironmentOverlay& get_ams_environment_overlay();

/**
 * @brief Open the environment UI for one unit.
 *
 * The branch lives here rather than inside either overlay, so neither has to decide
 * which one should have been opened. A unit with one zone goes straight to detail; a
 * few interchangeable zones get the tab strip; anything else gets the list.
 */
void open_environment_for_unit(int unit_index);

/**
 * @brief Register the AMS environment-indicator badge (component + click callback) once.
 *
 * The single-unit AmsPanel and the multi-unit AmsOverviewPanel both embed the
 * <ams_environment_indicator> badge. Each calls this before parsing the XML that
 * nests it. A process-lifetime guard ensures the component file and the
 * on_env_indicator_clicked event_cb are registered exactly once: the event_cb
 * double-registration is last-wins-safe, but lv_xml_register_component_from_data
 * does an unconditional insert with no dedup, so a second component registration
 * would orphan the first scope node (a bounded one-time leak).
 */
void ensure_ams_env_indicator_registered();

} // namespace helix::ui
