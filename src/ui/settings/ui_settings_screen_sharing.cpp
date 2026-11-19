// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_screen_sharing.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"

#include "remote_screen_settings_manager.h"
#include "screen_secrets.h"

namespace helix::settings {

lv_obj_t* ScreenSharingSettingsOverlay::create(lv_obj_t* parent) {
    lv_obj_t* root = OverlayBase::create(parent);
    if (root) {
        // The screen server hides the shared screen while the code row is on the
        // display. Without the row, the whole page counts as the secret.
        lv_obj_t* row = lv_obj_find_by_name(root, "row_remote_pair_code");
        helix::ui::mark_screen_secret(row ? row : root);
    }
    return root;
}

void ScreenSharingSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_remote_screen_changed",
         [](lv_event_t* e) {
             RemoteScreenSettingsManager::instance().set_enabled(helix::ui::event_checked(e));
         }},
        {"on_remote_webcam_changed",
         [](lv_event_t* e) {
             RemoteScreenSettingsManager::instance().set_webcam(helix::ui::event_checked(e));
         }},
        {"on_remote_control_changed",
         [](lv_event_t* e) {
             RemoteScreenSettingsManager::instance().set_control(helix::ui::event_checked(e));
         }},
        {"on_remote_forget_clicked",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Forget Paired Browsers?"),
                 lv_tr("Every browser that can control this screen will need the new pairing "
                       "code."),
                 ModalSeverity::Warning, lv_tr("Forget"),
                 [] { RemoteScreenSettingsManager::instance().forget_paired_browsers(); });
         }},
    });
}

} // namespace helix::settings
