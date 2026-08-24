// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file telemetry_manager.h
 * @brief Anonymous, opt-in telemetry for HelixScreen
 *
 * Collects aggregate usage data (session starts, print outcomes) to help
 * improve HelixScreen. All telemetry is:
 *
 * - **Opt-in**: Disabled by default, user must explicitly enable via settings UI.
 * - **Anonymous**: Device identity is a double-hashed UUID (SHA-256 of UUID + random salt).
 *   The raw UUID never leaves the device.
 * - **Minimal**: Only session and print outcome events are collected. No filenames,
 *   no G-code content, no network identifiers, no personal information.
 * - **Transparent**: Queue contents are inspectable via get_queue_snapshot().
 * - **GDPR-friendly**: Users can disable at any time; clear_queue() purges all
 *   pending events. No data is transmitted until the user opts in.
 *
 * Architecture:
 * @code
 * TelemetryManager (singleton)
 * +-- Event Queue (mutex-protected, persisted to disk)
 * |   +-- Session events (app launch)
 * |   +-- Print outcome events (success/failure/cancel)
 * +-- Device Identity (UUID v4 + salt, stored in config dir)
 * +-- LVGL Subject (reactive binding for settings toggle)
 * +-- Transmission (Phase 3: batched HTTPS POST to endpoint)
 * @endcode
 *
 * Thread safety:
 * - Event recording (record_session, record_print_outcome) is thread-safe
 *   and may be called from any thread.
 * - LVGL subject access (enabled_subject) must happen on the main LVGL thread.
 * - Transmission (try_send) runs on a background thread.
 *
 * Usage:
 * @code
 * auto& telemetry = TelemetryManager::instance();
 * telemetry.init("config");  // Load persisted state
 *
 * // User enables telemetry in settings UI (binds to enabled_subject())
 * telemetry.set_enabled(true);
 *
 * // Record events throughout the application lifetime
 * telemetry.record_session();
 * telemetry.record_print_outcome("success", 3600, 10, 1500.0f, "PLA", 210, 60);
 *
 * // On shutdown
 * telemetry.shutdown();
 * @endcode
 *
 * @see UpdateChecker for similar singleton + background thread + LVGL subject pattern
 * @see SettingsManager for similar singleton + LVGL subject + persistence pattern
 */

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "lvgl.h"
#include "memory_monitor.h"
#include "subject_managed_panel.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;

/**
 * @brief Thread-safe context published by main-thread producers so the memory
 *        monitor (running on its own thread) can include it in memory_warning
 *        events without locking.
 *
 * Set from main thread when state changes; read from monitor thread when
 * building a memory_warning. Plain atomics — racy reads return a slightly
 * stale value, which is fine for diagnostics.
 */
namespace telemetry_context {
/// PrintState enum value (cast to int). -1 = unset.
extern std::atomic<int> print_state_int;
/// PanelId enum value (cast to int). -1 = unset (no active panel yet).
extern std::atomic<int> active_panel_int;
/// True while a GCodeViewer holds parser data + renderer geometry.
extern std::atomic<bool> gcode_renderer_loaded;
} // namespace telemetry_context

} // namespace helix

/**
 * @brief Anonymous, opt-in telemetry manager
 *
 * Singleton that collects anonymous usage events and queues them for
 * batched transmission. Default state is OFF -- telemetry is only
 * active after explicit user opt-in via the settings UI.
 *
 * Events are persisted to disk so they survive restarts. The event
 * queue is capped at MAX_QUEUE_SIZE; oldest events are dropped when
 * the cap is reached.
 */
class TelemetryManager {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to global TelemetryManager
     */
    static TelemetryManager& instance();

    /**
     * @brief Classify whether a Moonraker websocket URL points at this machine
     *
     * Answers "is HelixScreen running on the printer, or driving it over the
     * network" for the hardware_profile event. Only the verdict is ever
     * recorded -- the host itself never leaves the device.
     *
     * Reads the same URL HelixPluginInstaller uses for is_local_moonraker(), so
     * telemetry and the installer cannot disagree about a given install.
     *
     * @param websocket_url URL from IMoonrakerAPI::get_websocket_url()
     * @return true if loopback, false if routable, nullopt if no URL is known
     *         (mock client, or not connected yet) -- absent is not "remote"
     */
    [[nodiscard]] static std::optional<bool>
    classify_moonraker_locality(const std::string& websocket_url);

    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    /**
     * @brief Initialize the telemetry manager
     *
     * Loads persisted enabled state, device ID, and event queue from disk.
     * Initializes the LVGL subject for settings UI binding. Idempotent --
     * safe to call multiple times.
     *
     * @param config_dir Directory for persistence files (default "config").
     *                   Accepts a custom path for test isolation.
     */
    void init(const std::string& config_dir = "config");

    /**
     * @brief Shutdown and cleanup
     *
     * Persists the event queue to disk, cancels any pending transmission,
     * and joins the send thread. Idempotent -- safe to call multiple times.
     */
    void shutdown();

    // =========================================================================
    // ENABLE / DISABLE (opt-in, default OFF)
    // =========================================================================

