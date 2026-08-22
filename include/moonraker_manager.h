// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "mock_http_file_server.h"
#include "moonraker_events.h"
#include "runtime_config.h"

#include <atomic>
#include <memory>
#include <queue>

// Forward declarations
namespace helix {
class Config;
}
namespace helix {
class IMoonrakerClient;
class MoonrakerClient;
} // namespace helix
class MoonrakerAPI;
namespace helix {
class PrinterState;
}
class PrintStartCollector;

// Need full enum definition for inline helper function
#include "printer_state.h"

namespace helix {
class MacroModificationManager;
}

/**
 * @brief Manages Moonraker client and API lifecycle
 *
 * MoonrakerManager handles:
 * - Creating mock or real helix::MoonrakerClient based on RuntimeConfig
 * - Creating mock or real MoonrakerAPI based on RuntimeConfig
 * - Thread-safe notification queue for WebSocket → main thread handoff
 * - Connection state change handling
 * - Timeout processing
 * - API injection to panels
 *
 * Thread Safety:
 * Moonraker callbacks run on libhv's event loop thread. LVGL is single-threaded.
 * This class queues notifications for processing on the main thread.
 *
 * Usage:
 *   MoonrakerManager mgr;
 *   mgr.init(runtime_config, app_config);
 *   mgr.connect(url);
 *   // In main loop:
 *   mgr.process_notifications();
 *   mgr.process_timeouts();
 */
class MoonrakerManager {
  public:
    // Out-of-line on purpose: the class holds unique_ptr members of
    // forward-declared types (e.g. MacroModificationManager), and a defaulted
    // constructor here would instantiate in every TU that constructs the
    // manager, requiring those types to be complete there.
    MoonrakerManager();
    ~MoonrakerManager();

    // Non-copyable, non-movable (owns resources, manages threads)
    MoonrakerManager(const MoonrakerManager&) = delete;
    MoonrakerManager& operator=(const MoonrakerManager&) = delete;
    MoonrakerManager(MoonrakerManager&&) = delete;
    MoonrakerManager& operator=(MoonrakerManager&&) = delete;

    /**
     * @brief Initialize Moonraker client and API
     * @param runtime_config Runtime configuration for mock modes
     * @param config Application config for timeouts
     * @return true if initialization succeeded
     */
    bool init(const RuntimeConfig& runtime_config, helix::Config* config);

    /**
     * @brief Shutdown and cleanup
     */
    void shutdown();

    /**
     * @brief Check if manager is initialized
     */
    bool is_initialized() const {
        return m_initialized;
    }

    /**
     * @brief Connect to Moonraker server
     * @param websocket_url WebSocket URL (e.g., "ws://192.168.1.100:7125/websocket")
     * @param http_base_url HTTP base URL (e.g., "http://192.168.1.100:7125")
     * @return 0 on success, non-zero on failure
     */
    int connect(const std::string& websocket_url, const std::string& http_base_url);

    /**
     * @brief Process queued notifications on main thread
     *
     * Must be called from the main thread (LVGL thread).
     * Processes all queued Moonraker notifications and connection state changes.
     */
    void process_notifications();

    /**
     * @brief Process client timeouts
     *
     * Should be called periodically (e.g., every 100ms) to check for
     * request timeouts and trigger reconnection if needed.
     */
    void process_timeouts();

    /**
     * @brief Get the Moonraker client
     */
    helix::IMoonrakerClient* client() const {
        return m_client.get();
    }

    /**
     * @brief Get the Moonraker API
     */
    MoonrakerAPI* api() const {
        return m_api.get();
    }

    /**
     * @brief Get number of pending notifications in queue
     */
    size_t pending_notification_count() const;

    /**
     * @brief Initialize print start collector after connection
     *
     * Sets up observers to monitor print startup phases.
     * Call after successful connect().
     */
    void init_print_start_collector();

    /**
     * @brief The pre-print detection collector, if one exists yet
     *
     * Exposed so the code that dispatches a host-side pre-start block can tell
     * the collector that the block is part of the measured window. Returns null
     * before init or after shutdown; callers must check.
     */
    [[nodiscard]] std::shared_ptr<PrintStartCollector> print_start_collector() const {
        return m_print_start_collector;
    }

