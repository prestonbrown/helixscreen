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
    deinitializers_.push_back({name, std::move(deinit_fn)});
    spdlog::trace("[StaticSubjectRegistry] Registered: {} (total: {})", name,
                  deinitializers_.size());
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

    // Deinit in reverse order (last registered = first deinitialized)
    for (auto it = deinitializers_.rbegin(); it != deinitializers_.rend(); ++it) {
        spdlog::trace("[StaticSubjectRegistry] Deinitializing: {}", it->name);
        if (it->deinit_fn) {
            it->deinit_fn();
        }
    }

    deinitializers_.clear();
    spdlog::trace("[StaticSubjectRegistry] All subjects deinitialized");
}
