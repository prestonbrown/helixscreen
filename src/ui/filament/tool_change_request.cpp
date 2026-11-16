// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_change_request.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "spool_latch_gate.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace helix::ui {

AmsError tool_change_print_refusal(PrintState lifecycle, bool filament_ops_self_home) {
    if (!print_blocks_filament_op(lifecycle, filament_ops_self_home)) {
        return AmsErrorHelper::success();
    }
    return AmsErrorHelper::print_active(lifecycle == PrintState::Paused,
                                        /*pause_allows_ops=*/!filament_ops_self_home);
}

AmsError spools_on_bed_refusal() {
    return AmsError(AmsResult::WRONG_STATE, "Tool change blocked: spools on the bed",
                    lv_tr(spool_latch_message()), "");
}

AmsError needs_attention_refusal() {
    return AmsError(AmsResult::WRONG_STATE, "Tool change blocked: filament system paused",
                    lv_tr("The filament system needs attention"),
                    lv_tr("Resolve it on the filament system screen, then try again"));
}

ToolChangeDecision decide_tool_change(const ToolChangeInputs& in) {
    if (in.already_active) {
        return {ToolChangeVerdict::AlreadyActive};
    }
    if (in.spools_on_bed) {
        return {ToolChangeVerdict::RefuseSpoolsOnBed, spools_on_bed_refusal()};
    }
    AmsError refusal = tool_change_print_refusal(in.lifecycle, in.filament_ops_self_home);
    if (!refusal.success()) {
        return {ToolChangeVerdict::RefusePrint, std::move(refusal)};
    }
    if (in.backend_action == AmsAction::PAUSED) {
        return {ToolChangeVerdict::RefuseNeedsAttention, needs_attention_refusal()};
    }
    if (ams_action_holds_system(in.backend_action)) {
        return {ToolChangeVerdict::Busy};
    }
    // Reaching here while PAUSED means the backend permits filament ops on a
    // paused job: pause-then-swap is the runout and colour-change recovery
    // workflow. It is confirmed whatever the setting says, because the head
    // moves into a part still on the bed.
    if (in.lifecycle == PrintState::Paused) {
        return {ToolChangeVerdict::ConfirmPaused};
    }
    if (in.confirm_enabled) {
        return {ToolChangeVerdict::ConfirmSetting};
    }
    return {ToolChangeVerdict::Immediate};
}

ToolChangeInputs tool_change_inputs(const PrinterState& printer_state, int tool_index) {
    ToolChangeInputs in;
    in.lifecycle = printer_state.print_state().get_print_lifecycle();
    in.spools_on_bed = printer_state.print_state().spool_latch_active();
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        in.filament_ops_self_home = backend->filament_ops_self_home();
        in.backend_action = backend->get_system_info().action;
    }
    in.already_active = tool_index == ToolState::instance().active_tool_index();
    in.confirm_enabled = SafetySettingsManager::instance().get_tool_change_require_confirmation();
    return in;
}

namespace {

void dispatch(int tool_index, const ToolChangeHooks& hooks) {
    spdlog::info("[ToolChange] Requesting tool change to T{}", tool_index);
    IMoonrakerAPI* api = hooks.resolve_api ? hooks.resolve_api() : get_moonraker_api();
    // A null api is not a reason to skip the call: an AMS backend changes tool
    // without one, and request_tool_change() reports "No API connection"
    // through on_error when there is no backend either.
    ToolState::instance().request_tool_change(
        tool_index, api, hooks.on_success, [on_failed = hooks.on_failed](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            if (on_failed) {
                on_failed();
            }
        });
}

/// Log and toast a verdict that dispatches nothing: refusals raise a warning,
/// Busy and AlreadyActive raise nothing.
void report_withheld(const ToolChangeDecision& decision, int tool_index) {
    switch (decision.verdict) {
    case ToolChangeVerdict::AlreadyActive:
        spdlog::debug("[ToolChange] T{} already active, ignoring", tool_index);
        break;
    case ToolChangeVerdict::Busy:
        spdlog::info("[ToolChange] T{} ignored: filament system busy", tool_index);
        break;
    case ToolChangeVerdict::RefuseNeedsAttention:
    case ToolChangeVerdict::RefuseSpoolsOnBed:
    case ToolChangeVerdict::RefusePrint:
        spdlog::info("[ToolChange] Tool change to T{} refused: {}", tool_index,
                     decision.refusal.technical_msg);
        notify_ams_warning(decision.refusal);
        break;
    case ToolChangeVerdict::ConfirmPaused:
    case ToolChangeVerdict::ConfirmSetting:
    case ToolChangeVerdict::Immediate:
        break;
    }
}

std::string full_tool_label(int tool_index) {
    const auto& tools = ToolState::instance().tools();
    if (tool_index < 0 || tool_index >= static_cast<int>(tools.size())) {
        return tool_label(tool_index);
    }
    const ToolInfo& tool = tools[static_cast<size_t>(tool_index)];
    return active_tool_label(tool.name, tool.index, ToolLabelForm::Full);
}

/// What an open confirmation owes its caller: the change on the primary
/// button, on_declined on every other way the dialog closes. The primary
/// button decides again from live state, because a print can start or the
/// filament system can stop while the dialog is open. A print paused since
/// the dialog opened asks again with the paused warning instead of changing. A close that
/// fires no dialog callback (a programmatic hide) still drops the dialog's
/// callbacks, and with them the last reference to this.
class PendingChange {
  public:
    PendingChange(const PrinterState& printer_state, int tool_index, bool warned_paused,
                  ToolChangeHooks hooks)
        : printer_state_(printer_state), tool_index_(tool_index), warned_paused_(warned_paused),
          hooks_(std::move(hooks)) {}
    PendingChange(const PendingChange&) = delete;
    PendingChange& operator=(const PendingChange&) = delete;

