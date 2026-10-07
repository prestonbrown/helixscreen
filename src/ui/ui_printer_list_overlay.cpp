// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_list_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_change_host_modal.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_nav_printer_badge.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

/// Walk up the parent chain to find the printer_list_item row.
/// The row is the child of "printer_list_container" and has the printer ID as its name.
std::string find_printer_id_from_event(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* obj = target;
    while (obj) {
        lv_obj_t* parent = lv_obj_get_parent(obj);
        if (parent) {
            const char* parent_name = lv_obj_get_name(parent);
            if (parent_name && std::string_view(parent_name) == "printer_list_container") {
                // obj is a direct child of the container; it's the row
                const char* row_name = lv_obj_get_name(obj);
                if (row_name && row_name[0] != '\0') {
                    return std::string(row_name);
                }
            }
        }
        obj = parent;
    }
    return {};
}

} // namespace

// =============================================================================
// Callback Registration
// =============================================================================

void PrinterListOverlay::register_callbacks() {
    register_xml_callbacks({
        {"printer_list_add_cb",
         [](lv_event_t*) { get_printer_list_overlay().handle_add_printer(); }},
        {"printer_list_row_cb",
         [](lv_event_t* e) {
             std::string id = find_printer_id_from_event(e);
             if (id.empty()) {
                 spdlog::warn("[PrinterListOverlay] Row click with no printer ID");
                 return;
             }
             get_printer_list_overlay().handle_switch_printer(id);
         }},
        {"printer_list_delete_cb",
         [](lv_event_t* e) {
             std::string id = find_printer_id_from_event(e);
             if (id.empty()) {
                 spdlog::warn("[PrinterListOverlay] Delete click with no printer ID");
                 return;
             }
             get_printer_list_overlay().handle_delete_printer(id);
         }},
        {"on_printer_switcher_changed",
         [](lv_event_t* e) {
             bool enabled = event_checked(e);
             spdlog::info("[PrinterListOverlay] Printer switcher toggled: {}",
                          enabled ? "ON" : "OFF");
             SettingsManager::instance().set_show_printer_switcher(enabled);
         }},
    });
}

// =============================================================================
// Lifecycle
// =============================================================================

void PrinterListOverlay::on_activate() {
    OverlayBase::on_activate();

    populate_printer_list();
}

// =============================================================================
// Printer List Population
// =============================================================================

void PrinterListOverlay::populate_printer_list() {
    auto* cfg = Config::get_instance();

    auto printer_ids = cfg->get_printer_ids();
    auto active_id = cfg->get_active_printer_id();

    lv_obj_t* container = find_required(overlay_root_, "printer_list_container", get_name());
    if (!container) {
        return;
    }

    active_dot_observer_.reset();
    active_dot_.reset();

    // Clean existing children before repopulating (freeze queue to prevent
    // background thread from enqueuing callbacks between drain and destroy)
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::safe_clean_children(container);
    }

    for (const auto& id : printer_ids) {
        bool is_active = (id == active_id);
        std::string name = cfg->get<std::string>("/printers/" + id + "/printer_name", id);

        // Create row from XML component
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(container, "printer_list_item", nullptr));
        if (!row) {
            spdlog::warn("[{}] Failed to create printer_list_item for '{}'", get_name(), id);
            continue;
        }

        // Tag with printer ID so callbacks can identify it
        lv_obj_set_name(row, id.c_str());

        // Set printer name
        lv_obj_t* name_label = find_required(row, "printer_name", get_name());
        if (name_label) {
            lv_label_set_text(name_label, name.c_str());
        }

        // Mark active printer with checked state (triggers left border accent style)
        if (is_active) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
            // Show check icon for active printer
            lv_obj_t* check_icon = find_required(row, "active_check", get_name());
            if (check_icon) {
                lv_obj_set_style_text_opa(check_icon, LV_OPA_COVER, LV_PART_MAIN);
            }
            lv_obj_t* dot = find_required(row, "connection_dot", get_name());
            if (dot) {
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
                active_dot_ = dot;
                active_dot_observer_ = observe<int>(
                    get_printer_state().network_state().get_printer_connection_state_subject(),
                    this,
                    [](PrinterListOverlay* self, int state) {
                        if (self->active_dot_) {
                            lv_obj_set_style_bg_color(self->active_dot_,
                                                      connection_dot_color(state), 0);
                        }
                    },
                    get_printer_state().get_subjects_lifetime());
            }
        }

        // Show delete button when more than 1 printer
        if (printer_ids.size() > 1) {
            lv_obj_t* del_btn = find_required(row, "delete_btn", get_name());
            if (del_btn) {
                lv_obj_remove_flag(del_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    spdlog::debug("[{}] Populated {} printers (active: {})", get_name(), printer_ids.size(),
                  active_id);
}

// =============================================================================
// Action Handlers
// =============================================================================

void PrinterListOverlay::handle_switch_printer(const std::string& printer_id) {
    auto* cfg = Config::get_instance();
    if (printer_id == cfg->get_active_printer_id()) {
        return; // Already active
    }
    spdlog::info("[{}] Switching to printer '{}'", get_name(), printer_id);
    helix::ui::drop_held_connection_failed();

    // Defer dismiss + switch — we're inside a click event on a child widget
    helix::ui::queue_update("PrinterListOverlay::handle_switch_printer", [printer_id]() {
        helix::nav::go_back();
        NavigationManager::instance().trigger_printer_switch(printer_id);
    });
}

void PrinterListOverlay::handle_delete_printer(const std::string& printer_id) {
    auto* cfg = Config::get_instance();

    std::string name =
        cfg->get<std::string>("/printers/" + printer_id + "/printer_name", printer_id);

    std::string msg = "Remove " + name + "? All settings for this printer will be deleted.";

    modal_confirm("Remove Printer", msg.c_str(), ModalSeverity::Error, "Remove", [printer_id] {
        if (printer_id.empty()) {
            return;
        }

        auto* cfg = Config::get_instance();
        bool was_active = (printer_id == cfg->get_active_printer_id());
        spdlog::info("[PrinterListOverlay] Removing printer '{}'", printer_id);
        cfg->remove_printer(printer_id);
        cfg->save();

        if (was_active) {
            // Defer switch out of modal callback - soft restart tears down UI
            auto remaining = cfg->get_printer_ids();
            if (!remaining.empty()) {
                std::string next_id = remaining.front();
                helix::ui::queue_update("PrinterListOverlay::handle_delete_printer", [next_id]() {
                    helix::nav::go_back(); // dismiss overlay
                    NavigationManager::instance().trigger_printer_switch(next_id);
                });
            }
        } else {
            // Defer repopulation out of modal callback to avoid widget
            // mutation mid-event
            helix::ui::queue_update("PrinterListOverlay::handle_delete_printer",
                                    []() { get_printer_list_overlay().populate_printer_list(); });
        }
    });
}

void PrinterListOverlay::handle_add_printer() {
    spdlog::info("[{}] Add printer requested", get_name());
    helix::ui::drop_held_connection_failed();

    // Defer dismiss + wizard launch — we're inside a click event on a child widget
    helix::ui::queue_update("PrinterListOverlay::handle_add_printer", []() {
        helix::nav::go_back();
        // go_back() queues the pop, and the pop hides every stray screen child.
        // Queued behind it, the add-printer modal opens after that sweep.
        helix::ui::queue_update("PrinterListOverlay::trigger_add_printer",
                                []() { NavigationManager::instance().trigger_add_printer(); });
    });
}

} // namespace helix::ui
