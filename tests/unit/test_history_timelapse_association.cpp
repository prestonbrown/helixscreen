// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_timelapse_association.cpp
 * @brief Regression test for timelapse<->job association on non-.gcode jobs.
 *
 * HistoryListPanel::associate_timelapse_files() derives a job's base name and
 * looks for it inside each timelapse video's filename. The base name used to be
 * hand-rolled as `rfind(".gcode")`, which:
 *   - misses .gco / .g / .3mf entirely, and
 *   - is case-sensitive, so "PART.GCODE" keeps its extension.
 *
 * The leftover extension then never appears in the video filename, so the match
 * silently fails: the job shows no timelapse and nothing is logged as an error.
 * helix::gcode::get_display_filename() does basename + case-insensitive strip of
 * all four extensions and is the single source of truth for this.
 */

#include "ui_panel_history_list.h"

#include "../test_fixtures.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "moonraker_types.h"
#include "print_history_data.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HistoryListPanelTestAccess;

namespace {

PrintHistoryJob job(const std::string& filename) {
    PrintHistoryJob j;
    j.job_id = filename;
    j.filename = filename;
    return j;
}

FileInfo video(const std::string& filename) {
    FileInfo f;
    f.filename = filename;
    f.is_dir = false;
    return f;
}

PrintHistoryJob find_job(HistoryListPanel& panel, const std::string& filename) {
    auto& jobs = HistoryListPanelTestAccess::jobs(panel);
    for (const auto& j : jobs) {
        if (j.filename == filename)
            return j;
    }
    FAIL("job not found: " << filename);
    return {};
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "History timelapse association handles every gcode extension",
                 "[history][timelapse][filename]") {
    HistoryListPanel panel;

    HistoryListPanelTestAccess::set_jobs(panel, {
                                                    job("classic.gcode"), // control: worked before
                                                    job("benchy.3mf"),    // 3MF job
                                                    job("PART.GCODE"),    // uppercase extension
                                                    job("sub/dir/tower.gco"), // .gco + subdirectory
                                                    job("bracket.g"),         // bare .g
                                                });

    std::vector<FileInfo> videos = {
        video("classic_20260101-120000.mp4"), video("benchy_20260102-120000.mp4"),
        video("part_20260103-120000.mp4"),    video("tower_20260104-120000.mp4"),
        video("bracket_20260105-120000.mp4"),
    };

    panel.associate_timelapse_files(videos);

    // Control — a plain .gcode job always matched.
    CHECK(find_job(panel, "classic.gcode").has_timelapse);
    CHECK(find_job(panel, "classic.gcode").timelapse_filename ==
          "timelapse/classic_20260101-120000.mp4");

    // .3mf jobs: rfind(".gcode") leaves "benchy.3mf", which is absent from the
    // video name, so the association was silently dropped.
    CHECK(find_job(panel, "benchy.3mf").has_timelapse);
    CHECK(find_job(panel, "benchy.3mf").timelapse_filename ==
          "timelapse/benchy_20260102-120000.mp4");

    // Uppercase extension: the strip must be case-insensitive.
    CHECK(find_job(panel, "PART.GCODE").has_timelapse);
    CHECK(find_job(panel, "PART.GCODE").timelapse_filename == "timelapse/part_20260103-120000.mp4");

    // .gco behind a subdirectory: basename + extension strip both required.
    CHECK(find_job(panel, "sub/dir/tower.gco").has_timelapse);
    CHECK(find_job(panel, "sub/dir/tower.gco").timelapse_filename ==
          "timelapse/tower_20260104-120000.mp4");

    // Bare .g.
    CHECK(find_job(panel, "bracket.g").has_timelapse);
    CHECK(find_job(panel, "bracket.g").timelapse_filename ==
          "timelapse/bracket_20260105-120000.mp4");
}

TEST_CASE_METHOD(XMLTestFixture, "History timelapse association leaves unmatched jobs alone",
                 "[history][timelapse][filename]") {
    HistoryListPanel panel;

    HistoryListPanelTestAccess::set_jobs(panel, {job("lonely.3mf")});
    panel.associate_timelapse_files({video("something_else_20260101.mp4")});

    const auto j = find_job(panel, "lonely.3mf");
    CHECK_FALSE(j.has_timelapse);
    CHECK(j.timelapse_filename.empty());
}

TEST_CASE_METHOD(XMLTestFixture, "History timelapse association ignores non-video files",
                 "[history][timelapse][filename]") {
    HistoryListPanel panel;

    HistoryListPanelTestAccess::set_jobs(panel, {job("benchy.3mf")});
    // A stray thumbnail in the timelapse root must not be associated.
    panel.associate_timelapse_files({video("benchy_20260102-120000.jpg")});

    CHECK_FALSE(find_job(panel, "benchy.3mf").has_timelapse);
}
