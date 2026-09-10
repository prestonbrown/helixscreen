// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * theme_manager_init() repeat guard. The registration pass re-reads and
 * expat-parses the whole ui_xml/ tree, and the test suite paid that cost once
 * per fixture instance (once per SECTION leaf) for a theme that never changed
 * between them — ~26% of main-thread time. An unchanged target (same display
 * pointer, resolution and mode, theme still initialized) now skips the pass.
 */

#include "ui_breakpoint.h"

#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "theme_manager_init skips repeats for an unchanged target",
                 "[1526][theme]") {
    const int before = theme_manager_full_init_count();

    // The fixture's own init_theme() ran as part of construction; a manual
    // repeat for the same display/mode must not rebuild.
    theme_manager_init(lv_display_get_default(), false);
    REQUIRE(theme_manager_full_init_count() == before);

    // A second identical repeat is also a no-op.
    theme_manager_init(lv_display_get_default(), false);
    REQUIRE(theme_manager_full_init_count() == before);

    // A mode change is a real change.
    theme_manager_init(lv_display_get_default(), true);
    REQUIRE(theme_manager_full_init_count() == before + 1);

    // And so is flipping back — this is also what restores the light mode the
    // rest of the fixture expects.
    theme_manager_init(lv_display_get_default(), false);
    REQUIRE(theme_manager_full_init_count() == before + 2);
}

/**
 * The skip path owns the responsive state too.
 *
 * ui_breakpoint, ui_breakpoint_v and ui_is_portrait are process-global, and
 * helix::widget_size::current_breakpoint() reads ui_breakpoint rather than the
 * display — so every widget size predicate answers from whatever that subject
 * holds. theme_manager_init() is the boundary that puts the responsive state
 * back in line with the display, and a repeat that skips the registration pass
 * still has to do that: a value left moved by an earlier caller would otherwise
 * persist for the rest of the process and decide layout for panels that never
 * touched it.
 *
 * The leak this reproduces is the shape a test leaves behind when it resizes
 * the display, refreshes to pick up the new tier, and restores only the
 * resolution: ScopedResolution puts the pixels back, nothing puts the subjects
 * back.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "a skipped repeat still reseeds the responsive state",
                 "[1526][theme]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    lv_subject_t* bp_v = lv_xml_get_subject(nullptr, "ui_breakpoint_v");
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(bp != nullptr);
    REQUIRE(bp_v != nullptr);
    REQUIRE(portrait != nullptr);

    const int32_t want_bp = lv_subject_get_int(bp);
    const int32_t want_bp_v = lv_subject_get_int(bp_v);
    const int32_t want_portrait = lv_subject_get_int(portrait);

    {
        // Refreshing at a taller, narrower panel moves all three; leaving the
        // scope restores the resolution and none of them.
        ScopedResolution tall(disp, 1080, 1920);
        theme_manager_refresh_layout_constants(disp);
    }
    // The tier change fired observers that queue through UpdateQueue; let them
    // run against the widgets they were queued for rather than at teardown.
    helix::ui::UpdateQueue::instance().drain();

    // Precondition: the leak really happened. Without this the reseed assertions
    // below would pass against a subject that never moved.
    REQUIRE(lv_subject_get_int(bp) != want_bp);
    REQUIRE(lv_subject_get_int(portrait) != want_portrait);

    const int before = theme_manager_full_init_count();
    theme_manager_init(disp, false);

    // Precondition: the guard really did skip. If a future change makes this a
    // full rebuild, the reseed below would pass for the wrong reason.
    REQUIRE(theme_manager_full_init_count() == before);

    CHECK(lv_subject_get_int(bp) == want_bp);
    CHECK(lv_subject_get_int(bp_v) == want_bp_v);
    CHECK(lv_subject_get_int(portrait) == want_portrait);

    helix::ui::UpdateQueue::instance().drain();
}

/**
 * Applying a theme invalidates the guard.
 *
 * The repeat guard keys on the display, its resolution and the mode — none of
 * which name the theme. theme_manager_apply_theme() replaces the active theme
 * and the effective mode in place, so without an explicit invalidation the next
 * theme_manager_init() would be free to skip its registration pass and keep
 * serving what was applied, instead of reloading the configured theme.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "applying a theme forces the next init to rebuild",
                 "[1526][theme]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Baseline: an untouched repeat skips.
    const int before = theme_manager_full_init_count();
    theme_manager_init(disp, false);
    REQUIRE(theme_manager_full_init_count() == before);

    // Re-applying the theme already in force is still an apply: it goes through
    // the same in-place replacement, so it must still invalidate.
    theme_manager_apply_theme(theme_manager_get_active_theme(), false);
    helix::ui::UpdateQueue::instance().drain();

    theme_manager_init(disp, false);
    CHECK(theme_manager_full_init_count() == before + 1);

    // And the guard re-arms rather than staying off.
    theme_manager_init(disp, false);
    CHECK(theme_manager_full_init_count() == before + 1);

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a deinitialized theme runs the full init again",
                 "[1526][theme]") {
    const int before = theme_manager_full_init_count();

    theme_manager_deinit();
    theme_manager_init(lv_display_get_default(), false);
    REQUIRE(theme_manager_full_init_count() == before + 1);

    // Deinit cleared the subject-initialized flag, so the NEXT repeat skips
    // again — the recovery is one rebuild, not a permanently disabled guard.
    theme_manager_init(lv_display_get_default(), false);
    REQUIRE(theme_manager_full_init_count() == before + 1);
}