    /**
     * @brief Set telemetry enabled state
     *
     * When enabled, events are queued and periodically transmitted.
     * When disabled, no events are recorded or sent. Persists the
     * preference to disk immediately.
     *
     * @param enabled true to opt in, false to opt out
     */
    void set_enabled(bool enabled);

    /**
     * @brief Check if telemetry is enabled (thread-safe)
     * @return true if user has opted in
     */
    bool is_enabled() const;

    // =========================================================================
    // EVENT RECORDING
    // =========================================================================

    /**
     * @brief Record a session start event
     *
     * Call once per application launch. Records HelixScreen version,
     * platform, and display resolution. No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     */
    void record_session();

    /**
     * @brief Record a print outcome event
     *
     * Call when a print finishes (success, failure, or cancellation).
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param outcome Result string ("success", "failure", "cancelled")
     * @param duration_sec Total print duration in seconds
     * @param phases_completed Number of print start phases completed (0-10)
     * @param filament_used_mm Filament consumed in millimeters
     * @param filament_type Filament material (e.g., "PLA", "PETG", "ABS")
     * @param nozzle_temp Target nozzle temperature in degrees C
     * @param bed_temp Target bed temperature in degrees C
     */
    void record_print_outcome(const std::string& outcome, int duration_sec, int phases_completed,
                              float filament_used_mm, const std::string& filament_type,
                              int nozzle_temp, int bed_temp);

    /**
     * @brief Record an update failure event
     *
     * Call when an in-app update fails at any stage (download, verify, install).
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param reason Short failure reason (e.g., "download_failed", "corrupt_download")
     * @param version Target version being installed
     * @param platform Platform key (e.g., "pi", "ad5m")
     * @param http_code HTTP status code (-1 to omit)
     * @param file_size Downloaded file size in bytes (-1 to omit)
     * @param exit_code install.sh exit code (-1 to omit)
     */
    void record_update_failure(const std::string& reason, const std::string& version,
                               const std::string& platform, int http_code = -1,
                               int64_t file_size = -1, int exit_code = -1);

    /**
     * @brief Check for a successful update from a previous session
     *
     * Looks for update_success.json flag file. If found, enqueues an
     * update_success event and deletes the file. Called from init().
     */
    void check_previous_update();

    /**
     * @brief Record a periodic memory snapshot event
     *
     * Captures current process memory usage (RSS, VM size, swap, etc.)
     * along with uptime. No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param trigger What triggered the snapshot ("hourly" or "session_start")
     */
    void record_memory_snapshot(const std::string& trigger);

    /**
     * @brief Record a memory pressure warning event
     *
     * Captures a memory warning with level, reason, stats, and smaps breakdown.
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param event Memory warning event data from MemoryMonitor callback
     */
    void record_memory_warning(const helix::MemoryWarningEvent& event);

    /**
     * @brief Record a comprehensive hardware profile event
     *
     * Captures full printer hardware inventory: MCUs, build volume, fans,
     * steppers, LEDs, sensors, probing, capabilities, MMU, tools, macros,
     * and plugin state. Call after printer discovery is complete.
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     */
    void record_hardware_profile();

    /**
     * @brief Record a settings snapshot event
     *
     * Captures current user configuration: theme, brightness, timeouts,
     * locale, sound, update channel, animations, and time format.
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     */
    void record_settings_snapshot();

    /**
     * @brief Record a panel usage summary event at shutdown
     *
     * Finalizes panel time tracking and enqueues a panel_usage event
     * with per-panel visit counts, time spent, and overlay count.
     * No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (accesses session trackers).
     */
    void record_panel_usage();

    /**
     * @brief Notify that the active panel has changed
     *
     * Tracks cumulative time on each panel and visit counts.
     * Always tracks regardless of enabled state (data is only
     * recorded at shutdown if enabled).
     *
     * @param panel_name Lowercase panel name (e.g., "home", "controls")
     */
    void notify_panel_changed(const std::string& panel_name);

    /**
     * @brief Notify that an overlay was opened
     *
     * Increments the overlay open counter and per-overlay visit count.
     * Always tracks regardless of enabled state.
     *
     * @param overlay_name Human-readable overlay name (from IPanelLifecycle::get_name())
     */
    void notify_overlay_opened(const std::string& overlay_name);

    /**
     * @brief Notify that a home widget was interacted with
     *
     * Tracks user-initiated interactions (clicks, toggles) per widget type.
     * Instance suffixes (e.g., "favorite_macro:2") are stripped for aggregation.
     * Always tracks regardless of enabled state (data is only recorded at
     * shutdown if enabled).
     *
     * Must be called from the LVGL/main thread only (same as panel tracking).
     *
     * @param widget_id Widget identifier (e.g., "power", "led", "temperature")
     */
    void notify_widget_interaction(const std::string& widget_id);

