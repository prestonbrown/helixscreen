// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_follow_up.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void offer_edit_follow_up(int slot_index, const SlotInfo& before, const SlotInfo& after) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const auto offer = backend->edit_follow_up(slot_index, before, after);
    if (!offer) {
        return;
    }
    spdlog::info("[AmsEditFollowUp] Offering '{}' for slot {}", offer->title, slot_index);
    ConfirmOptions options;
    options.cancel_text = lv_tr("Not now");
    modal_confirm(
        offer->title.c_str(), offer->message.c_str(), ModalSeverity::Info,
        offer->confirm_label.c_str(),
        [slot_index, before, after]() {
            // Re-fetched: the backend can be replaced while the dialog is up.
            AmsBackend* current = AmsState::instance().get_backend();
            if (!current) {
                return;
            }
            const AmsError err = current->run_edit_follow_up(slot_index, before, after);
            if (!err.success()) {
                notify_ams_error(err);
            }
        },
        options);
}

} // namespace helix::ui
