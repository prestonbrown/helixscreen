// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_recovery_service.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "spool_latch_gate.h"
#include "system/child_process.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>

namespace helix {

namespace {

// printer.firmware_restart returns code 503 when klippy_uds is disconnected
// (the klippy host crashed or never came up). That's the canonical signal to
// move on to step 2; everything else is either success or a transport error
// we should surface to the user.
bool firmware_restart_unrecoverable(const MoonrakerError& err) {
    // 503 — Klippy Host not connected (the most common case)
    if (err.code == 503)
        return true;
    // -32601 — Method not found (older Moonraker that doesn't expose it)
    if (err.code == -32601)
        return true;
    return false;
}

/// One recovery chain at a time, process-wide: two would race their restarts
/// and each would read the other's Klippy bounce as its own.
std::atomic<bool> g_chain_running{false};

void end_chain() {
    g_chain_running.store(false);
}

/// Run @p fn on the main thread once @p delay_ms has passed. Captures nothing
/// of a caller's object, so it outlives whatever started it.
void run_after_ms(uint32_t delay_ms, std::function<void()> fn) {
    auto* holder = new std::function<void()>(std::move(fn));
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            std::unique_ptr<std::function<void()>> owned(
                static_cast<std::function<void()>*>(lv_timer_get_user_data(t)));
            (*owned)();
        },
        delay_ms, holder);
    lv_timer_set_repeat_count(timer, 1);
}

} // namespace

/// One accepted firmware_restart being watched until Klippy settles. Main
/// thread only; shared by the poll's continuations, never by `this`.
struct PrinterRecoveryService::RestartVerification {
    IMoonrakerAPI* api;
    /// The Moonraker client's lifetime: `api` is only safe to call while it holds.
    std::weak_ptr<bool> alive;
    RecoveryTiming timing;
    SuccessCallback on_success;
    ErrorCallback on_error;
    /// When recover() was called: the chain's deadline counts from here.
    uint32_t chain_started_at = 0;
    /// When this round of polling began: the verify window counts from here.
    uint32_t round_started_at = 0;
    /// The local script already ran for this chain; it never runs twice.
    bool script_ran = false;

    static void start(const std::shared_ptr<RestartVerification>& self) {
        self->round_started_at = lv_tick_get();
        self->schedule_poll(self);
    }

    void schedule_poll(const std::shared_ptr<RestartVerification>& self) {
        run_after_ms(timing.poll_interval_ms, [self]() { self->poll(self); });
    }

    bool abandoned() {
        if (!alive.expired())
            return false;
        spdlog::info("[Recovery] Moonraker client gone — abandoning the restart check");
        end_chain();
        return true;
    }

    void poll(const std::shared_ptr<RestartVerification>& self) {
        if (abandoned())
            return;
        api->get_klippy_status(
            [self](const KlippyStatus& status) {
                ui::queue_update("PrinterRecoveryService::verify(status)",
                                 [self, status]() { self->on_status(self, status); });
            },
            [self](const MoonrakerError& err) {
                ui::queue_update("PrinterRecoveryService::verify(error)", [self, err]() {
                    self->on_status(self, KlippyStatus{"unreachable", err.message});
                });
            });
    }

    void on_status(const std::shared_ptr<RestartVerification>& self, const KlippyStatus& status) {
        if (abandoned())
            return;
        const auto state = parse_klippy_state(status.state);
        if (state == KlippyState::READY) {
            spdlog::info("[Recovery] Klippy ready after restart");
            on_success(RecoveryOutcome::KLIPPY_READY);
            return;
        }
        const uint32_t chain_elapsed = lv_tick_elaps(chain_started_at);
        if (chain_elapsed >= timing.chain_deadline_ms) {
            spdlog::warn("[Recovery] Klippy not ready {}ms into recovery (state '{}') — leaving "
                         "its state to the printer status",
                         chain_elapsed, status.state);
            on_success(RecoveryOutcome::RESTART_ISSUED);
            return;
        }
        if (state == KlippyState::ERROR) {
            spdlog::warn("[Recovery] Klippy failed to start after restart: {}",
                         status.state_message);
            escalate(self);
            return;
        }
        // A Klippy still answering `startup` is booting slowly, not failing.
        const uint32_t elapsed = lv_tick_elaps(round_started_at);
        if (state != KlippyState::STARTUP && elapsed >= timing.verify_window_ms) {
            spdlog::warn("[Recovery] Klippy not ready {}ms after restart (state '{}')", elapsed,
                         status.state);
            escalate(self);
            return;
        }
        spdlog::debug("[Recovery] Waiting for Klippy after restart: state '{}' at {}ms",
                      status.state, elapsed);
        schedule_poll(self);
    }