    /**
     * @brief Record print start context when a print begins
     *
     * Records metadata about the print job (source, thumbnail, file size,
     * estimated duration, slicer, tool count, AMS state). No-op if
     * telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param source Print source ("local" or "usb")
     * @param has_thumbnail Whether the file has a thumbnail
     * @param file_size_bytes File size in bytes
     * @param estimated_duration_sec Estimated print duration in seconds
     * @param slicer Slicer software name from metadata
     * @param tool_count_used Number of tools used
     * @param ams_active Whether an AMS/MMU system is active
     */
    void record_print_start_context(const std::string& source, bool has_thumbnail,
                                    int64_t file_size_bytes, int estimated_duration_sec,
                                    const std::string& slicer, int tool_count_used,
                                    bool ams_active);

    /**
     * @brief Mark that the next print was started from within HelixScreen
     *
     * Call this before starting a print via Moonraker so that the telemetry
     * observer can distinguish in-app prints from external prints (Mainsail,
     * Fluidd, Obico, etc.). The flag is consumed once when the next
     * print_start_context event is recorded.
     *
     * Thread-safe: may be called from any thread.
     */
    void notify_print_started_in_app();

    /**
     * @brief Check and consume the in-app print flag
     *
     * Returns true (once) if notify_print_started_in_app() was called since
     * the last consume. Used internally by the print telemetry observer.
     */
    bool consume_print_started_in_app();

    /**
     * @brief Record a non-fatal error event (rate-limited)
     *
     * Records non-fatal errors with category-based rate limiting (max 1 event
     * per category per 5 minutes) to prevent queue flooding. No-op if
     * telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param category Error category ("moonraker_api", "websocket", "file_io", "display")
     * @param code Error code ("timeout", "http_4xx", "http_5xx", "parse_error",
     * "connection_refused")
     * @param context Pre-defined context string (not user data)
     */
    void record_error(const std::string& category, const std::string& code,
                      const std::string& context);

    /**
     * @brief Record a connection stability summary event at shutdown
     *
     * Finalizes connection time tracking and enqueues a connection_stability
     * event with connect/disconnect counts, durations, and Klippy errors.
     * No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (accesses session trackers).
     */
    void record_connection_stability();

    /**
     * @brief Notify that the WebSocket connection state changed
     *
     * Tracks connection/disconnection counts and durations.
     * Always tracks regardless of enabled state.
     *
     * @param state Connection state: 0=disconnected, 1=connecting, 2=connected
     */
    void notify_connection_state_changed(int state);

    /**
     * @brief Notify that the Klippy state changed
     *
     * Tracks Klippy shutdown and error counts.
     * Always tracks regardless of enabled state.
     *
     * @param state Klippy state: 0=ready, 1=startup, 2=shutdown, 3=error
     */
    void notify_klippy_state_changed(int state);

    /**
     * @brief Record a periodic performance snapshot event
     *
     * Captures frame time statistics from the ring buffer and uptime.
     * Called automatically by the snapshot timer every SNAPSHOT_INTERVAL_MS.
     * No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (accesses frame ring buffer).
     */
    void record_performance_snapshot();

    /**
     * @brief Record a feature adoption event
     *
     * Captures which optional features (AMS, camera, macros, etc.) are actively
     * used, recorded once per session after FEATURE_ADOPTION_DELAY_MS. No-op if
     * telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only.
     */
    void record_feature_adoption();

    /**
     * @brief Record an AsyncLifetimeGuard skip-rate snapshot
     *
     * Drains `helix::async_lifetime::take_snapshot()` and emits an
     * `async_lifetime_skips` event with the per-tag breakdown. A hot producer
     * in this event is the early signal that an owner is repeatedly dying with
     * pending work — the shape of bug 5KNWUEKY before it crashed. Fires on the
     * periodic snapshot timer and once at shutdown. No-op if telemetry is
     * disabled.
     *
     * Must be called from the LVGL/main thread only.
     */
    void record_async_lifetime_snapshot();

    /**
     * @brief Notify that a user setting was changed
     *
     * Accumulates setting changes with debouncing. After SETTINGS_DEBOUNCE_MS of
     * inactivity, calls flush_settings_changes() to enqueue a single aggregated
     * settings_changes event. Always tracks regardless of enabled state (data is
     * only recorded if enabled when the debounce fires).
     *
     * Must be called from the LVGL/main thread only.
     *
     * @param setting_name  Setting key (e.g., "theme", "brightness")
     * @param old_value     Previous value as string
     * @param new_value     New value as string
     */
    void notify_setting_changed(const std::string& setting_name, const std::string& old_value,
                                const std::string& new_value);

    /**
     * @brief Record a frame render time sample into the ring buffer
     *
     * Stores the frame time alongside the current panel ID for per-panel
     * breakdown in performance snapshots. Wraps around when the ring is full.
     * Must be called from the LVGL/main thread only.
     *
     * @param frame_time_us Frame render duration in microseconds
     */
    void record_frame_time(uint32_t frame_time_us);

    /**
     * @brief Trigger an immediate performance snapshot (e.g., at shutdown)
     *
     * Fires the snapshot logic outside the normal timer cadence. Resets
     * the snapshot timer if it is running. No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only.
     */
    void fire_periodic_snapshot();

