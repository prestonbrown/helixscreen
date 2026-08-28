// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file temp_graph_controller.h
 * @brief Shared temperature graph lifecycle controller
 *
 * Consolidates the duplicated graph lifecycle pattern used by TempGraphWidget,
 * TempGraphOverlay, and TemperatureService's mini graph:
 *   create graph -> add series -> setup observers -> backfill history -> auto-range
 *
 * Consumers provide a TempGraphControllerConfig describing which series to display,
 * then call rebuild() on connect and the controller handles the rest.
 *
 * @pattern Controller owns graph + observers; consumer owns the container.
 * @threading Observer callbacks are deferred via UpdateQueue (observe_int_sync).
 */

#pragma once

#include "ui_observer_guard.h"
#include "ui_temp_graph.h"
#include "ui_temp_graph_scaling.h"

#include "async_lifetime_guard.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

// ============================================================================
// Color palette — shared across all temp graph consumers
// ============================================================================

static constexpr int TEMP_GRAPH_PALETTE_SIZE = 8;

/// Nord-inspired palette: nozzle red, bed cyan, chamber green, then 5 more.
///
/// Deliberately theme-invariant, like the object_color_* palette. These encode
/// *which sensor a line belongs to*, so they have to stay mutually
/// distinguishable; semantic theme slots cannot promise that, and on Nord's
/// light palette primary, info and focus are all #5E81AC, which would draw
/// three series in one color. Theme-following belongs to the surfaces behind
/// the data (the graph background reads screen_bg), not to the series.
///
/// This array is the only definition. Anything needing a series color indexes
/// it rather than repeating a literal.
extern const lv_color_t TEMP_GRAPH_SERIES_COLORS[TEMP_GRAPH_PALETTE_SIZE];

/// Series color at @p index as 0xRRGGBB, wrapping the palette.
///
/// Sensor config is persisted as JSON ints, so the lv_color_t -> hex
/// conversion lives here instead of being re-inlined at each call site.
uint32_t temp_graph_series_hex(int index);

// ============================================================================
// Configuration types
// ============================================================================

/**
 * @brief Describes a single temperature series to display
 *
 * The klipper_name determines how the controller resolves subjects:
 * - "extruder" / "extruder1" etc  -> extruder temp/target subjects
 * - "heater_bed"                  -> bed temp/target subjects
 * - "chamber"                     -> chamber temp/target subjects
 * - anything else                 -> TemperatureSensorManager lookup
 */
struct TempGraphSeriesSpec {
    std::string klipper_name; ///< Klipper object key (e.g., "extruder", "heater_bed")
    lv_color_t color{};       ///< Series line color
    bool show_target = false; ///< Whether this heater has a controllable target
    std::string
        display_name; ///< Label shown in the graph legend; falls back to klipper_name when empty
};

/**
 * @brief Full configuration for a TempGraphController instance
 */
struct TempGraphControllerConfig {
    int point_count = UI_TEMP_GRAPH_DEFAULT_POINTS; ///< Data points per series
    const char* axis_size = "sm";                   ///< Axis font size ("xs", "sm", "md", "lg")
    uint32_t initial_features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                                TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS |
                                TEMP_GRAPH_FEATURE_GRADIENTS;
    TempGraphScaleParams scale_params{};     ///< Y-axis auto-scaling parameters
    std::vector<TempGraphSeriesSpec> series; ///< Series to display
};

// ============================================================================
// Controller
// ============================================================================

/**
 * @brief Manages a temperature graph's full lifecycle
 *
 * Owns the ui_temp_graph_t, observers, and auto-range state.
 * The consumer provides the LVGL container; the controller creates the graph
 * inside it and handles all observer setup, history backfill, and teardown.
 *
 * Typical usage:
 * @code
 *   TempGraphControllerConfig cfg;
 *   cfg.series = {{"extruder", color_red, true}, {"heater_bed", color_cyan, true}};
 *   controller_ = std::make_unique<TempGraphController>(container, cfg);
 *   // Controller is now live. Call set_features() on resize, pause()/resume() on
 *   // activate/deactivate. Destruction handles full cleanup.
 * @endcode
 */
class TempGraphController {
  public:
    /**
     * @brief Construct and initialize the graph
     *
     * Creates the graph widget inside container, adds series, sets up observers,
     * backfills history, and applies initial auto-range.
     *
     * @param container Parent LVGL object (must outlive the controller)
     * @param config    Series and display configuration
     */
    TempGraphController(lv_obj_t* container, const TempGraphControllerConfig& config);

    ~TempGraphController();

    // Non-copyable, non-movable
    TempGraphController(const TempGraphController&) = delete;
    TempGraphController& operator=(const TempGraphController&) = delete;
    TempGraphController(TempGraphController&&) = delete;
    TempGraphController& operator=(TempGraphController&&) = delete;

    /// Update which graph features are visible (e.g., on resize)
    void set_features(uint32_t features);

    /// Pause observer-driven updates (e.g., when panel is off-screen)
    void pause();

    /// Resume updates and backfill any missed history
    void resume();

