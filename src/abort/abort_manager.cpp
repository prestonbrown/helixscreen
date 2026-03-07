// SPDX-License-Identifier: GPL-3.0-or-later

#include "abort_manager.h"

#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {

// ============================================================================
// Singleton Instance
// ============================================================================

AbortManager& AbortManager::instance() {
    static AbortManager instance;
    return instance;
}

// ============================================================================
// Initialization
// ============================================================================

void AbortManager::init(MoonrakerAPI* api, PrinterState* state) {
    api_ = api;
    printer_state_ = state;
    spdlog::debug("[AbortManager] Initialized with dependencies");
}

void AbortManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Register XML component for the modal
    lv_xml_register_component_from_file("A:ui_xml/abort_progress_modal.xml");

    // Register E-Stop button callback (unique name per L039)
    register_xml_callbacks({
        {"on_abort_emergency_stop",
         [](lv_event_t*) {
             spdlog::warn("[AbortManager] E-Stop button pressed from abort modal");
             AbortManager::instance().escalate_to_estop();
         }},
    });

    // Initialize state subject (default IDLE)
    UI_MANAGED_SUBJECT_INT(abort_state_subject_, static_cast<int>(State::IDLE), "abort_state",
                           subjects_);

    // Initialize progress message subject
    UI_MANAGED_SUBJECT_STRING(progress_message_subject_, progress_message_buf_, "",
                              "abort_progress_message", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "AbortManagerSubjects", []() { helix::AbortManager::instance().deinit_subjects(); });

    spdlog::debug("[AbortManager] Subjects initialized");

    // Create modal on lv_layer_top() after subjects are ready
    create_modal();
}

void AbortManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Cancel any pending timers
    cancel_all_timers();

    // Clear klippy observer before deinitializing subjects
    // (cancel_state_observer_ already handled by cancel_all_timers() above)
    klippy_observer_.reset();

    // Delete backdrop (and its child dialog) if it exists
    // (Display may already be deleted if window was closed via X button)
    helix::ui::safe_delete(backdrop_);

    // Deinitialize all subjects via RAII manager
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::trace("[AbortManager] Subjects deinitialized");
}

// ============================================================================
// Public API
// ============================================================================

void AbortManager::start_abort() {
    // Ignore if already aborting
    if (is_aborting()) {
        spdlog::debug("[AbortManager] Already aborting, ignoring start_abort()");
        return;
    }

    spdlog::info("[AbortManager] Starting abort sequence");
    escalation_level_ = 0;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        last_result_message_.clear();
    }

    // Decide starting state based on cached Kalico status
    if (kalico_status_ == KalicoStatus::NOT_PRESENT) {
        // Skip HEATER_INTERRUPT, go directly to PROBE_QUEUE
        spdlog::debug("[AbortManager] Kalico NOT_PRESENT cached, skipping to PROBE_QUEUE");
        set_state(State::PROBE_QUEUE);
        set_progress_message("Stopping print...");
        start_probe();
    } else {
        // Unknown or DETECTED - try HEATER_INTERRUPT
        // For DETECTED, we still send it as a soft interrupt (helps with M109 waits)
        spdlog::debug("[AbortManager] Trying HEATER_INTERRUPT (Kalico status: {})",
                      kalico_status_ == KalicoStatus::UNKNOWN ? "UNKNOWN" : "DETECTED");
        set_state(State::TRY_HEATER_INTERRUPT);
        set_progress_message("Stopping print...");
        try_heater_interrupt();
    }
}

bool AbortManager::is_aborting() const {
    State current = abort_state_.load();
    return current != State::IDLE && current != State::COMPLETE;
}

bool AbortManager::is_idle() const {
    return abort_state_.load() == State::IDLE;
}

bool AbortManager::is_handling_shutdown() const {
    // Check persistent flag first - this persists even after state machine completes
    if (shutdown_recovery_in_progress_.load()) {
        return true;
    }
    // Also check states for completeness
    State current = abort_state_.load();
    return current == State::SENT_ESTOP || current == State::SENT_RESTART ||
           current == State::WAITING_RECONNECT;
}

AbortManager::State AbortManager::get_state() const {
    return abort_state_.load();
}