    /**
     * @brief Flush accumulated settings changes to the event queue immediately
     *
     * Stops the debounce timer and enqueues a settings_changes event for all
     * pending changes. No-op if there are no pending changes or telemetry is
     * disabled.
     *
     * Must be called from the LVGL/main thread only.
     */
    void flush_settings_changes();

    /**
     * @brief Write update success flag file before restart
     *
     * Static method callable from UpdateChecker before _exit(0).
     * The flag is read by check_previous_update() on next boot.
     *
     * @param config_dir Config directory path
     * @param version Version that was installed
     * @param from_version Version before the update
     * @param platform Platform key
     */
    static void write_update_success_flag(const std::string& config_dir, const std::string& version,
                                          const std::string& from_version,
                                          const std::string& platform);

    // =========================================================================
    // CRASH REPORTING (Phase 5)
    // =========================================================================

    /**
     * @brief Check for a crash file from a previous crash and enqueue it
     *
     * Looks for a crash file at config_dir_/crash.txt. If found, parses
     * it into a crash event JSON object, enqueues it, and deletes the file.
     * Called automatically from init() after loading the queue.
     *
     * The crash event schema:
     * @code
     * {
     *   "schema_version": 1,
     *   "event": "crash",
     *   "device_id": "<double-hashed>",
     *   "timestamp": "<from crash file or current>",
     *   "signal": 11,
     *   "signal_name": "SIGSEGV",
     *   "app_version": "0.9.6",
     *   "uptime_sec": 3600,
     *   "backtrace": ["0x0040abcd", "0x0040ef01"]
     * }
     * @endcode
     */
    void check_previous_crash();

    /// True if crash.txt was suppressed because update_success.json was present.
    /// Used by Application to skip the crash report modal.
    bool had_update_restart() const {
        return had_update_restart_;
    }

    /**
     * @brief Enqueue and persist a crash event regardless of the telemetry
     *        opt-in setting.
     *
     * Called when the user explicitly consents to send a crash *bundle* (the
     * crash report modal's "Send" action). That consent also covers a
     * lightweight telemetry crash event, so this bypasses the enabled_ gate
     * that check_previous_crash() honors. Reads crash.txt from the config dir,
     * builds the crash event, enqueues and persists it. The caller transmits
     * with try_send(true).
     *
     * No-op (returns false) if a crash event was already enqueued this session
     * (opt-in boot path or a prior consent), if no parseable crash file exists,
     * or if the crash is the expected post-update restart (update_success.json
     * present).
     *
     * @return true if a crash event was enqueued, false otherwise.
     */
    bool enqueue_crash_event_unconditional();

    // =========================================================================
    // QUEUE MANAGEMENT
    // =========================================================================

    /**
     * @brief Get number of queued events (thread-safe)
     * @return Number of events waiting to be transmitted
     */
    size_t queue_size() const;

    /**
     * @brief Get a JSON snapshot of the current queue (thread-safe)
     *
     * Useful for transparency: lets the user inspect exactly what data
     * would be transmitted. Returns a JSON array of event objects.
     *
     * @return JSON array containing all queued events
     */
    nlohmann::json get_queue_snapshot() const;

    /**
     * @brief Clear all queued events (thread-safe)
     *
     * Removes all pending events from the queue and persists the
     * empty state to disk. Use when the user wants to purge telemetry data.
     */
    void clear_queue();

    // =========================================================================
    // TRANSMISSION (Phase 3)
    // =========================================================================

    /**
     * @brief Start periodic auto-send timer
     *
     * Creates an LVGL timer that calls try_send() periodically.
     * First call is delayed by INITIAL_SEND_DELAY to let the app settle.
     * Subsequent calls happen every AUTO_SEND_INTERVAL.
     *
     * Must be called from the LVGL thread.
     */
    void start_auto_send();

    /**
     * @brief Stop periodic auto-send timer
     *
     * Deletes the LVGL timer. Safe to call if timer is not active.
     * Must be called from the LVGL thread.
     */
    void stop_auto_send();

    /**
     * @brief Attempt to send queued events to the telemetry endpoint
     *
     * Sends up to MAX_BATCH_SIZE events in a single HTTPS POST.
     * Respects SEND_INTERVAL between transmissions and uses exponential
     * backoff on failure. Runs the HTTP request on a background thread.
     *
     * No-op if telemetry is disabled, queue is empty, or a send is
     * already in progress.
     *
     * @param force When true, bypass the opt-in (enabled_) gate and the
     *        send-interval backoff. Used by the crash-bundle consent path,
     *        where the user has explicitly consented to transmit this crash.
     */
    void try_send(bool force = false);

    /**
     * @brief Build a batch of events for transmission (public for testing)
     *
     * Takes at most MAX_BATCH_SIZE events from the front of the queue
     * without removing them. Returns a JSON array ready for POST body.
     *
     * @return JSON array of events (may be empty if queue is empty)
     */
    nlohmann::json build_batch() const;

