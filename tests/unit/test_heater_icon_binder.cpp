// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_heater_config.h"
#include "ui_heater_icon_binder.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::HeaterType;
using helix::PrinterState;
using helix::ui::HeaterIconBinder;
using helix::ui::UpdateQueue;

TEST_CASE("HeaterIconBinder: conventional glyph names per heater", "[heater_binder]") {
    REQUIRE(std::string(HeaterIconBinder::default_icon_name(HeaterType::Nozzle)) ==
            "nozzle_icon_glyph");
    REQUIRE(std::string(HeaterIconBinder::default_icon_name(HeaterType::Bed)) == "bed_icon_glyph");
    REQUIRE(std::string(HeaterIconBinder::default_icon_name(HeaterType::Chamber)) ==
            "chamber_icon_glyph");
}

TEST_CASE("HeaterIconBinder: starts unbound", "[heater_binder]") {
    HeaterIconBinder binder;
    REQUIRE_FALSE(binder.is_bound());
}

TEST_CASE("HeaterIconBinder: binding a null root is a safe no-op", "[heater_binder]") {
    HeaterIconBinder binder;
    REQUIRE_FALSE(binder.bind_subjects(nullptr, "bed_icon_glyph", nullptr, nullptr));
    REQUIRE_FALSE(binder.is_bound());
}

TEST_CASE("HeaterIconBinder: unbind on an unbound binder is safe", "[heater_binder]") {
    HeaterIconBinder binder;
    binder.unbind();
    binder.unbind();
    REQUIRE_FALSE(binder.is_bound());
}

// Before this, bind_subjects() returned true whenever the icon was found, even
// if BOTH subject pointers were null — the icon would then freeze forever at
// classify(250, 0) with no observers and no diagnostic. It must now refuse to
// bind and report false so a caller can notice the icon never tints.
TEST_CASE("HeaterIconBinder: bind_subjects refuses to bind when both subjects are null",
          "[heater_binder]") {
    LVGLTestFixture fixture;

    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_t* icon = lv_label_create(root);
    lv_obj_set_name(icon, "bed_icon_glyph");

    HeaterIconBinder binder;
    REQUIRE_FALSE(binder.bind_subjects(root, "bed_icon_glyph", nullptr, nullptr));
    REQUIRE_FALSE(binder.is_bound());
}

// One real subject is enough to be useful (e.g. a sensor-only display with no
// controllable target) — only the BOTH-null case is refused.
TEST_CASE("HeaterIconBinder: bind_subjects still binds when only one subject is non-null",
          "[heater_binder]") {
    LVGLTestFixture fixture;

    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_t* icon = lv_label_create(root);
    lv_obj_set_name(icon, "bed_icon_glyph");

    lv_subject_t current;
    lv_subject_init_int(&current, 250);

    HeaterIconBinder binder;
    REQUIRE(binder.bind_subjects(root, "bed_icon_glyph", &current, nullptr));
    REQUIRE(binder.is_bound());

    binder.unbind();
}

// ============================================================================
// Coverage below exercises a real bind() against a real icon widget and a real
// PrinterState — the path every Task 5-7 call site depends on. The four cases
// above would all still pass against a binder whose bind() silently did
// nothing: none of them ever calls bind() itself.
// ============================================================================

namespace {

// Mirrors the shape bind() looks for: a container with a single child named
// after the heater's conventional icon glyph.
lv_obj_t* create_icon_root(lv_obj_t* parent, const char* icon_name) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_t* icon = lv_obj_create(root);
    lv_obj_set_name(icon, icon_name);
    return root;
}

// The animator applies its thermal-state color via ui_icon_set_color(), which
// is plain lv_obj_set_style_text_color(). Reading it back is a black-box way
// to observe whether HeaterIconBinder actually reached animator_.update(),
// without reaching into its private animator_ member.
lv_color_t icon_text_color(lv_obj_t* icon) {
    return lv_obj_get_style_text_color(icon, LV_PART_MAIN);
}

// The animator's own tolerance (TEMP_TOLERANCE in ui_heating_animator.cpp) is
// 20 decidegrees = 2 degrees. Hardcoded here (matching the convention already
// used in test_heating_animator_state.cpp) so the expected color is derived
// independently rather than importing the private constant.
constexpr int TOLERANCE_DECI = 20;

