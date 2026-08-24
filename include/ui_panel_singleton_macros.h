// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_panel_singleton_macros.h
 * @brief Macros to reduce boilerplate for panel singleton getters
 *
 * This header provides macros to define global panel instances with proper
 * cleanup registration via StaticPanelRegistry.
 *
 * ## Usage
 *
 * ### For simple panels (default constructor):
 * ```cpp
 * // At the end of ui_panel_motion.cpp:
 * DEFINE_GLOBAL_PANEL(MotionPanel, motion)
 * // Expands to get_global_motion_panel() returning MotionPanel&
 * ```
 *
 * ### For PanelBase-derived panels (with PrinterState and API arguments):
 * ```cpp
 * // At the end of ui_panel_home.cpp:
 * DEFINE_GLOBAL_PANEL_WITH_STATE(HomePanel, home)
 * // Expands to get_global_home_panel() returning HomePanel&
 * // Constructs with: std::make_unique<HomePanel>(get_printer_state(), nullptr)
 * ```
 *
 * ## Notes
 * - The macro must be placed in the .cpp file (not header) after all includes
 * - Requires including "static_panel_registry.h" and <memory>
 * - For DEFINE_GLOBAL_PANEL_WITH_STATE, also requires "app_globals.h" for get_printer_state()
 * - The getter function is always named get_global_<name>_panel()
 *
 * ## Threading
 * These macros are NOT thread-safe. The getter functions must only be called
 * from the main (LVGL) thread. This matches LVGL's single-threaded model.
 *
 * ## API Parameter (WITH_STATE variant)
 * The API pointer is passed as nullptr at construction time. Panels should use
 * get_moonraker_api() when they need the API, not cache it in the constructor.
 * This supports the deferred initialization pattern where the API connection
 * may not be established until after panel construction.
 */

#include "static_panel_registry.h"

#include <memory>

/**
 * @brief Define a global panel instance with default constructor
 *
 * @param PanelClass The class name (e.g., MotionPanel)
 * @param name The short name used in the getter function (e.g., motion -> get_global_motion_panel)
 *
 * Example:
 * ```cpp
 * DEFINE_GLOBAL_PANEL(MotionPanel, motion)
 * ```
 * Expands to:
 * ```cpp
 * static std::unique_ptr<MotionPanel> g_motion_panel;
 * MotionPanel& get_global_motion_panel() {
 *     if (!g_motion_panel) {
 *         g_motion_panel = std::make_unique<MotionPanel>();
 *         StaticPanelRegistry::instance().register_destroy("MotionPanel",
 *                                                          []() { g_motion_panel.reset(); });
 *     }
 *     return *g_motion_panel;
 * }
 * ```
 */
#define DEFINE_GLOBAL_PANEL(PanelClass, name)                                                      \
    static std::unique_ptr<PanelClass> g_##name##_panel;                                           \
    PanelClass& get_global_##name##_panel() {                                                      \
        if (!g_##name##_panel) {                                                                   \
            g_##name##_panel = std::make_unique<PanelClass>();                                     \
            StaticPanelRegistry::instance().register_destroy(#PanelClass,                          \
                                                             []() { g_##name##_panel.reset(); });  \
        }                                                                                          \
        return *g_##name##_panel;                                                                  \
    }

/**
 * @brief Define a global panel instance for PanelBase-derived panels
 *
 * This variant is for panels that inherit from PanelBase and require
 * PrinterState& and IMoonrakerAPI* constructor arguments.
 *
 * @param PanelClass The class name (e.g., HomePanel)
 * @param name The short name used in the getter function (e.g., home -> get_global_home_panel)
 *
 * Example:
 * ```cpp
 * DEFINE_GLOBAL_PANEL_WITH_STATE(HomePanel, home)
 * ```
 * Expands to:
 * ```cpp
 * static std::unique_ptr<HomePanel> g_home_panel;
 * HomePanel& get_global_home_panel() {
 *     if (!g_home_panel) {
 *         g_home_panel = std::make_unique<HomePanel>(get_printer_state(), nullptr);
 *         StaticPanelRegistry::instance().register_destroy("HomePanel",
 *                                                          []() { g_home_panel.reset(); });
 *     }
 *     return *g_home_panel;
 * }
 * ```
 *
 * Requires: #include "app_globals.h" for get_printer_state()
 */
#define DEFINE_GLOBAL_PANEL_WITH_STATE(PanelClass, name)                                           \
    static std::unique_ptr<PanelClass> g_##name##_panel;                                           \
    PanelClass& get_global_##name##_panel() {                                                      \
        if (!g_##name##_panel) {                                                                   \
            g_##name##_panel = std::make_unique<PanelClass>(get_printer_state(), nullptr);         \
            StaticPanelRegistry::instance().register_destroy(#PanelClass,                          \
                                                             []() { g_##name##_panel.reset(); });  \
        }                                                                                          \
        return *g_##name##_panel;                                                                  \
    }
