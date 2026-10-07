// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_callback_helpers.h
 * @brief Helpers to reduce boilerplate in panel/overlay callback registration
 *
 * For widget lookup by name, use helix::ui::find_required() / find_optional()
 * in include/ui/ui_widget_helpers.h.
 *
 * @pattern Batch registration replaces repetitive lv_xml_register_event_cb() calls
 * @threading Main thread only
 */

#pragma once

#include "ui_event_safety.h"

#include "lvgl/lvgl.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <initializer_list>
#include <optional>
#include <type_traits>

namespace helix::ui {

/// Checked state of the widget the event is dispatched to (a toggle's value).
inline bool event_checked(lv_event_t* e) {
    return lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
}

/// Selected index of the dropdown the event is dispatched to.
inline int event_selected(lv_event_t* e) {
    return static_cast<int>(lv_dropdown_get_selected(lv_event_get_current_target_obj(e)));
}

/// The integer in the XML `user_data="..."` string the callback was bound with,
/// or nullopt when the binding carries none or it is not a number.
inline std::optional<int> event_user_int(lv_event_t* e) {
    const char* s = static_cast<const char*>(lv_event_get_user_data(e));
    if (!s) {
        return std::nullopt;
    }
    return helix::text_io::parse_leading<int>(s);
}

} // namespace helix::ui

/**
 * @brief Entry for batch XML event callback registration
 *
 * Pairs a callback name (matching XML event_cb attribute) with either a
 * function pointer or a captureless lambda. A lambda runs inside the exception
 * guard, which logs the callback's name; per-instance state arrives through
 * lv_event_get_user_data(), since the lambda cannot capture.
 */
struct XmlCallbackEntry {
    const char* name;
    lv_event_cb_t callback;

    XmlCallbackEntry(const char* n, lv_event_cb_t cb) : name(n), callback(cb) {}

    template <typename F, typename = std::enable_if_t<std::is_class_v<F>>>
    XmlCallbackEntry(const char* n, F f) : name(n), callback(guarded<F>(n, f)) {}

  private:
    // Every lambda expression is its own type, so these statics are per entry.
    // C++17 cannot default-construct a lambda, hence the optional.
    template <typename F> static lv_event_cb_t guarded(const char* n, F f) {
        static_assert(std::is_empty_v<F>, "XML callbacks cannot capture; use user_data");
        static const char* s_name;
        static std::optional<F> s_fn;
        s_name = n;
        s_fn.emplace(f);
        return [](lv_event_t* e) { helix::ui::event_safe_call(s_name, [e] { (*s_fn)(e); }); };
    }
};

/**
 * @brief Register multiple XML event callbacks in a single call
 *
 * Replaces repetitive blocks of lv_xml_register_event_cb() calls with
 * a compact table format. All callbacks are registered in the global
 * scope (nullptr component scope).
 *
 * @param callbacks Initializer list of {name, callback} pairs
 *
 * Example:
 * @code
 * register_xml_callbacks({
 *     {"on_home_all",  on_home_all},
 *     {"on_home_x",    on_home_x},
 *     {"on_home_y",    on_home_y},
 * });
 * @endcode
 */
inline void register_xml_callbacks(std::initializer_list<XmlCallbackEntry> callbacks) {
    for (const auto& cb : callbacks) {
        lv_xml_register_event_cb(nullptr, cb.name, cb.callback);
    }
}
