// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_ref.cpp
 * @brief WidgetRef clears itself on the widget's own delete, and leaves no
 *        hook behind when it lets go first (prestonbrown/helixscreen#1298)
 */

#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_widget_ref.h"

#include "../lvgl_test_fixture.h"

#include <memory>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::WidgetRef;

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself when its widget is deleted",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WidgetRef ref(obj);
    REQUIRE(ref.get() == obj);
    REQUIRE(ref);

    lv_obj_delete(obj);

    CHECK(ref.get() == nullptr);
    CHECK_FALSE(ref);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself when an ancestor subtree is deleted",
                 "[widget_ref]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    WidgetRef ref;
    ref = child;
    REQUIRE(ref.get() == child);

    lv_obj_delete(parent);

    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef clears itself on a deferred delete", "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WidgetRef ref(obj);

    lv_obj_delete_async(obj);
    REQUIRE(ref.get() == obj); // still alive until the async call runs
    process_lvgl(20);

    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef rebind unhooks the previous widget", "[widget_ref]") {
    lv_obj_t* first = lv_obj_create(test_screen());
    lv_obj_t* second = lv_obj_create(test_screen());
    const uint32_t first_events = lv_obj_get_event_count(first);

    WidgetRef ref(first);
    REQUIRE(lv_obj_get_event_count(first) == first_events + 1);

    ref = second;
    CHECK(lv_obj_get_event_count(first) == first_events);

    // The old widget dying must not touch a handle that moved on.
    lv_obj_delete(first);
    CHECK(ref.get() == second);

    lv_obj_delete(second);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef rebinding to the same widget keeps one hook",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WidgetRef ref(obj);
    ref = obj;
    ref.reset(obj);
    CHECK(lv_obj_get_event_count(obj) == base + 1);

    lv_obj_delete(obj);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef cleared by hand unhooks the live widget",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WidgetRef ref(obj);
    ref = nullptr;
    CHECK(ref.get() == nullptr);
    CHECK(lv_obj_get_event_count(obj) == base);

    lv_obj_delete(obj);
    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef destroyed before its widget leaves no hook behind",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    auto ref = std::make_unique<WidgetRef>(obj);
    REQUIRE(lv_obj_get_event_count(obj) == base + 1);
    ref.reset();

    // A hook left behind would write into the freed handle here.
    CHECK(lv_obj_get_event_count(obj) == base);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef destroyed after its widget touches nothing",
                 "[widget_ref]") {
    auto ref = std::make_unique<WidgetRef>(lv_obj_create(test_screen()));
    lv_obj_delete(ref->get());
    REQUIRE(ref->get() == nullptr);
    ref.reset(); // must not reach into the freed widget
    SUCCEED();
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef cleared from another delete handler on its widget",
                 "[widget_ref]") {
    // An owner whose own LV_EVENT_DELETE handler lets go of the handle while
    // LVGL is still walking that widget's event list.
    struct Owner {
        WidgetRef ref;
        int fired = 0;
    } owner;

    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            auto* o = static_cast<Owner*>(lv_event_get_user_data(e));
            o->ref = nullptr;
            ++o->fired;
        },
        LV_EVENT_DELETE, &owner);
    owner.ref = obj;

    lv_obj_delete(obj);

    CHECK(owner.fired == 1);
    CHECK(owner.ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef converts to lv_obj_t* for LVGL calls",
                 "[widget_ref]") {
    WidgetRef ref(lv_obj_create(test_screen()));
    lv_obj_add_flag(ref, LV_OBJ_FLAG_HIDDEN);
    CHECK(lv_obj_has_flag(ref.get(), LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* raw = ref;
    CHECK(raw == ref.get());
    CHECK(ref == raw);
    CHECK(ref != nullptr);
    lv_obj_delete(raw);
    CHECK(ref == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WidgetRef assigned during its widget's delete is empty",
                 "[widget_ref]") {
    WidgetRef ref;
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            *static_cast<WidgetRef*>(lv_event_get_user_data(e)) = lv_event_get_target_obj(e);
        },
        LV_EVENT_DELETE, &ref);

    lv_obj_delete(obj);

    CHECK(ref.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "WidgetRef names_dying answers inside the owner's delete callback",
                 "[widget_ref]") {
    struct Owner {
        WidgetRef ref;
        bool plain_compare = true;
        bool named = false;
    } owner;
    lv_obj_t* obj = lv_obj_create(test_screen());
    owner.ref = obj; // assigned first, so its hook runs before the callback below
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            auto* o = static_cast<Owner*>(lv_event_get_user_data(e));
            o->plain_compare = o->ref == lv_event_get_target_obj(e);
            o->named = o->ref.names_dying(lv_event_get_target_obj(e));
        },
        LV_EVENT_DELETE, &owner);
    lv_obj_t* other = lv_obj_create(test_screen());

    CHECK_FALSE(owner.ref.names_dying(other));
    lv_obj_delete(obj);

    CHECK_FALSE(owner.plain_compare);
    CHECK(owner.named);
    owner.ref = nullptr;
    CHECK_FALSE(owner.ref.names_dying(obj)); // letting go forgets the address
}