AbortManager::KalicoStatus AbortManager::get_kalico_status() const {
    return kalico_status_.load();
}

std::string AbortManager::get_state_name() const {
    switch (abort_state_.load()) {
    case State::IDLE:
        return "IDLE";
    case State::TRY_HEATER_INTERRUPT:
        return "TRY_HEATER_INTERRUPT";
    case State::PROBE_QUEUE:
        return "PROBE_QUEUE";
    case State::SENT_CANCEL:
        return "SENT_CANCEL";
    case State::SENT_ESTOP:
        return "SENT_ESTOP";
    case State::SENT_RESTART:
        return "SENT_RESTART";
    case State::WAITING_RECONNECT:
        return "WAITING_RECONNECT";
    case State::COMPLETE:
        return "COMPLETE";
    default:
        return "UNKNOWN";
    }
}

std::string AbortManager::get_progress_message() const {
    return std::string(progress_message_buf_);
}

std::string AbortManager::last_result_message() const {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return last_result_message_;
}

int AbortManager::escalation_level() const {
    return escalation_level_.load();
}

int AbortManager::get_commands_sent_count() const {
    return commands_sent_.load();
}

lv_subject_t* AbortManager::get_abort_state_subject() {
    return &abort_state_subject_;
}

lv_subject_t* AbortManager::get_progress_message_subject() {
    return &progress_message_subject_;
}

// ============================================================================
// State Machine Transitions
// ============================================================================

void AbortManager::try_heater_interrupt() {
    // If no API, just stay in TRY_HEATER_INTERRUPT state
    // Tests will call on_heater_interrupt_*() via AbortManagerTestAccess to progress
    if (!api_) {
        spdlog::debug("[AbortManager] No API, waiting for test callback in TRY_HEATER_INTERRUPT");
        return;
    }

    commands_sent_++;

    // Start timeout timer
    heater_interrupt_timer_ =
        lv_timer_create(heater_interrupt_timer_cb, HEATER_INTERRUPT_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(heater_interrupt_timer_, 1);

    // Send HEATER_INTERRUPT G-code
    api_->execute_gcode(
        "HEATER_INTERRUPT",
        [this]() {
            // Success callback - Kalico detected
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_heater_interrupt_success();
                },
                this);
        },
        [this](const MoonrakerError& err) {
            // Error callback - likely "Unknown command"
            spdlog::debug("[AbortManager] HEATER_INTERRUPT error: {}", err.message);
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_heater_interrupt_error();
                },
                this);
        });
}

void AbortManager::start_probe() {
    // If no API, just stay in PROBE_QUEUE state
    // Tests will call on_probe_*() via AbortManagerTestAccess to progress
    if (!api_) {
        spdlog::debug("[AbortManager] No API, waiting for test callback in PROBE_QUEUE");
        return;
    }

    commands_sent_++;

    // Start timeout timer
    probe_timer_ = lv_timer_create(probe_timer_cb, PROBE_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(probe_timer_, 1);

    // Send M115 to probe the queue
    api_->execute_gcode(
        "M115",
        [this]() {
            // Success callback - queue is responsive
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_probe_response();
                },
                this);
        },
        [this](const MoonrakerError& /* err */) {
            // Error callback - treat as timeout/blocked
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_probe_timeout();
                },
                this);
        });
}

