// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_navbar_button.h"

#include "ui_effects.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

void NavbarButton::create(lv_obj_t* navbar) {
    navbar_ = navbar;
    lv_obj_t* slot = lv_obj_find_by_name(navbar, slot_name_);
    lv_obj_t* screen = lv_obj_get_screen(navbar);
    if (!slot || !screen) {
        spdlog::debug("[NavigationManager] No {} in this navbar; no {}", slot_name_, component_);
        return;
    }
    button_ = static_cast<lv_obj_t*>(lv_xml_create(screen, component_, nullptr));
    if (!button_) {
        spdlog::error("[NavigationManager] {} component would not build", component_);
        return;
    }
    lv_obj_set_name(button_, widget_name_);
    add_always_on_top(button_, [this] { raise(); });
    // Built while a modal is open, it goes under that modal's backdrop like the
    // chrome already there.
    if (chrome_covered()) {
        lv_obj_t* backdrop = ModalStack::instance().lowest_covering_backdrop();
        if (backdrop && lv_obj_get_parent(backdrop) == screen) {
            lv_obj_move_to_index(button_, static_cast<int32_t>(lv_obj_get_index(backdrop)));
        }
    }
    spdlog::debug("[NavigationManager] {} created over {}", component_, slot_name_);
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(button_, button_deleted_cb, LV_EVENT_DELETE, this);
    // The slot moves whenever the navbar lays out (it appears, the orientation
    // flips), and the button has to follow it there.
    lv_obj_add_event_cb(navbar, navbar_layout_changed_cb, LV_EVENT_LAYOUT_CHANGED, this);
    sync();
}

void NavbarButton::button_deleted_cb(lv_event_t* e) {
    // Only the current button: a replaced one dying late must not clear its
    // successor.
    auto* self = static_cast<NavbarButton*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (self->button_.names_dying(target)) {
        self->button_ = nullptr;
    }
    remove_always_on_top(target);
}

void NavbarButton::navbar_layout_changed_cb(lv_event_t* e) {
    static_cast<NavbarButton*>(lv_event_get_user_data(e))->sync();
}

void NavbarButton::sync() {
    if (!button_ || !navbar_) {
        return;
    }
    lv_obj_t* slot = lv_obj_find_by_name(navbar_, slot_name_);
    if (!slot) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(slot, &area);

    // The keyboard can shift the whole layout up while it is open; the slot's
    // home position is its offset within that layout, which rests at y=0.
    lv_obj_t* layout_root = navbar_;
    while (lv_obj_get_parent(layout_root) &&
           lv_obj_get_parent(layout_root) != lv_obj_get_screen(layout_root)) {
        layout_root = lv_obj_get_parent(layout_root);
    }
    lv_area_t root_area;
    lv_obj_get_coords(layout_root, &root_area);
    int32_t y = area.y1 - root_area.y1;

    // With the keyboard open over the bottom of a side navbar, a lifting button
    // rides in the navbar column just above the keyboard's top edge, never over a
    // key. A portrait bottom navbar has no column above the keyboard: everything
    // there is the overlay's own content, the text field first.
    const bool side_navbar = lv_obj_get_height(navbar_) > lv_obj_get_width(navbar_);
    if (lifts_over_keyboard_ && keyboard_top_ >= 0 && side_navbar) {
        const int32_t size = lv_obj_get_height(button_);
        const int32_t above = keyboard_top_ - size - theme_manager_get_spacing("space_xs");
        y = std::min(y, above);
    }
    // Screen children are positioned in screen coordinates.
    lv_obj_set_pos(button_, area.x1, y);
}

bool NavbarButton::rides_above_keyboard() const {
    return lifts_over_keyboard_ && keyboard_top_ >= 0 && navbar_ &&
           lv_obj_get_height(navbar_) > lv_obj_get_width(navbar_);
}

void NavbarButton::raise() {
    if (!button_) {
        return;
    }
    lv_obj_move_foreground(button_);
    if (rides_above_keyboard()) {
        return;
    }
    // An open keyboard over the navbar covers the button; it stays just under it.
    lv_obj_t* keyboard = KeyboardManager::instance().get_instance();
    if (keyboard && KeyboardManager::instance().is_visible() &&
        lv_obj_get_parent(keyboard) == lv_obj_get_parent(button_)) {
        lv_obj_move_to_index(button_, static_cast<int32_t>(lv_obj_get_index(keyboard)));
    }
}

void NavbarButton::set_keyboard_top(int32_t top) {
    if (!lifts_over_keyboard_) {
        return;
    }
    keyboard_top_ = top;
    sync();
    // Only a side navbar leaves room above the keyboard. A portrait bottom navbar is
    // under it, and a button raised there would sit on the keyboard's keys.
    if (top >= 0 && button_ && rides_above_keyboard() && !chrome_covered()) {
        lv_obj_move_foreground(button_);
    }
}

void NavbarButton::destroy() {
    if (button_) {
        remove_always_on_top(button_);
        safe_delete_deferred(button_);
    }
    navbar_ = nullptr;
    keyboard_top_ = -1;
}

} // namespace helix::ui
