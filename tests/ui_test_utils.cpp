/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ui_test_utils.h"
#include "spdlog/spdlog.h"
#include <chrono>
#include <thread>

namespace UITest {

// Virtual input device for simulating touches/clicks
static lv_indev_t* virtual_indev = nullptr;
static lv_indev_data_t last_data;

// Input device read callback
static void virtual_indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    // Copy last simulated input state
    *data = last_data;
}

void init(lv_obj_t* screen) {
    (void)screen; // Screen parameter reserved for future use

    if (virtual_indev) {
        spdlog::warn("[UITest] Already initialized");
        return;
    }

    spdlog::info("[UITest] Initializing virtual input device");

    // Initialize last_data
    last_data.point.x = 0;
    last_data.point.y = 0;
    last_data.state = LV_INDEV_STATE_RELEASED;

    // Create virtual input device
    virtual_indev = lv_indev_create();
    lv_indev_set_type(virtual_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(virtual_indev, virtual_indev_read_cb);

    spdlog::info("[UITest] Virtual input device created");
}

void cleanup() {
    if (virtual_indev) {
        lv_indev_delete(virtual_indev);
        virtual_indev = nullptr;
        spdlog::info("[UITest] Virtual input device removed");
    }
}

bool click(lv_obj_t* widget) {
    if (!widget || !virtual_indev) {
        spdlog::error("[UITest] Invalid widget or input device not initialized");
        return false;
    }

    // Get widget center coordinates
    int32_t x = lv_obj_get_x(widget) + lv_obj_get_width(widget) / 2;
    int32_t y = lv_obj_get_y(widget) + lv_obj_get_height(widget) / 2;

    // Convert to absolute coordinates if widget has parent
    lv_obj_t* parent = lv_obj_get_parent(widget);
    while (parent) {
        x += lv_obj_get_x(parent);
        y += lv_obj_get_y(parent);
        parent = lv_obj_get_parent(parent);
    }

    return click_at(x, y);
}

bool click_at(int32_t x, int32_t y) {
    if (!virtual_indev) {
        spdlog::error("[UITest] Input device not initialized - call init() first");
        return false;
    }

    spdlog::debug("[UITest] Simulating click at ({}, {})", x, y);

    // Simulate press
    last_data.point.x = x;
    last_data.point.y = y;
    last_data.state = LV_INDEV_STATE_PRESSED;
    lv_timer_handler();  // Process press event
    wait_ms(50);         // Minimum press duration

    // Simulate release
    last_data.state = LV_INDEV_STATE_RELEASED;
    lv_timer_handler();  // Process release event
    wait_ms(50);         // Allow click handlers to execute

    spdlog::debug("[UITest] Click simulation complete");
    return true;
}

bool type_text(const std::string& text) {
    // Get focused textarea
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    if (!focused) {
        spdlog::error("[UITest] No focused textarea");
        return false;
    }

    // Check if it's a textarea
    if (!lv_obj_check_type(focused, &lv_textarea_class)) {
        spdlog::error("[UITest] Focused widget is not a textarea");
        return false;
    }

    spdlog::debug("[UITest] Typing text: {}", text);

    // Add text directly to textarea
    lv_textarea_add_text(focused, text.c_str());
    lv_timer_handler();
    wait_ms(50);  // Allow text processing

    return true;
}

bool type_text(lv_obj_t* textarea, const std::string& text) {
    if (!textarea) {
        spdlog::error("[UITest] Invalid textarea");
        return false;
    }

    // Check if it's a textarea
    if (!lv_obj_check_type(textarea, &lv_textarea_class)) {
        spdlog::error("[UITest] Widget is not a textarea");
        return false;
    }

    spdlog::debug("[UITest] Typing text into textarea: {}", text);

    // Add text directly to textarea
    lv_textarea_add_text(textarea, text.c_str());
    lv_timer_handler();
    wait_ms(50);  // Allow text processing

    return true;
}

bool send_key(uint32_t key) {
    // Get focused textarea
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    if (!focused) {
        spdlog::error("[UITest] No focused widget");
        return false;
    }

    spdlog::debug("[UITest] Sending key: {}", key);

    // For special keys (Enter, Backspace, etc.), use appropriate LVGL functions
    if (lv_obj_check_type(focused, &lv_textarea_class)) {
        if (key == LV_KEY_BACKSPACE) {
            lv_textarea_delete_char(focused);
        } else if (key == LV_KEY_ENTER) {
            // Trigger READY event on textarea
            lv_obj_send_event(focused, LV_EVENT_READY, nullptr);
        }
        lv_timer_handler();
        wait_ms(50);
        return true;
    }

    spdlog::warn("[UITest] send_key() only supports textarea widgets");
    return false;
}

void wait_ms(uint32_t ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_timer_handler();  // Process LVGL tasks
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool wait_until(std::function<bool()> condition, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_timer_handler();  // Process LVGL tasks

        if (condition()) {
            return true;  // Condition met
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    spdlog::warn("[UITest] wait_until() timed out after {}ms", timeout_ms);
    return false;  // Timeout
}

bool wait_for_visible(lv_obj_t* widget, uint32_t timeout_ms) {
    if (!widget) {
        spdlog::error("[UITest] Invalid widget");
        return false;
    }

    return wait_until([widget]() {
        return !lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN);
    }, timeout_ms);
}

bool wait_for_hidden(lv_obj_t* widget, uint32_t timeout_ms) {
    if (!widget) {
        spdlog::error("[UITest] Invalid widget");
        return false;
    }

    return wait_until([widget]() {
        return lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN);
    }, timeout_ms);
}

bool wait_for_timers(uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < end) {
        uint32_t next_timer = lv_timer_handler();

        // If next timer is in the far future (> 1 second), no active timers
        if (next_timer > 1000) {
            spdlog::debug("[UITest] All timers completed");
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    spdlog::warn("[UITest] wait_for_timers() timed out after {}ms", timeout_ms);
    return false;
}

bool is_visible(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }
    return !lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN);
}