    /**
     * @brief Determine if print start collector should be started
     *
     * Helper function for testing mid-print detection logic.
     * Returns true if collector should start based on state transition and progress.
     *
     * @param prev_state Previous print job state
     * @param new_state New print job state
     * @param current_progress Current print progress percentage (0-100)
     * @param is_initial_transition Whether this is the first state transition after app boot
     * @param current_print_duration Current Klipper print_duration in seconds (0 at start of a
     *                               normal print; >0 when joining a print already in progress)
     * @return true if collector should start, false otherwise
     */
    static inline bool should_start_print_collector(helix::PrintJobState prev_state,
                                                    helix::PrintJobState new_state,
                                                    int current_progress,
                                                    bool is_initial_transition,
                                                    int current_print_duration = 0) {
        // RAW_PRINT_STATE_OK: the collector arms on the PRINTER accepting the
        // job. On the lifecycle the transition would be Idle -> Preparing, which
        // is the window the collector exists to measure, not its end.
        // Only start on TRANSITION to PRINTING from non-printing state
        bool was_not_printing = (prev_state != helix::PrintJobState::PRINTING &&
                                 prev_state != helix::PrintJobState::PAUSED);
        bool is_now_printing = (new_state == helix::PrintJobState::PRINTING);

        if (!was_not_printing || !is_now_printing) {
            return false; // Not a transition to printing
        }

        // Mid-print detection: only applies on the FIRST transition after app boot.
        // If the app starts while a print is already running, skip the collector —
        // we joined mid-print. For all subsequent transitions (after
        // cancel/complete/error), the user explicitly started a new print.
        //
        // Two independent signals — either is sufficient:
        //   1. progress > 0 — slicer M73 / virtual_sdcard already past 0%
        //   2. print_duration > 0 — Klipper has been extruding for a while
        // print_duration is the more reliable signal: at a normal print start it
        // is exactly 0, but when joining mid-print it carries the real elapsed
        // print time from the initial subscription payload. Progress alone is
        // insufficient because the print_state_enum_ observer fires synchronously
        // before virtual_sdcard / display_status update progress in the same tick.
        //
        // BUT: the skip must only apply when prev_state is the genuinely-ambiguous
        // boot case (STANDBY — the printer was idle, so a high progress/duration
        // can only mean we joined an already-running print). A transition into
        // PRINTING from a TERMINAL state (COMPLETE / CANCELLED / ERROR) is
        // unambiguously a fresh, user-started reprint — even on the first
        // transition after boot. After an app restart with a just-finished print
        // still in print_stats, progress stays pinned at a stale 100% from the
        // terminal Complete state; the old unconditional skip then suppressed the
        // collector on the very next reprint (Complete(3) -> Printing(1),
        // progress=100%, initial=true), so a reprint-after-restart got no
        // pre-print phase tracking. Gating on prev==STANDBY fixes that while still
        // skipping the real boot-into-active-print case (which presents as
        // STANDBY -> PRINTING).
        bool prev_is_terminal = (prev_state == helix::PrintJobState::COMPLETE ||
                                 prev_state == helix::PrintJobState::CANCELLED ||
                                 prev_state == helix::PrintJobState::ERROR);
        if (is_initial_transition && !prev_is_terminal &&
            (current_progress > 0 || current_print_duration > 0)) {
            return false; // App joined mid-print (booted into a running print), skip collector
        }
        // Recovered mid-print error (e.g. AFC error recovery on a Voron): the print left
        // PRINTING for ERROR and returned without ever resetting. print_duration > 0 proves
        // the print was already underway, so the pre-print collector must not restart and
        // wipe phase state. A genuine reprint passes through STANDBY/COMPLETE/CANCELLED, not ERROR.
        if (prev_state == helix::PrintJobState::ERROR && current_print_duration > 0) {
            return false;
        }
        return true;
    }

    /**
     * @brief Decide whether a non-printing state should tear the collector down
     *
     * A print we initiated ourselves reaches PRINTING via a transient hop: the
     * printer leaves the previous job's terminal state, passes through STANDBY,
     * and only then reports PRINTING. The teardown must not fire on that hop, or
     * a collector armed at commit is stopped on the way into the very print it
     * was armed for.
     *
     * PRINTING and PAUSED never tear it down: PRINT_START runs inside the job,
     * so the collector has to survive the handoff and finish on its own phase
     * detection.
     *
     * @param new_state         The state just reported
     * @param has_preparing_job Whether a job is currently being prepared
     */
    static inline bool should_stop_print_collector(helix::PrintJobState new_state,
                                                   bool has_preparing_job) {
        // RAW_PRINT_STATE_OK: teardown mirrors the arming predicate above; the
        // preparing axis is carried by the separate has_preparing_job argument
        // so the two questions stay independent.
        if (new_state == helix::PrintJobState::PRINTING ||
            new_state == helix::PrintJobState::PAUSED) {
            return false;
        }
        return !has_preparing_job;
    }

