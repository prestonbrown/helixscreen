// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "app_globals.h"
#include "moonraker_client_mock_internal.h"
#include "printer_state.h"
#include "rpc_error_correlation.h"
#include "rpc_error_policy.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

using namespace helix;

namespace mock_internal {

void register_print_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // printer.gcode.script - Execute G-code script
    // Like real Moonraker, returns error for out-of-range moves and other gcode failures
    registry["printer.gcode.script"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        std::string script;
        if (params.contains("script")) {
            script = params["script"].get<std::string>();
        }

        // Real Klipper executes a multi-line script line-by-line; the per-command
        // parsers in gcode_script() assume a SINGLE command (e.g. they `find('S')`
        // globally, which a multi-line script would point at the wrong token and
        // throw std::stod). Split on newlines and process each non-empty line
        // independently. Each line's std::stod/std::stoi is guarded so a parse
        // quirk surfaces as an error result, never a C++ exception escaping the
        // RPC layer (real Moonraker returns an error string, it doesn't crash).
        //
        // The error message is latched HERE, the moment a line fails, exactly as
        // `result` is. gcode_script()'s latch is per-CALL (it clears on entry),
        // but the RPC's error is per-SCRIPT: every jog is "G91\nG0 X..\nG90", so
        // reading the latch after the loop returned whatever the trailing G90
        // left behind — empty — and the caller rendered "An unknown error
        // occurred." instead of the real rejection.
        int result = 0;
        std::string script_error;
        size_t line_start = 0;
        while (line_start <= script.size()) {
            size_t nl = script.find('\n', line_start);
            std::string line = (nl == std::string::npos)
                                   ? script.substr(line_start)
                                   : script.substr(line_start, nl - line_start);
            // Trim trailing CR / surrounding spaces so blank lines are skipped.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            size_t first = line.find_first_not_of(' ');
            if (first != std::string::npos) {
                try {
                    if (self->gcode_script(line.substr(first)) != 0) {
                        result = 1;
                        // Keep the FIRST failure: Klipper aborts the script at the
                        // first rejected command, so later lines cannot supersede it.
                        if (script_error.empty())
                            script_error = self->get_last_gcode_error();
                    }
                } catch (const std::exception& ex) {
                    spdlog::debug("[MoonrakerClientMock] gcode_script parse skipped for '{}': {}",
                                  line, ex.what());
                }
            }
            if (nl == std::string::npos)
                break;
            line_start = nl + 1;
        }

        // Test injection: simulate a response that never comes back at all (Klippy
        // restarted, connection stalled). Klipper still processed the gcode above;
        // NEITHER callback fires, so any caller-side in-flight counter stays pinned.
        if (self->take_forced_gcode_drop(script)) {
            return true;
        }

        // Test injection: simulate an RPC-layer failure (e.g. timeout) for this command
        // while Klipper still processed the gcode above. The collector-based APIs rely on
        // this to exercise paths where the RPC response is lost but Klipper keeps running.
        if (auto forced = self->take_forced_gcode_error(script)) {
            if (error_cb) {
                // has_broadcast_channel=true: this registry entry IS
                // printer.gcode.script, the method Klipper mirrors as `!!`.
                if (helix::rpc_error_policy::decide(
                        self->current_send_intent(),
                        helix::rpc_error_policy::RequestFacts{/*has_broadcast_channel=*/true,
                                                              /*suppress_all=*/false})
                        .record_for_dedup) {
                    helix::rpc_error_correlation::record_caller_handled(forced->message);
                }
                error_cb(*forced);
            }
            return true;
        }

        if (result != 0) {
            // G-code execution failed (e.g., out-of-range move)
            // Return error like real Moonraker does
            if (error_cb) {
                MoonrakerError err =
                    MoonrakerError::json_rpc_error("printer.gcode.script", script_error);
                // send_jsonrpc() dispatches into this registry inline and never
                // reaches MoonrakerRequestTracker, so the same policy is applied
                // here from the shared decide(). Without it the mock either
                // double-toasts a rejection that hardware reports once, or eats
                // the `!!` copy that hardware would still deliver.
                // has_broadcast_channel=true: this registry entry IS
                // printer.gcode.script, the method Klipper mirrors as `!!`.
                if (helix::rpc_error_policy::decide(
                        self->current_send_intent(),
                        helix::rpc_error_policy::RequestFacts{/*has_broadcast_channel=*/true,
                                                              /*suppress_all=*/false})
                        .record_for_dedup) {
                    helix::rpc_error_correlation::record_caller_handled(err.message);
                }
                error_cb(err);
            }
        } else if (success_cb) {
            success_cb(json::object()); // Return empty success response
        }
        return true;
    };