// ============================================================================
// WeakWidget
// ============================================================================

using helix::ui::WeakWidget;

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget reads nullptr once its widget dies", "[widget_ref]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_t* obj = lv_obj_create(test_screen());
    auto original = std::make_unique<WeakWidget>(obj);
    const WeakWidget copy = *original;
    const WeakWidget of_child(child);
    const auto captured = [weak = WeakWidget(obj)]() -> lv_obj_t* { return weak; };
    original.reset();
    REQUIRE(copy.get() == obj);
    REQUIRE(captured() == obj);
    REQUIRE(of_child.get() == child);

    lv_obj_delete(obj);
    lv_obj_delete(parent);

    CHECK(copy.get() == nullptr);
    CHECK_FALSE(copy);
    CHECK(captured() == nullptr);
    CHECK(of_child.get() == nullptr);

    // The allocator is free to hand the address back, the case a tree search cannot
    // tell apart. Whatever it does here, the handle stays null.
    std::vector<lv_obj_t*> churn;
    for (int i = 0; i < 16; ++i) {
        churn.push_back(lv_obj_create(test_screen()));
    }
    CHECK(copy.get() == nullptr);
    for (lv_obj_t* o : churn) {
        lv_obj_delete(o);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget handles to one widget share one hook",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WeakWidget first(obj);
    REQUIRE(lv_obj_get_event_count(obj) == base + 1);
    WeakWidget second(obj);
    WeakWidget assigned;
    assigned = obj;
    CHECK(lv_obj_get_event_count(obj) == base + 1);

    lv_obj_delete(obj);
    CHECK(first.get() == nullptr);
    CHECK(second.get() == nullptr);
    CHECK(assigned.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget reset drops this copy and leaves the others",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WeakWidget first(obj);
    WeakWidget second = first;

    first.reset();

    CHECK(first.get() == nullptr);
    CHECK(second.get() == obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget with every copy gone still rejoins the live widget",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);
    { WeakWidget gone(obj); }
    REQUIRE(lv_obj_get_event_count(obj) == base + 1);

    WeakWidget again(obj);
    CHECK(lv_obj_get_event_count(obj) == base + 1);
    CHECK(again.get() == obj);

    lv_obj_delete(obj); // the hook writes into a slot that must still exist
    CHECK(again.get() == nullptr);

    // No copy at all when the widget dies: under ASAN a slot freed with its last
    // copy is a write after free here.
    lv_obj_t* orphaned = lv_obj_create(test_screen());
    { WeakWidget gone(orphaned); }
    lv_obj_delete(orphaned);
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget made during its widget's delete is empty",
                 "[widget_ref]") {
    struct Seen {
        WeakWidget made;
        bool fired = false;
    } seen;

    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            auto* s = static_cast<Seen*>(lv_event_get_user_data(e));
            s->made = lv_event_get_target_obj(e);
            s->fired = true;
        },
        LV_EVENT_DELETE, &seen);

    lv_obj_delete(obj);

    REQUIRE(seen.fired);
    CHECK(seen.made.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget made off the LVGL thread is empty and hooks nothing",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);

    WeakWidget made;
    std::thread([&] { made = WeakWidget(obj); }).join();

    CHECK(made.get() == nullptr);
    CHECK(lv_obj_get_event_count(obj) == base);
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "WeakWidget copied and destroyed off the LVGL thread stays good",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    WeakWidget weak(obj);

    WeakWidget back;
    std::thread([&] {
        WeakWidget local = weak;
        back = std::move(local);
    }).join();

    CHECK(back.get() == obj);
    lv_obj_delete(obj);
    CHECK(back.get() == nullptr);
}

// ============================================================================
// Shared helpers take handles, so their callers need no check
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete on a handle deletes it if live and lets go",
                 "[widget_ref]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    WidgetRef dead_ref(lv_obj_create(parent));
    WeakWidget dead_weak(lv_obj_create(parent));
    lv_obj_delete(parent);
    CHECK_FALSE(helix::ui::safe_delete(dead_ref));
    CHECK_FALSE(helix::ui::safe_delete(dead_weak));

    WidgetRef ref(lv_obj_create(test_screen()));
    WeakWidget weak(lv_obj_create(test_screen()));
    const WeakWidget ref_watch(ref.get());
    const WeakWidget weak_watch = weak;
    CHECK(helix::ui::safe_delete(ref));
    CHECK(helix::ui::safe_delete(weak));
    CHECK(ref.get() == nullptr);
    CHECK(weak.get() == nullptr);
    CHECK(ref_watch.get() == nullptr);
    CHECK(weak_watch.get() == nullptr);

    WeakWidget deferred(lv_obj_create(test_screen()));
    const WeakWidget deferred_watch = deferred;
    helix::ui::safe_delete_deferred(deferred);
    CHECK(deferred.get() == nullptr);
    REQUIRE(deferred_watch.get() != nullptr); // alive and hidden until the async tick
    CHECK(lv_obj_has_flag(deferred_watch, LV_OBJ_FLAG_HIDDEN));
    process_lvgl(20);
    CHECK(deferred_watch.get() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Widget-guarded updates run for a live widget, skip a dead one",
                 "[widget_ref]") {
    lv_obj_t* live = lv_obj_create(test_screen());
    lv_obj_t* dead = lv_obj_create(test_screen());
    lv_obj_t* got = nullptr;
    bool dead_ran = false;
    auto data = std::make_shared<int>(1);

    helix::ui::queue_widget_update(live, [&](lv_obj_t* w) { got = w; });
    helix::ui::queue_widget_update(WeakWidget(dead), [&](lv_obj_t*) { dead_ran = true; });
    helix::ui::queue_update<std::shared_ptr<int>>(
        dead, std::make_unique<std::shared_ptr<int>>(data),
        [&](lv_obj_t*, std::shared_ptr<int>*) { dead_ran = true; });
    lv_obj_delete(dead);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(got == live);
    CHECK_FALSE(dead_ran);
    CHECK(data.use_count() == 1); // the queued data was freed with the skipped update
}

TEST_CASE_METHOD(LVGLTestFixture, "queue_widget_update off the LVGL thread needs a handle",
                 "[widget_ref]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t base = lv_obj_get_event_count(obj);
    const WeakWidget weak(obj);
    bool raw_ran = false;
    bool handle_ran = false;

    std::thread([&] {
        helix::ui::queue_widget_update(obj, [&](lv_obj_t*) { raw_ran = true; });
        helix::ui::queue_widget_update(weak, [&](lv_obj_t*) { handle_ran = true; });
    }).join();
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(raw_ran); // the raw pointer could not be hooked off-thread
    CHECK(handle_ran);
    CHECK(lv_obj_get_event_count(obj) == base + 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "A handle dead by then reaches the helpers taking lv_obj_t*",
                 "[widget_ref]") {
    int fired = 0;
    const auto cb = [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); };
    WidgetRef ref(lv_obj_create(test_screen()));
    lv_obj_add_event_cb(ref, cb, LV_EVENT_VALUE_CHANGED, &fired);

    helix::ui::remove_event_cb_if_alive(ref, cb, &fired);
    lv_obj_send_event(ref, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(fired == 0); // live: removed

    lv_obj_delete(ref);
    helix::ui::remove_event_cb_if_alive(ref, cb, &fired); // dead: reads nullptr
    helix::ui::safe_delete_subtree(ref);
    helix::ui::safe_delete_deferred_raw(ref);
    CHECK_FALSE(helix::ui::is_on_active_screen(ref));
}
