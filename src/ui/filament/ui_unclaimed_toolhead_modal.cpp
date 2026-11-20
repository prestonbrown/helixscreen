// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unclaimed_toolhead_modal.h"

#include "ui_bypass_toggle_controller.h"
#include "ui_unload_lane_choice.h"
#include "ui_widget_ref.h"

#include "ams_state.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

namespace {

/// The prompt on screen, if any. Weak: the modal stack owns and frees it.
WeakWidget s_dialog;

/// The state the prompt asked about still holds. A stale tap does nothing.
bool still_unclaimed(const char* action) {
    if (AmsState::instance().unclaimed_toolhead_now()) {
        return true;
    }
    spdlog::info("[UnclaimedToolhead] {} ignored: the toolhead's filament is accounted for now",
                 action);
    return false;
}

} // namespace

void UnclaimedToolheadModal::offer() {
    if (s_dialog.get() != nullptr) {
        return;
    }
    const bool unload = offers_unload(AmsState::instance().get_backend());
    const char* attrs[] = {"hide_unload", unload ? "false" : "true", "hide_by_hand",
                           unload ? "true" : "false", nullptr};
    auto modal = std::make_unique<UnclaimedToolheadModal>();
    UnclaimedToolheadModal* shown = modal.get();
    if (!Modal::show_owned(std::move(modal), lv_screen_active(), attrs)) {
        AmsState::instance().unclaimed_toolhead_prompt_closed();
        return;
    }
    s_dialog = shown->dialog();
}

bool UnclaimedToolheadModal::offers_unload(const AmsBackend* backend) {
    return backend != nullptr && backend->can_clear_unaccounted_toolhead();
}

void UnclaimedToolheadModal::dismiss() {
    if (lv_obj_t* dialog = s_dialog.get()) {
        Modal::hide(dialog);
    }
}

void UnclaimedToolheadModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");
}

void UnclaimedToolheadModal::on_hide() {
    s_dialog.reset();
    AmsState::instance().unclaimed_toolhead_prompt_closed();
}

void UnclaimedToolheadModal::on_ok() {
    if (still_unclaimed("Use as bypass")) {
        // No lane is loaded, so no unload has to run first and the engage
        // completes before the controller goes out of scope.
        BypassToggleController controller;
        controller.ensure_engaged_then([] {});
    }
    hide(ModalCloseReason::ButtonPress);
}

void UnclaimedToolheadModal::on_tertiary() {
    // Closed first: the lane question opens its own dialog.
    hide(ModalCloseReason::ButtonPress);
    if (!still_unclaimed("Unload")) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend == nullptr || !offers_unload(backend)) {
        return;
    }
    FilamentOpSurface surface;
    surface.log_tag = "[UnclaimedToolhead]";
    // The same slotless toolhead unload every other Unload surface runs: a
    // backend that cannot name the lane asks first.
    if (ask_lane_then_unload(backend, nullptr, surface)) {
        return;
    }
    execute_filament_unload(backend, backend->unaccounted_toolhead_clear_slot(),
                            /*target_is_loaded=*/true, surface);
}

} // namespace helix::ui