lv_color_t expected_color(int current_deci, int target_deci) {
    return helix::ui::temperature::get_heating_state_color(current_deci, target_deci,
                                                           TOLERANCE_DECI);
}

} // namespace

// Uses XMLTestFixture (not the lighter LVGLTestFixture) specifically because
// its one-time global setup initializes the theme (globals.xml color
// tokens) — without it, theme_manager_get_color() falls back to black for
// every token, and Off vs Heating colors would be indistinguishable, making
// the color assertions below vacuous.

TEST_CASE_METHOD(XMLTestFixture,
                 "HeaterIconBinder: bind() succeeds against a real icon, per heater type",
                 "[heater_binder]") {
    SECTION("nozzle") {
        const char* name = HeaterIconBinder::default_icon_name(HeaterType::Nozzle);
        lv_obj_t* root = create_icon_root(test_screen(), name);
        lv_obj_t* icon = lv_obj_find_by_name(root, name);
        REQUIRE(icon != nullptr);

        lv_subject_set_int(state().get_active_extruder_temp_subject(), 250);
        lv_subject_set_int(state().get_active_extruder_target_subject(), 2000);

        HeaterIconBinder binder;
        REQUIRE(binder.bind(root, state(), HeaterType::Nozzle));
        REQUIRE(binder.is_bound());
        REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 2000)));

        binder.unbind();
    }

    SECTION("bed") {
        const char* name = HeaterIconBinder::default_icon_name(HeaterType::Bed);
        lv_obj_t* root = create_icon_root(test_screen(), name);
        lv_obj_t* icon = lv_obj_find_by_name(root, name);
        REQUIRE(icon != nullptr);

        lv_subject_set_int(state().get_bed_temp_subject(), 250);
        lv_subject_set_int(state().get_bed_target_subject(), 2000);

        HeaterIconBinder binder;
        REQUIRE(binder.bind(root, state(), HeaterType::Bed));
        REQUIRE(binder.is_bound());
        REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 2000)));

        binder.unbind();
    }

    SECTION("chamber") {
        const char* name = HeaterIconBinder::default_icon_name(HeaterType::Chamber);
        lv_obj_t* root = create_icon_root(test_screen(), name);
        lv_obj_t* icon = lv_obj_find_by_name(root, name);
        REQUIRE(icon != nullptr);

        lv_subject_set_int(state().get_chamber_temp_subject(), 250);
        lv_subject_set_int(state().get_chamber_effective_target_subject(), 2000);

        HeaterIconBinder binder;
        REQUIRE(binder.bind(root, state(), HeaterType::Chamber));
        REQUIRE(binder.is_bound());
        REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 2000)));

        binder.unbind();
    }
}

TEST_CASE_METHOD(XMLTestFixture, "HeaterIconBinder: is_bound() toggles around a real bind/unbind",
                 "[heater_binder]") {
    const char* name = HeaterIconBinder::default_icon_name(HeaterType::Bed);
    lv_obj_t* root = create_icon_root(test_screen(), name);

    HeaterIconBinder binder;
    REQUIRE_FALSE(binder.is_bound());

    REQUIRE(binder.bind(root, state(), HeaterType::Bed));
    REQUIRE(binder.is_bound());

    binder.unbind();
    REQUIRE_FALSE(binder.is_bound());
}

