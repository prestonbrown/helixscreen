// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_utils.h"

#include "lvgl/src/core/lv_obj_draw_private.h" // lv_obj_get_ext_draw_size: no public getter
#include "lvgl/src/indev/lv_indev_private.h"   // pointer.act_obj: no public getter outside dispatch
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

// ============================================================================
// Responsive Layout
// ============================================================================

lv_coord_t ui_get_header_content_padding() {
    // Use unified space_* system - values are already responsive based on breakpoint
    // set during theme initialization (space_lg = 12/16/20px at small/medium/large)
    int32_t spacing = theme_manager_get_spacing("space_lg");

    // Fallback if theme not initialized (e.g., in unit tests)
    constexpr int32_t kDefaultSpaceLg = 16; // Medium breakpoint value
    if (spacing == 0) {
        spacing = kDefaultSpaceLg;
    }

    return spacing;
}

lv_coord_t ui_get_responsive_header_height(lv_coord_t screen_height) {
    // Responsive header heights for space efficiency:
    // Large/Medium (≥600px): 60px (comfortable)
    // Small (480-599px): 48px (compact)
    // Tiny (≤479px): 40px (minimal)

    if (screen_height >= UI_SCREEN_MEDIUM_H) {
        return 60;
    } else if (screen_height >= UI_SCREEN_SMALL_H) {
        return 48;
    } else {
        return 40;
    }
}

// ============================================================================
// LED Icon Utilities
// ============================================================================

const char* ui_brightness_to_lightbulb_icon(int brightness) {
    // Clamp to valid range
    if (brightness <= 0) {
        return "lightbulb_outline"; // OFF state
    }
    if (brightness < 15) {
        return "lightbulb_on_10";
    }
    if (brightness < 25) {
        return "lightbulb_on_20";
    }
    if (brightness < 35) {
        return "lightbulb_on_30";
    }
    if (brightness < 45) {
        return "lightbulb_on_40";
    }
    if (brightness < 55) {
        return "lightbulb_on_50";
    }
    if (brightness < 65) {
        return "lightbulb_on_60";
    }
    if (brightness < 75) {
        return "lightbulb_on_70";
    }
    if (brightness < 85) {
        return "lightbulb_on_80";
    }
    if (brightness < 95) {
        return "lightbulb_on_90";
    }
    return "lightbulb_on"; // 100%
}

// ============================================================================
// Owned user_data strings
// ============================================================================

namespace helix::ui {

namespace {

/// Frees the owned string and clears the slot. Registered exactly once per
/// object; nulling the slot makes a repeated DELETE delivery a no-op.
void owned_user_string_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    if (!obj) {
        return;
    }
    auto* owned = static_cast<char*>(lv_obj_get_user_data(obj));
    if (owned) {
        lv_free(owned);
        lv_obj_set_user_data(obj, nullptr);
    }
}

/// True when this helper installed the cleanup handler on @p obj — i.e. the
/// user_data slot belongs to us and may be read/freed (lesson L069).
bool owns_user_string(lv_obj_t* obj) {
    uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == owned_user_string_delete_cb) {
            return true;
        }
    }
    return false;
}

} // namespace

bool set_owned_user_string(lv_obj_t* obj, std::string_view s) {
    if (!obj) {
        return false;
    }

    // `s.size() + 1` must not wrap: a wrapped 0 would make lv_malloc succeed and
    // the copy below run off the end of the heap.
    if (s.size() == std::numeric_limits<size_t>::max()) {
        spdlog::error("[ui_utils] set_owned_user_string: length overflows");
        return false;
    }

    const bool mine = owns_user_string(obj);
    if (!mine && lv_obj_get_user_data(obj) != nullptr) {
        // L069: somebody else owns this slot. Never stomp it, never free it.
        spdlog::error("[ui_utils] set_owned_user_string: user_data already owned elsewhere");
        return false;
    }

    auto* copy = static_cast<char*>(lv_malloc(s.size() + 1));
    if (!copy) {
        // Allocation failed — leave user_data exactly as it was. Constrained
        // devices (AD5M/CC1) reach this under memory pressure; the pre-helper
        // code memcpy'd into the null result and took the process down.
        spdlog::error("[ui_utils] set_owned_user_string: failed to allocate {} bytes",
                      s.size() + 1);
        return false;
    }
    if (!s.empty()) {
        std::memcpy(copy, s.data(), s.size());
    }
    copy[s.size()] = '\0';

    if (mine) {
        // Replace our own previous copy; the cleanup handler stays registered.
        auto* previous = static_cast<char*>(lv_obj_get_user_data(obj));
        lv_free(previous);
        lv_obj_set_user_data(obj, copy);
    } else {
        lv_obj_set_user_data(obj, copy);
        lv_obj_add_event_cb(obj, owned_user_string_delete_cb, LV_EVENT_DELETE, nullptr);
    }
    return true;
}

const char* get_owned_user_string(lv_obj_t* obj) {
    if (!obj || !owns_user_string(obj)) {
        return nullptr;
    }
    return static_cast<const char*>(lv_obj_get_user_data(obj));
}

// ============================================================================
// Input reset scoped to a subtree
// ============================================================================

namespace {

bool in_subtree(const lv_obj_t* obj, const lv_obj_t* subtree) {
    for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        if (obj == subtree) {
            return true;
        }
    }
    return false;
}

} // namespace

void reset_input_within(lv_obj_t* subtree) {
    if (!subtree) {
        return;
    }
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        if (in_subtree(indev->pointer.act_obj, subtree) ||
            in_subtree(lv_indev_get_scroll_obj(indev), subtree)) {
            lv_indev_reset(indev, subtree);
        }
    }
}

// ============================================================================
// Required widget lookups
// ============================================================================

lv_obj_t* find_required(lv_obj_t* root, const char* name, const char* owner) {
    if (!root) {
        return nullptr;
    }
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    if (!obj) {
        // One report per (owner, name): a lookup on every activate would
        // otherwise flood the log in release builds.
        static std::unordered_set<std::string> reported;
        if (reported.insert(std::string(owner) + '/' + name).second) {
            std::string msg = std::string("[") + owner + "] required widget '" + name +
                              "' not found in its component's XML";
            report_ui_contract_breach(msg.c_str());
        }
    }
    return obj;
}

static void child_overhang_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    int32_t s = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            s = std::max<int32_t>(s, lv_obj_get_ext_draw_size(child));
    }
    lv_event_set_ext_draw_size(e, s);
}

void pass_child_overhang(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj, child_overhang_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
}

void refresh_overhang_chain(lv_obj_t* child) {
    lv_obj_refresh_ext_draw_size(child);
    for (lv_obj_t* p = lv_obj_get_parent(child);
         p && lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE); p = lv_obj_get_parent(p))
        lv_obj_refresh_ext_draw_size(p);
}

static void clear_slot_cb(lv_event_t* e) {
    auto* slot = static_cast<lv_obj_t**>(lv_event_get_user_data(e));
    if (slot && *slot == lv_event_get_target_obj(e)) {
        *slot = nullptr;
    }
}

void store_until_deleted(lv_obj_t** slot, lv_obj_t* obj) {
    *slot = obj;
    if (obj) {
        lv_obj_remove_event_cb_with_user_data(obj, clear_slot_cb, slot);
        // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
        lv_obj_add_event_cb(obj, clear_slot_cb, LV_EVENT_DELETE, slot);
    }
}

} // namespace helix::ui