    /**
     * @brief Remove sent events from the front of the queue (public for testing)
     *
     * After a successful send, call this to remove the events that were
     * transmitted. Removes min(count, queue_size) events from the front.
     *
     * @param count Number of events to remove from the front of the queue
     */
    void remove_sent_events(size_t count);

    // =========================================================================
    // PRINT OUTCOME OBSERVER
    // =========================================================================

    /**
     * @brief Create an observer that auto-records print outcomes
     *
     * Watches the print_state_enum subject for transitions from active
     * (PRINTING/PAUSED) to terminal states (COMPLETE/CANCELLED/ERROR).
     * When detected, gathers print data from PrinterState subjects and
     * calls record_print_outcome() automatically.
     *
     * Call once during initialization (e.g., from SubjectInitializer).
     * The returned ObserverGuard manages the observer's lifetime.
     *
     * @return ObserverGuard for RAII cleanup
     */
    ObserverGuard init_print_outcome_observer();

    // =========================================================================
    // DEVICE ID UTILITIES (public for testing)
    // =========================================================================

    /**
     * @brief Generate a random UUID v4 string
     * @return UUID string in standard format (e.g., "550e8400-e29b-41d4-a716-446655440000")
     */
    static std::string generate_uuid_v4();

    /**
     * @brief Double-hash a device UUID with a salt for anonymization
     *
     * Computes SHA-256(SHA-256(uuid) + salt) to produce an irreversible
     * device identifier that cannot be traced back to the original UUID.
     *
     * @param uuid Raw device UUID
     * @param salt Random salt string
     * @return Hex-encoded double-hashed identifier
     */
    static std::string hash_device_id(const std::string& uuid, const std::string& salt);

    // =========================================================================
    // PERSISTENCE
    // =========================================================================

    /**
     * @brief Save the event queue to disk
     *
     * Writes the queue as a JSON array to the config directory.
     * Called at shutdown, after successful transmission, and hourly
     * by the auto-send timer. Individual record_*() methods do NOT
     * call save_queue() — events are batched in memory to avoid
     * redundant disk writes.
     */
    void save_queue() const;

    /**
     * @brief Load the event queue from disk
     *
     * Restores previously persisted events. Called automatically during init().
     */
    void load_queue();

    // =========================================================================
    // LVGL SUBJECT (for settings UI binding)
    // =========================================================================

    /**
     * @brief Get LVGL subject for the enabled state
     *
     * Integer subject: 0 = disabled, 1 = enabled. Bind this to a toggle
     * switch in the settings XML for reactive opt-in/opt-out.
     *
     * Must be accessed on the main LVGL thread only.
     *
     * @return Pointer to the enabled state subject
     */
    lv_subject_t* enabled_subject();

    // =========================================================================
    // CONSTANTS
    // =========================================================================

    /** @brief Maximum number of events in the queue before oldest are dropped */
    static constexpr size_t MAX_QUEUE_SIZE = 100;

    /** @brief Delay before first auto-send attempt after startup */
    static constexpr uint32_t INITIAL_SEND_DELAY_MS = 60 * 1000; // 60 seconds

    /** @brief Interval between auto-send attempts */
    static constexpr uint32_t AUTO_SEND_INTERVAL_MS = 60 * 60 * 1000; // 1 hour

    /** @brief Schema version for event JSON structure */
    static constexpr int SCHEMA_VERSION = 2;

    /** @brief HTTPS endpoint for telemetry submission */
    static constexpr const char* ENDPOINT_URL = "https://telemetry.helixscreen.org/v1/events";

    /** @brief API key for telemetry ingestion authentication.
     *  Not a true secret (visible in source), but prevents casual spam.
     *  To rotate: update this constant, then run `wrangler secret put INGEST_API_KEY`
     *  in server/telemetry-worker/ with the new value, and release a new version. */
    static constexpr const char* API_KEY = "hx-tel-v1-a7f3c9e2d1b84056";

    /** @brief Minimum interval between transmission attempts */
    static constexpr auto SEND_INTERVAL = std::chrono::hours{24};

    /** @brief Maximum events per HTTPS POST batch */
    static constexpr size_t MAX_BATCH_SIZE = 20;

    /** @brief Interval between periodic performance snapshots */
    static constexpr uint32_t SNAPSHOT_INTERVAL_MS = 4 * 60 * 60 * 1000;

    /** @brief Number of frame time samples held in the ring buffer */
    static constexpr size_t FRAME_RING_SIZE = 1024;

    /** @brief Frame time threshold above which a frame is considered dropped (33ms = ~30fps) */
    static constexpr uint32_t DROPPED_FRAME_THRESHOLD_US = 33000;

    /** @brief Frame time floor below which a frame is considered idle (no rendering work) */
    static constexpr uint32_t IDLE_FRAME_THRESHOLD_US = 500;

    /** @brief Interval between frame performance snapshots (5 minutes) */
    static constexpr uint32_t FRAME_PERF_INTERVAL_MS = 5 * 60 * 1000;

    /** @brief Delay after session start before recording feature adoption */
    static constexpr uint32_t FEATURE_ADOPTION_DELAY_MS = 5 * 60 * 1000;