void AbortManager::send_cancel_print() {
    // Register observer on print_state_enum to detect when Klipper reports print ended
    // This allows early completion before the timeout when the CANCEL_PRINT macro finishes
    if (printer_state_) {
        cancel_state_observer_ = helix::ui::observe_int_immediate<AbortManager>(
            printer_state_->get_print_state_enum_subject(), this,
            [](AbortManager* self, int value) {
                self->on_print_state_during_cancel(static_cast<PrintJobState>(value));
            });
        spdlog::debug("[AbortManager] Registered print_state_enum observer for cancel detection");
    }

    // If no API, just stay in SENT_CANCEL state
    // Tests will call on_cancel_*() via AbortManagerTestAccess to progress
    if (!api_) {
        spdlog::debug("[AbortManager] No API, waiting for test callback in SENT_CANCEL");
        return;
    }

    commands_sent_++;

    // Start timeout timer
    bool escalation_enabled = SafetySettingsManager::instance().get_cancel_escalation_enabled();
    if (escalation_enabled) {
        uint32_t timeout_ms =
            static_cast<uint32_t>(
                SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds()) *
            1000;
        spdlog::info("[AbortManager] Cancel escalation enabled, timeout: {}ms", timeout_ms);
        cancel_timer_ = lv_timer_create(cancel_timer_cb, timeout_ms, this);
        lv_timer_set_repeat_count(cancel_timer_, 1);
    } else {
        // Escalation disabled — still create a nudge timer so user gets feedback
        spdlog::info("[AbortManager] Cancel escalation disabled, nudge timer at {}ms",
                     CANCEL_TIMEOUT_MS);
        cancel_timer_ = lv_timer_create(cancel_timer_cb, CANCEL_TIMEOUT_MS, this);
        lv_timer_set_repeat_count(cancel_timer_, 1);
    }

    // Send CANCEL_PRINT
    api_->execute_gcode(
        "CANCEL_PRINT",
        [this]() {
            // Success callback
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_cancel_success();
                },
                this);
        },
        [this](const MoonrakerError& err) {
            spdlog::warn("[AbortManager] CANCEL_PRINT error: {}", err.message);
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    if (self->abort_state_ != State::SENT_CANCEL) {
                        return;
                    }
                    bool esc =
                        SafetySettingsManager::instance().get_cancel_escalation_enabled();
                    if (esc) {
                        self->on_cancel_timeout();
                    } else {
                        spdlog::warn(
                            "[AbortManager] CANCEL_PRINT failed, escalation disabled");
                        self->complete_abort(
                            "Cancel command failed. Use E-Stop if print continues.");
                    }
                },
                this);
        });
}

void AbortManager::escalate_to_estop() {
    spdlog::warn("[AbortManager] Escalating to M112 emergency stop");
    escalation_level_++;

    // Set persistent flag - will be cleared when we see klippy READY
    // This suppresses the "Printer Shutdown" dialog during recovery
    shutdown_recovery_in_progress_.store(true);

    cancel_all_timers();
    set_state(State::SENT_ESTOP);
    set_progress_message("Emergency stopping...");

    // If no API, just stay in SENT_ESTOP state
    // Tests will call on_estop_sent() via AbortManagerTestAccess to progress
    if (!api_) {
        spdlog::debug("[AbortManager] No API, waiting for test callback in SENT_ESTOP");
        return;
    }

    commands_sent_++;

    // Send M112 emergency stop
    api_->emergency_stop(
        [this]() {
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_estop_sent();
                },
                this);
        },
        [this](const MoonrakerError& /* err */) {
            // Even on error, proceed to restart (M112 may have worked)
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_estop_sent();
                },
                this);
        });
}

void AbortManager::send_firmware_restart() {
    spdlog::info("[AbortManager] Sending FIRMWARE_RESTART");
    set_state(State::SENT_RESTART);
    set_progress_message("Restarting...");

    // If no API, just stay in SENT_RESTART state
    // Tests will call on_restart_sent() via AbortManagerTestAccess to progress
    if (!api_) {
        spdlog::debug("[AbortManager] No API, waiting for test callback in SENT_RESTART");
        return;
    }

    commands_sent_++;

    // Send FIRMWARE_RESTART
    api_->restart_firmware(
        [this]() {
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_restart_sent();
                },
                this);
        },
        [this](const MoonrakerError& /* err */) {
            // Even on error, proceed to wait for reconnect
            helix::ui::async_call(
                [](void* user_data) {
                    auto* self = static_cast<AbortManager*>(user_data);
                    self->on_restart_sent();
                },
                this);
        });
}

