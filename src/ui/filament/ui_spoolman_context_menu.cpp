// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spoolman_context_menu.h"

#include "ui_callback_helpers.h"
#include "ui_plugin_menu_rows.h"

#include "helix_psram_attr.h"
#include "plugin_menu_context.h"
#include "ui/ui_widget_helpers.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::ui {

namespace {
// Bound only by the menu's XML and written only by on_created(); one menu is on
// screen at a time, so every owner shares them.
helix::WidgetSubjects s_subjects{"SpoolmanContextMenuSubjects",
                                 StaticSubjectRegistry::Lifetime::Process};
HELIX_PSRAM_BSS lv_subject_t s_header_subject;
HELIX_PSRAM_BSS char s_header_buf[128];
// Spoolman's filament.name, the subtitle under a header that already carries
// vendor + material; hidden when the filament has no name.
HELIX_PSRAM_BSS lv_subject_t s_filament_subject;
HELIX_PSRAM_BSS char s_filament_buf[128];
HELIX_PSRAM_BSS lv_subject_t s_filament_shown_subject;
// 0 for the active spool: it is feeding the printer.
HELIX_PSRAM_BSS lv_subject_t s_archive_shown_subject;

void init_menu_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_STRING(s_header_subject, s_header_buf, "", "spoolman_menu_header",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_filament_subject, s_filament_buf, "", "spoolman_menu_filament",
                                  s_subjects);
        UI_MANAGED_SUBJECT_INT(s_filament_shown_subject, 0, "spoolman_menu_filament_shown",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_archive_shown_subject, 1, "spoolman_menu_archive_shown",
                               s_subjects);
    });
}
} // namespace

// Static member initialization
bool SpoolmanContextMenu::callbacks_registered_ = false;

// ============================================================================
// Construction / Destruction
// ============================================================================

SpoolmanContextMenu::SpoolmanContextMenu() {
    init_menu_subjects();
    spdlog::debug("[SpoolmanContextMenu] Constructed");
}

SpoolmanContextMenu::~SpoolmanContextMenu() {
    spdlog::trace("[SpoolmanContextMenu] Destroyed");
}

SpoolmanContextMenu::SpoolmanContextMenu(SpoolmanContextMenu&& other) noexcept
    : ContextMenu(std::move(other)), action_callback_(std::move(other.action_callback_)),
      pending_spool_(std::move(other.pending_spool_)), archive_allowed_(other.archive_allowed_) {}

