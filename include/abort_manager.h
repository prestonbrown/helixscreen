// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "moonraker_job_api.h"
#include "printer_recovery_service.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <mutex>
#include <string>

namespace helix {
class IMoonrakerAPI;

/**
 * @brief Smart print cancellation manager with progressive escalation
 *
 * Manages print abort operations using a state machine that progressively
 * tries softer abort methods before resorting to M112 emergency stop:
 *
 * 1. TRY_HEATER_INTERRUPT - Try soft interrupt where supported (1s timeout)
 * 2. PROBE_QUEUE - Send M115 to test if queue is responsive (2s timeout)
 * 3. SENT_CANCEL - Queue responsive, send printer.print.cancel RPC (configurable timeout)
 * 4. SENT_ESTOP - Queue blocked or cancel failed, send M112
 * 5. SENT_RESTART - Send FIRMWARE_RESTART after M112
 * 6. WAITING_RECONNECT - Wait for klippy_state == READY (30s timeout)
 *
 * State Machine:
 * ```
 * IDLE -> TRY_HEATER_INTERRUPT -> PROBE_QUEUE -> SENT_CANCEL -> COMPLETE
 *                                       |              |
 *                                 SENT_ESTOP <--------+
 *                                       |
 *                                 SENT_RESTART
 *                                       |
 *                              WAITING_RECONNECT
 *                                       |
 *                                   COMPLETE
 * ```
 *
 * Thread Safety:
 * - State is stored as std::atomic<State> for safe reads from any thread
 * - API callbacks reach the main thread through lifetime_.bg_cb()
 * - Callbacks from Moonraker WebSocket run on background thread
 *
 * Usage:
 * @code
 * // At startup (after PrinterState init):
 * AbortManager::instance().init(api, &printer_state);
 * AbortManager::instance().init_subjects();
 *
 * // When user requests abort:
 * AbortManager::instance().start_abort();
 *
 * // At shutdown:
 * AbortManager::instance().deinit_subjects();
 * @endcode
 */
class AbortManager {
  public:
    /**
     * @brief State machine states for abort process
     */
    enum class State {
        IDLE,                 ///< Not aborting, ready for new abort request
        TRY_HEATER_INTERRUPT, ///< Sending HEATER_INTERRUPT
        PROBE_QUEUE,          ///< Sending M115 to check if G-code queue is responsive
        SENT_CANCEL,          ///< Queue responsive, printer.print.cancel RPC sent
        SENT_ESTOP,           ///< Queue blocked or cancel failed, M112 sent
        SENT_RESTART,         ///< FIRMWARE_RESTART sent after M112
        WAITING_RECONNECT,    ///< Waiting for klippy_state to become READY
        COMPLETE              ///< Abort finished (success or after recovery)
    };

    /**
     * @brief Whether the firmware accepts HEATER_INTERRUPT (resolved from discovery at first abort)
     */
    enum class HeaterInterruptSupport {
        UNKNOWN,    ///< Not yet resolved (discovery not available)
        SUPPORTED,  ///< Discovery reports HEATER_INTERRUPT support
        UNSUPPORTED ///< Firmware without HEATER_INTERRUPT
    };

    /**
     * @brief Get singleton instance
     * @return Reference to the global AbortManager instance
     */
    static AbortManager& instance();

    /**
     * @brief Initialize with dependencies
     *
     * Must be called before start_abort(). Sets up references to API
     * and printer state for operation.
     *
     * @param api Pointer to IMoonrakerAPI for G-code execution
     * @param state Pointer to PrinterState for klippy_state observation
     */
    void init(IMoonrakerAPI* api, PrinterState* state);

    /**
     * @brief Initialize subjects for XML binding
     *
     * Registers the abort_state and progress_message subjects used by XML binding.
     * Must be called during subject initialization phase (before XML creation).
     */
    void init_subjects();

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     */
    void deinit_subjects();

    /**
     * @brief Start the abort process
     *
     * Begins the progressive abort state machine. If already aborting,
     * this call is ignored. State transitions are:
     * - Resolves HeaterInterruptSupport from printer.info "app" field if UNKNOWN
     * - If SUPPORTED -> TRY_HEATER_INTERRUPT (soft interrupt for M109 waits)
     * - If UNSUPPORTED or UNKNOWN -> PROBE_QUEUE (skip heater interrupt)
     *
     * @p entry starts further in when the caller already knows what the queue
     * is doing. Cancel sends printer.print.cancel at once: the caller has made
     * the wait holding the queue end (or the firmware ends it on a cancel), so
     * a probe would only stall behind it and escalate for nothing.
     * EmergencyStop goes straight to M112 + FIRMWARE_RESTART, for a wait that
     * nothing else ends; the caller confirms it with the user first.
     */
    enum class Entry { Probe, Cancel, EmergencyStop };
    void start_abort(Entry entry = Entry::Probe);