    ~PendingChange() {
        // An owner that is gone has nothing left to revert.
        if (!hooks_.owner_token || !hooks_.owner_token->expired()) {
            decline();
        }
    }

    void confirm() {
        if (settled_) {
            return;
        }
        const ToolChangeDecision decision =
            decide_tool_change(tool_change_inputs(printer_state_, tool_index_));
        if (!decision.dispatches()) {
            report_withheld(decision, tool_index_);
            decline();
            return;
        }
        settled_ = true;
        if (decision.verdict == ToolChangeVerdict::ConfirmPaused && !warned_paused_) {
            reoffer();
            return;
        }
        dispatch(tool_index_, hooks_);
    }

    void decline() {
        if (settled_) {
            return;
        }
        settled_ = true;
        if (hooks_.on_declined) {
            hooks_.on_declined();
        }
    }

  private:
    /// Offer the change afresh once this dialog has closed: confirm() runs
    /// before the dialog hides, so a dialog raised here would stack under it.
    void reoffer() {
        spdlog::info("[ToolChange] Print paused while confirming T{}: asking again", tool_index_);
        std::optional<LifetimeToken> token = hooks_.owner_token;
        auto again = [&printer_state = printer_state_, tool_index = tool_index_,
                      hooks = std::move(hooks_)]() mutable {
            offer_tool_change(printer_state, tool_index, std::move(hooks));
        };
        if (token) {
            token->defer("ToolChange::reoffer", std::move(again));
        } else {
            queue_update("ToolChange::reoffer", std::move(again));
        }
    }

    const PrinterState& printer_state_;
    int tool_index_;
    bool warned_paused_;
    ToolChangeHooks hooks_;
    bool settled_ = false;
};

} // namespace

ToolChangeVerdict offer_tool_change(const PrinterState& printer_state, int tool_index,
                                    ToolChangeHooks hooks) {
    const ToolChangeDecision decision =
        decide_tool_change(tool_change_inputs(printer_state, tool_index));

    switch (decision.verdict) {
    case ToolChangeVerdict::AlreadyActive:
    case ToolChangeVerdict::Busy:
    case ToolChangeVerdict::RefuseNeedsAttention:
    case ToolChangeVerdict::RefuseSpoolsOnBed:
    case ToolChangeVerdict::RefusePrint:
        report_withheld(decision, tool_index);
        break;
    case ToolChangeVerdict::Immediate:
        dispatch(tool_index, hooks);
        return decision.verdict;
    case ToolChangeVerdict::ConfirmPaused:
    case ToolChangeVerdict::ConfirmSetting: {
        spdlog::info("[ToolChange] Confirming tool change to T{}", tool_index);
        const bool paused = decision.verdict == ToolChangeVerdict::ConfirmPaused;
        const std::string message =
            paused ? std::string(lv_tr(
                         "The print is paused. Changing tools now moves the toolhead and swaps "
                         "the filament at the nozzle. Resume the print once the change finishes."))
                   : fmt::format(fmt::runtime(lv_tr("Switch to {}?")), full_tool_label(tool_index));
        ConfirmOptions options;
        options.owner_token = hooks.owner_token;
        auto pending =
            std::make_shared<PendingChange>(printer_state, tool_index, paused, std::move(hooks));
        options.on_cancel = [pending] { pending->decline(); };
        options.on_dismiss = [pending] { pending->decline(); };
        modal_confirm(
            paused ? lv_tr("Change Tool While Paused") : lv_tr("Change Tool"), message.c_str(),
            paused ? ::ModalSeverity::Warning : ::ModalSeverity::Info, lv_tr("Change Tool"),
            [pending] { pending->confirm(); }, options);
        return decision.verdict;
    }
    }

    if (hooks.on_declined) {
        hooks.on_declined();
    }
    return decision.verdict;
}

bool request_tool_park(const PrinterState& printer_state) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_tool_park()) {
        return false;
    }
    if (printer_state.print_state().spool_latch_active()) {
        spdlog::info("[ToolPark] Refused: spools are on the bed");
        notify_ams_warning(spools_on_bed_refusal());
        return false;
    }
    if (job_holds_machine(printer_state.print_state().get_print_lifecycle())) {
        spdlog::info("[ToolPark] Refused: print in progress");
        notify_ams_warning(AmsError(AmsResult::WRONG_STATE, "Tool park blocked: print in progress",
                                    lv_tr("Cannot park while a print is active"), ""));
        return false;
    }
    const int active = ToolState::instance().active_tool_index();
    if (active < 0) {
        spdlog::debug("[ToolPark] Ignored: no tool mounted");
        return false;
    }
    spdlog::info("[ToolPark] Parking T{}", active);
    const AmsError err = backend->park_active_tool();
    if (!err.success()) {
        notify_ams_error(err);
        return false;
    }
    return true;
}

} // namespace helix::ui
