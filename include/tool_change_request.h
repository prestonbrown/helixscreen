// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_error.h"
#include "ams_types.h"
#include "async_lifetime_guard.h"
#include "print_lifecycle_state.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {
class PrinterState;
class IMoonrakerAPI;
} // namespace helix

namespace helix::ui {

/**
 * @brief What a request to mount another tool gets, decided before anything moves.
 *
 * Every surface that changes tool on a tap (the home tool switcher, the AMS
 * path canvas toolhead, the filament panel extruder dropdown) asks this one
 * question, so a tap refused on one is refused on all of them with the same
 * words, and a confirmation shown on one is shown on all of them.
 */
enum class ToolChangeVerdict : uint8_t {
    AlreadyActive,        ///< The tool is the mounted one: nothing to change
    RefuseSpoolsOnBed,    ///< Spools lie on the bed: the head must not move
    RefusePrint,          ///< A print owns the toolhead (print_blocks_filament_op)
    RefuseNeedsAttention, ///< The filament system is paused waiting on the user
    Busy,                 ///< The filament system is mid-operation: the tap is ignored
    ConfirmPaused,        ///< Paused job: always confirmed, the head moves into the part
    ConfirmSetting,       ///< "Confirm tool changes" is on
    Immediate,            ///< Change now
};

/// Everything the verdict depends on, read from live state by tool_change_inputs().
struct ToolChangeInputs {
    PrintState lifecycle = PrintState::Idle;
    bool filament_ops_self_home = false; ///< AmsBackend::filament_ops_self_home(); false with none
    bool spools_on_bed = false;          ///< PrintStateTracker::spool_latch_active()
    AmsAction backend_action = AmsAction::IDLE; ///< AmsSystemInfo::action; IDLE with none
    bool already_active = false;                ///< The requested tool is the mounted one
    bool confirm_enabled = true;                ///< SafetySettingsManager tool-change confirmation
};

struct ToolChangeDecision {
    ToolChangeVerdict verdict = ToolChangeVerdict::Immediate;
    /// The copy to show for a Refuse* verdict; success otherwise.
    AmsError refusal = AmsErrorHelper::success();

    [[nodiscard]] bool dispatches() const {
        return verdict == ToolChangeVerdict::Immediate ||
               verdict == ToolChangeVerdict::ConfirmPaused ||
               verdict == ToolChangeVerdict::ConfirmSetting;
    }
};

/**
 * @brief The refusal a print gives a tool change, or success.
 *
 * Same copy the backend produces had the request reached it, so the pre-guard
 * and the backend refusal never say two different things.
 */
[[nodiscard]] AmsError tool_change_print_refusal(PrintState lifecycle, bool filament_ops_self_home);

/// The refusal every move gets while spools lie on the bed.
[[nodiscard]] AmsError spools_on_bed_refusal();

/// The refusal a tool change gets while the filament system waits on the user.
[[nodiscard]] AmsError needs_attention_refusal();

/// The verdict for @p in. Pure: no state is read.
[[nodiscard]] ToolChangeDecision decide_tool_change(const ToolChangeInputs& in);

/// The live inputs for a change to @p tool_index on @p printer_state's printer.
[[nodiscard]] ToolChangeInputs tool_change_inputs(const PrinterState& printer_state,
                                                  int tool_index);

/// What a caller of offer_tool_change() hears back. Every member is optional.
struct ToolChangeHooks {
    /// The change was accepted.
    std::function<void()> on_success;
    /// The change was requested and failed, after the shared error toast. May
    /// run off the main thread: a caller touching LVGL marshals it.
    std::function<void()> on_failed;
    /// Nothing was requested: refused, ignored, cancelled or dismissed.
    std::function<void()> on_declined;
    /// Gates the confirmation dialog's callbacks, the change itself included.
    std::optional<LifetimeToken> owner_token;
    /// The API to change tool through, asked when the change is sent, so a
    /// dialog left open across a reconnect never holds a stale one. Unset
    /// means get_moonraker_api().
    std::function<IMoonrakerAPI*()> resolve_api;
};

/**
 * @brief Decide a change to @p tool_index and carry it out.
 *
 * Refusals raise a warning toast; Busy and AlreadyActive raise nothing. A
 * confirmation verdict shows a modal_confirm whose primary button decides again
 * from live state and changes tool only if that verdict still dispatches,
 * otherwise reporting it as a tap would; a print paused since the dialog
 * opened closes it and offers the change again, so the paused warning is the
 * one answered. However else the dialog closes, or when the second verdict
 * withholds the change, on_declined runs. The change goes
 * through ToolState::request_tool_change().
 *
 * @return the verdict, so a caller can treat AlreadyActive its own way
 */
ToolChangeVerdict offer_tool_change(const PrinterState& printer_state, int tool_index,
                                    ToolChangeHooks hooks = {});

/**
 * @brief Put the mounted tool back in its dock, behind the refusals every move gets.
 *
 * Refused, with a warning toast, while spools lie on the bed or a job holds the
 * machine (paused included: a print resumed with its tool docked prints
 * nothing). Does nothing when nothing is mounted or the backend cannot park.
 *
 * @return true when the park was sent
 */
bool request_tool_park(const PrinterState& printer_state);

} // namespace helix::ui