    /** @brief Debounce window for aggregating settings changes before recording */
    static constexpr uint32_t SETTINGS_DEBOUNCE_MS = 30 * 1000;

  private:
    TelemetryManager() = default;
    ~TelemetryManager();

    // Non-copyable
    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;

    // =========================================================================
    // INTERNAL HELPERS
    // =========================================================================

    /**
     * @brief Perform the actual HTTP POST to the telemetry endpoint
     *
     * Called on a background thread from try_send(). Sends the batch
     * as a JSON array via HTTPS POST. On success, removes sent events
     * and resets backoff. On failure, increments backoff multiplier.
     *
     * @param batch JSON array of events to transmit
     */
    void do_send(const nlohmann::json& batch);

    /**
     * @brief Build a "crash" telemetry event from parsed crash-file data.
     *
     * Shared by check_previous_crash() (opt-in boot path) and
     * enqueue_crash_event_unconditional() (bundle-consent path).
     *
     * @param crash_data JSON parsed from crash.txt (read_crash_file() output)
     * @return JSON crash event following the telemetry schema
     */
    nlohmann::json build_crash_event(const nlohmann::json& crash_data) const;

    /**
     * @brief Add an event to the queue (mutex-protected)
     *
     * Drops the oldest event if the queue is at MAX_QUEUE_SIZE.
     *
     * @param event JSON event object to enqueue
     */
    void enqueue_event(nlohmann::json event);

    /**
     * @brief Build a session start event JSON object
     * @return JSON event with type "session", version, platform, resolution
     */
    nlohmann::json build_session_event() const;

    /**
     * @brief Build a print outcome event JSON object
     * @param outcome Result string ("success", "failure", "cancelled")
     * @param duration_sec Total print duration in seconds
     * @param phases_completed Number of print start phases completed
     * @param filament_used_mm Filament consumed in millimeters
     * @param filament_type Filament material type
     * @param nozzle_temp Target nozzle temperature
     * @param bed_temp Target bed temperature
     * @return JSON event with type "print_outcome" and all fields
     */
    nlohmann::json build_print_outcome_event(const std::string& outcome, int duration_sec,
                                             int phases_completed, float filament_used_mm,
                                             const std::string& filament_type, int nozzle_temp,
                                             int bed_temp) const;

    nlohmann::json build_update_failed_event(const std::string& reason, const std::string& version,
                                             const std::string& platform, int http_code,
                                             int64_t file_size, int exit_code) const;

    nlohmann::json build_update_success_event(const std::string& version,
                                              const std::string& from_version,
                                              const std::string& platform,
                                              const std::string& timestamp) const;

    /**
     * @brief Build a memory snapshot event JSON object
     * @param trigger What triggered the snapshot ("hourly" or "session_start")
     * @return JSON event with type "memory_snapshot" and memory stats
     */
    nlohmann::json build_memory_snapshot_event(const std::string& trigger) const;

    /**
     * @brief Build a memory warning event JSON object
     * @param warning Memory warning event data from MemoryMonitor
     * @return JSON event with type "memory_warning" and full diagnostics
     */
    nlohmann::json build_memory_warning_event(const helix::MemoryWarningEvent& warning) const;

    /**
     * @brief Build a hardware profile event JSON object
     * @return JSON event with type "hardware_profile" and nested hardware sections
     */
    nlohmann::json build_hardware_profile_event() const;

    // Hardware profile helper methods (used by build_hardware_profile_event)
    static nlohmann::json build_hw_fans_section(const helix::PrinterDiscovery& hw);
    static nlohmann::json build_hw_sensors_section(const helix::PrinterDiscovery& hw);
    static nlohmann::json build_hw_probe_section(const helix::PrinterDiscovery& hw);
    static nlohmann::json build_hw_capabilities_section(const helix::PrinterDiscovery& hw);
    nlohmann::json build_hw_ams_section(const helix::PrinterDiscovery& hw) const;
    static nlohmann::json build_hw_macros_section(const helix::PrinterDiscovery& hw);

    /**
     * @brief Build a settings snapshot event JSON object
     * @return JSON event with type "settings_snapshot" and user configuration
     */
    nlohmann::json build_settings_snapshot_event() const;

    /**
     * @brief Build a panel usage summary event JSON object
     * @return JSON event with per-panel visit counts, time, and overlay count
     */
    nlohmann::json build_panel_usage_event() const;

    /**
     * @brief Build a connection stability summary event JSON object
     * @return JSON event with connect/disconnect counts and durations
     */
    nlohmann::json build_connection_stability_event() const;

    /**
     * @brief Build a performance snapshot event JSON object
     * @return JSON event with frame time stats, dropped frame count, and uptime
     */
    nlohmann::json build_performance_snapshot_event() const;

    /**
     * @brief Build a feature adoption event JSON object
     * @return JSON event listing which optional features are actively used
     */
    nlohmann::json build_feature_adoption_event() const;

    /**
     * @brief Build a settings changes event JSON object
     * @return JSON event with list of setting name/old/new value triples
     */
    nlohmann::json build_settings_changes_event() const;

