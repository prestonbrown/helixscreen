// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "panel_widget.h"
#include "temp_graph_controller.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

// Test-only access shim (L065 friend pattern). Defined in tests.
class TempGraphWidgetTestAccess;

/// Dashboard widget displaying a real-time temperature graph.
/// Sizes adaptively based on grid span, showing more features at larger sizes.
/// Click opens the full TempGraphOverlay in GraphOnly mode.
class TempGraphWidget : public PanelWidget {
  public:
    friend class TempGraphWidgetTestAccess;

    explicit TempGraphWidget(const std::string& instance_id);
    ~TempGraphWidget() override;

    // PanelWidget interface
    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override;
    const char* id() const override {
        return instance_id_.c_str();
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    void on_activate() override;
    void on_deactivate() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    bool supports_reuse() const override {
        return true;
    }

    /// Map physical widget size (gutter-correct pixels) to feature flags
    static uint32_t features_for_size(int width_px, int height_px);

    /// Static click callback (XML-registered)
    static void on_temp_graph_widget_clicked(lv_event_t* e);

  private:
    void build_default_config();
    std::vector<TempGraphSeriesSpec> build_series_from_config() const;

    /// Body of the TempGraphConfigModal save callback. Persists the new config,
    /// then re-reads widget_obj_/parent_screen_ AT SAVE TIME (panel rebuilds
    /// during the modal can free the original lv_obj_t — bundle RP293UCW) and
    /// rebuilds the controller in place. Skips the rebuild if the underlying
    /// widget container is gone; the persisted config still propagates to any
    /// fresh widget instance that the layout manager attaches afterwards.
    void apply_config_save(const nlohmann::json& new_config);

    /// Append any discovered extruders not yet present in config["sensors"].
    /// Returns true if any rows were added (caller can decide whether to
    /// persist). New rows use the provided enabled flag.
    static bool merge_discovered_extruders(nlohmann::json& config, bool enabled);

    /// Stable signature of the visibility set most recently applied to the
    /// controller. Used by on_activate() to detect when the overlay snapshot
    /// has drifted and a rebuild is needed (follow mode only).
    std::string current_visibility_signature() const;

    /// Tear down and rebuild the controller against the current config_,
    /// snapshotting the container pointers first (detach() nulls them).
    /// Shared by apply_config_save() and the discovery rebuild.
    void rebuild_in_place();

    /// Queue a discovery-driven rebuild for the next LVGL async slot.
    ///
    /// Never rebuilds inline: the observer callback runs inside an UpdateQueue
    /// batch, and TempGraphController::detach() drains that same queue —
    /// re-entrant process_pending is the #732 crash. lv_async_call runs from
    /// lv_timer_handler, outside the batch.
    void schedule_discovery_rebuild();

    /// lv_async_call trampoline for schedule_discovery_rebuild().
    static void discovery_rebuild_async(void* self);

    /// Cancel a queued discovery rebuild. Must run from both detach() and the
    /// destructor, or the async fires on a freed `this`.
    void cancel_discovery_rebuild();

    std::string instance_id_;
    nlohmann::json config_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    std::unique_ptr<TempGraphController> controller_;

    /// When true, the card mirrors whatever curves the user last left visible
    /// on the full-screen TempGraphOverlay. When false, the card uses the
    /// per-sensor `enabled` flags from `config_["sensors"]`.
    bool follow_overlay_ = false;

    /// Signature applied during the most recent attach(); compared in
    /// on_activate() to decide whether to rebuild.
    std::string applied_visibility_signature_;

    /// Rebuilds the series list when discovery adds extruders the config has
    /// never seen. A widget attached at app startup (before the WebSocket
    /// connects) otherwise renders with no nozzle series until the next launch.
    ObserverGuard extruder_version_observer_;

    /// Coalesces repeated version bumps into a single queued rebuild.
    bool discovery_rebuild_pending_ = false;

    /// Modal for sensor toggle + color picker configuration
    class TempGraphConfigModal : public Modal {
      public:
        using SaveCallback = std::function<void(const nlohmann::json& new_config)>;

        TempGraphConfigModal(const nlohmann::json& config, SaveCallback on_save);
        ~TempGraphConfigModal() override = default;

        const char* get_name() const override {
            return "Temperature Graph Config";
        }
        const char* component_name() const override {
            return "temp_graph_config_modal";
        }

        /// Map a Klipper object key to its user-facing label.
        /// Public so the outer widget can label graph series with the same
        /// names the modal shows.
        static std::string sensor_display_name(const std::string& klipper_name);

      protected:
        void on_show() override;
        void on_ok() override;

      private:
        /// Per-row state for sensor toggles
        struct SensorRow {
            std::string name;    ///< Klipper sensor key
            std::string display; ///< Human-readable name
            bool enabled = true;
            int color_idx = 0; ///< Index into TEMP_GRAPH_SERIES_COLORS palette
            lv_obj_t* swatch = nullptr;
            lv_obj_t* sw = nullptr;
        };

        void populate_follow_toggle();
        void populate_sensor_list();
        static void color_swatch_clicked(lv_event_t* e);

        nlohmann::json config_;
        SaveCallback on_save_;
        std::vector<SensorRow> rows_;
        lv_obj_t* follow_switch_ = nullptr;
    };

    std::unique_ptr<TempGraphConfigModal> config_modal_;
};

} // namespace helix
