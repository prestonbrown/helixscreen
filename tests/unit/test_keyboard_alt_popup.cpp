// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_alt_popup.cpp
 * @brief The long-press popup on the on-screen keyboard: what it shows, where it
 *        sits, and which character a release over it types.
 *
 * Drives the real keyboard through the press, long-press and release events a
 * pointer delivers, with a test pointer device carrying the touch position, and
 * finds the popup by its widget name.
 */

#include "ui_keyboard_manager.h"

#include "../../lib/lvgl/src/indev/lv_indev_private.h"
#include "../../lib/lvgl/src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

namespace {

constexpr const char* kPopupName = "keyboard_alt_popup";

void no_read(lv_indev_t*, lv_indev_data_t*) {}

/// Shows the process-wide keyboard for one textarea, with animations off so the
/// keyboard sits at its final position, and a pointer device whose position the
/// test sets. Everything is put back afterwards.
struct ScopedKeyboardPress {
    lv_obj_t* textarea = nullptr;
    lv_indev_t* pointer = nullptr;
    bool prior_animations = false;

    explicit ScopedKeyboardPress(lv_obj_t* parent) {
        auto& settings = DisplaySettingsManager::instance();
        prior_animations = settings.get_animations_enabled();
        settings.set_animations_enabled(false);
        release_keyboard();
        KeyboardManager::instance().init(parent);

        textarea = lv_textarea_create(parent);
        lv_textarea_set_one_line(textarea, true);
        lv_obj_set_size(textarea, 120, 32);
        lv_obj_set_pos(textarea, 4, 4);
        KeyboardManager::instance().register_textarea(textarea);
        KeyboardManager::instance().show(textarea);
        lv_obj_update_layout(lv_screen_active());

        pointer = lv_indev_create();
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(pointer, no_read);
    }
    ~ScopedKeyboardPress() {
        lv_indev_delete(pointer);
        release_keyboard();
        lv_obj_delete(textarea);
        DisplaySettingsManager::instance().set_animations_enabled(prior_animations);
    }

