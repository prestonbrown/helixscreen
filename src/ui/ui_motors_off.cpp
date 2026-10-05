// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_motors_off.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "app_globals.h"
#include "printer_state.h"

namespace helix::ui {

void show_motors_off_confirm(IMoonrakerAPI* api, ModalGuard& stored,
                             std::optional<LifetimeToken> owner_token) {
    spdlog::debug("[MotorsOff] Showing motors disable confirmation");

    // The dialog closes itself on a button press; every close path just drops
    // the stored handle. on_dismiss alone is not enough: a button press that
    // reaches a callback is an answer, not a dismissal, so the cancel and
    // confirm sides must drop it too or the caller's next entry is blocked.
    auto drop = [&stored] { stored.release(); };

    ConfirmOptions opts;
    opts.on_cancel = drop;
    opts.on_dismiss = drop;
    opts.owner_token = std::move(owner_token);

    stored = modal_confirm(
        lv_tr("Disable Motors?"), lv_tr("Release all stepper motors. Position will be lost."),
        ModalSeverity::Warning, lv_tr("Disable"),
        [api, drop] {
            drop();
            if (!api) {
                return;
            }
            // A print can start while the dialog is open (Mainsail, the job
            // queue), and the button's gate was only checked when it opened.
            if (lv_subject_get_int(get_printer_state().get_machine_motion_blocked_subject()) != 0) {
                NOTIFY_WARNING(lv_tr("Motors stay on while a print is active"));
                return;
            }
            NOTIFY_INFO(lv_tr("Disabling motors..."));
            api->execute_gcode(
                "M84", // Klipper command to disable steppers
                []() { helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("Motors disabled")); },
                [](const MoonrakerError& err) {
                    helix::ui::notify_error_tr(TR_NOOP("Motors disable failed: {}"), err);
                });
        },
        opts);

    if (!stored) {
        LOG_ERROR_INTERNAL("Failed to create motors confirmation dialog");
        NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
    }
}

} // namespace helix::ui
