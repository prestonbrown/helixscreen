// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_subscription_backend.h"

#include "filament_op_router.h"
#include "moonraker_error.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "toolhead_homing.h"

#include <utility>

#include "hv/json.hpp"

AmsSubscriptionBackend::AmsSubscriptionBackend(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : api_(api), client_(client) {
    // Common defaults -- derived constructors set type-specific fields
    system_info_.version = "unknown";
    system_info_.current_tool = -1;
    system_info_.current_slot = -1;
    system_info_.filament_loaded = false;
    system_info_.action = AmsAction::IDLE;
    system_info_.total_slots = 0;
}

AmsSubscriptionBackend::~AmsSubscriptionBackend() {
    // Release without unsubscribe -- MoonrakerClient may already be destroyed
    subscription_.release();
}

AmsError AmsSubscriptionBackend::start() {
    bool should_emit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_) {
            return AmsErrorHelper::success();
        }

        if (!client_) {
            spdlog::error("{} Cannot start: MoonrakerClient is null", backend_log_tag());
            return AmsErrorHelper::not_connected("MoonrakerClient not provided");
        }

        if (!api_) {
            spdlog::error("{} Cannot start: IMoonrakerAPI is null", backend_log_tag());
            return AmsErrorHelper::not_connected("IMoonrakerAPI not provided");
        }

        // Derived class extra checks (e.g., ToolChanger requires tools discovered)
        auto extra_check = additional_start_checks();
        if (!extra_check.success()) {
            return extra_check;
        }

        helix::SubscriptionId id = client_->register_notify_update(
            [this, token = lifetime_.token()](const nlohmann::json& notification) {
                // L081 Mechanism C: handle_status_update mutates members + emits events.
                // High-volume WS notify path: every status frame goes through queue_update,
                // matching the rest of printer state which is already main-thread-marshaled.
                // First-fire baseline state (initial subscription frame from Klipper) used
                // to need defer_critical to survive the splash→home scoped_freeze(); under
                // the new buffer-not-drop semantics, plain defer is sufficient — buffered
                // callbacks splice back into pending_ when the freeze releases.
                token.defer("AmsSubscriptionBackend::notify_update",
                            [this, notification]() { handle_status_update(notification); });
            });

        if (id == helix::INVALID_SUBSCRIPTION_ID) {
            spdlog::error("{} Failed to register for status updates", backend_log_tag());
            return AmsErrorHelper::not_connected("Failed to subscribe to Moonraker updates");
        }

        subscription_ = SubscriptionGuard(client_, id);
        running_ = true;
        spdlog::info("{} Backend started, subscription ID: {}", backend_log_tag(), id);
        should_emit = true;
    }

    // Emit initial state event OUTSIDE the lock to avoid deadlock
    if (should_emit) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Derived class post-start work (version detection, config loading, etc.)
    on_started();

    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    on_stopping();
    subscription_.reset();
    running_ = false;
    spdlog::info("{} Backend stopped", backend_log_tag());
}

void AmsSubscriptionBackend::release_subscriptions() {
    subscription_.release();
}

bool AmsSubscriptionBackend::is_running() const {
    return running_;
}

void AmsSubscriptionBackend::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

void AmsSubscriptionBackend::emit_event(const std::string& event, const std::string& data) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = event_callback_;
    }
    if (cb) {
        cb(event, data);
    }
}

AmsAction AmsSubscriptionBackend::get_current_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.action;
}

int AmsSubscriptionBackend::get_current_tool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_tool;
}

int AmsSubscriptionBackend::get_current_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot;
}

bool AmsSubscriptionBackend::is_filament_loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded;
}

bool AmsSubscriptionBackend::op_moves_toolhead(FilamentOp op) const {
    switch (op) {
    case FilamentOp::Load:
    case FilamentOp::Unload:
    case FilamentOp::ChangeTool:
        // Pushing or pulling filament through the hotend, and swapping what is
        // on the carriage, are toolhead motion on every backend there is. No
        // override path exists for these on purpose.
        return true;
    case FilamentOp::SelectSlot:
        // The one genuinely per-backend answer. See select_slot_moves_toolhead().
        return select_slot_moves_toolhead();
    }
    return true; // Unreachable; fail closed if the enum ever grows.
}