SpoolmanContextMenu& SpoolmanContextMenu::operator=(SpoolmanContextMenu&& other) noexcept {
    if (this != &other) {
        // Let base class handle its state, including the active-menu registry
        ContextMenu::operator=(std::move(other));
        action_callback_ = std::move(other.action_callback_);
        pending_spool_ = std::move(other.pending_spool_);
        archive_allowed_ = other.archive_allowed_;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void SpoolmanContextMenu::set_action_callback(ActionCallback callback) {
    action_callback_ = std::move(callback);
}

bool SpoolmanContextMenu::show_for_spool(lv_obj_t* parent, const SpoolInfo& spool,
                                         lv_obj_t* near_widget, bool allow_archive) {
    register_callbacks();

    // Store spool info before base class calls on_created
    pending_spool_ = spool;
    archive_allowed_ = allow_archive;

    // Base class handles: XML creation, on_created callback, positioning, and
    // claiming the active-menu slot the static callbacks resolve through.
    // item_index = spool.id for action dispatch
    return ContextMenu::show_near_widget(parent, spool.id, near_widget);
}

// ============================================================================
// ContextMenu override
// ============================================================================

void SpoolmanContextMenu::on_created(lv_obj_t* menu_obj) {
    // Header: "Vendor Material" (e.g., "Polymaker PLA")
    std::string name = pending_spool_.vendor;
    if (!pending_spool_.material.empty()) {
        if (!name.empty()) {
            name += " ";
        }
        name += pending_spool_.material;
    }
    if (name.empty()) {
        name = fmt::format(lv_tr("Spool #{}"), pending_spool_.id);
    }
    lv_subject_copy_string(&s_header_subject, name.c_str());

    lv_subject_copy_string(&s_filament_subject, pending_spool_.filament_name.c_str());
    lv_subject_set_int(&s_filament_shown_subject, pending_spool_.filament_name.empty() ? 0 : 1);
    lv_subject_set_int(&s_archive_shown_subject, archive_allowed_ ? 1 : 0);

    if (plugin_menu::any_declared(plugin_menu::MenuKind::Spool)) {
        const plugin_menu::json context = plugin_menu::spool_context(pending_spool_);
        append_plugin_rows(*this, lv_obj_find_by_name(menu_obj, "plugin_rows"),
                           plugin_menu::resolve(plugin_menu::MenuKind::Spool, context), context);
    }

    // Prevent context menu buttons from triggering scroll on the underlying list
    lv_obj_remove_flag(menu_obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t* card = helix::ui::find_required(menu_obj, "context_menu", "SpoolmanContextMenu");
    if (card) {
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        uint32_t card_children = lv_obj_get_child_count(card);
        for (uint32_t i = 0; i < card_children; i++) {
            lv_obj_t* child = lv_obj_get_child(card, static_cast<int32_t>(i));
            if (child) {
                lv_obj_remove_flag(child, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            }
        }
    }

    spdlog::debug("[SpoolmanContextMenu] Shown for spool {} ({})", pending_spool_.id,
                  pending_spool_.display_name());
}

// ============================================================================
// Event Handlers
// ============================================================================

void SpoolmanContextMenu::dispatch_spoolman_action(MenuAction action) {
    int spool_id = get_item_index();
    ActionCallback callback_copy = action_callback_;

    hide();

    if (callback_copy) {
        callback_copy(action, spool_id);
    }
}

void SpoolmanContextMenu::on_backdrop_clicked() {
    spdlog::debug("[SpoolmanContextMenu] Backdrop clicked");
    dispatch_spoolman_action(MenuAction::CANCELLED);
}

void SpoolmanContextMenu::handle_set_active() {
    spdlog::info("[SpoolmanContextMenu] Set active requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::SET_ACTIVE);
}

void SpoolmanContextMenu::handle_edit() {
    spdlog::info("[SpoolmanContextMenu] Edit requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::EDIT);
}

void SpoolmanContextMenu::handle_print_label() {
    spdlog::info("[SpoolmanContextMenu] Print label requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::PRINT_LABEL);
}

void SpoolmanContextMenu::handle_duplicate() {
    spdlog::info("[SpoolmanContextMenu] Duplicate requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::DUPLICATE);
}

void SpoolmanContextMenu::handle_archive() {
    spdlog::info("[SpoolmanContextMenu] Archive requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::ARCHIVE);
}

void SpoolmanContextMenu::handle_delete() {
    spdlog::info("[SpoolmanContextMenu] Delete requested for spool {}", get_item_index());
    dispatch_spoolman_action(MenuAction::DELETE);
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void SpoolmanContextMenu::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"spoolman_context_set_active_cb", on_set_active_cb},
        {"spoolman_context_edit_cb", on_edit_cb},
        {"spoolman_context_print_label_cb", on_print_label_cb},
        {"spoolman_context_duplicate_cb", on_duplicate_cb},
        {"spoolman_context_archive_cb", on_archive_cb},
        {"spoolman_context_delete_cb", on_delete_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[SpoolmanContextMenu] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via ContextMenu::active())
// ============================================================================

SpoolmanContextMenu* SpoolmanContextMenu::get_active_instance() {
    auto* self = ContextMenu::active_as<SpoolmanContextMenu>();
    if (!self) {
        spdlog::warn("[SpoolmanContextMenu] No active instance for event");
    }
    return self;
}

void SpoolmanContextMenu::on_set_active_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_set_active();
    }
}

void SpoolmanContextMenu::on_edit_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_edit();
    }
}

void SpoolmanContextMenu::on_print_label_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_print_label();
    }
}

void SpoolmanContextMenu::on_duplicate_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_duplicate();
    }
}

void SpoolmanContextMenu::on_archive_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_archive();
    }
}

void SpoolmanContextMenu::on_delete_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_delete();
    }
}

} // namespace helix::ui
