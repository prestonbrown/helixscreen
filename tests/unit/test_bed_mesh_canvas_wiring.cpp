// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_canvas_wiring.cpp
 * @brief Regression coverage for the BedMeshPanel::canvas_ use-after-free fix
 *
 * bed_mesh_panel.xml's top-level <if cond="ui_is_portrait eq 1"> is REACTIVE
 * (its cond references a subject), so LVGL's XML engine rebuilds
 * overlay_content — and bed_mesh_canvas beneath it — in place on every
 * orientation flip (xml_frag_rebuild -> xml_frag_teardown,
 * lib/helix-xml/src/xml/lv_xml.c): the old widgets are reparented into an
 * off-tree, hidden "condemned" sink and lv_obj_delete_async()'d. Nothing
 * previously nulled BedMeshPanel::canvas_ on that path (only on_ui_destroyed(),
 * which a rebuild never calls), so every canvas_ dereference between a flip
 * and the panel's eventual destruction was a potential use-after-free.
 *
 * wire_canvas_and_content() / on_canvas_deleted_cb() /
 * rewire_after_orientation_flip() are the fix. Driving the actual XML <if>
 * end-to-end needs the full app's XML registration (header_bar, the bed_mesh
 * custom widget, both stat/profile card components, several modals) plus a
 * MoonrakerAPI — test_subject_initializer.cpp notes this whole class of
 * dependency is only practically exercised as a real running app, which is
 * exactly where this fix was ALSO verified live, by forcing real
 * `ui_is_portrait` flips against a running `--test` instance via
 * `helix-screen ctl set ui_is_portrait <0|1>` with the bed mesh panel open,
 * repeatedly, and confirming no crash, correct post-flip canvas geometry,
 * and correct mesh values via `ctl geom`/`ctl text`.
 *
 * This test instead isolates the two private methods that make the rebuild
 * safe, feeding them minimal hand-built trees standing in for
 * overlay_content/bed_mesh_canvas — no XML, no Moonraker, no
 * NavigationManager. In particular it pins a bug caught while writing it:
 * an early version of on_canvas_deleted_cb() nulled canvas_ unconditionally
 * whenever ANY canvas this panel had ever owned was deleted, including a
 * STALE one whose async delete lands after a rewire already pointed canvas_
 * at the new widget — see the fourth test case below.
 */

#include "ui_panel_bed_mesh.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/bed_mesh_panel_test_access.h"

#include "../catch_amalgamated.hpp"

using helix::ui::BedMeshPanelTestAccess;

namespace {

lv_obj_t* make_named(lv_obj_t* parent, const char* name) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_name(obj, name);
    return obj;
}

bool has_event_cb(lv_obj_t* obj, lv_event_cb_t cb) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == cb) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "wire_canvas_and_content finds and tracks the named canvas",
                 "[bed_mesh][canvas][1229]") {
    BedMeshPanel panel;
    lv_obj_t* content = make_named(test_screen(), "overlay_content");
    lv_obj_t* canvas = make_named(content, "bed_mesh_canvas");

    REQUIRE(BedMeshPanelTestAccess::wire(panel, content));
    CHECK(BedMeshPanelTestAccess::canvas(panel) == canvas);
}

