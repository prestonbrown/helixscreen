// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_external_spool_menu.h"

#include "ui_ams_edit_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_error_reporting.h"
#include "ui_overlay_qr_scanner.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "spoolman_types.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

/// Open the external-spool editor. It needs only a parent screen and the global
/// API, so it belongs beside the menu that raises it rather than in every
/// surface that shows the menu.
void show_external_spool_editor(lv_obj_t* parent_screen, const ExternalSpoolMenuHooks& hooks,
                                bool open_on_picker) {
    if (hooks.on_edit) {
        hooks.on_edit(open_on_picker);
        return;
    }
    if (!parent_screen) {
        return;
    }

    auto ext = helix::AmsState::instance().get_external_spool_info();
    SlotInfo initial = ext.value_or(SlotInfo{});
    initial.slot_index = EXTERNAL_SPOOL_SLOT;
    initial.global_index = EXTERNAL_SPOOL_SLOT;

    get_ams_edit_overlay().show_for_slot(
        parent_screen, EXTERNAL_SPOOL_SLOT, initial, get_moonraker_api(),
        [](const AmsEditOverlay::EditResult& result) {
            if (result.saved) {
                helix::AmsState::instance().commit_external_spool_edit(result.slot_info);
                NOTIFY_INFO(lv_tr("External spool updated"));
            }
        },
        open_on_picker);
}

} // namespace

ExternalSpoolMenuHooks sidebar_external_spool_hooks(AmsOperationSidebar* sidebar) {
    if (!sidebar) {
        return {};
    }
    ExternalSpoolMenuHooks hooks;
    // Dispatch only — the menu engages bypass before calling these.
    hooks.on_load = [sidebar]() { sidebar->handle_load_with_preheat(EXTERNAL_SPOOL_SLOT); };
    hooks.on_unload = [sidebar]() { sidebar->handle_unload(EXTERNAL_SPOOL_SLOT); };
    hooks.toggle = sidebar->bypass_controller();
    return hooks;
}

void show_external_spool_menu(lv_obj_t* parent_screen, lv_obj_t* anchor_widget,
                              std::unique_ptr<AmsContextMenu>& context_menu,
                              ExternalSpoolMenuHooks hooks) {
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

    // Read before the move: the reveal below needs it, and hooks is gone after.
    BypassToggleController* const hooks_toggle = hooks.toggle;

    context_menu->set_action_callback(
        [parent_screen, h = std::move(hooks)](AmsContextMenu::MenuAction action, int /*slot*/) {
            switch (action) {
            case AmsContextMenu::MenuAction::LOAD: {
                // Engage first, always. With bypass disengaged plan_load()
                // refuses EXTERNAL_SPOOL_SLOT outright, so dispatching straight
                // into the executor answers a tap on the bypass spool with
                // "Select a filament slot" — the dead end this menu exists to
                // remove. Owned here so no surface can omit it.
                std::function<void()> dispatch = h.on_load ? h.on_load : [] {
                    execute_filament_load(helix::AmsState::instance().get_backend(),
                                          EXTERNAL_SPOOL_SLOT, "[ExternalSpoolMenu]");
                };
                if (h.toggle) {
                    h.toggle->ensure_engaged_then(std::move(dispatch));
                } else {
                    dispatch();
                }
                break;
            }

            case AmsContextMenu::MenuAction::UNLOAD:
                // No engage: plan_unload() resolves EXTERNAL_SPOOL_SLOT whatever
                // the bypass state, and engaging in order to unload something
                // that is not there would move the path for nothing.
                if (h.on_unload) {
                    h.on_unload();
                } else {
                    AmsBackend* backend = helix::AmsState::instance().get_backend();
                    AmsSystemInfo info;
                    if (backend) {
                        info = backend->get_system_info();
                    }
                    // Never answered inline — the divergence
                    // read_unload_target_loaded() exists to prevent.
                    execute_filament_unload(
                        backend, EXTERNAL_SPOOL_SLOT,
                        read_unload_target_loaded(backend, info, EXTERNAL_SPOOL_SLOT),
                        "[ExternalSpoolMenu]");
                }
                break;

            case AmsContextMenu::MenuAction::PURGE: {
                // The gate was decided when the menu opened; a print or an AMS op
                // can start while it sits there. execute_filament_purge() has no
                // guard of its own, so re-ask before extruding.
                AmsBackend* backend = helix::AmsState::instance().get_backend();
                AmsSystemInfo info;
                if (backend) {
                    info = backend->get_system_info();
                }
                const OpButtonState state = build_external_spool_gating_state(
                    read_unload_target_loaded(backend, info, EXTERNAL_SPOOL_SLOT),
                    backend && info.is_busy(), get_printer_state().get_print_lifecycle(),
                    backend && backend->filament_ops_self_home());
                if (compute_op_button_gating(state).purge_disabled) {
                    NOTIFY_WARNING(lv_tr("Wait for the current filament operation to finish"));
                    break;
                }
                execute_filament_purge("[ExternalSpoolMenu]");
                break;
            }

            case AmsContextMenu::MenuAction::TOGGLE_BYPASS:
                if (h.toggle) {
                    h.toggle->toggle();
                }
                break;

            case AmsContextMenu::MenuAction::EDIT:
                show_external_spool_editor(parent_screen, h, /*open_on_picker=*/false);
                break;

            case AmsContextMenu::MenuAction::SPOOLMAN:
                show_external_spool_editor(parent_screen, h, /*open_on_picker=*/true);
                break;

            case AmsContextMenu::MenuAction::SCAN_QR: {
                auto& scanner = get_qr_scanner_overlay();
                scanner.show_for_active_spool(parent_screen, [](const SpoolInfo& spool) {
                    SlotInfo info;
                    apply_spool_to_slot(info, spool);
                    helix::AmsState::instance().commit_external_spool_edit(info);
                    spdlog::info("[ExternalSpoolMenu] QR scan assigned spool #{} to external spool",
                                 spool.id);
                });
                break;
            }

            case AmsContextMenu::MenuAction::CLEAR_SPOOL:
                helix::AmsState::instance().commit_external_spool_edit(SlotInfo{});
                NOTIFY_INFO(lv_tr("External spool cleared"));
                break;

            case AmsContextMenu::MenuAction::CANCELLED:
            default:
                break;
            }
        });

    context_menu->set_click_point(click_pt);
    context_menu->show_for_external_spool(parent_screen, anchor_widget,
                                          /*offer_toggle=*/hooks_toggle != nullptr);
}

} // namespace helix::ui
