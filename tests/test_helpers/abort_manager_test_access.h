// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "abort_manager.h"
#include "printer_state.h"
#include "safety_settings_manager.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <string>

namespace helix {

// Friend of AbortManager: resets the singleton between tests and drives its
// private state-machine handlers directly.
class AbortManagerTestAccess {
  public:
    static void reset(AbortManager& m) {
        reset_state(m);
        m.lifetime_.invalidate();
        m.heater_interrupt_support_ = AbortManager::HeaterInterruptSupport::UNKNOWN;
        m.commands_sent_ = 0;
        m.api_ = nullptr;
        m.printer_state_ = nullptr;
    }

    static void reset_state(AbortManager& m) {
        m.cancel_all_timers();
        m.klippy_observer_.reset();
        m.cancel_state_observer_.reset();
        m.abort_state_ = AbortManager::State::IDLE;
        m.escalation_level_ = 0;
        m.shutdown_recovery_in_progress_ = false;
        m.seen_shutdown_during_reconnect_ = false;
        {
            std::lock_guard<std::mutex> lock(m.message_mutex_);
            m.last_result_message_.clear();
        }
        if (m.subjects_initialized_) {
            lv_subject_set_int(&m.abort_state_subject_,
                               static_cast<int>(AbortManager::State::IDLE));
            lv_subject_set_string(&m.progress_message_subject_, "");
            m.update_visibility();
        }
    }

    static void on_heater_interrupt_success(AbortManager& m) {
        m.on_heater_interrupt_success();
    }

    static void on_heater_interrupt_error(AbortManager& m) {
        m.on_heater_interrupt_error();
    }

    static void on_heater_interrupt_timeout(AbortManager& m) {
        m.on_heater_interrupt_timeout();
    }

    static void on_probe_response(AbortManager& m) {
        m.on_probe_response();
    }

    static void on_probe_timeout(AbortManager& m) {
        m.on_probe_timeout();
    }

    static void on_cancel_success(AbortManager& m) {
        m.on_cancel_success();
    }

    static void on_cancel_timeout(AbortManager& m) {
        m.on_cancel_timeout();
    }

    static void on_estop_sent(AbortManager& m) {
        m.on_estop_sent();
    }

    static void on_restart_sent(AbortManager& m,
                                RecoveryOutcome outcome = RecoveryOutcome::RESTART_ISSUED) {
        m.on_restart_sent(outcome);
    }

    static void send_firmware_restart(AbortManager& m) {
        m.send_firmware_restart();
    }

    static lv_timer_t* restart_timer(AbortManager& m) {
        return m.restart_timer_.get();
    }

    static void on_klippy_ready(AbortManager& m) {
        m.on_klippy_state_changed(KlippyState::READY);
    }

    static void on_reconnect_timeout(AbortManager& m) {
        m.reconnect_timer_.reset();
        m.complete_abort("Abort complete (reconnect timeout). Check printer status.");
    }

    static void on_klippy_state_change(AbortManager& m, KlippyState state) {
        m.on_klippy_state_changed(state);
    }

    static void set_heater_interrupt_support(AbortManager& m,
                                             AbortManager::HeaterInterruptSupport status) {
        m.heater_interrupt_support_ = status;
    }

    // The backdrop modal is created lazily by update_visibility() on the first
    // visible state.
    static lv_obj_t* backdrop(AbortManager& m) {
        return m.backdrop_;
    }

    static void set_state(AbortManager& m, AbortManager::State state) {
        m.abort_state_ = state;
    }

    static void call_update_visibility(AbortManager& m) {
        m.update_visibility();
    }

    static void on_print_state_during_cancel(AbortManager& m, PrintJobState state) {
        m.on_print_state_during_cancel(state);
    }

    static void on_api_error(AbortManager& m, const std::string& /* error */) {
        switch (m.abort_state_.load()) {
        case AbortManager::State::TRY_HEATER_INTERRUPT:
            m.on_heater_interrupt_error();
            break;
        case AbortManager::State::PROBE_QUEUE:
            m.on_probe_timeout();
            break;
        case AbortManager::State::SENT_CANCEL:
            if (SafetySettingsManager::instance().get_cancel_escalation_enabled()) {
                m.escalate_to_estop();
            } else {
                m.complete_abort("Cancel command failed. Use E-Stop if print continues.");
            }
            break;
        default:
            spdlog::warn("[AbortManager] API error in state {}", m.get_state_name());
            break;
        }
    }

    static void set_shutdown_recovery(AbortManager& m) {
        m.abort_state_.store(AbortManager::State::SENT_ESTOP);
        m.shutdown_recovery_in_progress_.store(true);
    }
};

} // namespace helix
