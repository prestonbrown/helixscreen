// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_viewer_superseded_load.cpp
 * @brief Only the newest gcode viewer load reports (prestonbrown/helixscreen#1509).
 *
 * A load's result reaches the main thread through the UpdateQueue, so it can be
 * sitting there, already built, when the next helix::ui::gcode_viewer_load_file()
 * starts. That superseded result must be dropped: it must not install its file
 * and must not invoke the load callback. PrintStatusPanel names the print a
 * load was for when the load starts, so a superseded load that still reported
 * would be announced under the newer load's print.
 *
 * Each section holds load X's finished result in the queue, starts load Y,
 * and requires exactly one report, carrying Y.
 */

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"
#include "core/update_queue.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct LoadReport {
    bool success;
    std::string filename;
};

std::vector<LoadReport> g_reports;

void on_load_done(lv_obj_t* viewer, void*, bool success) {
    const char* name = helix::ui::gcode_viewer_get_filename(viewer);
    g_reports.push_back({success, name ? name : ""});
}

/// Resolve a shipped gcode asset from the repo root or a build subdirectory.
std::string find_test_asset(const std::string& filename) {
    for (const auto& prefix : {"", "../", "../../"}) {
        std::string path = std::string(prefix) + "assets/test_gcodes/" + filename;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

/// Wait on the real clock, without draining the UpdateQueue, until a worker
/// has queued something.
bool wait_for_queued_result(std::chrono::milliseconds budget) {
    auto& queue = helix::ui::UpdateQueue::instance();
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !helix::ui::UpdateQueueTestAccess::queue_empty(queue);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "GCode viewer: a load superseded by a newer one never reports",
                 "[gcode][viewer][1509][slow]") {
    const std::string first = find_test_asset("pause_markers_demo.gcode");
    const std::string second = find_test_asset("xyz-10mm-calibration-cube.gcode");
    REQUIRE_FALSE(first.empty()); // run helix-tests from the repo root
    REQUIRE_FALSE(second.empty());

    // HELIX_GCODE_STREAMING is read on every load; the guard puts it back so
    // later loads in this binary keep their own mode.
    helix::ScopedEnv restore_streaming("HELIX_GCODE_STREAMING");
    const char* stale_log = nullptr;
    SECTION("full load") {
        ::setenv("HELIX_GCODE_STREAMING", "off", 1);
        stale_log = "Stale async callback";
    }
    SECTION("streaming") {
        ::setenv("HELIX_GCODE_STREAMING", "on", 1);
        stale_log = "Stale streaming callback";
    }
    REQUIRE(stale_log != nullptr);

    lv_obj_t* viewer = helix::ui::gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 240, 240);
    lv_obj_update_layout(viewer);

    helix::ui::UpdateQueue::instance().drain_until_empty();
    g_reports.clear();
    helix::ui::gcode_viewer_set_load_callback(viewer, on_load_done, nullptr);

    {
        // Worker threads log while this is alive, which only an exclusive
        // capture tolerates.
        helix::ExclusiveLogCapture log(1024);

        helix::ui::gcode_viewer_load_file(viewer, first.c_str());

        // The first load's result is built and waiting in the queue, not yet
        // delivered, when the second load starts.
        REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));
        REQUIRE(g_reports.empty());

        helix::ui::gcode_viewer_load_file(viewer, second.c_str());

        REQUIRE(wait_until([] { return !g_reports.empty(); }, 30000));
        // Keep pumping after the report: a superseded result that slipped past
        // the viewer would be delivered in this window.
        wait_until([] { return false; }, 300);

        // The first load's result was queued and then rejected, so the single
        // report below is the gate working rather than a load that never ran.
        CHECK(log.count_containing(stale_log) >= 1);
    }

    CHECK(g_reports.size() == 1);
    REQUIRE_FALSE(g_reports.empty());
    CHECK(g_reports.front().success);
    CHECK(g_reports.front().filename.find("xyz-10mm-calibration-cube.gcode") != std::string::npos);
    CHECK(helix::ui::gcode_viewer_has_content(viewer));

    helix::ui::gcode_viewer_set_load_callback(viewer, nullptr, nullptr);
    helix::ui::gcode_viewer_clear(viewer);
    lv_obj_delete(viewer);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "GCode viewer: clear drops a queued load result and its spinner",
                 "[gcode][viewer][gcode_viewer][slow]") {
    const std::string path = find_test_asset("pause_markers_demo.gcode");
    REQUIRE_FALSE(path.empty()); // run helix-tests from the repo root

    helix::ScopedEnv restore_streaming("HELIX_GCODE_STREAMING");
    ::setenv("HELIX_GCODE_STREAMING", "off", 1);

    lv_obj_t* viewer = helix::ui::gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 240, 240);
    lv_obj_update_layout(viewer);
    const uint32_t children_before = lv_obj_get_child_count(viewer);

    helix::ui::UpdateQueue::instance().drain_until_empty();
    g_reports.clear();
    helix::ui::gcode_viewer_set_load_callback(viewer, on_load_done, nullptr);

    helix::ui::gcode_viewer_load_file(viewer, path.c_str());
    // The loading UI is up and the finished result is waiting in the queue.
    REQUIRE(lv_obj_get_child_count(viewer) > children_before);
    REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));
    helix::test_access::gcode_viewer_wait_for_build(viewer);

    helix::ui::gcode_viewer_clear(viewer);
    wait_until([] { return false; }, 300);

    CHECK(g_reports.empty());
    CHECK_FALSE(helix::ui::gcode_viewer_has_content(viewer));
    CHECK(helix::ui::gcode_viewer_get_parsed_file(viewer) == nullptr);
    CHECK(lv_obj_get_child_count(viewer) == children_before);

    helix::ui::gcode_viewer_set_load_callback(viewer, nullptr, nullptr);
    lv_obj_delete(viewer);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "GCode viewer: a viewer never accepts a result queued for a deleted one",
                 "[gcode][viewer][gcode_viewer][slow]") {
    const std::string path = find_test_asset("pause_markers_demo.gcode");
    REQUIRE_FALSE(path.empty()); // run helix-tests from the repo root

    helix::ScopedEnv restore_streaming("HELIX_GCODE_STREAMING");
    ::setenv("HELIX_GCODE_STREAMING", "off", 1);

    // The queued result is only dangerous when its successor lives at the same
    // address, which LVGL's heap allows but does not promise. Each round builds a
    // result for a viewer, deletes the viewer and creates the next one; a round
    // whose successor landed elsewhere is thrown away and tried again.
    auto& queue = helix::ui::UpdateQueue::instance();
    lv_obj_t* second = nullptr;
    for (int round = 0; round < 8 && second == nullptr; ++round) {
        queue.drain_until_empty();
        lv_obj_t* first = helix::ui::gcode_viewer_create(test_screen());
        REQUIRE(first != nullptr);

        helix::ui::gcode_viewer_load_file(first, path.c_str());
        REQUIRE(wait_for_queued_result(std::chrono::seconds(30)));
        helix::test_access::gcode_viewer_wait_for_build(first);

        // The finished result stays queued, addressed to the widget that is
        // about to go away.
        lv_obj_delete(first);
        lv_obj_t* candidate = helix::ui::gcode_viewer_create(test_screen());
        REQUIRE(candidate != nullptr);
        if (candidate == first) {
            second = candidate;
        } else {
            lv_obj_delete(candidate);
        }
    }
    if (second == nullptr) {
        SKIP("LVGL's heap did not reuse the deleted viewer's address");
    }

    // The successor's first generation bump: a viewer that counts from zero
    // again lands on the number the queued result carries.
    helix::ui::gcode_viewer_clear(second);
    queue.drain_until_empty();

    CHECK_FALSE(helix::ui::gcode_viewer_has_content(second));
    CHECK(helix::ui::gcode_viewer_get_parsed_file(second) == nullptr);

    lv_obj_delete(second);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "GCode viewer: no two viewers ever share a load generation",
                 "[gcode][viewer][gcode_viewer]") {
    lv_obj_t* first = helix::ui::gcode_viewer_create(test_screen());
    helix::ui::gcode_viewer_clear(first);
    const uint64_t first_gen = helix::test_access::gcode_viewer_load_generation(first);

    // A live neighbour and a successor of a deleted viewer both start elsewhere.
    lv_obj_t* neighbour = helix::ui::gcode_viewer_create(test_screen());
    helix::ui::gcode_viewer_clear(neighbour);
    lv_obj_delete(first);
    lv_obj_t* successor = helix::ui::gcode_viewer_create(test_screen());
    helix::ui::gcode_viewer_clear(successor);

    CHECK(helix::test_access::gcode_viewer_load_generation(neighbour) != first_gen);
    CHECK(helix::test_access::gcode_viewer_load_generation(successor) != first_gen);
    CHECK(helix::test_access::gcode_viewer_load_generation(successor) !=
          helix::test_access::gcode_viewer_load_generation(neighbour));

    lv_obj_delete(neighbour);
    lv_obj_delete(successor);
}

