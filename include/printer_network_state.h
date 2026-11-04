// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <cstdint>
#include <lvgl.h>
#include <mutex>
#include <optional>
#include <string>

// Forward declare ConnectionState and KlippyState (defined in moonraker_client.h and
// printer_state.h)
namespace helix {
enum class ConnectionState;
}
namespace helix {
enum class KlippyState;
}

namespace helix {

/// Whether the Moonraker this connection talks to runs on this host. Unknown
/// until the CONNECTED edge of the current connection settles it, and again
/// after any other connection edge or a printer switch, so a verdict from an
/// earlier connection is never read as this one's.
enum class MoonrakerLocality { Unknown = 0, Local = 1, Remote = 2 };

/**
 * @brief Manages network and connection state subjects for Moonraker connectivity
 *
 * Tracks WebSocket connection state to Moonraker, network connectivity status,
 * and Klipper firmware state. Also maintains a derived nav_buttons_enabled
 * subject that combines connection and klippy state for UI gating.
 *
 * Extracted from PrinterState as part of god class decomposition.
 *
 * Subjects (9 total):
 * - printer_connection_state_ (int) - ConnectionState enum values
 * - printer_connection_message_ (string, 128-byte buffer) - status message
 * - network_status_ (int) - NetworkStatus enum values
 * - klippy_state_ (int) - KlippyState enum values
 * - nav_buttons_enabled_ (int, derived) - 1 when connected AND klippy ready
 * - moonraker_connection_state_ (int, derived) - 1 when Moonraker WebSocket connected
 * - moonraker_is_remote_ (int) - 1 when the connected Moonraker is NOT this host
 * - moonraker_locality_ (int) - MoonrakerLocality for the current connection
 *
 * Additional state:
 * - was_ever_connected_ (bool) - tracks if ever successfully connected this session
 *
 * @note The was_ever_connected_ flag persists across resets - it tracks session lifetime.
 */
class PrinterNetworkState {
  public:
    PrinterNetworkState();
    ~PrinterNetworkState() = default;

    // Non-copyable
    PrinterNetworkState(const PrinterNetworkState&) = delete;
    PrinterNetworkState& operator=(const PrinterNetworkState&) = delete;

    /**
     * @brief Initialize network state subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    // ========================================================================
    // Setters
    // ========================================================================

    /**
     * @brief Set printer connection state (synchronous, must be on UI thread)
     *
     * This is a synchronous setter intended to be called from within
     * helix::ui::queue_update() by PrinterState, which handles the async dispatch.
     *
     * @param state ConnectionState enum value (0-4)
     * @param message Status message ("Connecting...", "Ready", "Disconnected", etc.)
     */
    void set_printer_connection_state_internal(int state, const char* message);

    /**
     * @brief Set network connectivity status
     *
     * @param status NetworkStatus enum value (0=disconnected, 1=connecting, 2=connected)
     */
    void set_network_status(int status);

    /**
     * @brief Set Klipper firmware state (synchronous, must be on UI thread)
     *
     * This is a synchronous setter intended to be called from within
     * helix::ui::queue_update() by PrinterState, which handles the async dispatch.
     *
     * @param state KlippyState enum value
     * @return true if the state actually changed (a genuine transition), false if
     *         it already held @p state. Callers use the edge to invalidate state
     *         that a Klipper restart makes meaningless (#1129).
     */
    bool set_klippy_state_internal(KlippyState state);

    /**
     * @brief Set Klipper state message (error/shutdown reason from webhooks)
     * @param message The state_message string from Moonraker webhooks
     * @param describes The klippy state the message was reported with, when known.
     *        A message reported with READY or STARTUP is never a fault reason.
     */
    void set_klippy_state_message(const std::string& message,
                                  std::optional<KlippyState> describes = std::nullopt);

    /**
     * @brief Apply a webhooks status object unless it is older than the state held
     *
     * Two signals gate it: a replayed snapshot never overrides a state a live
     * source set, and an eventtime below the watermark is an out-of-order frame.
     * state_message is gated with the state. Main thread only.
     *
     * @param frame_epoch klippy_epoch() when the frame was received; a frame from
     *        before the last reset_klippy_state_freshness() does not move the watermark
     * @return true when the klippy state changed
     */
    bool apply_webhooks(const nlohmann::json& webhooks, double eventtime, bool from_cached_snapshot,
                        std::optional<uint64_t> frame_epoch);

    /// A live source set the state: replayed snapshots cannot override it from here on.
    /// Any thread.
    void mark_klippy_state_live() {
        klippy_state_from_live_.store(true);
    }

    bool klippy_state_from_live() const {
        return klippy_state_from_live_.load();
    }

    /// Start a new connection session: clears the watermark and the live latch and
    /// advances klippy_epoch(). Synchronous, any thread.
    void reset_klippy_state_freshness();

    /// Which connection session a status frame belongs to.
    uint64_t klippy_epoch() const {
        return klippy_epoch_.load();
    }

    /**
     * @brief Set remote-screen verdict (synchronous, must be on UI thread)
     *
     * Published by MoonrakerManager on each CONNECTED edge from the live
     * websocket endpoint. 1 = the Moonraker we are talking to does not
     * resolve to this host (remote screen); 0 = local/unknown.
     */
    void set_moonraker_is_remote_internal(bool remote);

