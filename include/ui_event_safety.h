// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025 HelixScreen Contributors
/**
 * @file ui_event_safety.h
 * @brief Exception safety wrappers for LVGL event callbacks
 *
 * LVGL is a C library and cannot handle C++ exceptions. If an event callback
 * throws an exception, it will propagate through LVGL's C code into libhv/SDL,
 * causing undefined behavior and likely crashes.
 *
 * This module provides exception safety wrappers that catch and log exceptions,
 * preventing crashes and providing graceful degradation.
 *
 * Pattern matches moonraker_client.cpp's proven exception handling approach.
 */

#ifndef UI_EVENT_SAFETY_H
#define UI_EVENT_SAFETY_H

#include "lvgl/lvgl.h"
#include "spdlog/spdlog.h"
#include <functional>
#include <exception>

/**
 * @brief Safe wrapper for LVGL event callbacks
 *
 * Catches any exceptions thrown by the handler and logs them, preventing
 * crashes from propagating through LVGL's C code.
 *
 * Usage:
 * @code
 * lv_obj_add_event_cb(btn, [](lv_event_t* e) {
 *     ui_event_safe_call("home_button", [&]() {
 *         auto* client = get_moonraker_client();
 *         if (client) client->send_gcode("G28");
 *     });
 * }, LV_EVENT_CLICKED, nullptr);
 * @endcode
 *
 * @param callback_name Name for logging (e.g., "home_button_clicked")
 * @param handler Function to execute safely
 */
inline void ui_event_safe_call(const char* callback_name, std::function<void()> handler) {
    try {
        handler();
    } catch (const std::exception& ex) {
        spdlog::error("[LVGL Event Safety] Exception in '{}': {}", callback_name, ex.what());
    } catch (...) {
        spdlog::error("[LVGL Event Safety] Unknown exception in '{}'", callback_name);
    }
}

/**
 * @brief Macro for defining safe LVGL event callbacks
 *
 * This macro creates an event callback function that automatically wraps
 * the body in exception safety handlers.
 *
 * Usage:
 * @code
 * LVGL_SAFE_EVENT_CB(on_home_clicked, {
 *     spdlog::info("Home button clicked");
 *     auto* client = get_moonraker_client();
 *     if (client) client->send_gcode("G28");
 * })
 *
 * // Then register normally:
 * lv_obj_add_event_cb(btn, on_home_clicked, LV_EVENT_CLICKED, nullptr);
 * @endcode
 *
 * @param callback_name Name of the callback function to create
 * @param body Code body to execute (with exception safety)
 */
#define LVGL_SAFE_EVENT_CB(callback_name, body) \
    static void callback_name(lv_event_t* e) { \
        ui_event_safe_call(#callback_name, [&]() body); \
    }

/**
 * @brief Macro for defining safe LVGL event callbacks with access to event
 *
 * Similar to LVGL_SAFE_EVENT_CB but makes the lv_event_t* available to the body.
 *
 * Usage:
 * @code
 * LVGL_SAFE_EVENT_CB_WITH_EVENT(on_dropdown_changed, event, {
 *     lv_obj_t* dropdown = lv_event_get_target(event);
 *     uint16_t idx = lv_dropdown_get_selected(dropdown);
 *     spdlog::debug("Selected index: {}", idx);
 * })
 * @endcode
 *
 * @param callback_name Name of the callback function to create
 * @param event_var Name to use for the lv_event_t* parameter in body
 * @param body Code body to execute (with exception safety and event access)
 */
#define LVGL_SAFE_EVENT_CB_WITH_EVENT(callback_name, event_var, body) \
    static void callback_name(lv_event_t* e) { \
        lv_event_t* event_var = e; \
        ui_event_safe_call(#callback_name, [&]() body); \
    }

#endif // UI_EVENT_SAFETY_H
