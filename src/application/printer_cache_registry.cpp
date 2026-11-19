// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_cache_registry.h"

#include "exception_policy.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>

namespace helix {

namespace {
bool g_registry_destroyed = false;
}

PrinterCacheRegistry& PrinterCacheRegistry::instance() {
    static PrinterCacheRegistry registry;
    return registry;
}

bool PrinterCacheRegistry::is_destroyed() {
    return g_registry_destroyed;
}

PrinterCacheRegistry::~PrinterCacheRegistry() {
    g_registry_destroyed = true;
}

void PrinterCacheRegistry::register_invalidator(const char* name,
                                                std::function<void()> invalidate_fn,
                                                const void* owner) {
    if (name == nullptr) {
        spdlog::error("[PrinterCacheRegistry] Refusing to register an unnamed invalidator");
        return;
    }

    // Replace in place rather than appending: entries survive invalidate_all(), so an init
    // path that runs again after a soft restart would otherwise register a duplicate on
    // every printer switch.
    auto it = std::find_if(
        invalidators_.begin(), invalidators_.end(),
        [name, owner](const InvalidateEntry& e) { return e.name == name && e.owner == owner; });
    if (it != invalidators_.end()) {
        it->invalidate_fn = std::move(invalidate_fn);
        spdlog::trace("[PrinterCacheRegistry] Re-registered: {} (total: {})", name,
                      invalidators_.size());
        return;
    }

    invalidators_.push_back({name, std::move(invalidate_fn), owner});
    spdlog::trace("[PrinterCacheRegistry] Registered: {} (total: {})", name, invalidators_.size());
}

bool PrinterCacheRegistry::unregister(const char* name, const void* owner) {
    if (name == nullptr) {
        return false;
    }
    auto it = std::find_if(
        invalidators_.begin(), invalidators_.end(),
        [name, owner](const InvalidateEntry& e) { return e.name == name && e.owner == owner; });
    if (it == invalidators_.end()) {
        return false;
    }
    invalidators_.erase(it);
    spdlog::trace("[PrinterCacheRegistry] Unregistered: {} (total: {})", name,
                  invalidators_.size());
    return true;
}

bool PrinterCacheRegistry::invalidate_one(const char* name) {
    if (name == nullptr) {
        return false;
    }
    auto it = std::find_if(invalidators_.begin(), invalidators_.end(),
                           [name](const InvalidateEntry& e) { return e.name == name; });
    if (it == invalidators_.end()) {
        return false;
    }

    // Copy before running: the callback may register (and therefore reallocate the vector).
    std::function<void()> fn = it->invalidate_fn;
    if (fn) {
        helix::contain_exceptions(fmt::format("[PrinterCacheRegistry] Invalidator '{}'", name), fn);
    }
    return true;
}

void PrinterCacheRegistry::clear() {
    invalidators_.clear();
    spdlog::trace("[PrinterCacheRegistry] Cleared all entries (no callbacks run)");
}

void PrinterCacheRegistry::invalidate_all() {
    if (invalidators_.empty()) {
        spdlog::debug("[PrinterCacheRegistry] No per-printer caches registered, nothing to drop");
        return;
    }

    spdlog::debug("[PrinterCacheRegistry] Invalidating {} per-printer cache(s)...",
                  invalidators_.size());

    // Iterate a copy: an invalidator that (re-)registers would otherwise push_back into the
    // vector being iterated and invalidate the loop's iterators. Unlike
    // StaticSubjectRegistry::deinit_all() the member vector is NOT detached — registrations
    // must survive so the next printer switch fires them again.
    const std::vector<InvalidateEntry> entries = invalidators_;

    for (const auto& entry : entries) {
        if (!entry.invalidate_fn) {
            continue;
        }
        spdlog::trace("[PrinterCacheRegistry] Invalidating: {}", entry.name);
        // One component's bad cleanup must not leave every later component stale — that is
        // the exact failure this registry exists to prevent.
        helix::contain_exceptions(
            fmt::format("[PrinterCacheRegistry] Invalidator '{}'", entry.name),
            entry.invalidate_fn);
    }
    spdlog::debug("[PrinterCacheRegistry] Per-printer caches invalidated");
}

std::vector<std::string> PrinterCacheRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(invalidators_.size());
    for (const auto& entry : invalidators_) {
        out.push_back(entry.name);
    }
    return out;
}

} // namespace helix
