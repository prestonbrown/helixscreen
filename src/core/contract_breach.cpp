// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/contract_breach.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace helix::ui {

namespace {
std::atomic<bool> g_strict_ui_checks{false};
} // namespace

void set_strict_ui_checks(bool enabled) noexcept {
    g_strict_ui_checks.store(enabled, std::memory_order_release);
}

bool strict_ui_checks() noexcept {
    return g_strict_ui_checks.load(std::memory_order_acquire);
}

void report_ui_contract_breach(const char* message) {
    spdlog::error("{}", message);
#ifndef HELIX_RELEASE_BUILD
    if (g_strict_ui_checks.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "\n[UI contract] STRICT MODE: %s\n", message);
        std::abort();
    }
#endif
}

} // namespace helix::ui