TEST_CASE_METHOD(
    XMLTestFixture,
    "HeaterIconBinder: a subject change after bind() reaches the icon through refresh()",
    "[heater_binder]") {
    const char* name = HeaterIconBinder::default_icon_name(HeaterType::Nozzle);
    lv_obj_t* root = create_icon_root(test_screen(), name);
    lv_obj_t* icon = lv_obj_find_by_name(root, name);
    REQUIRE(icon != nullptr);

    lv_subject_t* current = state().get_active_extruder_temp_subject();
    lv_subject_t* target = state().get_active_extruder_target_subject();
    lv_subject_set_int(current, 250);
    lv_subject_set_int(target, 0); // Off

    HeaterIconBinder binder;
    REQUIRE(binder.bind(root, state(), HeaterType::Nozzle));
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 0)));

    // observe_int_sync() defers its handler through queue_update() — the icon
    // must NOT change before a drain.
    lv_subject_set_int(target, 2000); // Heating
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 0)));

    UpdateQueue::instance().drain();
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 2000)));
    REQUIRE_FALSE(lv_color_eq(icon_text_color(icon), expected_color(250, 0)));

    binder.unbind();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "HeaterIconBinder: bind() on an already-bound binder rebinds to the new icon",
                 "[heater_binder]") {
    const char* name = HeaterIconBinder::default_icon_name(HeaterType::Bed);
    lv_obj_t* root_a = create_icon_root(test_screen(), name);
    lv_obj_t* icon_a = lv_obj_find_by_name(root_a, name);
    lv_obj_t* root_b = create_icon_root(test_screen(), name);
    lv_obj_t* icon_b = lv_obj_find_by_name(root_b, name);
    REQUIRE(icon_a != nullptr);
    REQUIRE(icon_b != nullptr);

    lv_subject_t* current = state().get_bed_temp_subject();
    lv_subject_t* target = state().get_bed_target_subject();
    lv_subject_set_int(current, 250);
    lv_subject_set_int(target, 0); // Off

    HeaterIconBinder binder;
    REQUIRE(binder.bind(root_a, state(), HeaterType::Bed));
    REQUIRE(lv_color_eq(icon_text_color(icon_a), expected_color(250, 0)));
    lv_color_t icon_a_color_before_rebind = icon_text_color(icon_a);

    // Re-bind to a different icon without an explicit unbind() in between —
    // bind() must detach from icon_a and attach to icon_b cleanly (it calls
    // unbind() itself first; this proves that path works end to end, not just
    // double-unbind()).
    REQUIRE(binder.bind(root_b, state(), HeaterType::Bed));
    REQUIRE(binder.is_bound());
    REQUIRE(lv_color_eq(icon_text_color(icon_b), expected_color(250, 0)));

    // A subject change now must reach icon_b only — icon_a is detached and
    // must be left exactly as bind() left it, not still tracking.
    lv_subject_set_int(target, 2000); // Heating
    UpdateQueue::instance().drain();

    REQUIRE(lv_color_eq(icon_text_color(icon_b), expected_color(250, 2000)));
    REQUIRE(lv_color_eq(icon_text_color(icon_a), icon_a_color_before_rebind));
    REQUIRE_FALSE(lv_color_eq(icon_text_color(icon_a), expected_color(250, 2000)));

    binder.unbind();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "HeaterIconBinder: chamber binds the effective target, not the raw heater target",
                 "[heater_binder]") {
    const char* name = HeaterIconBinder::default_icon_name(HeaterType::Chamber);
    lv_obj_t* root = create_icon_root(test_screen(), name);
    lv_obj_t* icon = lv_obj_find_by_name(root, name);
    REQUIRE(icon != nullptr);

    lv_subject_set_int(state().get_chamber_temp_subject(), 250);
    lv_subject_set_int(state().get_chamber_target_subject(), 0);
    lv_subject_set_int(state().get_chamber_effective_target_subject(), 0);

    HeaterIconBinder binder;
    REQUIRE(binder.bind(root, state(), HeaterType::Chamber));
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 0))); // Off

    // Maintaining mode: the heater target stays 0 (the real setpoint is
    // parked on the cooling fan), so the raw chamber_target subject is a red
    // herring here. If bind() ever observed get_chamber_target_subject()
    // instead of get_chamber_effective_target_subject(), this change would
    // (wrongly) leave the icon looking Off.
    lv_subject_set_int(state().get_chamber_target_subject(), 2000);
    UpdateQueue::instance().drain();
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 0))); // still Off — unobserved

    // The effective target is the one that must actually drive the icon.
    lv_subject_set_int(state().get_chamber_effective_target_subject(), 2000);
    UpdateQueue::instance().drain();
    REQUIRE(lv_color_eq(icon_text_color(icon), expected_color(250, 2000))); // Heating

    binder.unbind();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "HeaterIconBinder: bed/chamber SubjectLifetime tokens survive subject teardown",
                 "[heater_binder]") {
    // bind() on Bed/Chamber must pass its SubjectLifetime token into
    // observe_int_sync() (CLAUDE.md invariant #4, issue #705): if it doesn't,
    // the ObserverGuard has no way to know the subject died, and reset()
    // below would call lv_observer_remove() on memory that
    // PrinterTemperatureState::deinit_subjects() already freed via
    // lv_subject_deinit(). This is the same bool-token path covered for a
    // synthetic subject in test_observer_cleanup_ordering.cpp's #816 tests —
    // here it runs against the real PrinterState accessors bind() uses.
    //
    // ObserverGuard::invalidate_all() is NOT involved: that is a separate,
    // Application-only teardown signal (see application.cpp), never called by
    // PrinterState::deinit_subjects() or any test fixture. So this exercises
    // the SubjectLifetime bool path specifically, not the invalidation-epoch
    // shortcut that would mask a missing lifetime token.
    lv_obj_t* bed_root =
        create_icon_root(test_screen(), HeaterIconBinder::default_icon_name(HeaterType::Bed));
    lv_obj_t* chamber_root =
        create_icon_root(test_screen(), HeaterIconBinder::default_icon_name(HeaterType::Chamber));

    HeaterIconBinder bed_binder;
    HeaterIconBinder chamber_binder;
    REQUIRE(bed_binder.bind(bed_root, state(), HeaterType::Bed));
    REQUIRE(chamber_binder.bind(chamber_root, state(), HeaterType::Chamber));
    REQUIRE(bed_binder.is_bound());
    REQUIRE(chamber_binder.is_bound());

    // Simulate a printer-state reinit cycle: signals subject death (each
    // lifetime bool -> false) then frees the subjects via lv_subject_deinit().
    state().deinit_subjects();

    // Must not crash.
    bed_binder.unbind();
    chamber_binder.unbind();
    REQUIRE_FALSE(bed_binder.is_bound());
    REQUIRE_FALSE(chamber_binder.is_bound());
}

