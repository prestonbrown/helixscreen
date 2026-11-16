// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolhead_homing.h"

#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

namespace helix {

bool toolhead_is_homed(const PrinterState& ps) {
    // get_homed_axes_subject() is non-const (it mirrors PrinterState's other
    // subject accessors, none of which are const), but this call only reads
    // the subject's string value and never mutates it, so the const_cast is
    // safe here despite @p ps being a const reference.
    //
    // Klipper reports homed_axes as a subset of "xyz". Anything short of all
    // three is not homed for our purposes: every caller needs full XYZ before
    // it can move the toolhead safely.
    const char* axes = lv_subject_get_string(
        const_cast<PrinterState&>(ps).motion_state().get_homed_axes_subject());
    if (axes == nullptr) {
        return false;
    }
    const std::string s(axes);
    return s.find('x') != std::string::npos && s.find('y') != std::string::npos &&
           s.find('z') != std::string::npos;
}

bool axis_is_homed(const PrinterState& ps, Axis axis) {
    // Same const_cast as toolhead_is_homed() directly above: the subject
    // accessors are all non-const, but this only reads the string value.
    const char* axes = lv_subject_get_string(
        const_cast<PrinterState&>(ps).motion_state().get_homed_axes_subject());
    if (axes == nullptr) {
        return false;
    }
    const char c = (axis == Axis::X) ? 'x' : (axis == Axis::Y) ? 'y' : 'z';
    return std::string(axes).find(c) != std::string::npos;
}

void ensure_homed_then(IMoonrakerAPI* api, AsyncLifetimeGuard& guard,
                       helix::SharedCallback<void()> then,
                       helix::SharedCallback<void(const MoonrakerError&)> on_error,
                       HomingPolicy policy) {
    if (policy == HomingPolicy::IfNeeded && toolhead_is_homed(get_printer_state())) {
        spdlog::debug("[ensure_homed_then] Already homed, proceeding");
        if (then) {
            then();
        }
        return;
    }

    if (!api) {
        spdlog::error(
            "[ensure_homed_then] Homing needed and no IMoonrakerAPI available — cannot send G28");
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::CONNECTION_LOST;
            err.message = "IMoonrakerAPI unavailable";
            on_error(err);
        }
        return;
    }

    spdlog::info("[ensure_homed_then] {}, sending G28",
                 policy == HomingPolicy::Always ? "Homing required" : "Not fully homed");

    // Capture the caller's error-reporting intent BEFORE on_error is moved into
    // the wrapper below. The wrapper is non-null for every call — including the
    // many that pass on_error == nullptr — so intent derived after the move
    // reads our own logging as a promise the caller never made, and silences
    // Klipper's `!!` broadcast for a G28 failure nobody else reports. See
    // include/rpc_error_policy.h.
    const bool caller_surfaces = (on_error != nullptr);

    // A homing macro can still be moving when G28 acks, so idle_timeout reads
    // "Printing" into the continuation. That episode is ours: without this the
    // motion guard refuses the continuation's move as an external operation.
    get_printer_state().app_motion_activity().note_episode_send();
    api->execute_gcode(
        "G28",
        guard.bg_cb("ensure_homed_then::g28_done",
                    [then = std::move(then)]() {
                        spdlog::info("[ensure_homed_then] G28 complete, proceeding");
                        // Restarts the ownership lapse, which a long
                        // homing macro would otherwise run out.
                        get_printer_state().app_motion_activity().note_still_active();
                        if (then) {
                            then();
                        }
                    }),
        guard.bg_cb("ensure_homed_then::g28_error",
                    [on_error = std::move(on_error)](const MoonrakerError& err) {
                        spdlog::warn("[ensure_homed_then] G28 failed: {}", err.message);
                        if (on_error) {
                            on_error(err);
                        }
                    }),
        IMoonrakerAPI::kHomingTimeoutMs, /*silent=*/false,
        /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/caller_surfaces);
}

} // namespace helix
