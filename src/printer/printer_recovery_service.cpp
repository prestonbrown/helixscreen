// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_recovery_service.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "http_executor.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>
#include <sys/wait.h>
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

} // namespace

bool PrinterRecoveryService::local_recovery_available() {
    const auto path = local_recovery_script_path();
    if (path.empty())
        return false;
    return ::access(path.c_str(), X_OK) == 0;
}

std::string PrinterRecoveryService::local_recovery_script_path() {
    const std::string root = app_get_install_root();
    if (root.empty())
        return "";
    return root + "/bin/helix-recover.sh";
}

void PrinterRecoveryService::run_local_recovery(SuccessCallback on_success,
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
    http::HttpExecutor::slow().submit([script, on_success, on_error]() {
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

        // Parent: wait synchronously on the slow-lane worker. waitpid() blocks
        // here, but that's exactly what HttpExecutor::slow() is for.
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) {
            const int e = errno;
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

        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        spdlog::info("[Recovery] helix-recover.sh exit: status={} (rc={})", ok ? "ok" : "FAILED",
                     rc);
        ui::queue_update(
            "PrinterRecoveryService::run_local_recovery(done)", [ok, rc, on_success, on_error]() {
                if (ok) {
                    on_success();
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

    // [L072] Capture api_ by value, not `this`. The recovery service is a
    // short-lived holder owned by a widget that may be destroyed between click
    // and ack; IMoonrakerAPI lives as long as MoonrakerManager (well past any
    // widget). Caller's on_success/on_error captures are the caller's problem.
    // Note: run_local_recovery and local_recovery_available are static, so the
    // continuation chain never needs `this`.
    IMoonrakerAPI* api = api_;

    auto run_services_restart = [api, on_success, on_error]() {
        spdlog::info("[Recovery] Falling back to machine.services.restart klipper");
        api->restart_service("klipper", on_success, [on_error](const MoonrakerError& err) {
            spdlog::warn("[Recovery] services.restart klipper failed: {} (code={})", err.message,
                         err.code);
            on_error(err);
        });
    };

    auto run_local_then_services = [on_success, run_services_restart]() {
        if (!local_recovery_available()) {
            spdlog::info("[Recovery] No local helix-recover.sh — skipping to services.restart");
            run_services_restart();
            return;
        }
        run_local_recovery(on_success, [run_services_restart](const MoonrakerError& err) {
            spdlog::warn("[Recovery] local recovery failed: {} — trying services.restart",
                         err.message);
            run_services_restart();
        });
    };

    // Step 1: printer.firmware_restart. This is what users see in the
    // "Restarting firmware..." toast — try it first because on a healthy
    // klippy it's the fastest path. If klippy is dead, the 503 short-circuits
    // us to step 2. We log the distinction between "expected dead path" (503,
    // -32601) and unexpected failures, but escalate identically — the user
    // clicked "restart" and we should exhaust the chain before giving up.
    spdlog::info("[Recovery] Trying printer.firmware_restart");
    api_->restart_firmware(on_success, [run_local_then_services](const MoonrakerError& err) {
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
