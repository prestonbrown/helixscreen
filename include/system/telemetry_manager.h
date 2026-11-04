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
 * +-- Transmission (Phase 3: batched HTTPS POST to endpoint)
 * @endcode
 *
 * Thread safety:
 * - Event recording (record_session, record_print_outcome) is thread-safe
 *   and may be called from any thread.
 * - set_enabled() must be called from the main LVGL thread (it manages
 *   LVGL timers); enabled_ itself is atomic for reads from any thread.
 * - Transmission (try_send) runs on a background thread.
 *
 * Usage:
 * @code
 * auto& telemetry = TelemetryManager::instance();
 * telemetry.init("config");  // Load persisted state
 *
 * // User enables telemetry in settings UI (SystemSettingsManager calls this)
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
#include "json_fwd.h"
#include "lvgl.h"
#include "memory_monitor.h"
#include "screensaver_usage.h"
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

/**
 * @brief Stable telemetry id for an overlay
 *
 * The root object's name is the XML component's identity and does not change
 * with wording or translation; the lifecycle name is the fallback for roots
 * that carry none. Both missing or empty yields "unnamed".
 *
 * @param root_obj_name Overlay root's lv_obj name (may be null)
 * @param lifecycle_name IPanelLifecycle::get_name() (may be null)
 */
inline std::string overlay_telemetry_id(const char* root_obj_name, const char* lifecycle_name) {
    if (root_obj_name && *root_obj_name)
        return root_obj_name;
    if (lifecycle_name && *lifecycle_name)
        return lifecycle_name;
    return "unnamed";
}

/// Why an update failed, for an update_failed event. Every field here is
/// already safe to send: fixed tokens, a known release host or "other", and
/// text that went through helix::redact::telemetry_text().
struct UpdateFailureDetail {
    std::string transfer_error; ///< helix::tls::failure_kind_name() token
    int transfer_code = 0;      ///< TransferFailure::code; 0 when there is none
    std::string host;           ///< release host the transfer failed at, or "other"
    std::string install_detail; ///< scrubbed cause line from install.sh's output
};

} // namespace helix

/**
 * @brief Anonymous, opt-in telemetry manager
 *
 * Singleton that collects anonymous usage events and queues them for
 * batched transmission. Default state is OFF -- telemetry is only
 * active after explicit user opt-in via the settings UI.
 *
 * Events are persisted to disk so they survive restarts. The event
 * queue is capped at kMaxQueueSize; at the cap, delta events merge into
 * a queued one of the same kind and other events evict the oldest
 * non-delta event.
 */
class TelemetryManager {
  public:
    using UpdateFailureDetail = helix::UpdateFailureDetail;

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
     * @param detail Why a download or install failed; empty fields are omitted
     */
    void record_update_failure(const std::string& reason, const std::string& version,
                               const std::string& platform, int http_code = -1,
                               int64_t file_size = -1, int exit_code = -1,
                               const UpdateFailureDetail& detail = {});

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
     * @brief Enqueue a panel_usage delta and start a new window
     *
     * The event carries the per-panel visit counts, time, widget interactions
     * and overlay visits accumulated since this kind's previous delta, and
     * session_duration_sec is that window's length. The accumulators reset to
     * zero afterwards, so summing a boot's rows gives its totals.
     * No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (accesses session trackers).
     */
    void record_panel_usage();

    /**
     * @brief Enqueue a screensaver_usage delta and start a new window
     *
     * Per saver: seconds shown, starts, how runs ended and the gate level each settled at, read
     * from the source set_screensaver_usage() gave. A window with no saver activity, or no source,
     * enqueues nothing. No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (the source is written there).
     */
    void record_screensaver_usage();

    /**
     * @brief Where record_screensaver_usage() reads the screensaver windows
     *
     * The source must outlive this manager's use of it; nullptr detaches it. Main thread only.
     */
    void set_screensaver_usage(helix::SaverUsage* usage);

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
     * @param overlay_name Stable overlay id (see overlay_telemetry_id())
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
     * @param category Error category, a short identifier matching [a-z0-9_]{1,32}
     *                 (e.g. "moonraker_api", "websocket", "file_io", "display"); anything
     *                 else is dropped
     * @param code Error code ("timeout", "http_4xx", "http_5xx", "parse_error",
     * "connection_refused")
     * @param context Pre-defined context string (not user data)
     */
    void record_error(const std::string& category, const std::string& code,
                      const std::string& context);