namespace {

/// The viewer's loading card: the child holding the spinner.
lv_obj_t* loading_card(lv_obj_t* viewer) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(viewer); ++i) {
        lv_obj_t* child = lv_obj_get_child(viewer, static_cast<int32_t>(i));
        for (uint32_t j = 0; j < lv_obj_get_child_count(child); ++j) {
            if (lv_obj_check_type(lv_obj_get_child(child, static_cast<int32_t>(j)),
                                  &lv_spinner_class)) {
                return child;
            }
        }
    }
    return nullptr;
}

} // namespace

// A load can start inside a queued callback (a file pick delivered through the
// queue), where running queued work or deleting synchronously corrupts the
// batch it runs in. Replacing the previous load's card does neither.
TEST_CASE_METHOD(LVGLTestFixture,
                 "GCode viewer: a new load replaces the loading card without running queued work",
                 "[gcode][viewer][drain_reentrancy]") {
    const std::string first = find_test_asset("xyz-10mm-calibration-cube.gcode");
    REQUIRE_FALSE(first.empty()); // run helix-tests from the repo root
    helix::ScopedEnv restore_streaming("HELIX_GCODE_STREAMING");
    ::setenv("HELIX_GCODE_STREAMING", "off", 1);

    lv_obj_t* viewer = helix::ui::gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 240, 240);
    lv_obj_update_layout(viewer);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    helix::ui::gcode_viewer_load_file(viewer, first.c_str());
    lv_obj_t* old_card = loading_card(viewer);
    REQUIRE(old_card != nullptr);

    int old_card_deleted = 0;
    lv_obj_add_event_cb(
        old_card, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, &old_card_deleted);

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran]() { ran = true; });
    helix::ui::gcode_viewer_load_file(viewer, first.c_str());

    CHECK_FALSE(ran);
    // Deferred: the old card is freed on LVGL's async tick, not under the load.
    CHECK(old_card_deleted == 0);

    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(ran);
    process_lvgl(20);
    CHECK(old_card_deleted == 1);
    lv_obj_delete(viewer);
    helix::ui::UpdateQueue::instance().drain_until_empty();
}
