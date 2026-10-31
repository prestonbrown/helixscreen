// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_nav_manager.h"

#include <utility>
#include <vector>

class NavigationManagerTestAccess {
  public:
    /// Stand in for a panel switch whose loading pill is up.
    static void set_nav_scrim_active(NavigationManager& nav, bool active) {
        nav.nav_scrim_active_ = active;
    }

    /// Force the keyboard-visible-at-press latch that take_backdrop_keyboard_dismiss()
    /// consults. The latch is normally set at LV_EVENT_PRESSED from live keyboard
    /// state; setting it directly keeps dismiss tests deterministic.
    static void set_backdrop_press_keyboard_visible(NavigationManager& nav, bool visible) {
        nav.backdrop_.note_press(visible);
    }

    /// Track a nested-overlay backdrop on a standalone OverlayBackdrop.
    static void track_nested_backdrop(helix::ui::OverlayBackdrop& backdrop, lv_obj_t* overlay,
                                      lv_obj_t* nested) {
        backdrop.nested_[overlay] = nested;
    }

    /// Read the raw backdrop pointer, to assert it does not outlive the widget.
    static lv_obj_t* overlay_backdrop(NavigationManager& nav) {
        return nav.backdrop_.primary();
    }

    /// Give @p overlay a nested-overlay backdrop, as a stacked push does.
    static void set_overlay_backdrop_for(NavigationManager& nav, lv_obj_t* overlay,
                                         lv_obj_t* backdrop) {
        nav.backdrop_.nested_[overlay] = backdrop;
    }

    /// Create the darkened snapshot backdrop, as the first push_overlay() does.
    /// `arriving` is the overlay being pushed; null when the test drives the
    /// snapshot with nothing arriving.
    static void adopt_overlay_backdrop(NavigationManager& nav, lv_obj_t* screen,
                                       lv_obj_t* arriving = nullptr) {
        nav.adopt_overlay_backdrop(screen, arriving);
    }

    /// Re-take the backdrop snapshot from the live tree.
    static void refresh_overlay_backdrop(NavigationManager& nav) {
        nav.refresh_overlay_backdrop();
    }

    /// Seed panel_stack_ without going through the queued push path, so a test
    /// can put a stand-in base panel and overlay in the stack synchronously.
    static void set_panel_stack(NavigationManager& nav, std::vector<lv_obj_t*> stack) {
        nav.panel_stack_ = std::move(stack);
    }

    /// Read panel_stack_, to assert a deleted widget left no dangling entry.
    static const std::vector<lv_obj_t*>& panel_stack(NavigationManager& nav) {
        return nav.panel_stack_;
    }

    /// Read the raw app-layout pointer, to assert it does not outlive the widget.
    static lv_obj_t* app_layout_widget(NavigationManager& nav) {
        return nav.app_layout_widget_;
    }

    /// Drive the active-panel observer body directly. In production this only
    /// ever runs from a queued UpdateQueue apply (observe<int> defers it),
    /// so a test cannot reach it without either the queue or this accessor.
    static void handle_active_panel_change(NavigationManager& nav, helix::PanelId panel_id) {
        nav.handle_active_panel_change(static_cast<int32_t>(panel_id));
    }

    /// Drive the navbar path directly. The production entry point is a static
    /// LV_EVENT_CLICKED handler on the navbar button, which a test cannot reach
    /// without a real navbar; this is the body that handler queues.
    static void switch_to_panel(NavigationManager& nav, helix::PanelId panel_id) {
        nav.switch_to_panel_impl(static_cast<int>(panel_id));
    }

    /// Resolve the lifecycle registered for an overlay widget, to assert a
    /// teardown unregistered it (nullptr after).
    static IPanelLifecycle* lifecycle_of(NavigationManager& nav, lv_obj_t* widget) {
        return nav.resolve_overlay_lifecycle(widget);
    }

    /// Whether a close callback is registered for the widget, to assert a
    /// teardown unregistered it.
    static bool has_close_callback(NavigationManager& nav, lv_obj_t* widget) {
        return nav.overlay_close_callbacks_.count(widget) > 0;
    }

    /// Take the close callback registered for `widget` off it, as a close path
    /// does when it defers the callback to a later tick. Empty if there is none.
    static helix::OverlayCloseCallback take_overlay_close_callback(NavigationManager& nav,
                                                                   lv_obj_t* widget) {
        auto it = nav.overlay_close_callbacks_.find(widget);
        if (it == nav.overlay_close_callbacks_.end()) {
            return {};
        }
        helix::OverlayCloseCallback callback = std::move(it->second);
        nav.overlay_close_callbacks_.erase(it);
        return callback;
    }

    /// The close path taken on connection loss or Klippy leaving READY.
    static void clear_overlay_stack(NavigationManager& nav) {
        nav.clear_overlay_stack();
    }

    /// The slide-out animation's completion callback, for a widget whose slide
    /// has just ended.
    static void slide_out_complete(lv_obj_t* panel) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, panel);
        NavigationManager::overlay_slide_out_complete_cb(&anim);
    }

    /// Run the overlay open / close slide directly on a widget.
    static void animate_slide_in(NavigationManager& nav, lv_obj_t* panel) {
        nav.overlay_animate_slide_in(panel);
    }
    static void animate_slide_out(NavigationManager& nav, lv_obj_t* panel) {
        nav.overlay_animate_slide_out(panel);
    }
};