    /// Set the moonraker_locality subject (synchronous, must be on UI thread).
    void set_moonraker_locality_internal(MoonrakerLocality locality);

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /// Printer connection state (0=disconnected, 1=connecting, 2=connected, 3=reconnecting,
    /// 4=failed)
    lv_subject_t* get_printer_connection_state_subject() {
        return &printer_connection_state_;
    }

    /// Status message string (128-byte buffer)
    lv_subject_t* get_printer_connection_message_subject() {
        return &printer_connection_message_;
    }

    /// Network connectivity (0=disconnected, 1=connecting, 2=connected)
    lv_subject_t* get_network_status_subject() {
        return &network_status_;
    }

    /// Klipper firmware state (0=ready, 1=startup, 2=shutdown, 3=error)
    lv_subject_t* get_klippy_state_subject() {
        return &klippy_state_;
    }

    /// Combined nav button enabled state (1 when connected AND klippy ready, else 0)
    lv_subject_t* get_nav_buttons_enabled_subject() {
        return &nav_buttons_enabled_;
    }

    /// Moonraker WebSocket connected (1 when WebSocket is up, independent of Klipper state)
    lv_subject_t* get_moonraker_connection_state_subject() {
        return &moonraker_connection_state_;
    }

    /// Remote-screen verdict (1 = connected Moonraker is not this host; 0 = local/unknown)
    lv_subject_t* get_moonraker_is_remote_subject() {
        return &moonraker_is_remote_;
    }

    /// Locality verdict for the current connection (MoonrakerLocality: 0 Unknown,
    /// 1 Local, 2 Remote)
    lv_subject_t* get_moonraker_locality_subject() {
        return &moonraker_locality_;
    }

    /// Klipper state message (error/shutdown reason from webhooks, e.g. "Max force exceeded...")
    const std::string& get_klippy_state_message() const {
        return klippy_state_message_;
    }

    /// The state message when it can describe a shutdown or error: empty when it
    /// was reported alongside READY or STARTUP ("Printer is ready"), which can
    /// still be cached when a shutdown edge lands ahead of its webhooks frame.
    std::string get_klippy_fault_message() const;

    /// Bumped whenever the state message changes, so a view showing it can refresh.
    lv_subject_t* get_klippy_state_message_seq_subject() {
        return &klippy_state_message_seq_;
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    /**
     * @brief Check if printer has ever connected this session
     *
     * Returns true if we've successfully connected to Moonraker at least once.
     * Used to distinguish "never connected" (gray icon) from "disconnected after
     * being connected" (yellow warning icon).
     *
     * @return true if ever connected this session
     */
    bool was_ever_connected() const {
        return was_ever_connected_;
    }

    /**
     * @brief Update derived connection subjects (nav_buttons_enabled + moonraker_connection_state)
     *
     * Recalculates nav_buttons_enabled (connected AND klippy ready) and
     * moonraker_connection_state (WebSocket connected, independent of Klipper).
     * Called whenever printer_connection_state or klippy_state changes.
     * Public so PrinterState can call it when needed.
     */
    void update_nav_buttons_enabled();

  private:
    friend class PrinterNetworkStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Network state subjects
    lv_subject_t printer_connection_state_{};   // Integer: ConnectionState enum values
    lv_subject_t printer_connection_message_{}; // String buffer
    lv_subject_t network_status_{};             // Integer: NetworkStatus enum values
    lv_subject_t klippy_state_{};               // Integer: KlippyState enum values
    lv_subject_t nav_buttons_enabled_{};        // Derived: 1 when connected AND klippy ready
    lv_subject_t moonraker_connection_state_{}; // Derived: 1 when Moonraker WebSocket connected
    lv_subject_t moonraker_is_remote_{};        // 1 when connected Moonraker is not this host
    lv_subject_t moonraker_locality_{};         // MoonrakerLocality for this connection
    lv_subject_t klippy_state_message_seq_{};   // Bumped on every state message change

    // String buffer for connection message
    char printer_connection_message_buf_[128];

    // Klipper state message (error/shutdown reason from webhooks.state_message)
    std::string klippy_state_message_;
    std::optional<KlippyState> klippy_state_message_describes_;

    // Track if we've ever successfully connected (for UI display)
    bool was_ever_connected_ = false;

    /// Highest Klipper eventtime that has carried a webhooks klippy state. Klipper
    /// derives it from the monotonic clock, so it survives a Klipper restart and
    /// only rewinds on a host reboot. Written from the WebSocket thread (reset)
    /// and the main thread (apply_webhooks); the mutex guards only the eventtime
    /// and is never held across anything else, so an observer can call back in.
    double klippy_state_eventtime_ = 0.0;
    std::mutex klippy_freshness_mutex_;

    /// Session counter behind klippy_epoch(). A frame stamped with an older
    /// value was received before the last reset.
    std::atomic<uint64_t> klippy_epoch_{0};

    /// True once a live-sourced klippy state has been applied. Latches the state
    /// against replayed snapshots (discovery re-dispatches its subscription
    /// response at the end of discovery) while still allowing that same snapshot
    /// to SEED the state when nothing live has arrived yet, which is the normal
    /// cold-start ordering.
    std::atomic<bool> klippy_state_from_live_{false};

    /// Last unrecognised webhooks.state string, so the warning fires once per
    /// distinct value rather than once per status frame. Main thread only.
    std::string last_unknown_klippy_state_;
};

} // namespace helix