AmsError AmsSubscriptionBackend::claim_filament_op(FilamentOp op, bool check_state) {
    AmsAction pending = AmsAction::LOADING;
    switch (op) {
    case FilamentOp::Load:
        pending = AmsAction::LOADING;
        break;
    case FilamentOp::Unload:
        pending = AmsAction::UNLOADING;
        break;
    case FilamentOp::SelectSlot:
    case FilamentOp::ChangeTool:
        pending = AmsAction::SELECTING;
        break;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // The started/busy read and the claim share ONE critical section. Split
    // across two, a second op could read IDLE between the first op's read and
    // its claim.
    //
    // The read is under mutex_ for the same reason every other system_info_ read
    // in this class is: it is the field's declared discipline. Every writer today
    // happens to land on the main thread — handle_status_update() and the gcode
    // acks all marshal through token.defer() — so an unlocked read here was
    // formally a race and practically quiet. Do not take that as licence to skip
    // the lock; the next background writer would make it loud.
    if (check_state) {
        if (auto e = state_preconditions_unlocked(); !e.success()) {
            return e;
        }
    }
    if (filament_op_in_flight_) {
        return AmsErrorHelper::busy(ams_action_to_string(filament_op_claimed_action_));
    }
    filament_op_in_flight_ = true;
    filament_op_claimed_action_ = pending;
    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::release_filament_op_claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    filament_op_in_flight_ = false;
    filament_op_claimed_action_ = AmsAction::IDLE;
}

AmsError AmsSubscriptionBackend::run_filament_op(FilamentOp op, int arg) {
    // Order of refusals is load-bearing and matches what check_preconditions()
    // has always produced: not-started, then busy, then print-active.
    if (auto e = claim_filament_op(op, filament_op_gate() == FilamentOpGate::Standard);
        !e.success()) {
        return e;
    }
    // Owns the claim from here. Every return below releases it, including the
    // print refusal — a claim that outlived a refused op would wedge the backend
    // into a permanent busy with no action to explain it.
    FilamentOpClaim claim(this);

    // Deliberately NOT under mutex_: this reads PrinterState, which has its own
    // synchronization and nothing in system_info_ to be atomic with, and the
    // claim already excludes a second op for the whole window.
    if (op_moves_toolhead(op)) {
        if (auto e = refuse_if_printing(); !e.success()) {
            return e;
        }
    }

    // mutex_ is NOT held across the hook. The hooks issue gcode and Moonraker
    // JSON-RPC, several call emit_event() (which takes mutex_ to copy the
    // callback), and AD5X's unload re-enters eject_lane() which locks — holding
    // it here would deadlock on the first two and serialize the network on the
    // third. The claim is a flag, not a lock: a contending op is refused
    // immediately rather than blocked behind the send.
    switch (op) {
    case FilamentOp::Load:
        return do_load_filament(arg);
    case FilamentOp::Unload:
        return do_unload_filament(arg);
    case FilamentOp::SelectSlot:
        return do_select_slot(arg);
    case FilamentOp::ChangeTool:
        return do_change_tool(arg);
    }
    return AmsErrorHelper::success(); // Unreachable; the enum is exhaustive.
}

AmsError AmsSubscriptionBackend::load_filament(int slot_index) {
    return run_filament_op(FilamentOp::Load, slot_index);
}

AmsError AmsSubscriptionBackend::unload_filament(int slot_index) {
    return run_filament_op(FilamentOp::Unload, slot_index);
}

AmsError AmsSubscriptionBackend::select_slot(int slot_index) {
    return run_filament_op(FilamentOp::SelectSlot, slot_index);
}

AmsError AmsSubscriptionBackend::change_tool(int tool_number) {
    return run_filament_op(FilamentOp::ChangeTool, tool_number);
}

AmsError AmsSubscriptionBackend::state_preconditions_unlocked() const {
    if (!running_) {
        return AmsErrorHelper::not_connected(std::string(backend_log_tag()) +
                                             " backend not started");
    }
    if (system_info_.is_busy()) {
        return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::check_preconditions(bool requires_toolhead_motion) const {
    if (auto e = state_preconditions_unlocked(); !e.success()) {
        return e;
    }
    // Toolhead-motion ops (load/unload/tool-change) additionally refuse while a
    // print is active — no-motion ops (eject_lane, select, unlock) pass false.
    if (requires_toolhead_motion) {
        if (auto e = refuse_if_printing(); !e.success()) {
            return e;
        }
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::refuse_if_printing() const {
    // Refuse toolhead-motion filament ops while a print OWNS the toolhead.
    //
    // PRINTING is always refused: the nozzle is laying plastic and any filament
    // move collides with the job.
    //
    // PAUSED splits on filament_ops_self_home(). Pause-then-swap is the runout /
    // colour-change recovery workflow — pause_resume has saved the gcode state,
    // the job resumes where it left off, and this is exactly what a user does
    // from Mainsail. Refusing it universally made HelixScreen the only surface
    // that could not perform the recovery Klipper had just asked for. What still
    // has to be refused is a backend whose firmware macro homes ITSELF: on the
    // loadcell-Z AD5X, `_IFS_REMOVE_CURRENT_PRUTOK` runs a buried `_G28` that
    // probes the nozzle down into the part, tripping ZMOD's ZCONTROL_AUTO into a
    // Klipper shutdown (bundle XWPBR2DX, commit 329e731e9). Layer 1's gcode-send
    // guard never sees that `_G28`, so it must be stopped here, before the op.
    //
    // Relaxing PAUSED does NOT widen what homing can reach the printer:
    // helix::api::reject_homing_during_active_print() still refuses every
    // app-emitted G28 while PRINTING or PAUSED, and ensure_homed_then() only
    // emits one when toolhead.homed_axes lacks "xyz" — which a paused print,
    // homed by construction, never does.
    //
    // PREPARING is refused like PRINTING. Asked of the raw print_stats.state
    // this window was invisible — a host-side pre-start block reads STANDBY (or
    // the previous job's terminal state) for its whole duration — so a filament
    // op was accepted while the pre-start G-code homed and probed. Note that
    // Layer 1 deliberately does NOT extend to Preparing: it would refuse the
    // app's own pre-start block, which is itself sent through execute_gcode()
    // and may contain a G28. That is why this guard has to cover the window.
    //
    // api_ can be null in unit tests / cold-boot; when it is, print state is
    // unknown and we do not block (mirrors ensure_homed_then's null-client path).
    if (!api_) {
        return AmsErrorHelper::success();
    }
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(api_->printer_state().get_print_lifecycle_subject()));
    if (!job_holds_machine(lifecycle)) {
        return AmsErrorHelper::success();
    }
    const bool is_paused = (lifecycle == PrintState::Paused);
    const bool self_homes = filament_ops_self_home();
    if (is_paused && !self_homes) {
        spdlog::info("{} Allowing filament operation on a PAUSED print "
                     "(backend does not self-home; Layer 1 still blocks any G28)",
                     backend_log_tag());
        return AmsErrorHelper::success();
    }
    spdlog::warn("{} Refusing filament operation while a print is active (lifecycle={}, "
                 "self_homes={})",
                 backend_log_tag(), static_cast<int>(lifecycle), self_homes);
    // A non-self-homing backend that reaches here is PRINTING, and pausing is a
    // recovery it can actually offer — say so instead of "finish or cancel".
    return AmsErrorHelper::print_active(is_paused, /*pause_allows_ops=*/!self_homes);
}

void AmsSubscriptionBackend::on_home_confirmation_declined() {
    spdlog::info("{} User declined the pre-op home; operation cancelled", backend_log_tag());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
    }
    emit_event(EVENT_STATE_CHANGED);
}

bool AmsSubscriptionBackend::toolhead_homed() const {
    if (!api_) {
        // No connection: callers fall back to dispatching directly, so the
        // answer is not consulted. Report homed so no G28 is ever synthesized
        // against a printer we cannot talk to.
        return true;
    }
    return helix::toolhead_is_homed(api_->printer_state());
}

AmsError
AmsSubscriptionBackend::ensure_homed_then(std::string gcode, std::function<void()> on_complete,
                                          std::function<void(const MoonrakerError&)> on_error,
                                          uint32_t timeout_ms, bool skip_homing, bool silent,
                                          std::optional<bool> caller_surfaces_errors) {
    // The homed answer comes from the live homed_axes subject, not an RPC:
    // toolhead is in the standing objects.subscribe set, so querying it again
    // was a redundant round trip. skip_homing short-circuits the check
    // entirely -- toolhead_homed() is never called -- for firmware macros that
    // home themselves (CFS Fork variant). delegates_homing_to_printer()
    // short-circuits the same way when the printer-side system homes (AFC
    // auto_home) — neither prompt nor G28.
    //
    // home_preconfirmed_ is intentionally NOT consulted here: it must never
    // substitute for the toolhead_homed() answer (that would skip the G28
    // itself, changing what the printer does), only for the PROMPT below. See
    // the std::exchange() consume further down, which only runs once this
    // branch has already proven the toolhead genuinely needs a G28.
    if (skip_homing || delegates_homing_to_printer() || toolhead_homed()) {
        return dispatch_payload(std::move(gcode), std::move(on_complete), std::move(on_error),
                                timeout_ms, silent, caller_surfaces_errors);
    }

    auto gcode_copy = std::move(gcode);

    // The confirmation always resolves through on_confirm/on_cancel below,
    // never back through this function's return value: with a real prompter
    // installed the answer arrives on a later main-thread tick (a modal
    // button tap), so there is nothing left here to return synchronously.
    // request_home_confirmation() is what makes on_confirm fire immediately
    // and synchronously when no prompter is installed -- the default every
    // pre-existing test relies on -- so the two branches below still run in
    // this same call for all of them.
    auto token = lifetime_.token();
    auto send_g28_then_dispatch = [this, token, gcode_copy, on_complete, on_error, timeout_ms,
                                   silent, caller_surfaces_errors]() {
        // Runs either inline in this call (no prompter, or a synchronous
        // test prompter) or later from an LVGL confirm-button event
        // callback. Both cases are on the main thread -- the modal only
        // ever fires its callbacks from lv_timer_handler() -- so this is
        // not the bg-thread TOCTOU the bare expired()-then-`this` pattern
        // usually flags.
        if (token.expired()) { // L081_OK: main-thread only, see comment above
            return;
        }
        spdlog::info("{} Sending G28 before operation", backend_log_tag());

        // No API: emit the G28 through the VIRTUAL execute_gcode rather
        // than skipping it. api_ is null only in fixtures, and they
        // override the virtual to capture - routing around it here would
        // make the unhomed branch untestable and silently drop the G28
        // from the recorded sequence. The real path below cannot use the
        // virtual because it needs an error callback and
        // HOMING_TIMEOUT_MS, which the virtual forms do not take.
        // Fixtures are synchronous (no real RPC), so the AmsError the
        // virtual returns IS the only failure signal available here --
        // there is no async MoonrakerError to forward, so a failure is
        // translated into one.
        if (!api_) {
            AmsError g28_result = execute_gcode("G28");
            if (!g28_result.success()) {
                MoonrakerError synthetic;
                synthetic.type = MoonrakerErrorType::UNKNOWN;
                synthetic.message = g28_result.technical_msg;
                handle_dispatch_error(synthetic, on_error);
                return;
            }
            dispatch_payload(gcode_copy, on_complete, on_error, timeout_ms, silent,
                             caller_surfaces_errors);
            return;
        }

        // IMoonrakerAPI::execute_gcode() returns void (it's inherently
        // async); dispatch_payload()'s result on the success leg mirrors
        // the pre-refactor behavior of the query path this replaces (it
        // never returned the send_jsonrpc() call either).
        api_->execute_gcode(
            "G28",
            [this, token, gcode_copy, on_complete, on_error, timeout_ms, silent,
             caller_surfaces_errors]() {
                // L081 Mechanism C: the ack lands on a bg thread and
                // dispatch_payload touches api_/members. Marshal to main.
                token.defer("AmsSubscriptionBackend::ensure_homed_then_g28_success",
                            [this, gcode_copy, on_complete, on_error, timeout_ms, silent,
                             caller_surfaces_errors]() {
                                dispatch_payload(gcode_copy, on_complete, on_error, timeout_ms,
                                                 silent, caller_surfaces_errors);
                            });
            },
            [this, token, on_error](const MoonrakerError& err) {
                token.defer("AmsSubscriptionBackend::ensure_homed_then_g28_error",
                            [this, err, on_error]() { handle_dispatch_error(err, on_error); });
            },
            IMoonrakerAPI::HOMING_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
            // The G28 error leg lands in handle_dispatch_error(), which without
            // a caller on_error only logs and resets the action to IDLE. Its
            // ownership answer is the caller's, not this wrapper's.
            caller_surfaces_errors.value_or(on_error != nullptr));
    };

    // Single-shot consume: a UI surface that already asked "home printer
    // first?" before its own preheat (FilamentPanel / AmsOperationSidebar)
    // arms this so we don't ask AGAIN here -- but the toolhead is still
    // genuinely unhomed at this point (nothing sends G28 early), so the G28
    // itself still fires, unprompted, exactly where it always has. Consuming
    // AFTER the toolhead_homed() branch above (never reached via short-circuit
    // when already homed) is what keeps a later, genuinely-unprompted dispatch
    // asking normally.
    if (std::exchange(home_preconfirmed_, false)) {
        spdlog::info("{} Not homed, but pre-confirmed by an earlier prompt -- sending G28 without "
                     "asking again",
                     backend_log_tag());
        send_g28_then_dispatch();
        return AmsErrorHelper::success();
    }

    spdlog::info("{} Not homed -- asking before sending G28", backend_log_tag());
    helix::ui::request_home_confirmation(send_g28_then_dispatch, [this, token]() {
        // Same main-thread-only reasoning as send_g28_then_dispatch above.
        if (token.expired()) { // L081_OK: main-thread only, see comment above
            return;
        }
        on_home_confirmation_declined();
    });

    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::handle_dispatch_error(
    const MoonrakerError& err, const std::function<void(const MoonrakerError&)>& on_error) {
    if (on_error) {
        on_error(err);
        return;
    }
    // Historical behaviour, preserved exactly for the 8 pre-existing callers
    // (none of which pass on_error): log and reset to IDLE so the UI doesn't
    // get stuck on a "loading" spinner after a failed G28/payload.
    spdlog::error("{} G-code failed: {}", backend_log_tag(), err.message);
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.action = AmsAction::IDLE;
}

AmsError
AmsSubscriptionBackend::dispatch_payload(std::string gcode, std::function<void()> on_complete,
                                         std::function<void(const MoonrakerError&)> on_error,
                                         uint32_t timeout_ms, bool silent,
                                         std::optional<bool> caller_surfaces_errors) {
    // Legacy shape: dispatch through the SAME two virtuals as before this
    // method grew these parameters. ~20 fixtures override ONLY
    // execute_gcode(gcode) / execute_gcode(gcode, on_complete) (see the
    // dispatch_payload() doc comment in the header); a payload with no
    // on_complete must reach the 1-arg override or those tests silently stop
    // capturing anything. All 8 pre-existing callers never pass
    // on_error/timeout_ms/silent, so they always land here, byte-for-byte the
    // pre-widening behaviour.
    if (!on_error && timeout_ms == IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS && silent) {
        return on_complete ? execute_gcode(gcode, std::move(on_complete)) : execute_gcode(gcode);
    }

    // A caller asked for its own error/timeout/toast policy -- the hardcoded
    // virtuals above can't carry that (they fix AMS_OPERATION_TIMEOUT_MS and
    // silent=true, and their error handling only logs). Talk to IMoonrakerAPI
    // directly, the same way AmsBackendCfs::dispatch_action_script used to
    // before this method existed to replace its fork.
    if (!api_) {
        MoonrakerError synthetic;
        synthetic.type = MoonrakerErrorType::CONNECTION_LOST;
        synthetic.message = "IMoonrakerAPI not available";
        handle_dispatch_error(synthetic, on_error);
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }

    const char* tag = backend_log_tag();
    auto token = lifetime_.token();

    // Captured from the CALLER's on_error, before the wrapper below makes the
    // callback we hand to IMoonrakerAPI unconditionally non-null. A caller that
    // passed none gets handle_dispatch_error()'s log-and-reset-to-IDLE, which no
    // user ever sees, so the `!!` router must keep the report. A caller whose
    // own on_error also only logs overrides this explicitly (CFS does). See
    // include/rpc_error_policy.h.
    const bool caller_surfaces = caller_surfaces_errors.value_or(on_error != nullptr);

    api_->execute_gcode(
        gcode,
        [tag, on_complete = std::move(on_complete)]() {
            spdlog::debug("{} G-code executed successfully", tag);
            if (on_complete) {
                on_complete();
            }
        },
        [this, token, on_error](const MoonrakerError& err) {
            // L081 Mechanism C: this lands on the libhv response thread and
            // touches system_info_/mutex_ (default path) or arbitrary caller
            // logic (on_error). Marshal to main either way.
            token.defer("AmsSubscriptionBackend::dispatch_payload_error",
                        [this, err, on_error]() { handle_dispatch_error(err, on_error); });
        },
        timeout_ms, silent, /*on_queued=*/nullptr, caller_surfaces);
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::execute_gcode(const std::string& gcode) {
    if (!api_) {
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, gcode);
    api_->execute_gcode(
        gcode, [tag]() { spdlog::debug("{} G-code executed successfully", tag); },
        [tag, gcode](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, gcode);
            } else if (err.type == MoonrakerErrorType::NOT_READY) {
                // IMoonrakerAPI already logs a [warning] when refusing g-code on a halted
                // Klipper. AD5X-IFS retries _IFS_VARS on every Adventurer5M.json poll, so
                // duplicating at [error] floods the log post-halt.
                spdlog::debug("{} G-code skipped (Klipper halted): {}", tag, gcode);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, gcode, err.message);
            }
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS,
        // silent=true: the phase tracker + IFS_STATUS own operation completion,
        // so the RPC timeout is advisory for these long macros. A cold-start
        // load can legitimately run past the 300s ceiling (bundle 77TDH9N6:
        // heat-from-cold + load + double purge + clean), completing fine ~47s
        // later — silent suppresses ONLY the false REQUEST_TIMEOUT toast and the
        // tracker's generic fallback.
        /*silent=*/true, /*on_queued=*/nullptr,
        // caller_surfaces_errors=false: the error_cb above only writes to the
        // log, which the user never sees. Declaring otherwise would silence
        // GcodeErrorRouter, so a real AFC/Happy Hare/CFS macro rejection would
        // reach nobody. With this false, Klipper's `!!` broadcast stays free to
        // surface it. See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::execute_gcode(const std::string& gcode,
                                               std::function<void()> on_complete) {
    if (!api_) {
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, gcode);
    api_->execute_gcode(
        gcode,
        [tag, on_complete = std::move(on_complete)]() {
            spdlog::debug("{} G-code executed successfully", tag);
            // Fires when the command finishes (Klipper acks the script), so a
            // long macro has fully run — the reliable completion signal.
            if (on_complete) {
                on_complete();
            }
        },
        [tag, gcode](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, gcode);
            } else if (err.type == MoonrakerErrorType::NOT_READY) {
                spdlog::debug("{} G-code skipped (Klipper halted): {}", tag, gcode);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, gcode, err.message);
            }
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS,
        // silent=true: completion is owned by the phase tracker + IFS_STATUS, not
        // the RPC return, so the advisory 300s timeout must not surface a false
        // "timed out" toast for a legitimately-long macro (bundle 77TDH9N6),
        // along with the tracker's generic fallback.
        /*silent=*/true, /*on_queued=*/nullptr,
        // caller_surfaces_errors=false: the error_cb above only writes to the
        // log, which the user never sees. Declaring otherwise would silence
        // GcodeErrorRouter, so a real AFC/Happy Hare/CFS macro rejection would
        // reach nobody. With this false, Klipper's `!!` broadcast stays free to
        // surface it. See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}
