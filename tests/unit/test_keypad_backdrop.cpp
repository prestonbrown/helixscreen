// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"

#include <algorithm>
#include <array>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class KeypadBackdropFixture : public LVGLUITestFixture {
  public:
    KeypadBackdropFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }

    ~KeypadBackdropFixture() override {
        // The keypad tree hangs off the screen; free it so the next case starts
        // with none.
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
    }
};

std::vector<lv_obj_t*> children_of(lv_obj_t* obj) {
    std::vector<lv_obj_t*> out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        out.push_back(lv_obj_get_child(obj, i));
    return out;
}

// Allocates widgets of `cls` under `parent` until one lands on `freed`'s address, and
// returns it (nullptr when the allocator never hands the block back). glibc returns a
// just-freed block of the same size straight away, so this lands within a few tries; an
// allocator that never recycles (ASan's quarantine) leaves nothing to test.
lv_obj_t* recycle_address(lv_obj_t* freed, const lv_obj_class_t* cls, lv_obj_t* parent) {
    std::vector<lv_obj_t*> spares;
    lv_obj_t* victim = nullptr;
    for (int i = 0; i < 4096 && victim == nullptr; i++) {
        lv_obj_t* obj = lv_obj_class_create_obj(cls, parent);
        lv_obj_class_init_obj(obj);
        if (obj == freed)
            victim = obj;
        else
            spares.push_back(obj);
    }
    for (lv_obj_t* s : spares)
        lv_obj_delete(s);
    return victim;
}

} // namespace

// The backdrop hangs off the screen, so anything clearing the screen takes it along
// without the keypad being told. Its address is then free for the next allocation, and
// helix::test::widget_in_tree() cannot tell a recycled address from the original object.
TEST_CASE_METHOD(KeypadBackdropFixture,
                 "Keypad: closing after the backdrop died leaves its recycled address alone",
                 "[keypad]") {
    lv_obj_t* screen = lv_screen_active();
    const std::vector<lv_obj_t*> before = children_of(screen);

    ui_keypad_config_t cfg{};
    cfg.min_value = 0;
    cfg.max_value = 300;
    cfg.title_label = "Test";
    ui_keypad_show(&cfg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ui_keypad_is_visible());

    // The show adds two backdrops to the screen besides the keypad itself: the keypad's
    // own, raised before the push, and the one navigation gives a first overlay during
    // it. The keypad's is therefore the lower of the two.
    std::vector<lv_obj_t*> added;
    for (lv_obj_t* c : children_of(screen)) {
        if (std::find(before.begin(), before.end(), c) == before.end() &&
            lv_obj_find_by_name(c, "header_title") == nullptr) {
            added.push_back(c);
        }
    }
    REQUIRE(added.size() == 2);
    lv_obj_t* backdrop = added.front();
    const lv_obj_class_t* backdrop_class = lv_obj_get_class(backdrop);

    lv_obj_delete(backdrop);

    lv_obj_t* victim = recycle_address(backdrop, backdrop_class, screen);
    if (victim == nullptr)
        SKIP("the allocator did not recycle the backdrop's address");

    ui_keypad_hide();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);

    // A deferred delete would have moved it to the top layer and then freed it.
    REQUIRE(helix::test::widget_in_tree(victim));
    CHECK(lv_obj_get_parent(victim) == screen);
    lv_obj_delete(victim);
}

// The widget tree is built on first show and hangs off the screen, so it dies with
// anything that clears the screen. A recycled address must not read as a visible keypad.
TEST_CASE_METHOD(KeypadBackdropFixture,
                 "Keypad: a widget that died elsewhere is not visible through its recycled address",
                 "[keypad]") {
    lv_obj_t* screen = lv_screen_active();
    const std::vector<lv_obj_t*> before = children_of(screen);

    ui_keypad_config_t cfg{};
    cfg.max_value = 300;
    cfg.title_label = "Test";
    ui_keypad_show(&cfg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ui_keypad_is_visible());
    ui_keypad_hide();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);
    REQUIRE_FALSE(ui_keypad_is_visible());

    lv_obj_t* widget = nullptr;
    for (lv_obj_t* c : children_of(screen)) {
        if (std::find(before.begin(), before.end(), c) == before.end() &&
            lv_obj_find_by_name(c, "header_title") != nullptr) {
            widget = c;
        }
    }
    REQUIRE(widget != nullptr);
    const lv_obj_class_t* widget_class = lv_obj_get_class(widget);

    lv_obj_delete(widget);

    // Where the allocator recycles the address, the stale pointer would read as a visible
    // keypad: the recycled object is not hidden.
    lv_obj_t* victim = recycle_address(widget, widget_class, screen);
    if (victim != nullptr) {
        CHECK_FALSE(ui_keypad_is_visible());
        lv_obj_delete(victim);
    }

    // Independent of recycling: with the pointer forgotten, the next show builds a new tree
    // instead of reusing freed memory.
    ui_keypad_show(&cfg);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ui_keypad_is_visible());
    int rebuilt = 0;
    for (lv_obj_t* c : children_of(screen)) {
        if (std::find(before.begin(), before.end(), c) == before.end() &&
            lv_obj_find_by_name(c, "header_title") != nullptr) {
            rebuilt++;
        }
    }
    CHECK(rebuilt == 1);
}

// The parent is captured at init and the tree is built under it on first show. If it died
// meanwhile, a show must not build the keypad inside whatever now sits at that address.
TEST_CASE_METHOD(KeypadBackdropFixture,
                 "Keypad: showing after the parent died builds nothing under it", "[keypad]") {
    lv_obj_t* screen = lv_screen_active();
    const std::vector<lv_obj_t*> before = children_of(screen);
    lv_obj_t* holder = lv_obj_create(screen);
    const lv_obj_class_t* holder_class = lv_obj_get_class(holder);
    ui_keypad_init(holder);

    lv_obj_delete(holder);

    lv_obj_t* victim = recycle_address(holder, holder_class, screen);

    ui_keypad_config_t cfg{};
    cfg.max_value = 300;
    cfg.title_label = "Test";
    ui_keypad_show(&cfg);
    helix::ui::UpdateQueue::instance().drain();

    // With the parent forgotten there is nowhere to build, whether or not the address
    // was recycled; a stale parent would build inside freed memory.
    CHECK_FALSE(ui_keypad_is_visible());
    for (lv_obj_t* c : children_of(screen)) {
        if (std::find(before.begin(), before.end(), c) == before.end()) {
            CHECK(lv_obj_find_by_name(c, "header_title") == nullptr);
        }
    }
    if (victim != nullptr) {
        CHECK(lv_obj_get_child_count(victim) == 0);
        lv_obj_delete(victim);
    }
}