void AbortManager::wait_for_reconnect() {
    spdlog::info("[AbortManager] Waiting for klippy reconnection");

    // Reset flag - we need to see SHUTDOWN before accepting READY
    // This prevents the observer's initial callback (with stale READY) from completing immediately
    seen_shutdown_during_reconnect_.store(false);

    set_state(State::WAITING_RECONNECT);
    set_progress_message("Restarting...");

    // Start reconnect timeout timer
    reconnect_timer_ = lv_timer_create(reconnect_timer_cb, RECONNECT_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(reconnect_timer_, 1);

    // Register observer on klippy_state subject to detect when klippy becomes ready
    if (printer_state_) {
        klippy_observer_ = helix::ui::observe_int_immediate<AbortManager>(
            printer_state_->get_klippy_state_subject(), this, [](AbortManager* self, int value) {
                self->on_klippy_state_changed(static_cast<KlippyState>(value));
            });
        spdlog::debug("[AbortManager] Registered klippy_state observer for reconnect detection");
    }
}

void AbortManager::complete_abort(const char* message) {
    spdlog::info("[AbortManager] Abort complete: {}", message);
    cancel_all_timers();

    // Clear klippy observer since we're no longer waiting for reconnect
    klippy_observer_.reset();

    // Set print outcome to CANCELLED for UI badge display
    // Moonraker reports "standby" after M112+restart, not "cancelled"
    if (printer_state_) {
        helix::ui::async_call(
            [](void* user_data) {
                auto* state = static_cast<PrinterState*>(user_data);
                state->set_print_outcome(PrintOutcome::CANCELLED);
            },
            printer_state_);
    }

    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        last_result_message_ = message;
    }
    set_state(State::COMPLETE);
    set_progress_message(message);
}

// ============================================================================
// Internal Callbacks
// ============================================================================

void AbortManager::on_heater_interrupt_success() {
    if (abort_state_ != State::TRY_HEATER_INTERRUPT) {
        return; // Stale callback
    }

    // Cancel timeout timer
    if (heater_interrupt_timer_) {
        lv_timer_delete(heater_interrupt_timer_);
        heater_interrupt_timer_ = nullptr;
    }

    spdlog::info("[AbortManager] Kalico detected (HEATER_INTERRUPT succeeded)");
    kalico_status_ = KalicoStatus::DETECTED;

    // Proceed to PROBE_QUEUE
    set_state(State::PROBE_QUEUE);
    set_progress_message("Stopping print...");
    start_probe();
}

void AbortManager::on_heater_interrupt_error() {
    if (abort_state_ != State::TRY_HEATER_INTERRUPT) {
        return; // Stale callback
    }

    // Cancel timeout timer
    if (heater_interrupt_timer_) {
        lv_timer_delete(heater_interrupt_timer_);
        heater_interrupt_timer_ = nullptr;
    }

    spdlog::info("[AbortManager] Kalico NOT present (HEATER_INTERRUPT failed)");
    kalico_status_ = KalicoStatus::NOT_PRESENT;

    // Proceed to PROBE_QUEUE
    set_state(State::PROBE_QUEUE);
    set_progress_message("Stopping print...");
    start_probe();
}

void AbortManager::on_heater_interrupt_timeout() {
    if (abort_state_ != State::TRY_HEATER_INTERRUPT) {
        return; // Stale callback
    }

    heater_interrupt_timer_ = nullptr;

    spdlog::warn("[AbortManager] HEATER_INTERRUPT timed out, treating as not-Kalico");
    kalico_status_ = KalicoStatus::NOT_PRESENT;

    // Proceed to PROBE_QUEUE
    set_state(State::PROBE_QUEUE);
    set_progress_message("Stopping print...");
    start_probe();
}

void AbortManager::on_probe_response() {
    if (abort_state_ != State::PROBE_QUEUE) {
        return; // Stale callback
    }

    // Cancel timeout timer
    if (probe_timer_) {
        lv_timer_delete(probe_timer_);
        probe_timer_ = nullptr;
    }

    spdlog::info("[AbortManager] Queue responsive, sending CANCEL_PRINT");
    set_state(State::SENT_CANCEL);
    set_progress_message("Stopping print...");
    send_cancel_print();
}

void AbortManager::on_probe_timeout() {
    if (abort_state_ != State::PROBE_QUEUE) {
        return; // Stale callback
    }

    probe_timer_ = nullptr;

    bool escalation_enabled = SafetySettingsManager::instance().get_cancel_escalation_enabled();
    if (escalation_enabled) {
        spdlog::warn("[AbortManager] Queue blocked (M115 timed out), escalating to ESTOP");
        escalate_to_estop();
    } else {
        spdlog::warn("[AbortManager] Queue blocked (M115 timed out), escalation disabled — "
                     "sending CANCEL_PRINT anyway");
        set_state(State::SENT_CANCEL);
        set_progress_message("Stopping print...");
        send_cancel_print();
    }
}

