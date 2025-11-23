// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025 HelixScreen Contributors
/**
 * @file ui_widget_memory.h
 * @brief RAII memory management for custom LVGL widgets
 *
 * LVGL is a C library that doesn't understand RAII. Custom widgets that allocate
 * memory for user_data must manually free it in LV_EVENT_DELETE callbacks.
 *
 * **PROBLEM:** Manual cleanup is error-prone:
 * - Easy to forget lv_free() call
 * - If delete callback throws exception, memory leaks
 * - Complex cleanup logic is hard to maintain
 *
 * **SOLUTION:** This module provides RAII wrappers that guarantee cleanup:
 * - `lvgl_unique_ptr<T>` - Owns data allocated with lv_malloc
 * - `lvgl_make_unique<T>()` - Creates unique_ptr for lv_malloc'd data
 * - Automatic cleanup via destructor (exception-safe)
 *
 * **REQUIRED PATTERN for all custom widgets with user_data:**
 *
 * @code
 * // Widget state structure
 * struct MyWidgetState {
 *     int value;
 *     char* buffer;
 * };
 *
 * // Delete callback - use RAII wrapper
 * static void my_widget_delete_cb(lv_event_t* e) {
 *     lv_obj_t* obj = lv_event_get_target_obj(e);
 *     // Transfer ownership to unique_ptr - automatic cleanup
 *     lvgl_unique_ptr<MyWidgetState> state(
 *         (MyWidgetState*)lv_obj_get_user_data(obj)
 *     );
 *     lv_obj_set_user_data(obj, nullptr);
 *
 *     // Do any other cleanup here
 *     // Even if this throws, ~unique_ptr() still frees memory
 * }
 *
 * // Widget creation
 * lv_obj_t* my_widget_create(lv_obj_t* parent) {
 *     lv_obj_t* obj = lv_obj_create(parent);
 *     if (!obj) return nullptr;
 *
 *     // Allocate state using RAII helper
 *     auto state = lvgl_make_unique<MyWidgetState>();
 *     if (!state) {
 *         lv_obj_delete(obj);
 *         return nullptr;
 *     }
 *
 *     // Initialize state
 *     state->value = 0;
 *     state->buffer = nullptr;
 *
 *     // Transfer ownership to LVGL widget
 *     lv_obj_set_user_data(obj, state.release());
 *
 *     // Register cleanup
 *     lv_obj_add_event_cb(obj, my_widget_delete_cb, LV_EVENT_DELETE, nullptr);
 *
 *     return obj;
 * }
 * @endcode
 */

#ifndef UI_WIDGET_MEMORY_H
#define UI_WIDGET_MEMORY_H

#include "lvgl/lvgl.h"

#include <cstring>  // for memset
#include <memory>

/**
 * @brief Custom deleter for lv_malloc'd memory
 *
 * Used with std::unique_ptr to ensure lv_free() is called instead of delete.
 */
struct LvglDeleter {
    void operator()(void* ptr) const {
        if (ptr) {
            lv_free(ptr);
        }
    }
};

/**
 * @brief RAII wrapper for LVGL-allocated memory
 *
 * Similar to std::unique_ptr but uses lv_free() instead of delete.
 * Guarantees memory cleanup even if exceptions are thrown.
 *
 * @tparam T Type of data stored
 */
template <typename T> using lvgl_unique_ptr = std::unique_ptr<T, LvglDeleter>;

/**
 * @brief Allocate memory using lv_malloc and wrap in RAII unique_ptr
 *
 * @tparam T Type to allocate
 * @tparam Args Constructor argument types
 * @param args Arguments forwarded to T's constructor
 * @return lvgl_unique_ptr<T> Owning pointer (nullptr on allocation failure)
 */
template <typename T, typename... Args> inline lvgl_unique_ptr<T> lvgl_make_unique(Args&&... args) {
    T* ptr = (T*)lv_malloc(sizeof(T));
    if (!ptr) {
        return lvgl_unique_ptr<T>(nullptr);
    }
    // Placement new - construct object in lv_malloc'd memory
    new (ptr) T(std::forward<Args>(args)...);
    return lvgl_unique_ptr<T>(ptr);
}

/**
 * @brief Allocate POD (plain old data) array using lv_malloc and wrap in RAII
 *
 * For types that don't need constructor calls (char, int, simple structs).
 * Memory is zero-initialized with memset.
 *
 * @tparam T Element type (must be POD)
 * @param count Number of elements to allocate
 * @return lvgl_unique_ptr<T> Owning pointer (nullptr on allocation failure)
 */
template <typename T> inline lvgl_unique_ptr<T> lvgl_make_unique_array(size_t count) {
    T* ptr = (T*)lv_malloc(count * sizeof(T));
    if (!ptr) {
        return lvgl_unique_ptr<T>(nullptr);
    }
    memset(ptr, 0, count * sizeof(T));
    return lvgl_unique_ptr<T>(ptr);
}

#endif // UI_WIDGET_MEMORY_H