    /**
     * @brief Enqueue a connection_stability delta and start a new window
     *
     * Counts and durations cover the window since this kind's previous delta.
     * An outage still open at the window's end keeps its start, so
     * longest_disconnect_sec measures it whole: to now while it is open, and
     * in full in the window where it closes.
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
     * Called automatically by the snapshot timer every kSnapshotIntervalMs.
     * No-op if telemetry is disabled.
     *
     * Must be called from the LVGL/main thread only (accesses frame ring buffer).
     */
    void record_performance_snapshot();

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
     * Accumulates setting changes with debouncing. After kSettingsDebounceMs of
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
     * @brief Emit this window's delta events and persist them
     *
     * Enqueues the panel_usage and connection_stability deltas under the
     * current snapshot_seq, advances it, then saves the queue and the (now
     * empty) accumulator so a power cut cannot lose or repeat the window.
     * No-op if telemetry is disabled.
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
     * Sends up to kMaxBatchSize events in a single HTTPS POST.
     * Respects kSendInterval between transmissions and uses exponential
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
     * Takes at most kMaxBatchSize events from the front of the queue
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

    // =========================================================================
    // CONSTANTS
    // =========================================================================

    /** @brief Maximum number of events in the queue before oldest are dropped */
    static constexpr size_t kMaxQueueSize = 100;

    /** @brief Delay before first auto-send attempt after startup */
    static constexpr uint32_t kInitialSendDelayMs = 60 * 1000; // 60 seconds

    /** @brief Interval between auto-send attempts */
    static constexpr uint32_t kAutoSendIntervalMs = 60 * 60 * 1000; // 1 hour

    /** @brief Schema version for event JSON structure */
    static constexpr int kSchemaVersion = 3;

    /** @brief HTTPS endpoint for telemetry submission */
    static constexpr const char* kEndpointUrl = "https://telemetry.helixscreen.org/v1/events";

    /** @brief Minimum interval between transmission attempts */
    static constexpr auto kSendInterval = std::chrono::hours{24};

    /** @brief Retry spacing after a failed send, multiplied by the backoff.
     *  A failure is spaced off this rather than kSendInterval so one unreachable
     *  moment costs hours instead of a day, while the backoff still applies. */
    static constexpr auto kRetryInterval = std::chrono::hours{1};

    /** @brief Ceiling on the backoff multiplier.
     *  Large enough that kRetryInterval * backoff still reaches the seven-day
     *  ceiling: a printer that never reaches the endpoint must end up retrying
     *  weekly, not every few hours. */
    static constexpr int kMaxBackoffMultiplier = 168;

    /** @brief Maximum events per HTTPS POST batch */
    static constexpr size_t kMaxBatchSize = 20;

    /** @brief Batches one send window will POST before giving the thread back.
     *  Must cover a full queue, or the drain cannot outrun the producers and
     *  the oldest events are discarded unsent. */
    static constexpr size_t kMaxBatchesPerSend = (kMaxQueueSize / kMaxBatchSize) + 1;

    /**
     * @brief Spacing required before the next transmission attempt.
     *
     * A backoff above 1 means the previous attempt failed. Those retries are
     * spaced off kRetryInterval rather than the full kSendInterval, and the
     * result is capped at seven days.
     *
     * @param backoff Current backoff multiplier (1 when the last send succeeded)
     * @return Minimum delay since the last attempt before another is allowed
     */
    static constexpr std::chrono::hours next_attempt_delay(int backoff) {
        auto interval = (backoff > 1) ? kRetryInterval * backoff : kSendInterval;
        constexpr auto max_interval = std::chrono::hours{24 * 7};
        return interval > max_interval ? max_interval : interval;
    }

    /** @brief Interval between periodic performance snapshots */
    static constexpr uint32_t kSnapshotIntervalMs = 4 * 60 * 60 * 1000;

    /** @brief Number of frame time samples held in the ring buffer */
    static constexpr size_t kFrameRingSize = 1024;

    /** @brief Frame time threshold above which a frame is considered dropped (33ms = ~30fps) */
    static constexpr uint32_t kDroppedFrameThresholdUs = 33000;

    /** @brief Frame time floor below which a frame is considered idle (no rendering work) */
    static constexpr uint32_t kIdleFrameThresholdUs = 500;

    /** @brief Interval between frame performance snapshots.
     *  The queue drains once per kSendInterval, so every periodic producer
     *  spends from one kMaxQueueSize budget for the whole window; this is the
     *  hottest of them and a shorter period pushes
     *  kUnattendedEventsPerSendWindow over that budget. The event is an
     *  aggregate over the last kFrameRingSize qualifying frames either way —
     *  the ring wraps well inside a window at any interactive frame rate — so a
     *  wider period costs resolution, not samples. */
    static constexpr uint32_t kFramePerfIntervalMs = 60 * 60 * 1000;

    /** @brief Debounce window for aggregating settings changes before recording */
    static constexpr uint32_t kSettingsDebounceMs = 30 * 1000;

