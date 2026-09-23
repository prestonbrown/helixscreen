// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

namespace {
bool g_registry_destroyed = false;
}

std::atomic<bool> StaticPanelRegistry::s_destroying_all_{false};

StaticPanelRegistry& StaticPanelRegistry::instance() {
    static StaticPanelRegistry registry;
    return registry;
}

bool StaticPanelRegistry::is_destroyed() {
    return g_registry_destroyed;
}

bool StaticPanelRegistry::is_destroying_all() {
    return s_destroying_all_.load(std::memory_order_acquire);
}

StaticPanelRegistry::~StaticPanelRegistry() {
    g_registry_destroyed = true;
    // Note: If we get here during static destruction and panels weren't
    // explicitly destroyed via destroy_all(), they'll be destroyed by
    // their own static unique_ptr destructors. We just mark ourselves
    // as destroyed so guards in panel destructors can check.
}

void StaticPanelRegistry::register_destroy(const char* name, std::function<void()> destroy_fn) {
    destroyers_.push_back({name, std::move(destroy_fn)});
    spdlog::trace("[StaticPanelRegistry] Registered: {} (total: {})", name, destroyers_.size());
}

void StaticPanelRegistry::record_orphaned_widget(lv_obj_t* widget) {
    // Outside the destroy_all() window the widget's deletion is its owner's
    // business; recording it here would have a later destroy_all() free
    // whatever now lives at the address.
    if (!widget || !is_destroying_all()) {
        return;
    }
    orphaned_widgets_.push_back(widget);
}

void StaticPanelRegistry::clear() {
    destroyers_.clear();
    orphaned_widgets_.clear();
    spdlog::trace("[StaticPanelRegistry] Cleared all entries (no callbacks run)");
}

std::vector<lv_obj_t*> StaticPanelRegistry::destroy_all() {
    if (destroyers_.empty()) {
        spdlog::debug("[StaticPanelRegistry] No panels registered, nothing to destroy");
        return {};
    }

    spdlog::trace("[StaticPanelRegistry] Destroying {} panels in reverse order...",
                  destroyers_.size());

    // Set flag so helix::ui::safe_delete() skips deletion during this window
    s_destroying_all_.store(true, std::memory_order_release);

    // Iterate a detached copy: a destroy callback that reaches through a lazy
    // get_global_*_panel() getter re-registers a fresh entry, and a push_back
    // into the vector being iterated would invalidate the loop's iterators.
    // Re-registrations land in the (now empty) member vector instead, so a
    // later destroy_all() pass can sweep them.
    std::vector<DestroyEntry> entries = std::move(destroyers_);
    destroyers_.clear();

    // Destroy in reverse registration order (LIFO)
    // This ensures dependencies are respected: panels created later
    // (which may depend on earlier ones) are destroyed first
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        spdlog::trace("[StaticPanelRegistry] Destroying: {}", it->name);
        if (it->destroy_fn) {
            it->destroy_fn();
        }
    }
    s_destroying_all_.store(false, std::memory_order_release);
    spdlog::trace("[StaticPanelRegistry] All panels destroyed");

    // Hand the roots the panel destructors recorded to the caller. Moved out
    // so nothing lingers past this call: a later destroy_all() must never be
    // handed a pointer freed since this one.
    return std::move(orphaned_widgets_);
}