    static void release_keyboard() {
        KeyboardManager::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* keyboard() const {
        return KeyboardManager::instance().get_instance();
    }

    /// Button id of the key labelled @p text, or UINT32_MAX.
    uint32_t key_id(const char* text) const {
        for (uint32_t id = 0; id < 200; ++id) {
            const char* t = lv_buttonmatrix_get_button_text(keyboard(), id);
            if (t == nullptr) {
                break;
            }
            if (std::strcmp(t, text) == 0) {
                return id;
            }
        }
        return UINT32_MAX;
    }

    void point_at(lv_point_t p) {
        pointer->pointer.act_point = p;
    }

    /// Press the middle of key @p id and hold it until the long-press fires.
    void long_press(uint32_t id) {
        const lv_area_t a = key_area(id);
        point_at({(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2});
        lv_buttonmatrix_set_selected_button(keyboard(), id);
        lv_obj_send_event(keyboard(), LV_EVENT_PRESSED, pointer);
        lv_obj_send_event(keyboard(), LV_EVENT_LONG_PRESSED, pointer);
    }

    void release_at(lv_point_t at) {
        point_at(at);
        lv_obj_send_event(keyboard(), LV_EVENT_RELEASED, pointer);
    }

    /// Absolute rect of key @p id as the matrix lays it out.
    lv_area_t key_area(uint32_t id) const {
        lv_obj_update_layout(keyboard());
        auto* btnm = reinterpret_cast<lv_buttonmatrix_t*>(keyboard());
        lv_area_t coords;
        lv_obj_get_coords(keyboard(), &coords);
        lv_area_t a = btnm->button_areas[id];
        lv_area_move(&a, coords.x1, coords.y1);
        return a;
    }

    std::string typed() const {
        return lv_textarea_get_text(textarea);
    }
};

lv_area_t coords_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}

lv_point_t center_of(const lv_area_t& a) {
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

lv_obj_t* find_popup() {
    return lv_obj_find_by_name(lv_screen_active(), kPopupName);
}

/// The popup is gone or on its way out (deletion is deferred past the event).
bool popup_dismissed() {
    lv_obj_t* popup = find_popup();
    return popup == nullptr || lv_obj_is_hidden(popup);
}

/// The characters the popup shows, left to right.
std::string popup_chars(lv_obj_t* popup) {
    std::string out;
    const uint32_t n = lv_obj_get_child_count(popup);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(popup, static_cast<int32_t>(i));
        out += lv_label_get_text(cell);
    }
    return out;
}

/// Rebuilds a key's alternate order from @p order, so a test that promotes a
/// character leaves the next test the order it found.
void restore_order(char key, const std::string& order) {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        KeyboardManager::instance().promote_alternative(key, *it);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Keyboard popup: a long-press shows the key's alternates",
                 "[keyboard][keyboard_popup]") {
    auto [w, h] =
        GENERATE(std::pair<int32_t, int32_t>{480, 272}, std::pair<int32_t, int32_t>{800, 480});
    const char* key = GENERATE(",", "q", "p", "g", "s", "d", "x");
    CAPTURE(w, h, key);
    ScopedResolution res(lv_display_get_default(), w, h);
    lv_obj_set_size(test_screen(), w, h);
    ScopedKeyboardPress kb(test_screen());

    const uint32_t id = kb.key_id(key);
    REQUIRE(id != UINT32_MAX);
    const std::string order = KeyboardManager::instance().find_alternatives(key[0]);
    REQUIRE_FALSE(order.empty());

    REQUIRE(find_popup() == nullptr);
    kb.long_press(id);
    lv_obj_t* popup = find_popup();
    REQUIRE(popup != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(popup));

    // One cell per alternate, in the key's current order, left to right inside
    // the popup.
    CHECK(popup_chars(popup) == order);
    const lv_area_t p = coords_of(popup);
    const uint32_t n = lv_obj_get_child_count(popup);
    int32_t prev_center = INT32_MIN;
    for (uint32_t i = 0; i < n; ++i) {
        const lv_area_t c = coords_of(lv_obj_get_child(popup, static_cast<int32_t>(i)));
        CHECK(c.x1 >= p.x1);
        CHECK(c.x2 <= p.x2);
        CHECK(center_of(c).x > prev_center);
        prev_center = center_of(c).x;
    }

    // Wholly on screen, and above the key it came from.
    CHECK(p.x1 >= 0);
    CHECK(p.x2 < w);
    CHECK(p.y1 >= 0);
    const lv_area_t k = kb.key_area(id);
    CHECK(p.y2 < k.y1);

    // Centered on the key unless that would push it off an edge.
    const bool clamped = p.x1 == 0 || p.x2 == w - 1;
    if (!clamped) {
        CHECK(std::abs(center_of(p).x - center_of(k).x) <= 1);
    }

    // The frame reads as a success-bordered card.
    CHECK(lv_obj_get_style_border_width(popup, LV_PART_MAIN) == 2);
    CHECK(lv_color_eq(lv_obj_get_style_border_color(popup, LV_PART_MAIN),
                      theme_manager_parse_hex_color(lv_xml_get_const(nullptr, "success"))));
    CHECK(lv_obj_get_style_bg_opa(popup, LV_PART_MAIN) == LV_OPA_90);

    // Letting go away from the popup and the key cancels: nothing typed.
    kb.release_at({w / 2, 0});
    CHECK(popup_dismissed());
    CHECK(kb.typed().empty());
    process_lvgl(20);
    CHECK(find_popup() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Keyboard popup: releasing over a cell types it and makes it the default",
                 "[keyboard][keyboard_popup]") {
    ScopedKeyboardPress kb(test_screen());

    const std::string order = KeyboardManager::instance().find_alternatives(',');
    REQUIRE(order.size() == 3);
    const uint32_t comma = kb.key_id(",");
    REQUIRE(comma != UINT32_MAX);

    kb.long_press(comma);
    lv_obj_t* popup = find_popup();
    REQUIRE(popup != nullptr);
    REQUIRE(lv_obj_get_child_count(popup) == 3);

    // Slide onto the last cell and let go.
    kb.release_at(center_of(coords_of(lv_obj_get_child(popup, 2))));

    CHECK(kb.typed() == std::string(1, order[2]));
    CHECK(KeyboardManager::instance().find_alternatives(',')[0] == order[2]);
    CHECK(popup_dismissed());
    process_lvgl(20);

    restore_order(',', order);
    REQUIRE(std::string(KeyboardManager::instance().find_alternatives(',')) == order);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Keyboard popup: releasing back on the key types the default",
                 "[keyboard][keyboard_popup]") {
    ScopedKeyboardPress kb(test_screen());

    const std::string order = KeyboardManager::instance().find_alternatives(',');
    REQUIRE_FALSE(order.empty());
    const uint32_t comma = kb.key_id(",");
    REQUIRE(comma != UINT32_MAX);

    kb.long_press(comma);
    REQUIRE(find_popup() != nullptr);
    kb.release_at(center_of(kb.key_area(comma)));

    CHECK(kb.typed() == std::string(1, order[0]));
    CHECK(popup_dismissed());
    process_lvgl(20);
}
