// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_app_stubs.cpp
 * @brief Stub implementations for application module unit tests
 *
 * Provides stub implementations for global functions from app_globals.h
 * that are referenced by other modules (like runtime_config.cpp) but
 * whose real implementations aren't linked into the test binary.
 */

// Note: PrinterState, MoonrakerClient, and MoonrakerAPI stubs are provided
// by the test fixtures when needed, not as global stubs. This allows each
// test to have its own isolated instance.

// ============================================================================
// Wizard State Stubs
// ============================================================================

// These stubs are needed because runtime_config.cpp calls is_wizard_active()
// but app_globals.cpp (where the real implementation lives) isn't linked
// into the test binary.

static bool g_test_wizard_active = false;

bool is_wizard_active() {
    return g_test_wizard_active;
}

void set_wizard_active(bool active) {
    g_test_wizard_active = active;
}

#include <functional>
void set_wizard_completion_callback(std::function<void()> /*cb*/) {
    // No-op in tests
}

static std::function<void()> g_test_wizard_cancel_cb;

void set_wizard_cancel_callback(std::function<void()> cb) {
    g_test_wizard_cancel_cb = std::move(cb);
}

std::function<void()> get_wizard_cancel_callback() {
    return g_test_wizard_cancel_cb;
}