// The hazard these tests exist to catch is NOT a single PrintStatusWidget
// instance's sequential attach -> detach -> attach recycle. That path is
// already safe: PrintStatusWidget::detach() always runs synchronously on the
// SAME instance before its own re-attach(), and each of its three
// HeaterIconBinder members is a plain by-value member of that instance (never
// on the shared/refcounted DetailedFormatter/s_formatter_, per
// print_status_widget.h) — so there is nothing for a "handover" to race
// against.
//
// The real hazard is MULTIPLE CONCURRENT PrintStatusWidget instances: the
// panel manager keeps up to one live instance per breakpoint variant, and all
// of them share a single DetailedFormatter (s_formatter_) via refcounting. If
// the three binders were ever moved onto that shared formatter instead of
// staying per-instance members, two live instances calling
// bind()/bind_subjects() would be racing to rebind the SAME HeaterIconBinder.
// bind()/bind_subjects() unconditionally call unbind() first
// (ui_heater_icon_binder.cpp:29-30, 63-65), so this would never crash — it
// would go silent: the second instance's bind() steals the animator away from
// the first, and the first instance's icon simply freezes and stops tracking
// temperature changes. The test below reproduces exactly that steal with one
// binder shared across two roots, and asserts the frozen-not-crashed outcome.

TEST_CASE_METHOD(
    XMLTestFixture,
    "HeaterIconBinder: a binder shared across two roots silently steals from the first on rebind",
    "[heater_binder][rebuild]") {
    const char* name = HeaterIconBinder::default_icon_name(HeaterType::Bed);
    lv_obj_t* root_a = create_icon_root(test_screen(), name); // stand-in for instance A's DOM
    lv_obj_t* icon_a = lv_obj_find_by_name(root_a, name);
    lv_obj_t* root_b = create_icon_root(test_screen(), name); // stand-in for instance B's DOM
    lv_obj_t* icon_b = lv_obj_find_by_name(root_b, name);
    REQUIRE(icon_a != nullptr);
    REQUIRE(icon_b != nullptr);

    lv_subject_t* current = state().get_bed_temp_subject();
    lv_subject_t* target = state().get_bed_target_subject();
    lv_subject_set_int(current, 250);
    lv_subject_set_int(target, 0); // Off

    // One binder standing in for a mistakenly-shared member (e.g. living on
    // s_formatter_ instead of on each PrintStatusWidget instance).
    HeaterIconBinder shared_binder;
    REQUIRE(shared_binder.bind(root_a, state(), HeaterType::Bed));
    REQUIRE(lv_color_eq(icon_text_color(icon_a), expected_color(250, 0)));
    lv_color_t icon_a_color_at_steal = icon_text_color(icon_a);

    // Instance B attaches "concurrently" — in the real bug this is a second
    // live PrintStatusWidget's attach() calling bind() on the same shared
    // binder, not a sequential detach/re-attach of one instance.
    REQUIRE(shared_binder.bind(root_b, state(), HeaterType::Bed));
    REQUIRE(shared_binder.is_bound());

    // A temperature change now reaches icon_b only. icon_a is not crashed and
    // not use-after-freed — it is just silently frozen at whatever it last
    // showed, because the one animator that used to watch it now watches
    // icon_b instead.
    lv_subject_set_int(target, 2000); // Heating
    UpdateQueue::instance().drain();

    REQUIRE(lv_color_eq(icon_text_color(icon_b), expected_color(250, 2000)));
    REQUIRE(lv_color_eq(icon_text_color(icon_a), icon_a_color_at_steal));
    REQUIRE_FALSE(lv_color_eq(icon_text_color(icon_a), expected_color(250, 2000)));

    shared_binder.unbind();
}

