// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_crash_delete_crumbs.cpp
 * @brief async_d / sync_d crumbs carry the widget name (#960).
 *
 * One debug bundle's final breadcrumb before a glibc heap abort was:
 *
 *     crumb:1827960 async_d lv_obj 57241184
 *
 * which identified nothing — every teardown crumb in that trace read the same
 * way, and "a plain container was deleted" does not point at a file. The XML
 * name does, so the crumb now carries "<class>:<name>".
 *
 * Lives in its own file rather than test_crash_handler.cpp because it needs
 * LVGL up to create a named widget, and that file is deliberately LVGL-free.
 * The pipe/dump helper is duplicated for the same reason
 * test_gcode_layer_renderer.cpp duplicates it.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/breadcrumb_capture.h"
#include "system/crash_handler.h"

#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// The ring holds 256 slots; fill it so the crumb under test is unambiguously
/// last regardless of what earlier tests left behind.
void drain_ring() {
    for (int i = 0; i < 256; ++i) {
        crash_handler::breadcrumb::note("drain", "drain");
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Crash: delete crumbs carry the widget name",
                 "[telemetry][crash][960]") {
    drain_ring();

    lv_obj_t* named = lv_obj_create(lv_screen_active());
    REQUIRE(named != nullptr);
    lv_obj_set_name(named, "print_card_thumb");

    helix_crash_note_async_del(named, "lv_obj");

    auto lines = helix::capture_breadcrumb_lines();
    REQUIRE_FALSE(lines.empty());
    const std::string& last = lines.back();
    CHECK(last.find("async_d") != std::string::npos);
    CHECK(last.find("lv_obj:print_card_thumb") != std::string::npos);

    lv_obj_delete(named);
}

TEST_CASE_METHOD(LVGLTestFixture, "Crash: delete crumbs fall back to the bare class when unnamed",
                 "[telemetry][crash][960]") {
    // Most widgets have no XML name. Emitting a dangling ":" for them would add
    // noise to every teardown burst, so the bare class must survive unchanged.
    drain_ring();

    lv_obj_t* anon = lv_obj_create(lv_screen_active());
    REQUIRE(anon != nullptr);

    helix_crash_note_sync_del(anon, "lv_obj");

    auto lines = helix::capture_breadcrumb_lines();
    REQUIRE_FALSE(lines.empty());
    const std::string& last = lines.back();
    CHECK(last.find("sync_d") != std::string::npos);
    CHECK(last.find("lv_obj") != std::string::npos);
    CHECK(last.find("lv_obj:") == std::string::npos);

    lv_obj_delete(anon);
}

TEST_CASE_METHOD(LVGLTestFixture, "Crash: an over-long widget name cannot overrun the crumb slot",
                 "[telemetry][crash][960]") {
    // Subject is a fixed 60-byte slot. "<class>:<name>" is assembled by hand,
    // so an unbounded name is exactly where a buffer overrun would land — in
    // the crash handler, which is the worst place to have one.
    drain_ring();

    lv_obj_t* obj = lv_obj_create(lv_screen_active());
    REQUIRE(obj != nullptr);
    const std::string huge(400, 'N');
    lv_obj_set_name(obj, huge.c_str());

    helix_crash_note_async_del(obj, "lv_obj");

    auto lines = helix::capture_breadcrumb_lines();
    REQUIRE_FALSE(lines.empty());
    const std::string& last = lines.back();

    // Format: "crumb:<ms> <cat> <subj> <detail>". Subject must be truncated to
    // the slot and still null-terminated (garbage would leak neighbouring bytes).
    CHECK(last.find("async_d") != std::string::npos);
    CHECK(last.find("lv_obj:N") != std::string::npos);
    CHECK(last.size() < 200);

    lv_obj_delete(obj);
}