    /**
     * @brief Decide whether retiring a preparing job should stop the collector
     *
     * The collector is armed at COMMIT, so every way a preparing job can end has
     * to answer this. `Confirmed` is the one that must NOT stop it: the printer
     * took the job and PRINT_START runs inside it, so the collector has to
     * survive the handoff and finish on its own phase detection. Every other
     * exit - Failed, Cancelled, TimedOut, Superseded - means no print is coming.
     *
     * Reading the job state is what separates them without needing the reason:
     * only a Confirmed retirement leaves the printer PRINTING or PAUSED.
     *
     * This is not merely tidy-up. A collector left armed keeps parsing gcode
     * responses, so the next command the user runs by hand - a home from the
     * Motion panel - is read as a pre-print phase, which re-raises the
     * "Preparing Print" overlay and greys the controls they are using.
     */
    static inline bool should_stop_collector_on_retirement(helix::PrintJobState job_state) {
        return should_stop_print_collector(job_state, /*has_preparing_job=*/false);
    }

    /**
     * @brief Decide whether the pre-print phase should end (hand off to printing)
     *
     * The pre-print → printing hand-off must be gated on the REAL first layer,
     * not raw extrusion. On firmwares whose PRINT_START purges / auto-feeds
     * during the print_stats.state=printing window (Snapmaker U1, and many
     * Klipper setups), print_duration goes positive while the nozzle is still
     * heating and the toolhead is still homing — so the old "first extrusion"
     * (print_duration > 0) shortcut dropped the Preparing/Homing phase minutes
     * early. The firmware-agnostic real-first-layer signal is
     * print_stats.info.current_layer >= 1.
     *
     * Edge-relative, NOT absolute-level: on back-to-back prints the previous
     * print leaves current_layer at a stale positive value (e.g. 250) and
     * reset_for_new_print() — which zeroes it — is dispatched asynchronously,
     * AFTER the collector becomes active. A pure `current_layer >= 1` level
     * read would fire on that stale 250 and complete the NEW print's pre-print
     * phase instantly (the very regression this fix cures). `seen_layer_zero`
     * closes that window: completion requires that the collector has observed
     * current_layer == 0 since THIS print started, so only a genuine 0 -> >=1
     * transition within this print completes it. The old print_duration trigger
     * was naturally edge-based (0 -> positive); this restores that property for
     * the layer signal.
     *
     * Branch discriminator is the STICKY printer_reports_layers, NOT the
     * per-print has_real_layer_data. reset_for_new_print() clears
     * has_real_layer_data to false AFTER the collector starts, and a
     * layer-reporting printer that doesn't continuously re-emit
     * info.current_layer during pre-print (Snapmaker U1) leaves it false through
     * the whole purge. Discriminating on that racy per-print flag therefore took
     * the print_duration fallback DURING pre-print on the U1 — the exact
     * premature-completion bug. printer_reports_layers latches true on the first
     * layer field ever seen this session (the U1 sends total_layer at print
     * start) and never resets, so a layer-reporting printer ALWAYS takes the
     * real-first-layer path and NEVER the print_duration fallback.
     *
     * Fallback: printers that have NEVER reported any layer field all session
     * (printer_reports_layers == false) have no trustworthy current_layer — the
     * subject only carries a progress-derived ESTIMATE that can read >= 1 during
     * pre-print. For those we keep the old behavior and complete on first
     * extrusion (print_duration > 0) so they still leave Preparing.
     *
     * @param printer_reports_layers STICKY: has the printer ever reported a real
     *        layer field (info.current_layer / info.total_layer /
     *        virtual_sdcard.layer) this session. Never reset between prints.
     * @param current_layer Current layer (real when printer_reports_layers true)
     * @param print_duration Klipper print_stats.print_duration in seconds
     * @param seen_layer_zero Whether the collector has observed current_layer==0
     *        since the current print started (arms the layer-1 edge). Ignored on
     *        the non-reporting fallback path.
     * @return true if the pre-print phase should be marked COMPLETE
     */
    static inline bool should_complete_preprint(bool printer_reports_layers, int current_layer,
                                                int print_duration, bool seen_layer_zero,
                                                bool layer_advanced = false) {
        if (printer_reports_layers) {
            // Authoritative: layer data proven to belong to THIS print, plus a
            // layer >= 1. Two ways to prove it, because either alone hangs:
            //
            //  - seen_layer_zero: a genuine 0 -> >=1 edge.
            //  - layer_advanced: the counter moved while we were collecting.
            //
            // Both reject the case the guard exists for — a stale positive
            // carried over from the previous print before reset_for_new_print()
            // zeroes the subject — because a stale value is static and is never
            // preceded by a zero. Requiring the zero edge *alone* was a hang:
            // notify_status_update is coalesced, so a fast first layer or a
            // reconnect part-way in can make the first observed sample >= 1,
            // after which nothing ever completes the pre-print phase and the
            // overlay covers the whole print.
            //
            // The print_duration fallback is NEVER used for a layer-reporting
            // printer — that was the U1 regression.
            return (seen_layer_zero || layer_advanced) && current_layer >= 1;
        }
        // Printer never reported a layer field — fall back to the old
        // first-extrusion signal so genuine non-reporters still complete.
        // current_layer is only a progress-derived estimate here, so
        // seen_layer_zero is irrelevant.
        return print_duration > 0;
    }