    /** @brief kSendInterval in milliseconds, to compare against the timer periods */
    static constexpr uint32_t kSendIntervalMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kSendInterval).count());

    /**
     * @brief Events the periodic producers enqueue in one send window with
     *        nobody touching the device.
     *
     * The auto-send timer's hourly memory snapshot, the frame performance
     * snapshot, and the three events fire_periodic_snapshot() emits. The queue
     * is drained once per kSendInterval and holds kMaxQueueSize, so a total
     * above that is the count enqueue_event() discards unsent every day, on
     * every platform (prestonbrown/helixscreen#1476).
     */
    static constexpr size_t kUnattendedEventsPerSendWindow =
        kSendIntervalMs / kAutoSendIntervalMs + kSendIntervalMs / kFramePerfIntervalMs +
        3 * (kSendIntervalMs / kSnapshotIntervalMs);

  private:
    friend class TelemetryManagerTestAccess;

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
     * @brief Write the envelope keys every event carries.
     *
     * Sets schema_version, app_version, app_platform, app_board and boot_id.
     * Every build_*_event calls this first; a builder whose event describes a
     * version other than the running binary's overwrites app_version afterwards.
     */
    void stamp_envelope(nlohmann::json& event) const;

    /// 16 lowercase hex characters from the same RNG as generate_uuid_v4().
    static std::string generate_boot_id();

    /**
     * @brief Add an event to the queue (mutex-protected)
     *
     * At kMaxQueueSize a delta event (panel_usage, connection_stability)
     * merges into the newest queued one of its kind and boot that no send has
     * attempted; any other event evicts the oldest non-delta event. Delta
     * events are never evicted, since a lost delta is gone for good. Events in
     * the batch a send has in flight are never touched: the send removes them
     * by count.
     *
     * @param event JSON event object to enqueue
     */
    void enqueue_event(nlohmann::json event);

    /// Copy the next batch from the front of the queue and mark it in flight
    /// and attempted.
    nlohmann::json claim_batch();

    /// The body of build_batch() and claim_batch(). Caller holds mutex_.
    nlohmann::json front_batch_locked() const;

    /// Clear the in-flight mark once a send has finished with its batch.
    void release_in_flight();

    /// The hourly auto-send timer's work: send, memory snapshot, persistence.
    void on_auto_send_tick();

    /// The clock every session tracker reads: steady_clock, or the manual clock
    /// TelemetryManagerTestAccess sets so a test can cover hours without waiting.
    std::chrono::steady_clock::time_point tracker_now() const;

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
                                             int64_t file_size, int exit_code,
                                             const UpdateFailureDetail& detail = {}) const;

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

    /// The panel_usage fields accumulated since the window began, as of @p now.
    nlohmann::json panel_usage_delta(std::chrono::steady_clock::time_point now) const;

    /// The connection_stability fields accumulated since the window began, as
    /// of @p now. An open outage is measured from its real start.
    nlohmann::json connection_stability_delta(std::chrono::steady_clock::time_point now) const;

    /// The screensaver_usage fields of the source's window as of @p now; empty with no source.
    nlohmann::json screensaver_usage_delta(std::chrono::steady_clock::time_point now) const;

    /// Zero the panel accumulators and start a new window at @p now.
    void reset_panel_usage_window(std::chrono::steady_clock::time_point now);

    /// Zero the connection accumulators and start a new window at @p now.
    /// The open outage, if any, keeps its start.
    void reset_connection_window(std::chrono::steady_clock::time_point now);

    /// Envelope, identity and sequence around a delta payload of @p kind.
    nlohmann::json build_delta_event(const char* kind, const nlohmann::json& payload) const;

    /**
     * @brief Build a performance snapshot event JSON object
     * @return JSON event with frame time stats, dropped frame count, and uptime
     */
    nlohmann::json build_performance_snapshot_event() const;

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

    /// Persist the unsent accumulator with this boot's id and next
    /// snapshot_seq, so an unclean exit loses only the time since the save.
    void save_accumulator() const;

    /// Emit a previous boot's persisted accumulator as one delta per kind under
    /// that boot's id and next snapshot_seq, then delete the file. A delta the
    /// loaded queue already holds under the same key is not emitted twice.
    void recover_accumulator();

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

    /// 16 lowercase hex characters minted by init(), never persisted. Identifies
    /// one process lifetime so the server can tell a restart from a retry.
    std::string boot_id_;

    // =========================================================================
    // EVENT QUEUE (mutex-protected)
    // =========================================================================

    /// Protects queue_, events_dropped_since_send_, in_flight_, attempted_, device_uuid_,
    /// device_salt_, error_rate_limit_
    mutable std::mutex mutex_;

    /// Pending events awaiting transmission
    std::vector<nlohmann::json> queue_;

    /// Events enqueue_event() discarded since the last batch reached the
    /// server. Non-zero means the next batch under-reports its window.
    size_t events_dropped_since_send_ = 0;

    /// Leading queue_ entries a send has in flight. enqueue_event() leaves
    /// them alone, because the send removes them by count when it succeeds.
    size_t in_flight_ = 0;

    /// Leading queue_ entries a send has POSTed, or a previous process loaded,
    /// without a confirmed removal. The server may already hold them, so they
    /// are only ever resent as they are and never merged into. Cleared only by
    /// remove_sent_events(), never by a failed send.
    size_t attempted_ = 0;

    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    /// Directory for persistence files (queue, device ID, enabled state)
    std::string config_dir_;

    /// Set to true when crash.txt is suppressed due to update_success.json being present
    bool had_update_restart_ = false;

    /// True once a crash event for this session has been enqueued — via the
    /// opt-in boot path (check_previous_crash) or bundle consent
    /// (enqueue_crash_event_unconditional). Prevents a duplicate crash event.
    bool crash_event_queued_ = false;

    // =========================================================================
    // TRANSMISSION STATE (Phase 3)
    // =========================================================================

    /// Timestamp of last successful (or attempted) send. Only meaningful once
    /// send_attempted_ is set — steady_clock's epoch is boot, so a stamp can
    /// legitimately be negative and its sign says nothing about whether a send
    /// has happened.
    std::chrono::steady_clock::time_point last_send_time_{};

    /// Whether try_send() has ever cleared its gate. The first attempt has no
    /// interval to wait out.
    bool send_attempted_ = false;

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
    // notify_*() called via LVGL observers; record_*() from the snapshot timer
    // and shutdown(). Each holds only the window since that kind's last delta.
    // =========================================================================

    // Panel usage tracking
    std::unordered_map<std::string, int> panel_time_sec_;
    std::unordered_map<std::string, int> panel_visits_;
    std::unordered_map<std::string, int> widget_interactions_;
    std::string current_panel_;
    std::chrono::steady_clock::time_point panel_start_time_;
    int overlay_open_count_{0};
    std::unordered_map<std::string, int> overlay_visits_;

    // Screensaver runs, accumulated by ScreensaverManager.
    helix::SaverUsage* saver_usage_ = nullptr;

    // Error rate limiting (max 1 event per category per 5 minutes).
    // Protected by mutex_ — accessed from background threads via record_error().
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> error_rate_limit_;
    static constexpr auto kErrorRateLimitInterval = std::chrono::minutes{5};

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
    /// Start of the outage in progress. Never moved by a snapshot, so an
    /// outage spanning one is measured whole.
    std::chrono::steady_clock::time_point outage_start_time_;

    /// Start of the window the next delta of each kind covers.
    std::chrono::steady_clock::time_point panel_window_start_;
    std::chrono::steady_clock::time_point connection_window_start_;

    /// Set only by TelemetryManagerTestAccess. When present, tracker_now()
    /// returns it instead of steady_clock, so a window holds exactly the time a
    /// test steps and none of the real time the test spends running.
    std::optional<std::chrono::steady_clock::time_point> manual_clock_;

    // =========================================================================
    // PERIODIC SNAPSHOT STATE (LVGL/main thread only)
    // =========================================================================

    /// LVGL timer that fires every kSnapshotIntervalMs
    lv_timer_t* snapshot_timer_{nullptr};

    /// LVGL timer that fires every kFramePerfIntervalMs for frame performance snapshots
    lv_timer_t* frame_perf_timer_{nullptr};

    /// Sequence number of the next delta snapshot. Starts at 0 every boot;
    /// with device_id and boot_id it is the delta's idempotency key.
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

    /// Ring buffer of recent frame samples (wraps; kFrameRingSize entries)
    std::array<FrameSample, kFrameRingSize> frame_ring_{};

    /// Write index for the next sample (mod kFrameRingSize)
    size_t frame_ring_idx_{0};

    /// Qualifying frames recorded since the last snapshot; exceeds kFrameRingSize once the
    /// ring wraps, so the retained sample count is min(this, kFrameRingSize)
    size_t frame_ring_count_{0};

    /// Ordered list of panel names for panel_id → name lookup
    std::vector<std::string> panel_names_;

    /// ID of the panel active at the time of the most recent frame sample
    uint16_t current_panel_id_{0};

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

    /// LVGL timer reset on each notify_setting_changed() call; fires after kSettingsDebounceMs
    lv_timer_t* settings_debounce_timer_{nullptr};
};
