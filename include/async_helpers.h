// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_update_queue.h"

#include <functional>
#include <memory>
#include <type_traits>

/**
 * @file async_helpers.h
 * @brief Convenience wrappers for helix::ui::queue_update()
 *
 * Provides type-safe shortcuts for common patterns when deferring work
 * to the LVGL main thread via helix::ui::queue_update(). All helpers ultimately
 * call helix::ui::queue_update() — use that directly for simple lambdas.
 *
 * Exception safety is provided by UpdateQueue::process_pending().
 *
 * @section safety Thread Safety
 * WARNING: These helpers capture `this` pointers by value. If the object
 * is destroyed before the callback runs, use-after-free occurs. Callers
 * must ensure object lifetime exceeds callback execution. For long-lived
 * singletons like PrinterState, this is typically safe.
 *
 * For short-lived objects, use invoke_weak() with std::weak_ptr.
 */

namespace helix::async {

/**
 * @brief Queue a member function call with one parameter
 *
 * Convenience wrapper for the common pattern of calling an internal setter
 * on the main thread. Copies the value to ensure it survives across threads.
 *
 * @tparam T Instance type (e.g., PrinterState)
 * @tparam V Value type (copied, not referenced)
 * @param instance Pointer to the object (must outlive the async call)
 * @param method Member function pointer with signature void(V)
 * @param value Value to pass (copied into the async context)
 *
 * @code
 * helix::async::call_method(this, &PrinterState::set_temp_internal, temp);
 * @endcode
 */
template <typename T, typename V> void call_method(T* instance, void (T::*method)(V), V value) {
    if (!instance) {
        return;
    }

    helix::ui::queue_update(
        [instance, method, val = std::move(value)]() mutable { (instance->*method)(val); });
}

/**
 * @brief Queue a member function call with const reference parameter
 *
 * @tparam T Instance type
 * @tparam V Value type (copied into context)
 * @param instance Pointer to the object
 * @param method Member function pointer with signature void(const V&)
 * @param value Value to pass (copied, then passed as const ref)
 *
 * @code
 * helix::async::call_method_ref(this, &PrinterState::set_version_internal, version_string);
 * @endcode
 */
template <typename T, typename V>
void call_method_ref(T* instance, void (T::*method)(const V&), const V& value) {
    if (!instance) {
        return;
    }

    helix::ui::queue_update([instance, method, val = value]() { (instance->*method)(val); });
}

/**
 * @brief Queue a zero-argument member function call
 *
 * @tparam T Instance type
 * @param instance Pointer to the object (must outlive the async call)
 * @param method Member function pointer with signature void()
 *
 * @code
 * helix::async::call_method(this, &PrinterState::clear_backlash_internal);
 * @endcode
 */
template <typename T> void call_method(T* instance, void (T::*method)()) {
    if (!instance) {
        return;
    }

    helix::ui::queue_update([instance, method]() { (instance->*method)(); });
}

/**
 * @brief Queue a member function call with two parameters
 *
 * @tparam T Instance type
 * @tparam V1 First parameter type
 * @tparam V2 Second parameter type
 * @param instance Pointer to the object
 * @param method Member function pointer
 * @param v1 First value (copied)
 * @param v2 Second value (copied)
 *
 * @code
 * helix::async::call_method2(this, &PrinterState::set_connection_internal, state, message);
 * @endcode
 */
template <typename T, typename V1, typename V2>
void call_method2(T* instance, void (T::*method)(V1, V2), V1 v1, V2 v2) {
    if (!instance) {
        return;
    }

    helix::ui::queue_update([instance, method, val1 = std::move(v1),
                             val2 = std::move(v2)]() mutable { (instance->*method)(val1, val2); });
}

/**
 * @brief Queue a callable that only runs if a weak_ptr is still valid
 *
 * Recommended for objects with uncertain lifetime.
 *
 * @tparam T Object type
 * @tparam Callable Function taking T& as parameter
 * @param weak Weak pointer to the object
 * @param callable Function to invoke with the object
 *
 * @code
 * auto weak = std::weak_ptr<MyObject>(shared_obj);
 * helix::async::invoke_weak(weak, [value](MyObject& obj) {
 *     obj.set_value(value);
 * });
 * @endcode
 */
template <typename T, typename Callable>
void invoke_weak(std::weak_ptr<T> weak, Callable&& callable) {
    helix::ui::queue_update([w = std::move(weak), fn = std::forward<Callable>(callable)]() {
        if (auto shared = w.lock()) {
            fn(*shared);
        }
    });
}

} // namespace helix::async
