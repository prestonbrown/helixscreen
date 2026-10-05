// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_overlay_host.h"

#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::plugin {

PluginOverlayHost::OverlayLifecycle& PluginOverlayHost::overlay_lifecycle() {
    static OverlayLifecycle lifecycle;
    return lifecycle;
}

int PluginOverlayHost::open(const std::string& plugin_id, const std::string& component,
                            std::function<void()> on_closed, const PluginUi::Attrs& attrs) {
    // One live overlay per component: a re-fired open handler gets the overlay
    // it already opened rather than a buried twin with a second same-named
    // canvas. The showing overlay keeps the attributes and on_closed it was
    // opened with. A record whose root left the nav stack is mid-close (its
    // callback runs after the slide-out), so a re-open in that window builds a
    // fresh overlay instead of the dying handle.
    auto& nav = NavigationManager::instance();
    for (const auto& rec : records_) {
        if (rec.plugin_id == plugin_id && rec.component == component &&
            nav.is_panel_in_stack(rec.root))
            return rec.handle;
    }

    std::vector<const char*> pairs;
    pairs.reserve(attrs.size() * 2 + 1);
    for (const auto& [name, value] : attrs) {
        pairs.push_back(name.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), component.c_str(), pairs.data()));
    if (!root) {
        spdlog::warn("[PluginOverlayHost] {}: cannot create component '{}'", plugin_id, component);
        return 0;
    }
    // Hidden until the queued push shows it: a visible root would render a
    // frame on the bare screen and land in the backdrop snapshot as a dimmed
    // ghost of itself.
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    auto& rec = records_.emplace_back();
    rec.handle = next_handle_++;
    rec.plugin_id = plugin_id;
    rec.component = component;
    rec.root = root;
    rec.on_closed = std::move(on_closed);

    push(root, &overlay_lifecycle(), [this, handle = rec.handle] { on_nav_closed(handle); });
    return rec.handle;
}

void PluginOverlayHost::push(lv_obj_t* root, IPanelLifecycle* lifecycle,
                             std::function<void()> on_nav_closed) {
    auto& nav = NavigationManager::instance();
    nav.register_overlay_instance(root, lifecycle);
    // The navigation manager runs close callbacks deferred, after the slide-out, so
    // this is where a live host tears an overlay down.
    nav.register_overlay_close_callback(
        root, [fn = std::move(on_nav_closed), token = guard_.token()] {
            if (token.expired())
                return; // the destroyed host's owner already deleted the root
            fn();
        });
    nav.push_overlay(root);
}

PluginOverlayHost::~PluginOverlayHost() {
    guard_.invalidate();
    for (auto& rec : records_) {
        if (!NavigationManager::is_destroyed()) {
            auto& nav = NavigationManager::instance();
            nav.unregister_overlay_close_callback(rec.root);
            nav.unregister_overlay_instance(rec.root);
        }
        helix::ui::safe_delete_deferred(rec.root);
    }
}

void PluginOverlayHost::on_nav_closed(int handle) {
    auto it = find(handle);
    if (it == records_.end())
        return; // the record already finished by an earlier callback
    // close_all empties on_closed of the records it silences; a user or Lua close
    // leaves it in place, and this is where it runs.
    finish(it);
}

void PluginOverlayHost::close(int handle) {
    auto it = find(handle);
    if (it == records_.end())
        return;
    // The on-top decision happens in queue order inside close_overlay, so a push
    // queued ahead of this close (the wizard "open next, close current" pattern)
    // is already on the stack when the decision is made.
    NavigationManager::instance().close_overlay(it->root);
}

void PluginOverlayHost::close_all(const std::string& plugin_id) {
    auto& nav = NavigationManager::instance();
    // Newest first, matching pop order. Every record leaves through navigation: an
    // on-top root needs go_back's restore path, a buried one is dropped by
    // close_overlay itself, and either way the nav close callback finishes the
    // record - even when the host is destroyed before the queued operations run.
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->plugin_id != plugin_id)
            continue;
        it->on_closed = {}; // the plugin is unloading; its Lua is going away
        nav.close_overlay(it->root);
    }
}

size_t PluginOverlayHost::open_count(const std::string& plugin_id) const {
    return static_cast<size_t>(
        std::count_if(records_.begin(), records_.end(),
                      [&plugin_id](const Record& r) { return r.plugin_id == plugin_id; }));
}

std::list<PluginOverlayHost::Record>::iterator PluginOverlayHost::find(int handle) {
    return std::find_if(records_.begin(), records_.end(),
                        [handle](const Record& r) { return r.handle == handle; });
}

std::list<PluginOverlayHost::Record>::iterator
PluginOverlayHost::finish(std::list<Record>::iterator it) {
    auto on_closed = std::move(it->on_closed);
    lv_obj_t* root = it->root;
    auto& nav = NavigationManager::instance();
    nav.unregister_overlay_close_callback(root);
    nav.unregister_overlay_instance(root);
    auto next = records_.erase(it);
    // Deferred: this runs from inside a queued callback, where a sync delete would
    // corrupt LVGL's event list mid-batch.
    helix::ui::safe_delete_deferred(root);
    if (on_closed)
        on_closed();
    return next;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