    /**
     * @brief Shape an already-drained skip snapshot as an event JSON
     *
     * Pure with respect to the counters — the caller owns the drain, so
     * suppressing an event can never also swallow a window.
     */
    nlohmann::json
    build_async_lifetime_snapshot_event(const helix::async_lifetime::SkipSnapshot& snap) const;

    /// Create and arm the periodic snapshot LVGL timer
    void start_snapshot_timer();

    /// Delete the periodic snapshot LVGL timer (safe if nullptr)
    void stop_snapshot_timer();

    /** @brief Start the 5-minute frame performance snapshot timer */
    void start_frame_perf_timer();

    /** @brief Stop the frame performance snapshot timer */
    void stop_frame_perf_timer();

    /// Persist snapshot sequence counter to disk so it survives restarts
    void save_snapshot_state() const;

    /// Load snapshot sequence counter from disk (called from init())
    void load_snapshot_state();

    /// Create and arm the feature adoption delay LVGL timer
    void start_feature_adoption_timer();

    /// Delete the feature adoption delay LVGL timer (safe if nullptr)
    void stop_feature_adoption_timer();

    /// Create and arm the settings debounce LVGL timer
    void start_settings_debounce_timer();

    /// Delete the settings debounce LVGL timer (safe if nullptr)
    void stop_settings_debounce_timer();

    /**
     * @brief Build a print start context event JSON object
     * @return JSON event with print metadata (source, size bucket, slicer, etc.)
     */
    nlohmann::json build_print_start_context_event(const std::string& source, bool has_thumbnail,
                                                   int64_t file_size_bytes,
                                                   int estimated_duration_sec,
                                                   const std::string& slicer, int tool_count_used,
                                                   bool ams_active) const;

    /**
     * @brief Build an error encountered event JSON object
     * @return JSON event with error category, code, context, and uptime
     */
    nlohmann::json build_error_event(const std::string& category, const std::string& code,
                                     const std::string& context) const;

    /// Bucket a file size in bytes into a human-readable range string
    static std::string bucket_file_size(int64_t bytes);

    /// Bucket a duration in seconds into a human-readable range string
    static std::string bucket_duration(int sec);

    /**
     * @brief Get the double-hashed device identifier
     * @return Hex-encoded hashed device ID for inclusion in events
     */
    std::string get_hashed_device_id() const;

    /**
     * @brief Get current ISO 8601 timestamp
     * @return Timestamp string (e.g., "2026-02-08T12:00:00Z")
     */
    std::string get_timestamp() const;

    /**
     * @brief Ensure device UUID and salt exist, generating if needed
     *
     * On first run, generates a UUID v4 and random salt, then persists
     * them to the config directory. On subsequent runs, loads from disk.
     */
    void ensure_device_id();

    // =========================================================================
    // PERSISTENCE PATHS
    // =========================================================================

    /**
     * @brief Get filesystem path for the event queue file
     * @return Path to telemetry_queue.json in the config directory
     */
    std::string get_queue_path() const;

    /**
     * @brief Get filesystem path for the device identity file
     * @return Path to telemetry_device.json in the config directory
     */
    std::string get_device_id_path() const;

    // =========================================================================
    // STATE
    // =========================================================================

    /// Telemetry enabled flag (atomic for thread-safe reads from record_*)
    std::atomic<bool> enabled_{false};

    /// Whether init() has been called
    std::atomic<bool> initialized_{false};

    /// Whether shutdown() has been called (prevents new work)
    std::atomic<bool> shutting_down_{false};

    /// Set by notify_print_started_in_app(), consumed by print_start_context recording
    std::atomic<bool> print_started_in_app_{false};

    /// Timestamp of when init() was called (for uptime calculation)
    std::chrono::steady_clock::time_point init_time_{};

    // =========================================================================
    // DEVICE IDENTITY
    // =========================================================================

    /// Raw UUID v4, stored on disk, never transmitted
    std::string device_uuid_;

    /// Random salt for double-hashing, stored alongside UUID
    std::string device_salt_;

    // =========================================================================
    // EVENT QUEUE (mutex-protected)
    // =========================================================================

    /// Protects queue_, device_uuid_, device_salt_, error_rate_limit_
    mutable std::mutex mutex_;

    /// Pending events awaiting transmission
    std::vector<nlohmann::json> queue_;

    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    /// Directory for persistence files (queue, device ID, enabled state)
    std::string config_dir_;

    // =========================================================================
    // LVGL SUBJECT
    // =========================================================================

    /// Integer subject: 0 = disabled, 1 = enabled
    lv_subject_t enabled_subject_{};

    /// RAII cleanup for the enabled subject
    SubjectManager subjects_;

    /// Expires the deferred `enabled_subject_` write. Declared after `subjects_`
    /// so reverse-order member destruction invalidates it before the subject it
    /// protects; also invalidated by shutdown(), which is where the subject is
    /// actually torn down (#1165, #1146).
    helix::AsyncLifetimeGuard async_lifetime_;

    /// Guards against double-initialization of subjects
    bool subjects_initialized_{false};