void AbortManager::on_cancel_success() {
    if (abort_state_ != State::SENT_CANCEL) {
        return; // Stale callback
    }

    // Cancel timeout timer
    if (cancel_timer_) {
        lv_timer_delete(cancel_timer_);
        cancel_timer_ = nullptr;
    }

    // Note: cancel_state_observer_ is cleaned up by complete_abort() → cancel_all_timers()

    spdlog::info("[AbortManager] CANCEL_PRINT succeeded");
    complete_abort("Print cancelled");
}

void AbortManager::on_cancel_timeout() {
    if (abort_state_ != State::SENT_CANCEL) {
        return; // Stale callback
    }

    cancel_timer_ = nullptr;

    bool escalation_enabled = SafetySettingsManager::instance().get_cancel_escalation_enabled();
    if (escalation_enabled) {
        // Note: cancel_state_observer_ is cleaned up by escalate_to_estop() → cancel_all_timers()
        spdlog::warn("[AbortManager] CANCEL_PRINT timed out, escalating to ESTOP");
        escalate_to_estop();
    } else {
        spdlog::info("[AbortManager] Cancel taking a while, escalation disabled — showing nudge");
        set_progress_message("Cancelling is taking a while.\nUse E-Stop to stop immediately.");
    }
}

void AbortManager::on_estop_sent() {
    if (abort_state_ != State::SENT_ESTOP) {
        return; // Stale callback
    }

    spdlog::info("[AbortManager] M112 sent, sending FIRMWARE_RESTART");
    send_firmware_restart();
}

void AbortManager::on_restart_sent() {
    if (abort_state_ != State::SENT_RESTART) {
        return; // Stale callback
    }

    spdlog::info("[AbortManager] FIRMWARE_RESTART sent, waiting for reconnect");
    wait_for_reconnect();
}

void AbortManager::on_klippy_state_changed(KlippyState klippy_state) {
    if (abort_state_ != State::WAITING_RECONNECT) {
        return; // Not waiting for reconnect
    }

    // Track when we've seen SHUTDOWN - this prevents completing immediately
    // when the observer fires with a stale READY value on registration
    if (klippy_state == KlippyState::SHUTDOWN) {
        spdlog::debug("[AbortManager] Observed SHUTDOWN state during reconnect wait");
        seen_shutdown_during_reconnect_.store(true);
        return;
    }

    if (klippy_state == KlippyState::READY) {
        // Only complete if we've actually seen SHUTDOWN first
        // (prevents completing from observer's initial callback with stale READY)
        if (!seen_shutdown_during_reconnect_.load()) {
            spdlog::debug(
                "[AbortManager] Ignoring READY - haven't seen SHUTDOWN yet (stale value)");
            return;
        }

        // Cancel reconnect timer
        if (reconnect_timer_) {
            lv_timer_delete(reconnect_timer_);
            reconnect_timer_ = nullptr;
        }

        // Clear the persistent shutdown recovery flag - we've seen READY after SHUTDOWN
        shutdown_recovery_in_progress_.store(false);
        spdlog::debug("[AbortManager] Cleared shutdown_recovery_in_progress flag");

        spdlog::info("[AbortManager] Klippy READY after SHUTDOWN, abort complete");
        complete_abort("Print aborted. Home before resuming.");
    }
    // For other states (STARTUP, ERROR), continue waiting
}