std::string get_text(lv_obj_t* widget) {
    if (!widget) {
        return "";
    }

    // Try label first
    if (lv_obj_check_type(widget, &lv_label_class)) {
        const char* text = lv_label_get_text(widget);
        return text ? std::string(text) : "";
    }

    // Try textarea
    if (lv_obj_check_type(widget, &lv_textarea_class)) {
        const char* text = lv_textarea_get_text(widget);
        return text ? std::string(text) : "";
    }

    spdlog::warn("[UITest] get_text() called on non-text widget");
    return "";
}

bool is_checked(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }
    return lv_obj_has_state(widget, LV_STATE_CHECKED);
}

lv_obj_t* find_by_name(lv_obj_t* parent, const std::string& name) {
    if (!parent) {
        return nullptr;
    }
    return lv_obj_find_by_name(parent, name.c_str());
}

int count_children_with_marker(lv_obj_t* parent, const char* marker) {
    if (!parent || !marker) {
        return 0;
    }

    int count = 0;
    uint32_t child_count = lv_obj_get_child_count(parent);

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child) continue;

        const void* user_data = lv_obj_get_user_data(child);
        if (user_data && strcmp((const char*)user_data, marker) == 0) {
            count++;
        }
    }

    return count;
}

} // namespace UITest

// Stub implementations for main.cpp functions needed by wizard tests
// These return nullptr since wizard tests don't actually use Moonraker
#include "moonraker_client.h"
#include "moonraker_api.h"
#include "app_globals.h"
#include "printer_state.h"

MoonrakerClient* get_moonraker_client() {
    return nullptr;
}

MoonrakerAPI* get_moonraker_api() {
    return nullptr;
}

PrinterState& get_printer_state() {
    static PrinterState instance;
    return instance;
}

// Stub implementations for notification functions (tests don't display UI)
void ui_notification_init() {
    // No-op in tests
}

void ui_notification_info(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_info: {}", message ? message : "(null)");
}

void ui_notification_success(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_success: {}", message ? message : "(null)");
}

void ui_notification_warning(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_warning: {}", message ? message : "(null)");
}

void ui_notification_error(const char* message, const char* title, bool modal) {
    spdlog::debug("[Test Stub] ui_notification_error: {} (title={}, modal={})",
                  message ? message : "(null)",
                  title ? title : "(null)",
                  modal);
}

// Stub implementations for toast functions (tests don't display UI)
#include "ui_toast.h"

void ui_toast_init() {
    // No-op in tests
}

void ui_toast_show(ToastSeverity severity, const char* message, uint32_t duration_ms) {
    (void)severity;
    (void)duration_ms;
    spdlog::debug("[Test Stub] ui_toast_show: {}", message ? message : "(null)");
}

void ui_toast_show_with_action(ToastSeverity severity, const char* message,
                                const char* action_text, toast_action_callback_t action_callback,
                                void* user_data, uint32_t duration_ms) {
    (void)severity;
    (void)action_text;
    (void)action_callback;
    (void)user_data;
    (void)duration_ms;
    spdlog::debug("[Test Stub] ui_toast_show_with_action: {}", message ? message : "(null)");
}

void ui_toast_hide() {
    // No-op in tests
}

bool ui_toast_is_visible() {
    return false;
}

// Note: FanPanel is provided by ui_panel_fan.o which is linked in TEST_PANEL_DEPS
// The get_global_fan_panel() function is defined in ui_panel_fan.cpp
