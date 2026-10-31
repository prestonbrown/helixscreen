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
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        std::string script;
        if (params.contains("script")) {
            script = params["script"].get<std::string>();
        }

        // The all-in-one tool offset calibration blocks until it is done, so
        // its rpc cannot be answered by the line loop below: the simulation
        // owns the callbacks and fires them when the run ends.
        if (self->simulate_tool_offset_calibration(script, success_cb, error_cb)) {
            return true;
        }

        // Pressure advance completes asynchronously (a handful of candidate
        // probes) so the panel's phase list and attempt counter are observable
        // in mock mode rather than snapping straight to a number.
        if (self->simulate_pa_calibration(script, success_cb, error_cb)) {
            return true;
        }

        // The U1's offset calibration steps each block for a heat, a wipe or a
        // probe pass, and answer only when it ends.
        if (self->simulate_nozzle_offset_calibration(script, success_cb, error_cb)) {
            return true;
        }

        // INDX's calibrations and tool cycle answer only when they end.
        if (self->simulate_indx_blocking(script, success_cb, error_cb)) {
            return true;
        }

        // A MedusaHC swap blocks its script until the docks confirm it, so a
        // run that starts one stops there and is answered when the swap ends.
        MoonrakerClientMock::GcodeScriptRun run = self->run_gcode_script_lines(script, 0);
        if (run.result == 0 && run.resume_at != std::string::npos) {
            self->hold_medusa_reply(script, run.resume_at, self->current_send_intent(),
                                    std::move(success_cb), std::move(error_cb));
            return true;
        }
        self->answer_gcode_script(script, run, self->current_send_intent(), success_cb, error_cb);
        return true;
    };

    // printer.print.start - Start a print job
    registry["printer.print.start"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
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
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
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
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
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
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
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

    // printer.dragonbreath.set_target - the chamber appliance glue's webhook,
    // proxied by Moonraker. VENDOR_OK: the mock simulates that appliance.
    registry["printer.dragonbreath.set_target"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           [[maybe_unused]] helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        const double target = params.contains("target") && params["target"].is_number()
                                  ? params["target"].get<double>()
                                  : 0.0;
        const json reply = self->chamber_route_set_target(target);
        if (success_cb) {
            success_cb(reply);
        }
        return true;
    };

    // printer.control.nozzle_properties - the U1 firmware's webhook for a
    // head's fitted nozzle, proxied by Moonraker. It answers inside a
    // successful reply, refusals included. Other printers have no such
    // endpoint. VENDOR_OK: the mock simulates that firmware.
    registry["printer.control.nozzle_properties"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (self->get_printer_type() != MoonrakerClientMock::PrinterType::SNAPMAKER_U1) {
            if (error_cb) {
                error_cb(MoonrakerError::json_rpc_error("printer.control.nozzle_properties",
                                                        "Method not found"));
            }
            return true;
        }
        const json reply = self->set_nozzle_properties(params);
        if (success_cb) {
            success_cb(json{{"result", reply}});
        }
        return true;
    };

    // printer.emergency_stop - Execute emergency stop (M112)
    registry["printer.emergency_stop"] =
        [](MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           [[maybe_unused]] helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        self->emergency_stop_internal();

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // printer.firmware_restart - Restart firmware (MCU reset)
    //
    // Restarting is trigger_restart()'s job, not a second private imitation of
    // it: a restart drops the print, zeroes the heater targets, clears excluded
    // objects and reloads everything Klipper reads from printer.cfg. This
    // handler used to fake only the klippy state, so a restart reached through
    // the RPC left the mock claiming a print was still running on a machine that
    // had just rebooted, and left unsaved per-tool offsets alive.
    //
    // It also poked get_printer_state() directly - the process-global one, not
    // whoever subscribed to THIS client - instead of dispatching the webhooks
    // status update a real restart arrives as. And it used an lv_timer for the
    // return to READY, which never fires in a test that does not pump LVGL, so
    // klippy stayed SHUTDOWN forever there.
    //
    // The ack is right, though, and stays: Moonraker's do_restart() catches
    // "Klippy Disconnected" and returns "ok" (klippy_apis.py). That is exactly
    // why SAVE_CONFIG needs SaveConfigWatch and this does not - SAVE_CONFIG goes
    // through the generic printer.gcode.script path, which has no such catch.
    registry["printer.firmware_restart"] =
        [](MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           [[maybe_unused]] helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        self->trigger_restart(/*is_firmware=*/true);

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // printer.restart - Restart Klipper (soft restart). See the firmware_restart
    // handler above for why this delegates rather than simulating its own.
    registry["printer.restart"] =
        [](MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           [[maybe_unused]] helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        self->trigger_restart(/*is_firmware=*/false);

        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };
}

} // namespace mock_internal

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
// reading the latch after the loop would return whatever the trailing G90
// left behind - empty - and the caller would render "An unknown error
// occurred." instead of the real rejection.
MoonrakerClientMock::GcodeScriptRun
MoonrakerClientMock::run_gcode_script_lines(const std::string& script, size_t start) {
    GcodeScriptRun run;
    size_t line_start = start;
    while (line_start <= script.size()) {
        size_t nl = script.find('\n', line_start);
        std::string line = (nl == std::string::npos) ? script.substr(line_start)
                                                     : script.substr(line_start, nl - line_start);
        // Trim trailing CR / surrounding spaces so blank lines are skipped.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t first = line.find_first_not_of(' ');
        if (first != std::string::npos) {
            // Only this line's arm counts: a direct gcode_script() call may
            // have armed a swap no script is waiting on.
            take_medusa_swap_armed();
            try {
                if (gcode_script(line.substr(first)) != 0) {
                    run.result = 1;
                    // Keep the FIRST failure: Klipper aborts the script at the
                    // first rejected command, so later lines cannot supersede it.
                    if (run.error.empty())
                        run.error = get_last_gcode_error();
                }
            } catch (const std::exception& ex) {
                spdlog::debug("[MoonrakerClientMock] gcode_script parse skipped for '{}': {}", line,
                              ex.what());
            }
            if (take_medusa_swap_armed()) {
                run.resume_at = (nl == std::string::npos) ? script.size() : nl + 1;
                return run;
            }
        }
        if (nl == std::string::npos)
            break;
        line_start = nl + 1;
    }
    return run;
}

void MoonrakerClientMock::answer_gcode_script(
    const std::string& script, const GcodeScriptRun& run,
    const helix::rpc_error_policy::CallerIntent& intent,
    const helix::SharedCallback<void(const json&)>& success_cb,
    const helix::SharedCallback<void(const MoonrakerError&)>& error_cb) {
    // Test injection: a console line the script produced, delivered before the
    // RPC answers as Klipper's own replies are.
    if (auto reply = take_forced_console_reply(script)) {
        dispatch_gcode_response(*reply);
    }

    // Test injection: simulate a response that never comes back at all (Klippy
    // restarted, connection stalled). Klipper still processed the gcode above;
    // NEITHER callback fires, so any caller-side in-flight counter stays pinned.
    if (take_forced_gcode_drop(script)) {
        return;
    }

    // send_jsonrpc() dispatches into the registry inline and never reaches
    // MoonrakerRequestTracker, so the same policy is applied here from the
    // shared decide(). Without it the mock either double-toasts a rejection
    // that hardware reports once, or eats the `!!` copy that hardware would
    // still deliver. has_broadcast_channel=true: this IS printer.gcode.script,
    // the method Klipper mirrors as `!!`.
    auto reject = [&](const MoonrakerError& err) {
        if (!error_cb) {
            return;
        }
        if (helix::rpc_error_policy::decide(
                intent, helix::rpc_error_policy::RequestFacts{/*has_broadcast_channel=*/true,
                                                              /*suppress_all=*/false})
                .record_for_dedup) {
            helix::rpc_error_correlation::record_caller_handled(err.message);
        }
        error_cb(err);
    };

    // Test injection: simulate an RPC-layer failure (e.g. timeout) for this command
    // while Klipper still processed the gcode above. The collector-based APIs rely on
    // this to exercise paths where the RPC response is lost but Klipper keeps running.
    if (auto forced = take_forced_gcode_error(script)) {
        reject(*forced);
        return;
    }

    if (run.result != 0) {
        // G-code execution failed (e.g., out-of-range move), answered as real
        // Moonraker does.
        reject(MoonrakerError::json_rpc_error("printer.gcode.script", run.error));
    } else if (success_cb) {
        success_cb(json::object());
    }
}