    void escalate(const std::shared_ptr<RestartVerification>& self) {
        if (script_ran) {
            spdlog::warn("[Recovery] Klippy still not ready after helix-recover.sh");
            on_success(RecoveryOutcome::RESTART_ISSUED);
            return;
        }
        if (!local_recovery_available()) {
            spdlog::info("[Recovery] No local helix-recover.sh — leaving Klippy's state to the "
                         "printer status");
            on_success(RecoveryOutcome::RESTART_ISSUED);
            return;
        }
        script_ran = true;
        run_local_recovery(
            timing.script_timeout_ms,
            [self](RecoveryOutcome) {
                spdlog::info("[Recovery] helix-recover.sh done — verifying Klippy comes back");
                start(self);
            },
            on_error);
    }
};

bool PrinterRecoveryService::local_recovery_available() {
    const auto path = local_recovery_script_path();
    if (path.empty())
        return false;
    return ::access(path.c_str(), X_OK) == 0;
}

std::string PrinterRecoveryService::local_recovery_script_path() {
    if (const std::string& override_path = detail::local_recovery_script_override_ref();
        !override_path.empty())
        return override_path;
    const std::string root = app_get_install_root();
    if (root.empty())
        return "";
    return root + "/bin/helix-recover.sh";
}

void PrinterRecoveryService::run_local_recovery(uint32_t timeout_ms, SuccessCallback on_success,
                                                ErrorCallback on_error) {
    const std::string script = local_recovery_script_path();
    if (script.empty() || ::access(script.c_str(), X_OK) != 0) {
        spdlog::info("[Recovery] local script not present ({}) — skipping local exec",
                     script.empty() ? "no install root" : script);
        on_error(MoonrakerError{MoonrakerErrorType::VALIDATION_ERROR,
                                -1,
                                "Local recovery script not installed",
                                "helix_recover_local",
                                {}});
        return;
    }

    spdlog::info("[Recovery] Running local recovery script: {}", script);
    // Slow lane: this can block 30s+ on K2's klipper_mcu bounce. Fast lane
    // workers are scarce and meant for REST round-trips.
    http::HttpExecutor::slow().submit([script, timeout_ms, on_success, on_error]() {
        pid_t pid = ::fork();
        if (pid < 0) {
            const int e = errno;
            spdlog::error("[Recovery] fork() for helix-recover.sh failed: {}", strerror(e));
            ui::queue_update("PrinterRecoveryService::run_local_recovery(fork-fail)", [on_error,
                                                                                       e]() {
                on_error(MoonrakerError{MoonrakerErrorType::UNKNOWN,
                                        -1,
                                        std::string("Local recovery fork failed: ") + strerror(e),
                                        "helix_recover_local",
                                        {}});
            });
            return;
        }
        if (pid == 0) {
            // Child: exec the script. Argv0 is the script itself; no other args.
            // We don't redirect stderr — let the script's output go to whatever
            // the parent inherited (typically the syslog if launched under the
            // SysV init script, console if run standalone).
            const char* argv[] = {script.c_str(), nullptr};
            ::execv(script.c_str(), const_cast<char* const*>(argv));
            _exit(127); // execv only returns on failure
        }

        // Parent: wait on the slow-lane worker, bounded. A script that never
        // exits would hold the recovery chain, and with it every later
        // recovery, for the life of the process.
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        constexpr auto kTermGrace = std::chrono::milliseconds(500);
        const ChildWaitResult wait = terminate_and_reap(
            pid, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms),
            kTermGrace, kPollInterval);
        if (wait.outcome == ChildWaitOutcome::TimedOut) {
            spdlog::error("[Recovery] helix-recover.sh still running after {}ms — killed it",
                          timeout_ms);
            ui::queue_update(
                "PrinterRecoveryService::run_local_recovery(timeout)", [on_error, timeout_ms]() {
                    on_error(MoonrakerError{MoonrakerErrorType::TIMEOUT,
                                            -1,
                                            "Local recovery script timed out after " +
                                                std::to_string(timeout_ms / 1000) + "s",
                                            "helix_recover_local",
                                            {}});
                });
            return;
        }
        if (wait.outcome == ChildWaitOutcome::WaitFailed) {
            const int e = wait.wait_errno;
            spdlog::error("[Recovery] waitpid() failed: {}", strerror(e));
            ui::queue_update("PrinterRecoveryService::run_local_recovery(wait-fail)", [on_error,
                                                                                       e]() {
                on_error(MoonrakerError{MoonrakerErrorType::UNKNOWN,
                                        -1,
                                        std::string("Local recovery wait failed: ") + strerror(e),
                                        "helix_recover_local",
                                        {}});
            });
            return;
        }

        const int rc = wait.exit_code();
        const bool ok = rc == 0;
        spdlog::info("[Recovery] helix-recover.sh exit: status={} (rc={})", ok ? "ok" : "FAILED",
                     rc);
        ui::queue_update(
            "PrinterRecoveryService::run_local_recovery(done)", [ok, rc, on_success, on_error]() {
                if (ok) {
                    on_success(RecoveryOutcome::RESTART_ISSUED);
                } else {
                    on_error(MoonrakerError{
                        MoonrakerErrorType::UNKNOWN,
                        rc,
                        "Local recovery script returned non-zero (rc=" + std::to_string(rc) + ")",
                        "helix_recover_local",
                        {}});
                }
            });
    });
}

