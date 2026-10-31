// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"
#include "ui_modal.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
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

    /// Map physical widget size (gutter-correct pixels) to feature flags,
    /// judged against the size bands of the tier now on screen.
    static uint32_t features_for_size(int width_px, int height_px);

    /// Same mapping against an explicit tier's bands. A measured extent only
    /// means something next to the bands of the panel it was measured on, so a
    /// caller reasoning about a panel other than the live one names its tier.
    static uint32_t features_for_size(int width_px, int height_px, UiBreakpoint bp);

    /// Static click callback (XML-registered)
    static void on_temp_graph_widget_clicked(lv_event_t* e);

    /// Settings dialog callbacks (XML-registered); a row's index is the user_data.
    static void config_swatch_cb(lv_event_t* e);
    static void config_series_switch_cb(lv_event_t* e);
    static void config_follow_switch_cb(lv_event_t* e);

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
    /// re-entrant process_pending is the #732 crash. run_next_tick runs from
    /// lv_timer_handler, outside the batch.
    void schedule_discovery_rebuild();

    /// Body of the rebuild schedule_discovery_rebuild() queues.
    void run_discovery_rebuild();

    /// Cancel a queued discovery rebuild. Runs from detach(), so a recycled
    /// instance does not rebuild on behalf of its previous attach.
    void cancel_discovery_rebuild();

    std::string instance_id_;
    nlohmann::json config_;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;

    std::unique_ptr<TempGraphController> controller_;

    bool active_ = false; ///< between on_activate() and on_deactivate()

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
    ObserverGuard language_observer_;

    /// Rename every series to its display name in the current language.
    void refresh_series_names();

    /// Coalesces repeated version bumps into a single queued rebuild.
    bool discovery_rebuild_pending_ = false;
    helix::AsyncLifetimeGuard discovery_rebuild_guard_;

    /// Modal for sensor toggle + color picker configuration
    class TempGraphConfigModal : public Modal {
      public:
        using SaveCallback = std::function<void(const nlohmann::json& new_config)>;

        TempGraphConfigModal(const nlohmann::json& config, SaveCallback on_save);
        ~TempGraphConfigModal() override;

        /// The dialog on screen, or nullptr.
        static TempGraphConfigModal* active() {
            return s_active_;
        }
        /// Step row @p idx's series colour to the next palette entry.
        void cycle_color(size_t idx);
        void set_series_on(size_t idx, bool on);
        void set_follow(bool on);

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
            int color_idx = 0;   ///< Index into kTempGraphSeriesColors palette
        };

        /// Publish one subject slot per series (name, colour, on/off) and the
        /// row count the dialog's <repeat> expands.
        void populate_sensor_list();

        static TempGraphConfigModal* s_active_;

        nlohmann::json config_;
        SaveCallback on_save_;
        std::vector<SensorRow> rows_;
    };

    std::unique_ptr<TempGraphConfigModal> config_modal_;
};

} // namespace helix