    /**
     * @brief Check if abort is currently in progress
     * @return true if state is not IDLE
     */
    bool is_aborting() const;

    /**
     * @brief Check if manager is in idle state
     * @return true if state is IDLE
     */
    bool is_idle() const;

    /**
     * @brief Check if AbortManager is handling a controlled shutdown
     *
     * Returns true when AbortManager has initiated an M112 emergency stop
     * and is managing the recovery. This flag persists until klippy returns
     * to READY state, even after the state machine reaches COMPLETE.
     * Used to suppress the global "Printer Shutdown" recovery dialog.
     *
     * @return true if currently handling M112/restart recovery
     */
    bool is_handling_shutdown() const;

    /**
     * @brief Get current state machine state
     * @return Current State enum value
     */
    State get_state() const;

    /**
     * @brief Get HEATER_INTERRUPT support status
     * @return Current HeaterInterruptSupport enum value
     */
    HeaterInterruptSupport get_heater_interrupt_support() const;

    /**
     * @brief Get state name as string for debugging
     * @return Human-readable state name
     */
    std::string get_state_name() const;

    /**
     * @brief Get current progress message for UI display
     * @return Progress message string describing current operation
     */
    std::string get_progress_message() const;

    /**
     * @brief Get result message from last completed abort
     * @return Result message (success, escalation info, or error)
     */
    std::string last_result_message() const;

    /**
     * @brief Get escalation level from last abort
     *
     * Returns 0 for soft cancel, >0 if M112 was required.
     * @return Escalation level (0 = no escalation)
     */
    int escalation_level() const;

    /**
     * @brief Get number of commands sent (for testing)
     * @return Count of G-code commands sent since init/reset
     */
    int get_commands_sent_count() const;

    /**
     * @brief Get abort state subject for UI binding
     *
     * Integer subject holding State enum value for XML bindings.
     * @return Pointer to state subject
     */
    lv_subject_t* get_abort_state_subject();

    /**
     * @brief Get progress message subject for UI binding
     *
     * String subject with current progress message for display.
     * @return Pointer to progress message subject
     */
    lv_subject_t* get_progress_message_subject();

    // ========================================================================
    // Timeout Constants (public for testing)
    // ========================================================================

    static constexpr uint32_t kHeaterInterruptTimeoutMs = 1000;                     ///< 1 second
    static constexpr uint32_t kProbeTimeoutMs = 2000;                               ///< 2 seconds
    static constexpr uint32_t kCancelTimeoutMs = MoonrakerJobAPI::kCancelTimeoutMs; ///< 5 minutes
    static constexpr uint32_t kCancelNudgeMs = 15000;                               ///< 15 seconds
    /// Wait window for klippy_state to reach READY after the recovery service
    /// returns. The recovery service can take 30s+ on K2 (klipper_mcu bounce),
    /// but that's handled async — this timer only covers the gap between
    /// "script returned" and "klippy reports READY". 30s gives platforms with
    /// slow MCU re-enumeration (CFS rediscovery, RS-485 bus settle) enough
    /// headroom without leaving the user staring at "Restarting..." forever.
    static constexpr uint32_t kReconnectTimeoutMs = 30000; ///< 30 seconds
    /// Backstop for the recovery service itself. The chain's worst case is its
    /// deadline (polling stops), plus a script started just before it running
    /// to its bound, plus one poll interval and one printer.info request
    /// timing out after that: 120s + 90s + 2s + 60s = 272s with the defaults.
    /// This sits above that, so it fires only when the chain never reports back;
    /// the static_assert in abort_manager.cpp keeps the two in step.
    static constexpr uint32_t kRestartTimeoutMs = 300000; ///< 5 minutes

  private:
    friend class AbortManagerTestAccess;

    AbortManager() = default;
    ~AbortManager() = default;

    // Non-copyable
    AbortManager(const AbortManager&) = delete;
    AbortManager& operator=(const AbortManager&) = delete;

    // Dependencies (set via init())
    IMoonrakerAPI* api_ = nullptr;
    PrinterState* printer_state_ = nullptr;

