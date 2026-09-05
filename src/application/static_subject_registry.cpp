// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace {
bool g_registry_destroyed = false;
}

StaticSubjectRegistry& StaticSubjectRegistry::instance() {
    static StaticSubjectRegistry registry;
    return registry;
}

bool StaticSubjectRegistry::is_destroyed() {
    return g_registry_destroyed;
}

StaticSubjectRegistry::~StaticSubjectRegistry() {
    g_registry_destroyed = true;
}

void StaticSubjectRegistry::register_deinit(const char* name, std::function<void()> deinit_fn) {
    // Names identify a subject source, not a registration event. A source that is
    // torn down and rebuilt (PrintStatusWidget's DetailedFormatter, when the last
    // print-status widget leaves the dashboard and one is added back) re-registers
    // under the same name, and appending would leave a stale callback closed over
    // the previous instance to run alongside the live one. Drop the old entry and
    // re-append, so the entry keeps the "last registered, first deinitialized"
    // position deinit_all()'s reverse walk depends on.
    auto it = std::find_if(deinitializers_.begin(), deinitializers_.end(),
                           [name](const DeinitEntry& e) { return e.name == name; });
    if (it != deinitializers_.end()) {
        deinitializers_.erase(it);
        spdlog::trace("[StaticSubjectRegistry] Replacing existing entry: {}", name);
    }
    deinitializers_.push_back({name, std::move(deinit_fn)});
    spdlog::trace("[StaticSubjectRegistry] Registered: {} (total: {})", name,
                  deinitializers_.size());
}

bool StaticSubjectRegistry::deinit_one(const char* name) {
    auto it = std::find_if(deinitializers_.begin(), deinitializers_.end(),
                           [name](const DeinitEntry& e) { return e.name == name; });
    if (it == deinitializers_.end()) {
        return false;
    }

    // Detach before running: the callback may register new entries.
    std::function<void()> fn = std::move(it->deinit_fn);
    deinitializers_.erase(it);
    if (fn) {
        fn();
    }
    return true;
}

void StaticSubjectRegistry::clear() {
    deinitializers_.clear();
    spdlog::trace("[StaticSubjectRegistry] Cleared all entries (no callbacks run)");
}

void StaticSubjectRegistry::deinit_all() {
    if (deinitializers_.empty()) {
        spdlog::debug("[StaticSubjectRegistry] No subjects registered, nothing to deinit");
        return;
    }

    spdlog::trace("[StaticSubjectRegistry] Deinitializing {} subject sources in reverse order...",
                  deinitializers_.size());

    // Iterate a detached copy: a deinit callback that re-creates a subject
    // source registers a fresh entry, and a push_back into the vector being
    // iterated would invalidate the loop's iterators. Re-registrations land in
    // the (now empty) member vector instead.
    std::vector<DeinitEntry> entries = std::move(deinitializers_);
    deinitializers_.clear();

    // Deinit in reverse order (last registered = first deinitialized)
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        spdlog::trace("[StaticSubjectRegistry] Deinitializing: {}", it->name);
        if (it->deinit_fn) {
            it->deinit_fn();
        }
    }
    spdlog::trace("[StaticSubjectRegistry] All subjects deinitialized");
}
