// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_camera.cpp
 * @brief camera picks compact (icon-only) vs. live-stream layout from
 * physical pixels, not colspan/rowspan — and unlike every other migrated
 * widget, the transition is edge-triggered around a network resource: it
 * starts/stops an MJPEG stream on the compact<->non-compact boundary
 * (camera_widget.cpp on_size_changed(), :257-294).
 *
 * `compact_ = (width_px < w_normal())` — width alone decides. The live view is
 * scaled with LV_IMAGE_ALIGN_COVER, so it fills whatever cell it is given
 * either way; extra height never buys the widget new content the way a
 * resolved name or a second column would, so gating on height too would only
 * ever produce false results. Large and XLarge's single-row 1x1 cells (107px,
 * 141px / 134px, 169px) are the case that matters: their row height alone
 * clears what used to be the tall threshold, so a plain 1x1 widget there must
 * not read as big enough to open a stream nobody asked for. Width-only keeps
 * those compact without also dropping the stream on every 2-column cell whose
 * row happens to be short (Medium, Portrait) — the cases below pin both
 * sides.
 *
 * No network I/O happens in this file. CameraWidget::start_stream() builds a
 * CameraStream and calls CameraStream::configure_from_printer(), which reads
 * PrinterState's webcam stream/snapshot URLs and returns false when both are
 * empty (src/system/camera_stream.cpp:86-107) — start_stream() then resets
 * stream_ and returns *before* CameraStream::start() (the call that opens a
 * socket) is ever reached. This test actively zeroes the global PrinterState
 * singleton's webcam config at the top (get_printer_state() is process-wide
 * across the test binary, and MoonrakerClientMock::discover_printer() —
 * exercised by other test files sharing this shard — DOES set it), rather
 * than only asserting it happens to be empty already; see the zeroing step
 * below.
 *
 * start_stream()/stop_stream() are private and, with no camera configured,
 * their own interesting bodies (below the URL check) never run — so
 * "stream_ non-null" cannot tell a test whether the call happened and
 * no-op'd versus never happened. CameraWidgetTestAccess exposes call counts
 * instead: on_size_changed()'s stop_stream()/start_stream() invocations sit
 * behind guards (`!fullscreen_overlay_`, `active_`) strictly narrower than
 * the compact-transition conditions that reach them, so on_activate() is
 * called first (setting active_ = true) to put the start path on the same
 * footing as the stop path — otherwise the "leaving compact" branch's
 * start_stream() call would never execute at all, and the assertion would be
 * as vacuous as checking a proxy that no code touches.
 */

#include "lvgl.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

#if HELIX_HAS_CAMERA

#include "ui_breakpoint.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/camera_widget_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/camera_widget.h"
#include "theme_manager.h"

using namespace helix;
using namespace helix::widget_size;

// The display singleton is shared by every case in a shard, and the case
// below judges its resize ladder against the tier that display yields
// (w_normal() at the fixture's 800x480 is Medium's 174px; at this case's
// 1080x1920 the narrow axis is XXLarge's 309px, which swallows the 233px
// Medium cell below). This case moves the resolution and rotation, publishes
// the leaked geometry into the layout subjects the way an aborted resize
// scope leaves behind, and deliberately puts none of it back, then checks the
// reset the direct way: LVGLTestFixture::reclaim_display() is exactly what
// the next case's fixture constructor runs, called here as a plain static
// rather than by constructing a second fixture inside the live one (a nested
// fixture would tear the update queue down mid-case). A stray display holds
// the default slot at reclaim time, so the slot restore executes on every
// run of this case, not only in shards where a translation unit's static
// display happens to sit there. Pinning it in the same case keeps the oracle
// with the leak no matter where Catch2's shard boundaries fall.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "camera: a leaked display resolution does not reach the next case",
                 "[1563][widget_size][camera]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    lv_display_set_resolution(disp, 1080, 1920);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    // Publish the leaked geometry into the process-global layout state, as
    // an aborted resize scope leaves it.
    theme_manager_refresh_layout_constants(disp);

    lv_subject_t* const breakpoint = lv_xml_get_subject(nullptr, "ui_breakpoint");
    lv_subject_t* const breakpoint_v = lv_xml_get_subject(nullptr, "ui_breakpoint_v");
    REQUIRE(breakpoint != nullptr);
    REQUIRE(breakpoint_v != nullptr);
    // The leak landed in the derived subjects, not just the raw pixels.
    REQUIRE(lv_subject_get_int(breakpoint) == to_int(UiBreakpoint::XXLarge));

    // A stray display owning the default slot, as test translation units'
    // static initializers leave behind.
    lv_display_t* stray = lv_display_create(64, 64);
    REQUIRE(stray != nullptr);
    lv_display_set_default(stray);

    LVGLTestFixture::reclaim_display();

    lv_display_t* const d = lv_display_get_default();
    CHECK(d == LVGLTestFixture::s_display);
    CHECK(lv_display_get_rotation(d) == LV_DISPLAY_ROTATION_0);
    CHECK(lv_display_get_horizontal_resolution(d) == kTestDisplayWidth);
    CHECK(lv_display_get_vertical_resolution(d) == kTestDisplayHeight);
    CHECK(lv_subject_get_int(breakpoint) == to_int(UiBreakpoint::Medium));
    CHECK(lv_subject_get_int(breakpoint_v) == to_int(UiBreakpoint::Medium));
    // Consumer level: current_breakpoint() reads the subject rather than the
    // display, so a stale tier surfaces here as XXLarge's 309px band.
    CHECK(w_normal() == w_normal(UiBreakpoint::Medium));

    lv_display_delete(stray);
}

