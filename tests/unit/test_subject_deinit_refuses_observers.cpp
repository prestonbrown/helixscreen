// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_deinit_refuses_observers.cpp
 * @brief A deinitialized subject refuses observers until it is initialized again.
 *
 * lv_subject_deinit() empties the observer list but leaves the subject's type set,
 * and every lv_subject_init_*() starts from a fresh list without freeing the old
 * one. An observer attached in between would be orphaned by the next init, along
 * with the context its user_data owns (prestonbrown/helixscreen#1279). The refusal
 * is logged, so an attach in that window is visible rather than silent.
 */

#include "ui_observer_guard.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "core/observer_factory.h"
#include "lvgl_log_handler.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

int g_refusal_lines = 0;

void refusal_log_cb(lv_log_level_t level, const char* buf) {
    if (level == LV_LOG_LEVEL_WARN &&
        std::string(buf).find("Subject is deinitialized; observer refused") != std::string::npos) {
        ++g_refusal_lines;
    }
}

// Registered for the test body only; nullptr restores LVGL's default print path.
class ScopedRefusalCounter {
  public:
    ScopedRefusalCounter() {
        g_refusal_lines = 0;
        lv_log_register_print_cb(refusal_log_cb);
    }
    ~ScopedRefusalCounter() {
        lv_log_register_print_cb(nullptr);
    }
    ScopedRefusalCounter(const ScopedRefusalCounter&) = delete;
    ScopedRefusalCounter& operator=(const ScopedRefusalCounter&) = delete;

    static int count() {
        return g_refusal_lines;
    }
};

int g_raw_calls = 0;
void counting_cb(lv_observer_t* /*observer*/, lv_subject_t* /*subject*/) {
    ++g_raw_calls;
}

struct Owner {
    int notifications = 0;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Deinitialized subject refuses lv_subject_add_observer",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 7);
    lv_subject_deinit(&subject);

    ScopedRefusalCounter refusals;
    g_raw_calls = 0;
    lv_observer_t* observer = lv_subject_add_observer(&subject, counting_cb, nullptr);

    CHECK(observer == nullptr);
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);
    CHECK(g_raw_calls == 0);
    CHECK(ScopedRefusalCounter::count() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "Deinitialized subject refuses a widget-bound observer",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    lv_subject_deinit(&subject);

    lv_obj_t* obj = lv_obj_create(test_screen());
    const uint32_t events_before = lv_obj_get_event_count(obj);

    ScopedRefusalCounter refusals;
    lv_observer_t* observer = lv_subject_add_observer_obj(&subject, counting_cb, obj, nullptr);

    CHECK(observer == nullptr);
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);
    // A refused bind must not leave an unsubscribe-on-delete callback on the widget.
    CHECK(lv_obj_get_event_count(obj) == events_before);
    CHECK(ScopedRefusalCounter::count() == 1);

    lv_obj_delete(obj);
}

TEST_CASE_METHOD(LVGLTestFixture, "observe<int> on a deinitialized subject returns an empty guard",
                 "[observer][subject_lifetime][factory][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 3);
    lv_subject_deinit(&subject);

    Owner owner;
    ScopedRefusalCounter refusals;
    ObserverGuard guard = observe<int>(
        &subject, &owner, [](Owner* o, int /*v*/) { o->notifications++; }, subject_never_freed(),
        Dispatch::Immediate);

    CHECK_FALSE(guard);
    CHECK(owner.notifications == 0);
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);
    CHECK(ScopedRefusalCounter::count() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "Re-initializing a deinitialized subject accepts observers again",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    lv_subject_deinit(&subject);

    SECTION("int") {
        lv_subject_init_int(&subject, 5);
    }
    SECTION("string") {
        static char buf[16];
        static char prev[16];
        lv_subject_init_string(&subject, buf, prev, sizeof(buf), "x");
    }
    SECTION("pointer") {
        lv_subject_init_pointer(&subject, nullptr);
    }

    g_raw_calls = 0;
    lv_observer_t* observer = lv_subject_add_observer(&subject, counting_cb, nullptr);
    REQUIRE(observer != nullptr);
    CHECK(g_raw_calls == 1);
    CHECK(lv_ll_get_len(&subject.subs_ll) == 1);

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "Re-initializing a deinitialized group subject accepts observers",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t a;
    lv_subject_t b;
    lv_subject_init_int(&a, 1);
    lv_subject_init_int(&b, 2);
    lv_subject_t* members[] = {&a, &b};

    lv_subject_t group;
    lv_subject_init_group(&group, members, 2);
    lv_subject_deinit(&group);
    REQUIRE(lv_subject_add_observer(&group, counting_cb, nullptr) == nullptr);

    lv_subject_init_group(&group, members, 2);
    g_raw_calls = 0;
    lv_observer_t* observer = lv_subject_add_observer(&group, counting_cb, nullptr);
    REQUIRE(observer != nullptr);
    CHECK(g_raw_calls == 1);

    lv_subject_deinit(&group);
    lv_subject_deinit(&a);
    lv_subject_deinit(&b);
}