    /**
     * @brief Re-read history from the manager and repopulate every series.
     *
     * Backfill runs once in the constructor, which is empty for graphs built
     * before the WebSocket connects — persistent panels (filament mini graph,
     * home dashboard widget) are constructed at app startup, so their
     * construction-time backfill finds nothing and #944's post-discovery
     * seed_from_store never reaches them. Calling this once history has been
     * seeded pulls it in, matching how on-demand overlays backfill fresh on
     * each open. Safe no-op when the history manager is unavailable or empty.
     * See refresh_all_from_history() for the seed-time broadcast (#1124).
     *
     * Deliberately NOT gated on "the chart already has data": the two callers
     * that matter both need it to run on an already-populated chart — resume()
     * pulls in what accumulated while backgrounded, and the post-seed broadcast
     * pushes newly seeded store history into graphs that backfilled earlier.
     * A series whose history is empty is skipped individually inside
     * backfill_history(), so a re-backfill never blanks live data.
     */
    void refresh_from_history();

    /**
     * @brief Re-backfill every live controller from the history manager.
     *
     * Call right after the temperature history is (re)seeded — i.e. after
     * seed_from_store — so persistent graphs built at startup pick up history
     * that only became available post-connect. Idempotent for on-demand graphs
     * that already backfilled at construction. Main-thread only (#1124).
     */
    static void refresh_all_from_history();

    /**
     * @brief Tear down and recreate the graph from scratch
     *
     * @note NO production caller. Reconnect goes through reattach_observers()
     *       instead (#1245) — rebuilding flashes the chart, drops the series
     *       data, and fires once per live controller. This remains as the
     *       full-reset path exercised by tests/unit/test_temp_graph_controller.cpp
     *       (it is what pins the tearing_down_ / generation_ interlock against
     *       the deferred-delete race of #1117). Do not wire it back into the
     *       connection observer.
     */
    void rebuild();

    /**
     * @brief Re-attach observers to current subjects WITHOUT destroying the chart
     *
     * Called on reconnect. Preserves the graph widget, series data, and chip
     * visibility — only detaches old observers and re-attaches fresh ones to
     * whatever subjects are currently live. Avoids the rebuild's chart flash,
     * data loss, and triple-rebuild across controller instances (#1245).
     */
    void reattach_observers();

    /**
     * @brief Detach all observers and invalidate lifetime tokens
     *
     * Used before deferred destruction (prevents observer removal on freed
     * objects) and during rebuild (tears down before recreating). Idempotent —
     * safe to call multiple times; the destructor calls it again as a guard.
     */
    void detach();

    /// Access the underlying graph (for chip toggles, custom styling, etc.)
    ui_temp_graph_t* graph() const {
        return graph_;
    }

    /// Check if graph was created successfully
    bool is_valid() const {
        return graph_ != nullptr;
    }

    /**
     * @brief Look up the graph series ID for a given Klipper name
     * @return Series ID (>= 0) or -1 if not found
     */
    int series_id_for(const std::string& klipper_name) const;

  private:
    /// Per-series runtime state (extends the spec with observer handles)
    struct SeriesState {
        std::string klipper_name;
        int series_id = -1;
        bool show_target = false;
        bool is_dynamic = false;
        /// Bound to a stand-in subject because the real one is not discovered
        /// yet; must be re-resolved once discovery publishes the real one.
        bool provisional = false;
        int64_t last_update_ms = 0; ///< Throttle graph updates to 1Hz per series
        ObserverGuard temp_obs;
        ObserverGuard target_obs;
        SubjectLifetime lifetime;
    };

    void create_graph();
    void setup_series();
    void setup_observers();
    void setup_connection_observer();
    void backfill_history();
    void apply_auto_range();

    /**
     * @brief Attach temp/target observers for one series
     * @return true if the temperature subject resolved (series is live)
     *
     * Extruder and sensor subjects are plain map lookups that return nullptr
     * until discovery creates them, so a graph built before the WebSocket
     * connects resolves nothing and never samples.
     */
    bool attach_series_observers(size_t i);

    /**
     * @brief Retry the series that had no subject when the graph was built
     *
     * Fired by the discovery version subjects. Attaches only what is still
     * unresolved, then re-backfills so the newly reachable history appears at
     * once instead of redrawing one live sample at a time. Deliberately NOT a
     * rebuild(): tearing the graph down here would destroy widgets from inside
     * a queued observer callback.
     */
    void resolve_pending_series();

    TempGraphControllerConfig config_;
    lv_obj_t* container_ = nullptr;
    ui_temp_graph_t* graph_ = nullptr;

    std::vector<SeriesState> series_;
    ObserverGuard connection_observer_;
    /// Installed only while some series is unresolved; retries on discovery.
    ObserverGuard discovery_observer_;
    ObserverGuard sensor_discovery_observer_;

    AsyncLifetimeGuard lifetime_;
    uint32_t generation_ = 0;
    bool paused_ = false;
    bool tearing_down_ = false; ///< Set by detach(); guards rebuild()/setup_observers()
    /**
     * @brief Drops the attach-time sample push while reattach_observers() runs.
     *
     * observe_int_sync() attaches with lv_subject_add_observer_obj(), which
     * fires the observer once immediately — but that fire only *queues* the
     * handler (observer_factory.h), so it lands in UpdateQueue and runs on a
     * later process_pending tick. On reconnect the subjects still hold their
     * pre-disconnect values, so that deferred fire would stamp a stale
     * temperature with a fresh `now` timestamp: the phantom spike of #1245.
     *
     * Set before setup_observers() and cleared by a lifetime_.defer() queued
     * immediately after it. UpdateQueue is FIFO, so the clear runs behind every
     * attach-time fire and ahead of any genuinely new sample. detach() clears
     * it unconditionally so a rebuild() cannot inherit a stuck suppression.
     */
    bool suppress_attach_fire_ = false;
    float y_axis_max_ = 100.0f;
};

} // namespace helix