    // printer.print.start - Start a print job
    registry["printer.print.start"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            if (self->start_print_internal(filename)) {
                if (success_cb) {
                    success_cb(json::object());
                }
            } else if (error_cb) {
                MoonrakerError err = MoonrakerError::validation_error("printer.print.start",
                                                                      "Failed to start print");
                error_cb(err);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error("printer.print.start",
                                                                  "Missing filename parameter");
            error_cb(err);
        }
        return true;
    };

    // printer.print.pause - Pause current print
    registry["printer.print.pause"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)params;
        if (self->pause_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error(
                "printer.print.pause", "Cannot pause - not currently printing");
            error_cb(err);
        }
        return true;
    };

    // printer.print.resume - Resume paused print
    registry["printer.print.resume"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)params;
        if (self->resume_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error(
                "printer.print.resume", "Cannot resume - not currently paused");
            error_cb(err);
        }
        return true;
    };

    // printer.print.cancel - Cancel current print
    registry["printer.print.cancel"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)params;
        if (self->cancel_print_internal()) {
            if (success_cb) {
                success_cb(json::object());
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error(
                "printer.print.cancel", "Cannot cancel - no active print");
            error_cb(err);
        }
        return true;
    };

    // printer.emergency_stop - Execute emergency stop (M112)
    registry["printer.emergency_stop"] =
        [](MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        self->emergency_stop_internal();

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // printer.firmware_restart - Restart firmware (MCU reset)
    registry["printer.firmware_restart"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        spdlog::info("[MoonrakerClientMock] Firmware restart initiated");

        // Simulate restart: briefly go SHUTDOWN, then READY after 1 second
        helix::ui::async_call(
            [](void*) {
                get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);

                // Schedule recovery to READY after 1 second
                static lv_timer_t* timer = nullptr;
                if (timer) {
                    lv_timer_delete(timer);
                }
                timer = lv_timer_create(
                    [](lv_timer_t* t) {
                        spdlog::info("[MoonrakerClientMock] Firmware restart complete - READY");
                        get_printer_state().set_klippy_state_sync(KlippyState::READY);
                        lv_timer_delete(t);
                        timer = nullptr;
                    },
                    1000, nullptr);
                lv_timer_set_repeat_count(timer, 1);
            },
            nullptr);

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // printer.restart - Restart Klipper (soft restart)
    registry["printer.restart"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        spdlog::info("[MoonrakerClientMock] Klipper restart initiated");

        // Simulate restart: briefly go SHUTDOWN, then READY after 500ms
        helix::ui::async_call(
            [](void*) {
                get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);

                // Schedule recovery to READY after 500ms (faster than firmware restart)
                static lv_timer_t* timer = nullptr;
                if (timer) {
                    lv_timer_delete(timer);
                }
                timer = lv_timer_create(
                    [](lv_timer_t* t) {
                        spdlog::info("[MoonrakerClientMock] Klipper restart complete - READY");
                        get_printer_state().set_klippy_state_sync(KlippyState::READY);
                        lv_timer_delete(t);
                        timer = nullptr;
                    },
                    500, nullptr);
                lv_timer_set_repeat_count(timer, 1);
            },
            nullptr);

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };
}

} // namespace mock_internal
