// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_external_spool_menu.h"

#include "ui_error_reporting.h"
#include "ui_overlay_qr_scanner.h"

#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "spoolman_types.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void show_external_spool_menu(lv_obj_t* parent_screen, lv_obj_t* anchor_widget,
                              std::unique_ptr<AmsContextMenu>& context_menu,
                              std::function<void(bool open_on_picker)> on_edit_action) {
    if (!parent_screen || !anchor_widget) {
        return;
    }

    lv_point_t click_pt = {0, 0};
    if (auto* indev = lv_indev_active()) {
        lv_indev_get_point(indev, &click_pt);
    }

    if (!context_menu) {
        context_menu = std::make_unique<AmsContextMenu>();
    }

    context_menu->set_action_callback([parent_screen, edit = std::move(on_edit_action)](
                                          AmsContextMenu::MenuAction action, int /*slot*/) {
        switch (action) {
        case AmsContextMenu::MenuAction::EDIT:
            if (edit) {
                edit(/*open_on_picker=*/false);
            }
            break;

        case AmsContextMenu::MenuAction::SPOOLMAN:
            if (edit) {
                edit(/*open_on_picker=*/true);
            }
            break;

        case AmsContextMenu::MenuAction::SCAN_QR: {
#if HELIX_HAS_CAMERA
            auto& scanner = get_qr_scanner_overlay();
            scanner.show_for_active_spool(parent_screen, [](const SpoolInfo& spool) {
                SlotInfo info;
                apply_spool_to_slot(info, spool);
                ::AmsState::instance().commit_external_spool_edit(info);
                spdlog::info("[ExternalSpoolMenu] QR scan assigned spool #{} to external spool",
                             spool.id);
            });
#endif // HELIX_HAS_CAMERA
            break;
        }

        case AmsContextMenu::MenuAction::CLEAR_SPOOL:
            ::AmsState::instance().commit_external_spool_edit(SlotInfo{});
            NOTIFY_INFO(lv_tr("External spool cleared"));
            break;

        case AmsContextMenu::MenuAction::CANCELLED:
        default:
            break;
        }
    });

    context_menu->set_click_point(click_pt);
    context_menu->show_for_external_spool(parent_screen, anchor_widget);
}

} // namespace helix::ui