TEST_CASE_METHOD(LVGLTestFixture, "wire_canvas_and_content fails gracefully without a canvas",
                 "[bed_mesh][canvas][1229]") {
    BedMeshPanel panel;
    lv_obj_t* content = make_named(test_screen(), "overlay_content");
    // No child named "bed_mesh_canvas" - simulates the XML not matching (or
    // not yet having built) the expected structure.

    CHECK_FALSE(BedMeshPanelTestAccess::wire(panel, content));
    CHECK(BedMeshPanelTestAccess::canvas(panel) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "canvas_ is nulled when its widget is actually deleted - the "
                 "use-after-free this fix closes",
                 "[bed_mesh][canvas][1229]") {
    BedMeshPanel panel;
    lv_obj_t* content = make_named(test_screen(), "overlay_content");
    lv_obj_t* canvas = make_named(content, "bed_mesh_canvas");
    REQUIRE(BedMeshPanelTestAccess::wire(panel, content));
    REQUIRE(BedMeshPanelTestAccess::canvas(panel) == canvas);

    // Simulates xml_frag_teardown's eventual lv_obj_delete_async() landing.
    // Without the LV_EVENT_DELETE guard this fix adds, canvas_ would still
    // point at this now-freed lv_obj_t* — remove the
    // lv_obj_add_event_cb(canvas_, on_canvas_deleted_cb, ...) call in
    // wire_canvas_and_content() and this assertion goes red.
    lv_obj_delete(canvas);

    CHECK(BedMeshPanelTestAccess::canvas(panel) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a STALE canvas's delete does not clobber an already-rewired canvas_",
                 "[bed_mesh][canvas][1229]") {
    // TEST_MIRROR_OK: the re-wire itself is NOT reimplemented — every wire below
    // goes through BedMeshPanelTestAccess::wire(), i.e. the production
    // wire_canvas_and_content() that rewire_after_orientation_flip() calls. Only
    // the surrounding XML <if> rebuild is staged by hand, since it needs a live
    // overlay_root_ + Moonraker API.
    // Mirrors what a real orientation flip does: rewire_after_orientation_flip()
    // re-wires canvas_ to a brand-new widget SYNCHRONOUSLY (LVGL's FIFO
    // observer order guarantees this runs right after the XML <if>'s own
    // rebuild), but the OLD canvas's condemned-sink deletion is asynchronous
    // and lands on a LATER tick. on_canvas_deleted_cb() must check WHICH
    // widget triggered it - without that check, this late delete nulls out
    // the CURRENT, perfectly valid canvas_ instead of the dead one that
    // actually fired, since every canvas this panel has ever owned shares
    // the same user_data (`this`) on this same callback.
    BedMeshPanel panel;
    lv_obj_t* content_a = make_named(test_screen(), "overlay_content");
    lv_obj_t* old_canvas = make_named(content_a, "bed_mesh_canvas");
    REQUIRE(BedMeshPanelTestAccess::wire(panel, content_a));
    REQUIRE(BedMeshPanelTestAccess::canvas(panel) == old_canvas);

    // Simulate the rebuild: a fresh overlay_content/bed_mesh_canvas pair, as
    // xml_frag_expand builds for the other <if> branch, wired the same way
    // rewire_after_orientation_flip() does.
    lv_obj_t* content_b = make_named(test_screen(), "overlay_content");
    lv_obj_t* new_canvas = make_named(content_b, "bed_mesh_canvas");
    REQUIRE(BedMeshPanelTestAccess::wire(panel, content_b));
    REQUIRE(BedMeshPanelTestAccess::canvas(panel) == new_canvas);

    // The OLD canvas's delete arrives late (as it would from the condemned
    // sink's async delete) - it must not affect the already-rewired canvas_.
    lv_obj_delete(old_canvas);
    CHECK(BedMeshPanelTestAccess::canvas(panel) == new_canvas);

    // Sanity: the NEW (current) canvas's own delete still correctly nulls it.
    lv_obj_delete(new_canvas);
    CHECK(BedMeshPanelTestAccess::canvas(panel) == nullptr);

    lv_obj_delete(content_a);
    lv_obj_delete(content_b);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a wired overlay_content outliving the panel keeps no callback into it",
                 "[bed_mesh][canvas][1229]") {
    // wire_canvas_and_content() registers on_content_size_changed with
    // user_data=this. Whenever overlay_content outlives the panel, that
    // registration is a dangling `this`: the next layout pass to resize the
    // widget calls apply_portrait_canvas_height() on freed memory, reads
    // overlay_root_ out of it, and writes through whatever that garbage
    // resolves to — heap corruption that detonates far from here.
    //
    // The destructor used to reach the registration only via overlay_root_,
    // which is null on every path that wires without create() and on any
    // teardown that clears the root first, so it silently removed nothing.
    lv_obj_t* content = make_named(test_screen(), "overlay_content");
    make_named(content, "bed_mesh_canvas");

    {
        BedMeshPanel panel;
        REQUIRE(BedMeshPanelTestAccess::wire(panel, content));
        REQUIRE(has_event_cb(content, BedMeshPanelTestAccess::content_size_changed_cb()));
    } // panel destroyed; content deliberately outlives it

    CHECK_FALSE(has_event_cb(content, BedMeshPanelTestAccess::content_size_changed_cb()));
}