void PrinterRecoveryService::recover(SuccessCallback on_success, ErrorCallback on_error) {
    if (!api_) {
        spdlog::error("[Recovery] No Moonraker API — cannot recover");
        on_error(MoonrakerError::connection_lost("printer_recovery"));
        return;
    }
    // Refused here, before the chain: a refused firmware_restart would
    // otherwise escalate to the local script and a service restart, which
    // release the steppers just the same (prestonbrown/helixscreen#1730).
    if (get_printer_state().print_state().spool_latch_active()) {
        spdlog::warn("[Recovery] Refusing firmware restart while spools are on the bed");
        on_error(
            MoonrakerError::refusal("printer.firmware_restart", spool_latch_restart_message()));
        return;
    }

    if (g_chain_running.exchange(true)) {
        spdlog::warn("[Recovery] A recovery is already running — refusing a second one");
        on_error(
            MoonrakerError::refusal("printer_recovery", TR_NOOP("Operation already in progress")));
        return;
    }
    // Every terminal path goes through these two, so the chain always ends.
    on_success = [cb = std::move(on_success)](RecoveryOutcome outcome) {
        end_chain();
        cb(outcome);
    };
    on_error = [cb = std::move(on_error)](const MoonrakerError& err) {
        end_chain();
        cb(err);
    };

    // [L072] Capture api_ by value, not `this`. The recovery service is a
    // short-lived holder owned by a widget that may be destroyed between click
    // and ack; IMoonrakerAPI lives as long as MoonrakerManager (well past any
    // widget). Caller's on_success/on_error captures are the caller's problem.
    // Note: run_local_recovery and local_recovery_available are static, so the
    // continuation chain never needs `this`.
    IMoonrakerAPI* api = api_;

    auto run_services_restart = [api, on_success, on_error]() {
        spdlog::info("[Recovery] Falling back to machine.services.restart klipper");
        api->restart_service(
            "klipper", [on_success]() { on_success(RecoveryOutcome::RESTART_ISSUED); },
            [on_error](const MoonrakerError& err) {
                spdlog::warn("[Recovery] services.restart klipper failed: {} (code={})",
                             err.message, err.code);
                on_error(err);
            });
    };

    auto run_local_then_services = [script_timeout = timing_.script_timeout_ms, on_success,
                                    run_services_restart]() {
        if (!local_recovery_available()) {
            spdlog::info("[Recovery] No local helix-recover.sh — skipping to services.restart");
            run_services_restart();
            return;
        }
        run_local_recovery(
            script_timeout, on_success, [run_services_restart](const MoonrakerError& err) {
                spdlog::warn("[Recovery] local recovery failed: {} — trying services.restart",
                             err.message);
                run_services_restart();
            });
    };

    auto verify_restart = [api, alive = api->client_lifetime_weak(), timing = timing_,
                           chain_start = lv_tick_get(), on_success, on_error]() {
        spdlog::info("[Recovery] firmware_restart accepted — verifying Klippy comes back ready");
        // The first poll waits one interval: until Klippy actually drops,
        // printer.info can still report the state from before the restart.
        ui::queue_update("PrinterRecoveryService::verify(start)", [api, alive, timing, chain_start,
                                                                   on_success, on_error]() {
            auto verification = std::make_shared<RestartVerification>(
                RestartVerification{api, alive, timing, on_success, on_error, chain_start});
            if (!verification->abandoned())
                RestartVerification::start(verification);
        });
    };

    // Step 1: printer.firmware_restart. This is what users see in the
    // "Restarting firmware..." toast — try it first because on a healthy
    // klippy it's the fastest path. If klippy is dead, the 503 short-circuits
    // us to step 2. We log the distinction between "expected dead path" (503,
    // -32601) and unexpected failures, but escalate identically — the user
    // clicked "restart" and we should exhaust the chain before giving up.
    spdlog::info("[Recovery] Trying printer.firmware_restart");
    api_->restart_firmware(verify_restart, [run_local_then_services](const MoonrakerError& err) {
        if (firmware_restart_unrecoverable(err)) {
            spdlog::info("[Recovery] firmware_restart not viable "
                         "(code={} msg='{}') — escalating",
                         err.code, err.message);
        } else {
            spdlog::warn("[Recovery] firmware_restart hard-failed: {} — "
                         "escalating anyway",
                         err.message);
        }
        run_local_then_services();
    });
}

} // namespace helix