    // State machine
    std::atomic<State> abort_state_{State::IDLE};
    std::atomic<HeaterInterruptSupport> heater_interrupt_support_{HeaterInterruptSupport::UNKNOWN};
    std::atomic<int> escalation_level_{0};
    std::atomic<int> commands_sent_{0};

    // Result message from last abort (protected by mutex for thread safety)
    mutable std::mutex message_mutex_;
    std::string last_result_message_;

    // Subject for UI binding
    lv_subject_t abort_state_subject_{};
    lv_subject_t progress_message_subject_{};
    char progress_message_buf_[128]{};
    bool subjects_initialized_ = false;

    // Modal backdrop + dialog (created on lv_layer_top() for overlay behavior)
    helix::ui::WidgetRef backdrop_;

    // Observer for klippy state changes during WAITING_RECONNECT
    ObserverGuard klippy_observer_;

    // Observer for print_state_enum changes during SENT_CANCEL
    ObserverGuard cancel_state_observer_;

    // RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    // Persistent flag: set when M112 sent, cleared when klippy returns to READY
    // Used to suppress "Printer Shutdown" dialog even after state machine completes
    std::atomic<bool> shutdown_recovery_in_progress_{false};

    // Flag to track if we've seen SHUTDOWN state during WAITING_RECONNECT
    // This prevents completing immediately when observer fires with stale READY value
    std::atomic<bool> seen_shutdown_during_reconnect_{false};

    // Timeout timers (RAII — auto-deleted on destruction or reassignment)
    helix::ui::LvglTimerGuard heater_interrupt_timer_;
    helix::ui::LvglTimerGuard probe_timer_;
    helix::ui::LvglTimerGuard cancel_timer_;
    helix::ui::LvglTimerGuard reconnect_timer_;
    helix::ui::LvglTimerGuard restart_timer_;

    // Invalidated by start_abort(), so replies from an earlier sequence are dropped
    helix::AsyncLifetimeGuard lifetime_;

    // ========================================================================
    // State Machine Transitions
    // ========================================================================

    /**
     * @brief Try HEATER_INTERRUPT command to probe for HEATER_INTERRUPT support
     */
    void try_heater_interrupt();

    /**
     * @brief Start probing G-code queue with M115
     */
    void start_probe();

    /**
     * @brief Send cancel via Moonraker printer.print.cancel RPC
     */
    void send_cancel_print();

    /**
     * @brief Escalate to M112 emergency stop
     */
    void escalate_to_estop();

    /**
     * @brief Send FIRMWARE_RESTART after M112
     */
    void send_firmware_restart();

    /**
     * @brief Enter wait state for klippy reconnection
     */
    void wait_for_reconnect();

    /**
     * @brief Complete the abort process
     * @param message Result message for UI
     */
    void complete_abort(const char* message);

    // ========================================================================
    // Internal Callbacks
    // ========================================================================

    void on_heater_interrupt_success();
    void on_heater_interrupt_error();
    void on_heater_interrupt_timeout();
    void on_probe_response();
    void on_probe_timeout();
    void on_cancel_success();
    void on_cancel_timeout();
    void on_estop_sent();
    void on_restart_sent(RecoveryOutcome outcome);
    void on_klippy_state_changed(KlippyState klippy_state);

    // ========================================================================
    // Helper Methods
    // ========================================================================

    /**
     * @brief Update state and notify observers
     * @param new_state New state to transition to
     */
    void set_state(State new_state);

    /**
     * @brief Update progress message subject
     * @param message New progress message
     */
    void set_progress_message(const char* message);

    /**
     * @brief Cancel all pending timers
     */
    void cancel_all_timers();

    /**
     * @brief Create the abort progress modal on lv_layer_top()
     */
    void create_modal();

    /**
     * @brief Update modal visibility based on current state
     */
    void update_visibility();

    /**
     * @brief Handle print state changes during SENT_CANCEL phase
     *
     * Terminal states (STANDBY, CANCELLED, COMPLETE, ERROR) complete the abort.
     * Non-terminal states (PRINTING, PAUSED) are ignored.
     */
    void on_print_state_during_cancel(PrintJobState state);

    // Static timer callbacks
    static void heater_interrupt_timer_cb(lv_timer_t* timer);
    static void probe_timer_cb(lv_timer_t* timer);
    static void cancel_timer_cb(lv_timer_t* timer);
    static void reconnect_timer_cb(lv_timer_t* timer);
    static void restart_timer_cb(lv_timer_t* timer);
};

} // namespace helix
