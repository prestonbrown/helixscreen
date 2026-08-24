// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_is_portrait_subject.cpp
 * @brief ui_is_portrait exposes orientation to XML so <if cond="ui_is_portrait eq 1">
 *        can branch layout inline instead of maintaining ui_xml/portrait/ variant files.
 *
 * ui_breakpoint classifies the cramped-axis *tier* and cannot answer "is this
 * portrait" -- 480x800 and 800x480 can land on the same tier. This subject is
 * the missing piece: 1 for any portrait class, 0 otherwise.
 *
 * Two sources, in priority order (#1255):
 *   1. LayoutManager::type() once initialized -- override-aware, so a --layout
 *      override and the XML that branches on ui_is_portrait agree.
 *   2. detect_layout_type() of the live display -- the startup seed and the
 *      fallback before LayoutManager (Phase 8b) is constructed.
 *
 * The name is pinned exactly -- "ui_is_portrait" is the API XML depends on, and
 * a typo there fails silently at the cond= site, the worst failure mode
 * available.
 */

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// Resets LayoutManager between tests so a prior init()/set_override() cannot
// leak into the fallback-path cases. Identical body to the friend class in
// test_layout_manager.cpp / test_grid_layout.cpp — Catch2 amalgamated builds
// compile each TU separately, no ODR conflict.
class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

namespace {

/// Bufferless display: the axis accessors only read the resolution.
lv_display_t* make_test_display(int32_t w, int32_t h) {
    return lv_display_create(w, h);
}

int subject_value(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

/// Puts the fixture's display geometry and LayoutManager back on the way out --
/// see the identical helper (and its comment) in
/// test_vertical_breakpoint_subject.cpp. theme_manager writes into a SHARED XML
/// scope, so leaving a test display registered leaks its geometry into every
/// later test in the binary; an init'd LayoutManager likewise leaks its type.
///
/// The constructor reset is just as important as the destructor one:
/// LayoutManager is a singleton shared across every test file in this binary,
/// and cases in test_layout_manager.cpp / test_grid_layout.cpp leave it init'd.
/// Without resetting on entry, the fallback-path tests see a stale type instead
/// of detect_layout_type().
struct RestoreDisplayConsts {
    lv_display_t* prev = lv_display_get_default();

    RestoreDisplayConsts() {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }

    ~RestoreDisplayConsts() {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        if (prev != nullptr) {
            lv_display_set_default(prev);
            theme_manager_refresh_layout_constants(prev);
        }
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ui_is_portrait is registered under its exact name",
                 "[theme][layout][is-portrait-subject]") {
    RestoreDisplayConsts restore;

    lv_subject_t* s = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(s != nullptr);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ui_is_portrait fallback: tracks detect_layout_type() before LayoutManager init",
                 "[theme][layout][is-portrait-subject]") {
    RestoreDisplayConsts restore;

    struct Case {
        int32_t w;
        int32_t h;
    };
    // A spread across landscape and portrait classes, including a pair that
    // shares a ui_breakpoint tier (800x480 vs 480x800) to prove this subject
    // answers a question ui_breakpoint cannot. LayoutManager is NOT init'd here,
    // so theme_manager_refresh_orientation() falls back to detect_layout_type().
    const Case cases[] = {
        {800, 480}, {480, 800}, {1024, 600}, {480, 272}, {272, 480}, {320, 1480}, {1480, 320},
    };

    for (const auto& c : cases) {
        INFO(c.w << "x" << c.h);
        lv_display_t* d = make_test_display(c.w, c.h);
        theme_manager_refresh_layout_constants(d);

        const int expected = helix::is_portrait_layout(helix::detect_layout_type(c.w, c.h)) ? 1 : 0;
        CHECK(subject_value("ui_is_portrait") == expected);

        lv_display_delete(d);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "800x480 and 480x800 disagree on ui_is_portrait",
                 "[theme][layout][is-portrait-subject]") {
    RestoreDisplayConsts restore;

    // The load-bearing assertion: ui_breakpoint alone cannot distinguish these
    // two geometries (same cramped-axis min(w,h)=480), but ui_is_portrait must.
    lv_display_t* landscape = make_test_display(800, 480);
    theme_manager_refresh_layout_constants(landscape);
    CHECK(subject_value("ui_is_portrait") == 0);
    lv_display_delete(landscape);

    lv_display_t* portrait = make_test_display(480, 800);
    theme_manager_refresh_layout_constants(portrait);
    CHECK(subject_value("ui_is_portrait") == 1);
    lv_display_delete(portrait);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "the STARTUP publish sets ui_is_portrait, not only the rotation path",
                 "[theme][layout][is-portrait-subject]") {
    // Regression guard mirroring test_vertical_breakpoint_subject.cpp's equivalent
    // section: every other case here drives theme_manager_refresh_layout_constants(),
    // so a bug that only wires the subject into the rotation path -- and skips
    // theme_manager_init() -- would pass every one of them. A display that is
    // never rotated (the common case on real hardware) would carry a stale or
    // never-set value for its whole session.
    RestoreDisplayConsts restore;

    lv_display_t* d = make_test_display(480, 800);
    theme_manager_init(d, theme_manager_is_dark_mode());

    CHECK(subject_value("ui_is_portrait") == 1);

    lv_display_delete(d);
}

TEST_CASE_METHOD(XMLTestFixture, "rotation republishes ui_is_portrait",
                 "[theme][layout][is-portrait-subject]") {
    // The failure this pins: updating ui_breakpoint/ui_breakpoint_v on rotation
    // but forgetting ui_is_portrait leaves the pre-rotation orientation latched.
    RestoreDisplayConsts restore;

    lv_display_t* tall = make_test_display(480, 800);
    theme_manager_refresh_layout_constants(tall);
    REQUIRE(subject_value("ui_is_portrait") == 1);
    lv_display_delete(tall);

    lv_display_t* wide = make_test_display(800, 480);
    theme_manager_refresh_layout_constants(wide);
    CHECK(subject_value("ui_is_portrait") == 0);
    lv_display_delete(wide);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ui_is_portrait follows LayoutManager override, not physical geometry",
                 "[theme][layout][is-portrait-subject][1255]") {
    // The #1255 contract: once LayoutManager is initialized, ui_is_portrait
    // tracks is_portrait_layout(LayoutManager::type()) so a --layout override
    // and the XML branching on ui_is_portrait agree. The divergence this fixes:
    // --layout=portrait on landscape hardware left C++ visual decisions seeing
    // portrait while XML stayed landscape.
    RestoreDisplayConsts restore;

    auto& lm = helix::LayoutManager::instance();

    // --layout=portrait on 800x480 (landscape) hardware.
    lm.set_override("portrait");
    lm.init(800, 480);
    REQUIRE(lm.type() == helix::LayoutType::PORTRAIT);

    lv_display_t* landscape = make_test_display(800, 480);
    theme_manager_refresh_orientation();
    CHECK(subject_value("ui_is_portrait") == 1);

    // The override wins even if a refresh runs with the physical display.
    theme_manager_refresh_layout_constants(landscape);
    CHECK(subject_value("ui_is_portrait") == 1);
    lv_display_delete(landscape);

    // --layout=standard (auto) on the same hardware snaps back to landscape.
    LayoutManagerTestAccess::reset(lm);
    lm.init(800, 480);
    REQUIRE(lm.type() == helix::LayoutType::STANDARD);

    lv_display_t* landscape2 = make_test_display(800, 480);
    theme_manager_refresh_orientation();
    CHECK(subject_value("ui_is_portrait") == 0);
    lv_display_delete(landscape2);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ui_is_portrait override is reactive across set_override + refresh",
                 "[theme][layout][is-portrait-subject][1255]") {
    // The override can change at runtime (config flip). A refresh must republish
    // the new orientation so <if cond="ui_is_portrait eq 1"> rebuilds.
    RestoreDisplayConsts restore;

    auto& lm = helix::LayoutManager::instance();

    // Landscape hardware: a portrait override must still win.
    lm.init(1024, 600);
    theme_manager_refresh_orientation();
    CHECK(subject_value("ui_is_portrait") == 0);

    // Flip to portrait override mid-session.
    lm.set_override("portrait");
    lm.init(1024, 600);
    theme_manager_refresh_orientation();
    CHECK(subject_value("ui_is_portrait") == 1);

    // And back.
    LayoutManagerTestAccess::reset(lm);
    lm.init(1024, 600);
    theme_manager_refresh_orientation();
    CHECK(subject_value("ui_is_portrait") == 0);
}
