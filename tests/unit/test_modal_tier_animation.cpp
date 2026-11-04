// SPDX-License-Identifier: GPL-3.0-or-later

// A modal's entrance and exit fade the full-screen backdrop and scale the
// dialog through a transform layer on every frame. The limited tiers show and
// remove the modal in its final state instead.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lib/lvgl/src/misc/lv_timer_private.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

namespace {
constexpr const char* kTestComponent = "print_cancel_confirm_modal";

/// An owned modal records whether its widget tree was already deleted when its
/// destructor ran; a destructor that touches its widgets needs them alive.
bool g_tree_deleted = false;
int g_destroyed = 0;
bool g_tree_deleted_at_dtor = false;

class TreeOrderModal : public Modal {
  public:
    ~TreeOrderModal() override {
        ++g_destroyed;
        g_tree_deleted_at_dtor = g_tree_deleted;
    }
    const char* get_name() const override {
        return "TreeOrderModal";
    }
    const char* component_name() const override {
        return kTestComponent;
    }
    void on_show() override {
        lv_obj_add_event_cb(
            dialog(), [](lv_event_t*) { g_tree_deleted = true; }, LV_EVENT_DELETE, nullptr);
    }
};

/// Fires the ready one-shot timers (lv_async_call, lv_obj_delete_async) without
/// draining the UpdateQueue first. On the device a fresh one-shot sits at the
/// head of LVGL's timer list and runs before the queue's own timer; the test
/// pump drains the queue first, which would hide an instance-after-tree order.
void fire_one_shots_before_queue() {
    for (int safety = 0; safety < 100; ++safety) {
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t && !(t->repeat_count > 0 && t->period == 0 && t->timer_cb))
            t = lv_timer_get_next(t);
        if (!t)
            return;
        t->repeat_count--;
        t->timer_cb(t);
    }
}

struct TierScope {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    int saved_tier = 0;
    bool saved_anims = helix::DisplaySettingsManager::instance().get_animations_enabled();

    explicit TierScope(PlatformTier t, bool animations = true) {
        REQUIRE(tier != nullptr);
        saved_tier = lv_subject_get_int(tier);
        lv_subject_set_int(tier, static_cast<int>(t));
        helix::DisplaySettingsManager::instance().set_animations_enabled(animations);
    }
    ~TierScope() {
        lv_anim_delete_all();
        ModalStack::instance().clear();
        lv_subject_set_int(tier, saved_tier);
        helix::DisplaySettingsManager::instance().set_animations_enabled(saved_anims);
        helix::ui::UpdateQueue::instance().drain();
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "modal: limited tiers show and hide without animating",
                 "[modal][modal_tier][platform_tier]") {
    for (PlatformTier t : {PlatformTier::BASIC, PlatformTier::EMBEDDED}) {
        TierScope scope(t);
        lv_obj_t* dialog = Modal::show(kTestComponent);
        REQUIRE(dialog != nullptr);
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);

        CHECK(lv_obj_get_style_transform_scale_x(dialog, LV_PART_MAIN) == LV_SCALE_NONE);
        CHECK(lv_obj_get_style_transform_scale_y(dialog, LV_PART_MAIN) == LV_SCALE_NONE);
        CHECK(lv_anim_get(dialog, nullptr) == nullptr);
        CHECK(lv_anim_get(backdrop, nullptr) == nullptr);
        CHECK(lv_obj_get_style_opa(backdrop, LV_PART_MAIN) == LV_OPA_COVER);
        CHECK(lv_obj_get_style_opa(dialog, LV_PART_MAIN) == LV_OPA_COVER);

        Modal::hide(dialog);
        CHECK(lv_anim_get(backdrop, nullptr) == nullptr);
        CHECK(lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(ModalStack::instance().stack_empty());
        process_lvgl(50);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "modal: the capable tier still animates entrance and exit",
                 "[modal][modal_tier][platform_tier]") {
    TierScope scope(PlatformTier::STANDARD);
    lv_obj_t* dialog = Modal::show(kTestComponent);
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);

    CHECK(lv_obj_get_style_transform_scale_x(dialog, LV_PART_MAIN) < LV_SCALE_NONE);
    CHECK(lv_anim_get(dialog, nullptr) != nullptr);
    CHECK(lv_anim_get(backdrop, nullptr) != nullptr);

    Modal::hide(dialog);
    CHECK(ModalStack::instance().is_exiting(backdrop));
    CHECK(lv_anim_get(backdrop, nullptr) != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "modal: an immediate exit frees the owned instance first",
                 "[modal][modal_tier][platform_tier]") {
    auto [tier, animations] =
        GENERATE(std::pair{PlatformTier::EMBEDDED, true}, std::pair{PlatformTier::STANDARD, false});
    TierScope scope(tier, animations);
    g_tree_deleted = false;
    g_destroyed = 0;
    g_tree_deleted_at_dtor = true;

    REQUIRE(Modal::show_owned(std::make_unique<TreeOrderModal>(), test_screen()));
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    Modal::hide(dialog);
    fire_one_shots_before_queue();
    for (int i = 0; i < 5 && (g_destroyed == 0 || !g_tree_deleted); ++i) {
        process_lvgl(20);
    }
    REQUIRE(g_destroyed == 1);
    REQUIRE(g_tree_deleted);
    CHECK_FALSE(g_tree_deleted_at_dtor);
    CHECK(ModalStack::instance().stack_empty());
}