void AbortManager::on_print_state_during_cancel(PrintJobState state) {
    if (abort_state_ != State::SENT_CANCEL) {
        return; // Not in cancel phase, ignore
    }

    // Terminal states indicate the print has ended — cancel worked
    switch (state) {
    case PrintJobState::STANDBY:
    case PrintJobState::CANCELLED:
    case PrintJobState::COMPLETE:
    case PrintJobState::ERROR:
        spdlog::info("[AbortManager] Print state {} during cancel — completing abort",
                     print_job_state_to_string(state));

        // complete_abort() → cancel_all_timers() handles timer + observer cleanup
        complete_abort("Print cancelled");
        break;

    case PrintJobState::PRINTING:
    case PrintJobState::PAUSED:
        // Non-terminal — cancel macro is still running, keep waiting
        spdlog::debug("[AbortManager] Print state {} during cancel — still waiting",
                      print_job_state_to_string(state));
        break;
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

void AbortManager::set_state(State new_state) {
    State old_state = abort_state_.exchange(new_state);
    spdlog::debug("[AbortManager] State: {} -> {}", static_cast<int>(old_state),
                  static_cast<int>(new_state));

    // Update subject for UI binding
    if (subjects_initialized_) {
        lv_subject_set_int(&abort_state_subject_, static_cast<int>(new_state));
        update_visibility();
    }
}

void AbortManager::set_progress_message(const char* message) {
    std::strncpy(progress_message_buf_, message, sizeof(progress_message_buf_) - 1);
    progress_message_buf_[sizeof(progress_message_buf_) - 1] = '\0';

    if (subjects_initialized_) {
        lv_subject_copy_string(&progress_message_subject_, progress_message_buf_);
    }
}

void AbortManager::cancel_all_timers() {
    if (heater_interrupt_timer_) {
        lv_timer_delete(heater_interrupt_timer_);
        heater_interrupt_timer_ = nullptr;
    }
    if (probe_timer_) {
        lv_timer_delete(probe_timer_);
        probe_timer_ = nullptr;
    }
    if (cancel_timer_) {
        lv_timer_delete(cancel_timer_);
        cancel_timer_ = nullptr;
    }
    if (reconnect_timer_) {
        lv_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }

    // Also clean up cancel state observer (may be active during SENT_CANCEL)
    cancel_state_observer_.reset();
}

void AbortManager::create_modal() {
    if (backdrop_) {
        spdlog::warn("[AbortManager] Modal already exists - skipping creation");
        return;
    }

    // Create fullscreen backdrop on lv_layer_top() so it survives screen changes
    // (Same pattern as Modal::show() but targeting lv_layer_top() instead of lv_screen_active())
    // Opacity 200 matches modal_backdrop_opacity in globals.xml
    backdrop_ = helix::ui::create_fullscreen_backdrop(lv_layer_top(), 200);
    if (!backdrop_) {
        spdlog::error("[AbortManager] Failed to create backdrop on lv_layer_top()");
        return;
    }

    // Create XML dialog component inside backdrop
    lv_obj_t* dialog =
        static_cast<lv_obj_t*>(lv_xml_create(backdrop_, "abort_progress_modal", nullptr));
    if (!dialog) {
        spdlog::error("[AbortManager] Failed to create abort_progress_modal");
        lv_obj_del(backdrop_);
        backdrop_ = nullptr;
        return;
    }

    // Start hidden — update_visibility() will show when abort begins
    lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_HIDDEN);
    spdlog::debug("[AbortManager] Modal created on lv_layer_top() (hidden)");
}

void AbortManager::update_visibility() {
    if (!backdrop_) {
        return;
    }

    // Modal is visible when state is not IDLE and not COMPLETE
    State current = abort_state_.load();
    bool visible = (current != State::IDLE && current != State::COMPLETE);
    if (visible) {
        lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_HIDDEN);
    }
    spdlog::debug("[AbortManager] Visibility updated: {}", visible ? "visible" : "hidden");
}

// ============================================================================
// Static Timer Callbacks
// ============================================================================

void AbortManager::heater_interrupt_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<AbortManager*>(lv_timer_get_user_data(timer));
    if (self) {
        self->heater_interrupt_timer_ = nullptr; // Timer auto-deletes after one-shot
        self->on_heater_interrupt_timeout();
    }
}

void AbortManager::probe_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<AbortManager*>(lv_timer_get_user_data(timer));
    if (self) {
        self->probe_timer_ = nullptr;
        self->on_probe_timeout();
    }
}

void AbortManager::cancel_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<AbortManager*>(lv_timer_get_user_data(timer));
    if (self) {
        self->cancel_timer_ = nullptr;
        self->on_cancel_timeout();
    }
}

void AbortManager::reconnect_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<AbortManager*>(lv_timer_get_user_data(timer));
    if (self) {
        self->reconnect_timer_ = nullptr;
        // Timeout without reconnect - still complete (with warning message)
        self->complete_abort("Abort complete (reconnect timeout). Check printer status.");
    }
}

} // namespace helix
