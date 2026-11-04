// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_timelapse_local_path.cpp
 * @brief Same-host playback opens videos under the root Moonraker reports.
 *
 * Run with: ./build/bin/helix-tests "[timelapse][roots]"
 *
 * The stock printer_data/timelapse layout was the only path the player ever
 * got, so a Moonraker configured with another data_path played nothing
 * (prestonbrown/helixscreen#1373). server.files.roots names the directory;
 * the stock layout is the fallback until it answers.
 */

#include "ui_overlay_timelapse_videos.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

struct TimelapseVideosOverlayTestAccess {
    static void apply_roots(TimelapseVideosOverlay& o, const std::vector<FileRoot>& roots) {
        o.apply_timelapse_root(roots);
    }
    static std::string local_path(const TimelapseVideosOverlay& o, const std::string& filename) {
        return o.local_video_path(filename);
    }
    /// Stand in for the grid create() finds, so a case can clear it without
    /// the overlay's XML.
    static void set_video_grid(TimelapseVideosOverlay& o, lv_obj_t* grid) {
        o.video_grid_container_ = grid;
    }
    static void clear_video_grid(TimelapseVideosOverlay& o) {
        o.clear_video_grid();
    }
    /// A player that exists, against a remote Moonraker: a tap downloads first.
    static void set_remote_player(TimelapseVideosOverlay& o, const std::string& command) {
        o.can_play_ = true;
        o.player_command_ = command;
        o.is_local_moonraker_ = false;
    }
    /// The card handlers populate_video_grid() attaches.
    static void attach_card_handlers(TimelapseVideosOverlay& o, lv_obj_t* card) {
        lv_obj_add_event_cb(card, TimelapseVideosOverlay::on_card_clicked, LV_EVENT_CLICKED, &o);
        lv_obj_add_event_cb(card, TimelapseVideosOverlay::on_card_long_pressed,
                            LV_EVENT_LONG_PRESSED, &o);
    }
    static const std::string& pending_delete(const TimelapseVideosOverlay& o) {
        return o.pending_delete_filename_;
    }
};

namespace {

FileRoot root(const std::string& name, const std::string& path, const std::string& perms) {
    FileRoot r;
    r.name = name;
    r.path = path;
    r.permissions = perms;
    return r;
}

} // namespace

TEST_CASE("timelapse local path follows Moonraker's reported root", "[timelapse][roots]") {
    TimelapseVideosOverlay overlay(nullptr);

    SECTION("before roots arrive: the stock data_path layout") {
        std::string p = TimelapseVideosOverlayTestAccess::local_path(overlay, "a.mp4");
        CHECK(p.find("printer_data/timelapse/a.mp4") != std::string::npos);
    }

    SECTION("a reported root wins, with or without its trailing slash") {
        TimelapseVideosOverlayTestAccess::apply_roots(
            overlay, {root("gcodes", "/data/gcodes", "rw"),
                      root("timelapse", "/data/moonraker/timelapse", "rw")});
        CHECK(TimelapseVideosOverlayTestAccess::local_path(overlay, "a.mp4") ==
              "/data/moonraker/timelapse/a.mp4");

        TimelapseVideosOverlayTestAccess::apply_roots(overlay,
                                                      {root("timelapse", "/srv/tl/", "r")});
        CHECK(TimelapseVideosOverlayTestAccess::local_path(overlay, "b.mp4") == "/srv/tl/b.mp4");
    }

    SECTION("an answer without a timelapse root keeps the fallback") {
        TimelapseVideosOverlayTestAccess::apply_roots(overlay, {root("gcodes", "/data/g", "rw")});
        std::string p = TimelapseVideosOverlayTestAccess::local_path(overlay, "a.mp4");
        CHECK(p.find("printer_data/timelapse/a.mp4") != std::string::npos);
    }
}

// A refresh clears the grid from inside the queued scan result; queued work,
// such as a thumbnail that finds its card by filename, waits for the next tick
// instead of running under the clear.
TEST_CASE_METHOD(LVGLTestFixture, "timelapse grid clears without running queued work",
                 "[timelapse][drain_reentrancy]") {
    TimelapseVideosOverlay overlay(nullptr);
    lv_obj_t* grid = lv_obj_create(test_screen());
    lv_obj_create(grid);
    lv_obj_create(grid);
    TimelapseVideosOverlayTestAccess::set_video_grid(overlay, grid);

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::drain_canary", [&ran]() { ran = true; });
    TimelapseVideosOverlayTestAccess::clear_video_grid(overlay);
    CHECK_FALSE(ran);
    CHECK(lv_obj_get_child_count(grid) == 0);

    helix::ui::UpdateQueue::instance().drain();
    CHECK(ran);
    TimelapseVideosOverlayTestAccess::set_video_grid(overlay, nullptr);
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "timelapse: a held card asks to delete and its lift plays nothing",
                 "[timelapse][tap_hold]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};
    TimelapseVideosOverlay overlay(&api);
    TimelapseVideosOverlayTestAccess::set_remote_player(overlay, "/bin/true");

    lv_obj_t* card = lv_obj_create(test_screen());
    lv_obj_set_size(card, 200, 120);
    REQUIRE(helix::ui::set_owned_user_string(card, "a.mp4"));
    TimelapseVideosOverlayTestAccess::attach_card_handlers(overlay, card);
    lv_obj_update_layout(test_screen());
    lv_area_t a;
    lv_obj_get_coords(card, &a);
    const int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
    const auto& downloads = api.transfers_mock().download_destinations();
    helix_test::ScopedPointerIndev finger;

    // A quick tap plays the video: for a remote Moonraker, a download first.
    finger.press(x, y);
    finger.release(x, y);
    REQUIRE(downloads.size() == 1);

    finger.press(x, y);
    finger.hold(finger.long_press_hold_ms());
    finger.release(x, y);
    CHECK(TimelapseVideosOverlayTestAccess::pending_delete(overlay) == "a.mp4");
    CHECK(downloads.size() == 1);
    helix::ui::UpdateQueue::instance().drain();
}