// General widget-teardown safety, independent of the sharing hazard above:
// deleting the icon widget out from under a bound HeaterIconBinder — without
// ever calling unbind() — must not leave a dangling observer. The animator's
// own LV_EVENT_DELETE handler has to auto-detach so a later subject change
// cannot touch freed memory.
TEST_CASE("HeaterIconBinder: observers survive widget deletion without unbind",
          "[heater_binder][rebuild]") {
    LVGLTestFixture fixture;

    lv_subject_t current;
    lv_subject_t target;
    lv_subject_init_int(&current, 250);
    lv_subject_init_int(&target, 2000);

    lv_obj_t* root = lv_obj_create(lv_screen_active());
    lv_obj_t* icon = lv_label_create(root);
    lv_obj_set_name(icon, "bed_icon_glyph");

    HeaterIconBinder binder;
    REQUIRE(binder.bind_subjects(root, "bed_icon_glyph", &current, &target));

    // Deleting the widget without calling unbind() must auto-detach the animator
    // via LV_EVENT_DELETE. A later subject change must not touch freed memory.
    lv_obj_delete(root);
    REQUIRE_FALSE(binder.is_bound());
    lv_subject_set_int(&current, 1990);
    lv_subject_set_int(&target, 0);
    REQUIRE_FALSE(binder.is_bound());
}

// The icon carries the thermal state; the "Nozzle"/"Bed"/"Chamber" label next to
// it must stay in normal text color. They are siblings today, so nothing tints
// the label — this pins that. It breaks if someone later puts the icon and the
// label in a shared container and binds the animator to that container, because
// style_text_color would then inherit down onto the label.
//
// Uses XMLTestFixture (not LVGLTestFixture) so the theme is initialized —
// otherwise theme_manager_get_color() falls back to the same value for every
// token and a color-equality assertion would pass vacuously.

TEST_CASE_METHOD(XMLTestFixture,
                 "HeaterIconBinder: tinting an icon leaves a sibling label untouched",
                 "[heater_binder][label_neutrality]") {
    lv_obj_t* row = lv_obj_create(test_screen());
    lv_obj_t* icon = lv_label_create(row);
    lv_obj_set_name(icon, "bed_icon_glyph");
    lv_obj_t* label = lv_label_create(row);
    lv_obj_set_name(label, "bed_text_label");
    lv_label_set_text(label, "Bed");

    lv_color_t label_before = lv_obj_get_style_text_color(label, LV_PART_MAIN);

    lv_subject_t current;
    lv_subject_t target;
    lv_subject_init_int(&current, 250);
    lv_subject_init_int(&target, 0);

    HeaterIconBinder binder;
    REQUIRE(binder.bind_subjects(row, "bed_icon_glyph", &current, &target));

    // Drive through every thermal state; the label must never move.
    const int states[][2] = {{250, 0}, {250, 600}, {600, 600}, {900, 600}};
    for (const auto& s : states) {
        lv_subject_set_int(&current, s[0]);
        lv_subject_set_int(&target, s[1]);
        UpdateQueue::instance().drain();
        lv_color_t label_now = lv_obj_get_style_text_color(label, LV_PART_MAIN);
        REQUIRE(label_now.red == label_before.red);
        REQUIRE(label_now.green == label_before.green);
        REQUIRE(label_now.blue == label_before.blue);
    }

    binder.unbind();
}