    /**
     * @brief Initialize macro analysis manager
     *
     * Creates the manager for PRINT_START macro analysis and wizard.
     * Call after init() but before connect().
     */
    void init_macro_analysis(helix::Config* config);

    /**
     * @brief Get macro modification manager
     * @return Pointer to manager, or nullptr if not initialized
     */
    helix::MacroModificationManager* macro_analysis() const;

  private:
    // Initialization helpers
    void create_client(const RuntimeConfig& runtime_config);
    void configure_timeouts(helix::Config* config);
    void register_callbacks();
    void create_api(const RuntimeConfig& runtime_config);

    /// Present one Moonraker event. MAIN THREAD ONLY — the registered event
    /// handler marshals here through lifetime_.bg_cb(), because everything this
    /// touches (lv_tr, toasts, modals) is LVGL-facing while the handler itself
    /// runs on whatever thread raised the event (#1219).
    void present_event(const MoonrakerEvent& evt);

    // Owned resources
    std::unique_ptr<helix::IMoonrakerClient> m_client;
    std::unique_ptr<MoonrakerAPI> m_api;

    // Thread-safe notification queue
    std::queue<nlohmann::json> m_notification_queue;
    mutable std::mutex m_notification_mutex;

    // Print start collector (monitors PRINT_START macro progress)
    std::shared_ptr<PrintStartCollector> m_print_start_collector;
    ObserverGuard m_preparing_epoch_observer;
    ObserverGuard m_print_start_observer;
    ObserverGuard m_print_start_phase_observer;
    SubjectLifetime m_print_bed_target_fallback_lifetime;
    ObserverGuard m_print_bed_target_fallback_observer;
    ObserverGuard m_print_ext_target_fallback_observer;
    /// Toolhead position observers feeding the print-start collector's
    /// silent-window inference (one guard per axis subject; the callback
    /// reads all three coherently).
    ObserverGuard m_print_position_observers[3];
    // Pre-print completion observers. The hand-off to the printing phase is
    // gated on the REAL first layer (print_stats.info.current_layer >= 1) — see
    // should_complete_preprint(). The layer observer is the primary signal; the
    // print_duration observer is retained as the fallback for printers that
    // never report real layer data (its body now defers to
    // should_complete_preprint() instead of completing on print_duration > 0
    // unconditionally — which fired during pre-print purge/auto-feed on the
    // Snapmaker U1).
    ObserverGuard m_print_layer_observer;
    ObserverGuard m_print_duration_observer;

    // Macro modification manager (PRINT_START wizard integration)
    std::unique_ptr<helix::MacroModificationManager> m_macro_analysis;

#ifdef HELIX_ENABLE_MOCKS
    /// Loopback HTTP server backing thumbnail/gcode downloads under --test.
    /// Owned here because its lifetime must match the mock client's, and the
    /// HTTP base URL it publishes is consumed by connect().
    std::unique_ptr<helix::MockHttpFileServer> m_mock_http;

    /// Non-owning alias for m_client under its concrete type, recorded by
    /// create_client(). MoonrakerAPIMock still takes a concrete
    /// helix::MoonrakerClient&, and this is how create_api() gets one without a
    /// dynamic_cast (the firmware builds -fno-rtti). Only meaningful on non-ESP
    /// builds, which is the only place HELIX_ENABLE_MOCKS is ever defined, and
    /// there m_client is always a helix::MoonrakerClient or a subclass of one.
    helix::MoonrakerClient* m_concrete_client = nullptr;
#endif

    // Destruction flag for async callback safety [L012]
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    // Generation guard for callbacks that must run on the main thread. Used by
    // the Moonraker event handler, whose body is LVGL-facing but is raised on the
    // libhv event-loop thread (#1219). Invalidated in shutdown() alongside
    // m_alive.
    helix::AsyncLifetimeGuard lifetime_;

    bool m_initialized = false;
};