    /// Set to true when crash.txt is suppressed due to update_success.json being present
    bool had_update_restart_ = false;

    /// True once a crash event for this session has been enqueued — via the
    /// opt-in boot path (check_previous_crash) or bundle consent
    /// (enqueue_crash_event_unconditional). Prevents a duplicate crash event.
    bool crash_event_queued_ = false;

    // =========================================================================
    // TRANSMISSION STATE (Phase 3)
    // =========================================================================

    /// Timestamp of last successful (or attempted) send
    std::chrono::steady_clock::time_point last_send_time_{};

    /// Exponential backoff multiplier (resets to 1 on success).
    /// Atomic: read on LVGL thread (try_send), written on send thread (do_send).
    std::atomic<int> backoff_multiplier_{1};

    /// Whether SSL cert availability has been checked (one-time on send thread)
    bool ssl_verified_{false};

    /// Whether sends are disabled (e.g., no CA cert bundle found)
    bool send_disabled_{false};

    /// Background thread for HTTP POST
    std::thread send_thread_;

    /// LVGL timer for periodic auto-send (nullptr when not active)
    lv_timer_t* auto_send_timer_{nullptr};

    /// Whether the initial delay has fired (switches to normal interval after)
    bool auto_send_initial_fired_{false};

    /// Whether start_auto_send() has been called (discovery complete).
    /// LVGL-thread-only (same as session trackers below).
    bool discovery_complete_{false};

    // =========================================================================
    // SESSION TRACKERS (panel usage + connection stability)
    // All accessed from LVGL/main thread only — no mutex needed.
    // notify_*() called via LVGL observers, record_*() called from shutdown().
    // =========================================================================

    // Panel usage tracking
    std::unordered_map<std::string, int> panel_time_sec_;
    std::unordered_map<std::string, int> panel_visits_;
    std::unordered_map<std::string, int> widget_interactions_;
    std::string current_panel_;
    std::chrono::steady_clock::time_point panel_start_time_;
    int overlay_open_count_{0};
    std::unordered_map<std::string, int> overlay_visits_;

    // Error rate limiting (max 1 event per category per 5 minutes).
    // Protected by mutex_ — accessed from background threads via record_error().
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> error_rate_limit_;
    static constexpr auto ERROR_RATE_LIMIT_INTERVAL = std::chrono::minutes{5};

    // Connection stability tracking
    int connect_count_{0};
    int disconnect_count_{0};
    int total_connected_sec_{0};
    int total_disconnected_sec_{0};
    int longest_disconnect_sec_{0};
    int klippy_error_count_{0};
    int klippy_shutdown_count_{0};
    bool connection_tracking_connected_{false};
    std::chrono::steady_clock::time_point connection_state_start_time_;

    // =========================================================================
    // PERIODIC SNAPSHOT STATE (LVGL/main thread only)
    // =========================================================================

    /// LVGL timer that fires every SNAPSHOT_INTERVAL_MS
    lv_timer_t* snapshot_timer_{nullptr};

    /// LVGL timer that fires every FRAME_PERF_INTERVAL_MS for frame performance snapshots
    lv_timer_t* frame_perf_timer_{nullptr};

    /// Monotonically increasing sequence number for snapshot events (persisted)
    int snapshot_seq_{0};

    /// True when fire_periodic_snapshot() is called from shutdown path
    bool is_shutdown_snapshot_{false};

    // =========================================================================
    // FRAME TIME RING BUFFER (LVGL/main thread only)
    // =========================================================================

    /// Single frame sample: render duration and which panel was active
    struct FrameSample {
        uint32_t frame_time_us;
        uint16_t panel_id;
    };

    /// Ring buffer of recent frame samples (wraps; FRAME_RING_SIZE entries)
    std::array<FrameSample, FRAME_RING_SIZE> frame_ring_{};

    /// Write index for the next sample (mod FRAME_RING_SIZE)
    size_t frame_ring_idx_{0};

    /// Number of valid samples currently in the ring (capped at FRAME_RING_SIZE)
    size_t frame_ring_count_{0};

    /// Ordered list of panel names for panel_id → name lookup
    std::vector<std::string> panel_names_;

    /// ID of the panel active at the time of the most recent frame sample
    uint16_t current_panel_id_{0};

    // =========================================================================
    // FEATURE ADOPTION STATE (LVGL/main thread only)
    // =========================================================================

    /// One-shot LVGL timer that fires after FEATURE_ADOPTION_DELAY_MS
    lv_timer_t* feature_adoption_timer_{nullptr};

    // =========================================================================
    // SETTINGS CHANGE DEBOUNCE STATE (LVGL/main thread only)
    // =========================================================================

    /// Pending per-setting change record for aggregated reporting
    struct SettingChange {
        std::string setting;
        std::string old_value;
        std::string new_value;
    };

    /// Accumulates changes while the debounce timer is running
    std::vector<SettingChange> pending_settings_changes_;

    /// LVGL timer reset on each notify_setting_changed() call; fires after SETTINGS_DEBOUNCE_MS
    lv_timer_t* settings_debounce_timer_{nullptr};
};
