// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_tile_layout.cpp
 * @brief decide_job_queue_tile_layout()'s flags per rung and tile state. Rung choice
 * itself is choose_size_rung's (test_size_rung.cpp).
 *
 * Run with: ./build/bin/helix-tests "[job_queue_tile][layout]"
 */

#include "src/ui/panel_widgets/job_queue_tile_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

RungNeeds synthetic_needs() {
    RungNeeds n;
    n.strip = rung_box(100, 40);
    n.cell = rung_box(100, 100);
    n.wide = rung_box(220, 100);
    n.full = rung_box(220, 220);
    return n;
}

JobQueueTileLayoutInput input_for(int w, int h,
                                  JobQueueTileState state = JobQueueTileState::WaitingForStart,
                                  int jobs = 4) {
    JobQueueTileLayoutInput in;
    in.content_w = w;
    in.content_h = h;
    in.state = state;
    in.job_count = jobs;
    in.needs = synthetic_needs();
    in.more_line_h = 20;
    in.list_row_h = 40;
    in.auto_line_h = 30;
    return in;
}

JobQueueTileVerdict verdict_for(int w, int h,
                                JobQueueTileState state = JobQueueTileState::WaitingForStart,
                                int jobs = 4) {
    return decide_job_queue_tile_layout(input_for(w, h, state, jobs));
}

} // namespace

TEST_CASE("job queue tile: a waiting job's flags per rung", "[job_queue_tile][layout]") {
    const auto strip = verdict_for(100, 40);
    CHECK(strip.button_icon_only);
    CHECK_FALSE(strip.show_thumbnail);
    CHECK_FALSE(strip.show_detail);
    CHECK_FALSE(strip.show_more);
    CHECK(strip.list_rows == 0);

    const auto cell = verdict_for(100, 120);
    CHECK_FALSE(cell.button_icon_only);
    CHECK_FALSE(cell.show_thumbnail);
    CHECK(cell.show_more);
    CHECK(cell.more_count == 3);
    CHECK_FALSE(cell.show_detail);
    CHECK(cell.list_rows == 0);

    const auto wide = verdict_for(220, 100);
    CHECK(wide.show_thumbnail);
    CHECK(wide.show_detail);
    CHECK(wide.show_more);
    CHECK(wide.more_count == 3);
    CHECK(wide.list_rows == 0);

    // Full lists the three following jobs, so nothing is left for "+N more".
    const auto full = verdict_for(220, 220);
    CHECK(full.show_thumbnail);
    CHECK(full.show_detail);
    CHECK(full.list_rows == 3);
    CHECK_FALSE(full.show_more);
    CHECK(full.more_count == 0);
}

TEST_CASE("job queue tile: +N more counts only the jobs not on the tile",
          "[job_queue_tile][layout]") {
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 9).more_count == 5);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 3).more_count == 0);
    CHECK(verdict_for(220, 100, JobQueueTileState::WaitingForStart, 2).more_count == 1);
    CHECK(verdict_for(220, 100, JobQueueTileState::WaitingForStart, 1).more_count == 0);
    // A Cell shows the line only where the height under the name holds it.
    CHECK(verdict_for(100, 119).more_count == 0);
    CHECK_FALSE(verdict_for(100, 119).show_more);
    CHECK(verdict_for(100, 120).more_count == 3);
    CHECK(verdict_for(220, 100, JobQueueTileState::Printing, 9).more_count == 0);
}

TEST_CASE("job queue tile: the following-jobs list follows the queue", "[job_queue_tile][layout]") {
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 2).list_rows == 1);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 3).list_rows == 2);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 9).list_rows ==
          kJobQueueTileListRows);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 1).list_rows == 0);
    CHECK(verdict_for(220, 100, JobQueueTileState::WaitingForStart, 9).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::Empty, 0).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::NotLoaded, 5).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::Unavailable, 5).list_rows == 0);
}

TEST_CASE("job queue tile: the list holds only whole rows", "[job_queue_tile][layout]") {
    auto in = input_for(220, 179, JobQueueTileState::WaitingForStart, 9);
    in.needs.full = rung_box(220, 140);
    CHECK(decide_job_queue_tile_layout(in).list_rows == 1);
    in.content_h = 180;
    CHECK(decide_job_queue_tile_layout(in).list_rows == 2);
    CHECK(decide_job_queue_tile_layout(in).more_count == 6);
}

TEST_CASE("job queue tile: thumbnail only from Wide while a job waits or loads",
          "[job_queue_tile][layout]") {
    const auto printing = verdict_for(220, 100, JobQueueTileState::Printing);
    CHECK_FALSE(printing.show_thumbnail);
    CHECK(printing.show_detail);
    CHECK(verdict_for(220, 100, JobQueueTileState::Starting).show_thumbnail);
    CHECK_FALSE(verdict_for(100, 120, JobQueueTileState::Starting).show_thumbnail);
    CHECK_FALSE(verdict_for(100, 120).show_thumbnail);
    CHECK_FALSE(verdict_for(220, 100, JobQueueTileState::AutoNext).show_thumbnail);
}

TEST_CASE("job queue tile: Starts automatically only while the queue advances itself",
          "[job_queue_tile][layout]") {
    CHECK(verdict_for(220, 100, JobQueueTileState::AutoNext).show_auto_line);
    CHECK_FALSE(verdict_for(220, 100, JobQueueTileState::Printing).show_auto_line);
    CHECK_FALSE(verdict_for(100, 40, JobQueueTileState::AutoNext).show_auto_line);
    // At Cell it wraps under the hero, and shows only where that height is there.
    CHECK_FALSE(verdict_for(100, 129, JobQueueTileState::AutoNext).show_auto_line);
    CHECK(verdict_for(100, 130, JobQueueTileState::AutoNext).show_auto_line);
    CHECK(verdict_for(100, 129, JobQueueTileState::AutoNext).show_detail);
}

TEST_CASE("job queue tile: a Cell name on two lines takes the room under it",
          "[job_queue_tile][layout]") {
    auto in = input_for(100, 140);
    CHECK(decide_job_queue_tile_layout(in).show_more);
    in.cell_name_extra_h = 21;
    CHECK_FALSE(decide_job_queue_tile_layout(in).show_more);
    in.cell_name_extra_h = 20;
    CHECK(decide_job_queue_tile_layout(in).show_more);

    auto autonext = input_for(100, 140, JobQueueTileState::AutoNext);
    CHECK(decide_job_queue_tile_layout(autonext).show_auto_line);
    autonext.cell_name_extra_h = 11;
    CHECK_FALSE(decide_job_queue_tile_layout(autonext).show_auto_line);
}