TEST_CASE_METHOD(LVGLTestFixture, "A deinitialized subject still reads and writes its value",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 11);
    lv_subject_deinit(&subject);

    CHECK(lv_subject_get_int(&subject) == 11);
    lv_subject_set_int(&subject, 12);
    CHECK(lv_subject_get_int(&subject) == 12);
}

// The binders own heap user data they hand to the observer; a refused attach must
// leave no NULL dereference, no applied style and no allocation behind.
TEST_CASE_METHOD(LVGLTestFixture, "Style and span binders survive a deinitialized subject",
                 "[observer][subject_lifetime][1279]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 1);
    lv_subject_deinit(&subject);

    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_opa(&style, 77);
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != 77);

    SECTION("lv_obj_bind_style") {
        CHECK(lv_obj_bind_style(obj, &style, LV_PART_MAIN, &subject, 1) == nullptr);
        CHECK(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != 77);
    }
    SECTION("lv_obj_bind_style_prop") {
        CHECK(lv_obj_bind_style_prop(obj, LV_STYLE_BG_OPA, LV_PART_MAIN, &subject) == nullptr);
    }
    SECTION("lv_obj_bind_flag_if_eq") {
        CHECK(lv_obj_bind_flag_if_eq(obj, &subject, LV_OBJ_FLAG_HIDDEN, 1) == nullptr);
        CHECK_FALSE(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("lv_obj_bind_state_if_eq") {
        CHECK(lv_obj_bind_state_if_eq(obj, &subject, LV_STATE_DISABLED, 1) == nullptr);
        CHECK_FALSE(lv_obj_has_state(obj, LV_STATE_DISABLED));
    }
    SECTION("lv_obj_bind_checked") {
        const uint32_t events_before = lv_obj_get_event_count(obj);
        CHECK(lv_obj_bind_checked(obj, &subject) == nullptr);
        // No write-back handler into a subject the widget does not observe.
        CHECK(lv_obj_get_event_count(obj) == events_before);
    }
    SECTION("lv_spangroup_bind_span_text") {
        lv_obj_t* group = lv_spangroup_create(test_screen());
        lv_span_t* span = lv_spangroup_add_span(group);
        CHECK(lv_spangroup_bind_span_text(group, span, &subject, "%d") == nullptr);
        lv_obj_delete(group);
    }
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);

    lv_obj_delete(obj);
    lv_style_reset(&style);
}

namespace {

constexpr const char* kBindStyleXml = R"(<component>
  <styles>
    <style name="deinit_probe_style" bg_opa="77"/>
  </styles>
  <view name="root" extends="lv_obj">
    <lv_obj name="target">
      <bind_style_if_eq name="deinit_probe_style" subject="deinit_probe_subject" ref_value="0"/>
    </lv_obj>
  </view>
</component>)";

lv_subject_t& deinit_probe_subject() {
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        lv_subject_init_int(&subject, 0);
        lv_xml_register_subject(nullptr, "deinit_probe_subject", &subject);
        registered = true;
    }
    return subject;
}

lv_obj_t* create_bind_style_probe(lv_obj_t* parent) {
    static bool registered = false;
    if (!registered) {
        REQUIRE(lv_xml_register_component_from_data("deinit_bind_style_probe", kBindStyleXml) ==
                LV_RESULT_OK);
        registered = true;
    }
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(parent, "deinit_bind_style_probe", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* target = lv_obj_find_by_name(root, "target");
    REQUIRE(target != nullptr);
    return target;
}

} // namespace

TEST_CASE("XML bind_style_if_eq on a deinitialized subject leaves the style off",
          "[observer][subject_lifetime][xml][1279]") {
    XMLTestFixture fixture;
    lv_subject_t& subject = deinit_probe_subject();

    // The binding applies the style while its subject is live, so the refusal case
    // below is the one that decides the outcome.
    lv_subject_init_int(&subject, 0);
    lv_obj_t* live = create_bind_style_probe(fixture.test_screen());
    REQUIRE(lv_obj_get_style_bg_opa(live, LV_PART_MAIN) == 77);

    lv_subject_deinit(&subject);
    lv_obj_t* refused = create_bind_style_probe(fixture.test_screen());
    CHECK(lv_obj_get_style_bg_opa(refused, LV_PART_MAIN) != 77);
    CHECK(lv_ll_get_len(&subject.subs_ll) == 0);

    lv_subject_init_int(&subject, 0);
}

TEST_CASE("A refused observer maps to one anomaly code whatever subject it names",
          "[observer][subject_lifetime][logging][1279]") {
    using helix::logging::anomaly_code_for_warning;
    const char* a = anomaly_code_for_warning(
        "lv_subject_add_observer_obj: Subject is deinitialized; observer refused "
        "(ptr=0x64ecf0c754d0, type=2) lv_observer.c:514");
    const char* b = anomaly_code_for_warning(
        "lv_subject_add_observer_with_target: Subject is deinitialized; observer refused "
        "(ptr=0x7f00000010, type=3) lv_observer.c:556");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(std::string(a) == "observer_refused_deinit_subject");
    CHECK(std::string(b) == "observer_refused_deinit_subject");
    CHECK(anomaly_code_for_warning("lv_subject_add_observer_obj: Subject not initialized yet") ==
          nullptr);
}
