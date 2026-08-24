// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_scroll_auto_inject.h"

#include "display_settings_manager.h"
#include "panel_widget.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

PageScrollAutoInject& PageScrollAutoInject::instance() {
    static PageScrollAutoInject inst;
    return inst;
}

void PageScrollAutoInject::init() {
    // Intentionally NOT a subject observer. DisplaySettingsManager's setting subject
    // is (re)initialized lazily when the Settings panel is first built — AFTER this
    // runs during xml registration — and that re-init wipes any observer registered
    // here (same subject address, cleared observer list), so an observer would never
    // fire. Instead, runtime toggles are driven directly from the Display-settings
    // callback via on_setting_toggled(), and default-on-at-startup is handled by the
    // NavigationManager on_root_shown() hooks. Retained as an explicit lifecycle
    // hook (paired with shutdown()) and a home for this rationale.
}

void PageScrollAutoInject::on_setting_toggled(bool enabled) {
    if (enabled) {
        // Panels live on the active screen, overlays on the top layer — walk both so
        // enabling the setting while already on a scrollable screen injects
        // immediately. on_root_shown is idempotent.
        on_root_shown(lv_screen_active());
        on_root_shown(lv_layer_top());
    } else {
        detach_all();
    }
}

bool PageScrollAutoInject::enabled() const {
    return helix::DisplaySettingsManager::instance().get_page_scroll_buttons();
}

void PageScrollAutoInject::shutdown() {
    detach_all();
}

bool PageScrollAutoInject::qualifies(lv_obj_t* obj) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) {
        return false;
    }
    if ((lv_obj_get_scroll_dir(obj) & LV_DIR_VER) == 0) {
        return false; // horizontal-only (e.g. carousel)
    }
    return lv_obj_get_scroll_bottom(obj) > 0 || lv_obj_get_scroll_y(obj) > 0;
}

void PageScrollAutoInject::walk_and_attach(lv_obj_t* obj, bool ancestor_managed) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return; // don't inject into hidden/stacked panels
    }
    // Page scrolling is a page affordance. A home widget tile is sized by the
    // grid and scrolled by dragging it, and the gutter is two chevrons plus a
    // gap tall (172px at the medium tier) against a tile that is often not much
    // taller, so a gutter inside one sits on the widget's own content instead of
    // beside it. Cut the whole subtree: most tile roots are non-scrollable and
    // the walk would otherwise sail through them into a widget's inner list.
    if (lv_obj_has_flag(obj, helix::PANEL_WIDGET_TILE_FLAG)) {
        return;
    }
    // A container already managed from an earlier on_root_shown() pass must
    // still propagate the claim to its subtree on a repeated walk over a
    // persistent tree — otherwise a nested qualifying container underneath it
    // wrongly gets its own controller (double/nested gutters).
    bool claimed = ancestor_managed || (controllers_.count(obj) > 0);
    if (!claimed && qualifies(obj)) {
        auto ctl = std::make_unique<PageScrollController>();
        if (ctl->attach(obj)) {
            lv_obj_t* key = obj;
            // Synchronous erase is safe ONLY because PageScrollController's
            // on_container_deleted() nulls container_ BEFORE invoking this callback
            // (so the destroyed controller's dtor is a no-op) and invokes it as its
            // provably-last statement with no `self` access afterward (Task 3
            // hardening). This is a `delete this`-adjacent pattern — do not add code
            // that touches the controller after erase, and keep that invariant if you
            // edit the controller.
            ctl->set_on_container_deleted([this, key]() {
                controllers_.erase(key); // container gone; drop controller
            });
            controllers_.emplace(key, std::move(ctl));
            claimed = true;
        } else {
            spdlog::debug("[PageScroll] attach failed for container {}", static_cast<void*>(obj));
        }
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        walk_and_attach(lv_obj_get_child(obj, static_cast<int32_t>(i)), claimed);
    }
}

void PageScrollAutoInject::on_root_shown(lv_obj_t* root) {
    // Read the setting live rather than trusting a cached flag: observe_int_sync
    // defers its callback via queue_update, so a member cache could lag the
    // subject's synchronous value by one process_lvgl tick (e.g. a caller that
    // flips the setting and immediately shows a panel in the same frame). This
    // class is main-thread-only, so a direct subject read here (via enabled(),
    // which itself reads live) is safe and keeps on_root_shown() correct
    // without waiting for the deferred observer to catch up.
    if (!enabled() || root == nullptr) {
        return;
    }
    prune_dead();
    lv_obj_update_layout(root); // overflow must be measurable
    walk_and_attach(root, /*ancestor_managed=*/false);
    spdlog::debug("[PageScroll] on_root_shown root={} -> managed={}", static_cast<void*>(root),
                  controllers_.size());
}

void PageScrollAutoInject::detach_all() {
    spdlog::debug("[PageScroll] detach_all: detaching {} controller(s)", controllers_.size());
    for (auto& [key, ctl] : controllers_) {
        ctl->set_on_container_deleted(nullptr); // avoid erase-during-iteration
        ctl->detach();
    }
    controllers_.clear();
}

void PageScrollAutoInject::prune_dead() {
    // Defensive belt-and-suspenders: normally a controller is erased eagerly the
    // instant its container fires LV_EVENT_DELETE (the set_on_container_deleted
    // callback), so no !alive() entry survives to here. This reaps any that could
    // linger if a future caller nulls that callback without clearing the map.
    for (auto it = controllers_.begin(); it != controllers_.end();) {
        if (!it->second->alive()) {
            it = controllers_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace helix::ui
