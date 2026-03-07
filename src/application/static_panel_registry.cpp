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

void StaticPanelRegistry::clear() {
    destroyers_.clear();
    spdlog::trace("[StaticPanelRegistry] Cleared all entries (no callbacks run)");
}

void StaticPanelRegistry::destroy_all() {
    if (destroyers_.empty()) {
        spdlog::debug("[StaticPanelRegistry] No panels registered, nothing to destroy");
        return;
    }

    spdlog::trace("[StaticPanelRegistry] Destroying {} panels in reverse order...",
                  destroyers_.size());

    // Set flag so helix::ui::safe_delete() skips deletion during this window
    s_destroying_all_.store(true, std::memory_order_release);

    // Destroy in reverse registration order (LIFO)
    // This ensures dependencies are respected: panels created later
    // (which may depend on earlier ones) are destroyed first
    for (auto it = destroyers_.rbegin(); it != destroyers_.rend(); ++it) {
        spdlog::trace("[StaticPanelRegistry] Destroying: {}", it->name);
        if (it->destroy_fn) {
            it->destroy_fn();
        }
    }

    destroyers_.clear();
    s_destroying_all_.store(false, std::memory_order_release);
    spdlog::trace("[StaticPanelRegistry] All panels destroyed");
}
