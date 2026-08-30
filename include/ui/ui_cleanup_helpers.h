// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_cleanup_helpers.h
 * @brief RAII-style safe deletion helpers for LVGL objects and timers
 *
 * These helpers eliminate the repetitive if-delete-null pattern found in
 * panel destructors. Each helper safely checks for null, deletes the resource,
 * and nulls the pointer to prevent double-free.
 *
 * @code{.cpp}
 * // Before (repeated 7+ times per panel):
 * if (overlay_cache_) {
 *     lv_obj_del(overlay_cache_);
 *     overlay_cache_ = nullptr;
 * }
 *
 * // After:
 * safe_delete_obj(overlay_cache_);
 * @endcode
 */

#include "lvgl.h"
#include "static_panel_registry.h"

namespace helix::ui {

/**
 * @brief Safely delete an LVGL object and null the pointer
 * @param obj Reference to object pointer (will be nulled after deletion)
 *
 * Safe to call with nullptr - no-op in that case.
 * Skips deletion during shutdown (lv_deinit will clean up).
 * Prevents double-free by nulling pointer after deletion.
 */
inline void safe_delete_obj(lv_obj_t*& obj) {
    if (!obj) {
        return;
    }
    // Skip if LVGL not initialized or no display exists
    if (!lv_is_initialized() || !lv_display_get_next(nullptr)) {
        obj = nullptr;
        return;
    }
    // Skip during destroy_all() - lv_deinit() will clean up all widgets
    if (StaticPanelRegistry::is_destroying_all()) {
        obj = nullptr;
        return;
    }
    lv_obj_del(obj);
    obj = nullptr;
}

} // namespace helix::ui
