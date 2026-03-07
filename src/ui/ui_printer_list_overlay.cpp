// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_list_overlay.h"

#include "app_globals.h"
#include "config.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

// =============================================================================
// Global Instance
// =============================================================================

static std::unique_ptr<helix::ui::PrinterListOverlay> g_printer_list_overlay;

namespace helix::ui {

bool PrinterListOverlay::s_callbacks_registered_ = false;
std::string PrinterListOverlay::s_pending_delete_id_;

PrinterListOverlay& get_printer_list_overlay() {
    if (!g_printer_list_overlay) {
        g_printer_list_overlay = std::make_unique<PrinterListOverlay>();
        StaticPanelRegistry::instance().register_destroy("PrinterListOverlay",
                                                         []() { g_printer_list_overlay.reset(); });
    }
    return *g_printer_list_overlay;
}

// =============================================================================
// Callback Registration
// =============================================================================

void PrinterListOverlay::register_callbacks() {
    if (s_callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"printer_list_add_cb", on_add_printer_cb},
        {"printer_list_row_cb", on_printer_row_cb},
        {"printer_list_delete_cb", on_delete_printer_cb},
    });

    s_callbacks_registered_ = true;
    spdlog::debug("[PrinterListOverlay] Callbacks registered");
}

// =============================================================================
// Create / Show
// =============================================================================

lv_obj_t* PrinterListOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    if (!create_overlay_from_xml(parent, "printer_list_overlay")) {
        return nullptr;
    }

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void PrinterListOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    register_callbacks();

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// =============================================================================
// Lifecycle
// =============================================================================

void PrinterListOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_printer_list();
}

void PrinterListOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// =============================================================================
// Printer List Population
// =============================================================================

void PrinterListOverlay::populate_printer_list() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        spdlog::warn("[{}] No config instance", get_name());
        return;
    }

    auto printer_ids = cfg->get_printer_ids();
    auto active_id = cfg->get_active_printer_id();

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "printer_list_container");
    if (!container) {
        spdlog::error("[{}] printer_list_container not found in XML", get_name());
        return;
    }

    // Clean existing children before repopulating (freeze queue to prevent
    // background thread from enqueuing callbacks between drain and destroy)
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_clean(container);
    }

    for (const auto& id : printer_ids) {
        bool is_active = (id == active_id);
        std::string name = cfg->get<std::string>("/printers/" + id + "/printer_name", id);

        // Create row from XML component
        auto* row = static_cast<lv_obj_t*>(
            lv_xml_create(container, "printer_list_item", nullptr));
        if (!row) {
            spdlog::warn("[{}] Failed to create printer_list_item for '{}'", get_name(), id);
            continue;
        }

        // Tag with printer ID so callbacks can identify it
        lv_obj_set_name(row, id.c_str());

        // Set printer name
        lv_obj_t* name_label = lv_obj_find_by_name(row, "printer_name");
        if (name_label) {
            lv_label_set_text(name_label, name.c_str());
        }

        // Mark active printer with checked state (triggers left border accent style)
        if (is_active) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
            // Show check icon for active printer
            lv_obj_t* check_icon = lv_obj_find_by_name(row, "active_check");
            if (check_icon) {
                lv_obj_set_style_text_opa(check_icon, LV_OPA_COVER, LV_PART_MAIN);
            }
        }

        // Show delete button when more than 1 printer
        if (printer_ids.size() > 1) {
            lv_obj_t* del_btn = lv_obj_find_by_name(row, "delete_btn");
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
    if (cfg && printer_id == cfg->get_active_printer_id()) {
        return; // Already active
    }
    spdlog::info("[{}] Switching to printer '{}'", get_name(), printer_id);

    // Defer dismiss + switch — we're inside a click event on a child widget
    helix::ui::queue_update([printer_id]() {
        NavigationManager::instance().go_back();
        NavigationManager::instance().trigger_printer_switch(printer_id);
    });
}