// The sibling case above pins the geometry-moved arm of the reclaim. This one
// pins the other way the derived state goes stale: with rotation, resolution
// and the default slot all exactly home, the breakpoint subject alone can
// still disagree — the shape a resize scope leaves when it republishes from
// moved geometry and then restores the pixels only, or a direct subject write.
// Every precondition below is REQUIREd, so the repaint that follows can only
// have fired through the subject-disagreement arm; a reclaim that skipped the
// refresh because "nothing moved" leaves the stale tier in place.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "camera: a stale breakpoint subject is reclaimed without the pixels moving",
                 "[1563][widget_size][camera]") {
    lv_display_t* const disp = lv_display_get_default();
    REQUIRE(disp == LVGLTestFixture::s_display);
    REQUIRE(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);
    REQUIRE(lv_display_get_horizontal_resolution(disp) == kTestDisplayWidth);
    REQUIRE(lv_display_get_vertical_resolution(disp) == kTestDisplayHeight);

    lv_subject_t* const breakpoint = theme_manager_get_breakpoint_subject();
    REQUIRE(breakpoint != nullptr);
    REQUIRE(breakpoint->type == LV_SUBJECT_TYPE_INT);
    REQUIRE(lv_subject_get_int(breakpoint) == to_int(UiBreakpoint::Medium));

    lv_subject_set_int(breakpoint, to_int(UiBreakpoint::XXLarge));
    REQUIRE(w_normal() == w_normal(UiBreakpoint::XXLarge)); // stale tier is live

    LVGLTestFixture::reclaim_display();

    CHECK(lv_subject_get_int(breakpoint) == to_int(UiBreakpoint::Medium));
    CHECK(w_normal() == w_normal(UiBreakpoint::Medium));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "camera compact/live layout follows width alone, and stream "
                 "start/stop stays edge-triggered",
                 "[1563][widget_size][camera]") {
    // Actively zero the global PrinterState singleton's webcam config rather
    // than trusting it is already empty: MoonrakerClientMock::discover_printer()
    // (src/mock/moonraker_client_mock_discovery.cpp) calls
    // get_printer_state().set_webcam_available(true, ...) on this SAME
    // process-wide singleton, and MoonrakerAPIDomainTestFixture's constructor
    // (test_moonraker_api_domain.cpp) triggers that before every test case in
    // that file. Whether this test observes that pollution depends entirely
    // on Catch2's shard split — set_webcam_available() defers through
    // async_lifetime_, so it needs a drain to actually apply.
    get_printer_state().capabilities_state().set_webcam_available(false);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(get_printer_state().capabilities_state().get_webcam_stream_url().empty());
    REQUIRE(get_printer_state().capabilities_state().get_webcam_snapshot_url().empty());

    // Widget-owned subjects (camera_status_text) are registered lazily; the
    // harness alone does not trigger it, and panel_widget_camera.xml's
    // camera_status label binds to it.
    PanelWidgetManager::instance().init_widget_subjects();

    PanelWidgetHarness<CameraWidget> h(test_screen());

    lv_obj_t* camera_image = h.child("camera_image");
    lv_obj_t* camera_overlay = h.child("camera_overlay");
    lv_obj_t* camera_status = h.child("camera_status");
    REQUIRE(camera_image != nullptr);
    REQUIRE(camera_overlay != nullptr);
    REQUIRE(camera_status != nullptr);

    // Put the widget in the same activation state PanelWidgetManager grants a
    // real, visible instance — active_ gates the "leaving compact ->
    // start_stream()" call, and the harness alone never calls this. compact_
    // is still false (its default), so this itself invokes start_stream()
    // once synchronously (still a no-op — no camera configured) and is the
    // baseline every count below is measured from.
    h.widget().on_activate();
    process_lvgl(30);

    REQUIRE(CameraWidgetTestAccess::active(h.widget()));
    REQUIRE(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1);
    REQUIRE(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 0);

    // --- Entering compact: width below floor (height also below floor here,
    // but only width is load-bearing). Contradicting span: 4x4 (old
    // shipping predicate: colspan<=1 && rowspan<=1 -> false, not compact).
    // Seed a non-null image source first (a real, decodable asset —
    // lv_image_set_src() rejects a fabricated path via
    // lv_image_decoder_get_info() and silently leaves src null, see
    // lv_image.c:180-201) so clearing it on this transition is a real
    // assertion, not a check against an already-null default.
    lv_image_set_src(camera_image, "A:assets/images/helix-icon-64.png");
    REQUIRE(lv_image_get_src(camera_image) != nullptr);

    h.resize(4, 4, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_image_get_src(camera_image) == nullptr);
    CHECK_FALSE(lv_obj_has_flag(camera_overlay, LV_OBJ_FLAG_HIDDEN)); // icon overlay shown
    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));        // status text hidden
    // The actual lifecycle call: entering compact must invoke stop_stream()
    // exactly once, and must NOT touch start_stream().
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1);

    // --- Edge-trigger check (entering-compact direction): resize to the
    // EXACT SAME compact width again (no transition — compact_ was already
    // true). Re-seed the sentinel source; a correct edge-triggered
    // implementation must NOT re-enter the "compact && !was_compact" branch,
    // so the sentinel must survive AND stop_stream_calls must not advance.
    lv_image_set_src(camera_image, "A:assets/images/ams/afc_64.png");
    h.resize(3, 3, w_normal() - 1, h_tall() - 1); // spans vary, width identical
    process_lvgl(30);

    CHECK(lv_image_get_src(camera_image) != nullptr);                  // NOT re-cleared
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1); // NOT re-called
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1);

    // --- Large-tier 1x1 (107x141): must stay compact. This is the case the
    // width-only rule exists for — a single grid row on Large already
    // clears what used to be the tall threshold (141px), so reading height
    // at all here would open a stream for a widget that never grew past one
    // column.
    h.resize(1, 1, 107, 141);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_overlay, LV_OBJ_FLAG_HIDDEN)); // still icon-only
    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1); // unchanged
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);  // unchanged

    // --- XLarge-tier 1x1 (134x169): same shape, taller still — must also
    // stay compact.
    h.resize(1, 1, 134, 169);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_overlay, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1);
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);

    // --- Micro-tier 1x2 (70x131): genuinely tall (rowspan==2) but still
    // only one column wide. Width never clears w_normal(), so this stays
    // compact too — a stream scaled to LV_IMAGE_ALIGN_COVER doesn't need
    // width *content* the way a resolved label would, but the rule tracks
    // width alone regardless of why a widget happens to be tall.
    h.resize(1, 2, 70, 131);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_overlay, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 1);
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);

    // --- Medium-tier 2x1 (233x112): the case a both-axes-required rule gets
    // wrong. Height (112) never reaches the old tall floor, but this cell is
    // genuinely two columns wide and streams in production — Medium is the
    // most common panel size in the fleet, so losing this would be the
    // regression that matters most. Leaving compact here must invoke
    // start_stream() exactly once.
    h.resize(2, 1, 233, 112);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN)); // status text shown again
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 2);
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);

    // --- Portrait-tier 2x1 (309x106): same shape as Medium above, and an
    // edge-trigger check in the same direction — width clears the floor at a
    // different value, compact_ was already false, so this must NOT re-fire
    // start_stream().
    h.resize(2, 1, 309, 106);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 2); // NOT re-called
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 1);

    // --- Back to compact (Large 1x1 again): confirms the transition still
    // fires both directions, not just once at startup.
    h.resize(1, 1, 107, 141);
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_overlay, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 2);
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 2);

    // --- Leaving compact: width at/over its floor. Contradicting span: 1x1
    // (old shipping predicate -> compact stays true).
    h.resize(1, 1, w_wide(), h_taller());
    process_lvgl(30);

    CHECK_FALSE(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN)); // status text shown again
    // active_ is true (set above), so leaving compact must invoke
    // start_stream() exactly once more, and must NOT touch stop_stream().
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 3);
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 2);

    // --- Edge-trigger check (leaving-compact direction): manually re-hide
    // camera_status, then resize to a different non-compact width again (no
    // transition — compact_ was already false). A correct edge-triggered
    // implementation must NOT re-enter the "!compact_ && was_compact"
    // branch, so the manually-set hidden flag must survive AND
    // start_stream_calls must not advance.
    lv_obj_add_flag(camera_status, LV_OBJ_FLAG_HIDDEN);
    h.resize(2, 2, w_wide(), h_taller()); // spans vary, width identical
    process_lvgl(30);

    CHECK(lv_obj_has_flag(camera_status, LV_OBJ_FLAG_HIDDEN));          // NOT re-shown
    CHECK(CameraWidgetTestAccess::start_stream_calls(h.widget()) == 3); // NOT re-called
    CHECK(CameraWidgetTestAccess::stop_stream_calls(h.widget()) == 2);
}

#endif // HELIX_HAS_CAMERA