void PrinterListOverlay::handle_delete_printer(const std::string& printer_id) {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        return;
    }

    std::string name =
        cfg->get<std::string>("/printers/" + printer_id + "/printer_name", printer_id);

    // Store pending delete ID as static (overlay is a singleton; only one confirm at a time)
    s_pending_delete_id_ = printer_id;

    std::string msg = "Remove " + name + "? All settings for this printer will be deleted.";

    modal_show_confirmation("Remove Printer", msg.c_str(), ModalSeverity::Error, "Remove",
                            on_delete_confirm_cb, on_delete_cancel_cb, nullptr);
}

void PrinterListOverlay::handle_add_printer() {
    spdlog::info("[{}] Add printer requested", get_name());

    // Defer dismiss + wizard launch — we're inside a click event on a child widget
    helix::ui::queue_update([]() {
        NavigationManager::instance().go_back();
        NavigationManager::instance().trigger_add_printer();
    });
}

// =============================================================================
// Helpers
// =============================================================================

/// Walk up the parent chain to find the printer_list_item row.
/// The row is the child of "printer_list_container" and has the printer ID as its name.
static std::string find_printer_id_from_event(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* obj = target;
    while (obj) {
        lv_obj_t* parent = lv_obj_get_parent(obj);
        if (parent) {
            const char* parent_name = lv_obj_get_name(parent);
            if (parent_name && std::string_view(parent_name) == "printer_list_container") {
                // obj is a direct child of the container — it's the row
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

// =============================================================================
// Static Callbacks
// =============================================================================

void PrinterListOverlay::on_add_printer_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterListOverlay] on_add_printer_cb");
    get_printer_list_overlay().handle_add_printer();
    LVGL_SAFE_EVENT_CB_END();
}

void PrinterListOverlay::on_printer_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterListOverlay] on_printer_row_cb");

    std::string id = find_printer_id_from_event(e);
    if (id.empty()) {
        spdlog::warn("[PrinterListOverlay] Row click with no printer ID");
        return;
    }

    get_printer_list_overlay().handle_switch_printer(id);

    LVGL_SAFE_EVENT_CB_END();
}

void PrinterListOverlay::on_delete_printer_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterListOverlay] on_delete_printer_cb");

    std::string id = find_printer_id_from_event(e);
    if (id.empty()) {
        spdlog::warn("[PrinterListOverlay] Delete click with no printer ID");
        return;
    }

    get_printer_list_overlay().handle_delete_printer(id);

    LVGL_SAFE_EVENT_CB_END();
}

void PrinterListOverlay::on_delete_confirm_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterListOverlay] on_delete_confirm_cb");

    (void)e;
    std::string printer_id = s_pending_delete_id_;
    s_pending_delete_id_.clear();
    if (printer_id.empty()) {
        return;
    }

    // Close the modal first
    lv_obj_t* top = Modal::get_top();
    if (top) {
        Modal::hide(top);
    }

    auto* cfg = Config::get_instance();
    if (cfg) {
        bool was_active = (printer_id == cfg->get_active_printer_id());
        spdlog::info("[PrinterListOverlay] Removing printer '{}'", printer_id);
        cfg->remove_printer(printer_id);
        cfg->save();

        if (was_active) {
            // Defer switch out of modal callback — soft restart tears down UI
            auto remaining = cfg->get_printer_ids();
            if (!remaining.empty()) {
                std::string next_id = remaining.front();
                helix::ui::queue_update([next_id]() {
                    NavigationManager::instance().go_back();  // dismiss overlay
                    NavigationManager::instance().trigger_printer_switch(next_id);
                });
            }
        } else {
            // Defer repopulation out of modal callback to avoid widget mutation mid-event
            helix::ui::queue_update([]() {
                auto* c = Config::get_instance();
                if (c) {
                    auto remaining = c->get_printer_ids();
                    get_printer_state().set_multi_printer_enabled(remaining.size() > 1);
                }
                get_printer_list_overlay().populate_printer_list();
            });
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrinterListOverlay::on_delete_cancel_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterListOverlay] on_delete_cancel_cb");

    (void)e;
    s_pending_delete_id_.clear();

    // Close the modal
    lv_obj_t* top = Modal::get_top();
    if (top) {
        Modal::hide(top);
    }

    LVGL_SAFE_EVENT_CB_END();
}

}  // namespace helix::ui
