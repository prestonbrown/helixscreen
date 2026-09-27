// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_history_list.h"

#include "../test_fixtures.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "print_history_data.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// std::sort needs a strict weak ordering; a comparator that answers true for
// equal keys lets it run past the vector (prestonbrown/helixscreen#1713).

namespace {

PrintHistoryJob row(const std::string& filename, double start_time, double total_duration) {
    PrintHistoryJob job;
    job.job_id = filename + std::to_string(start_time);
    job.filename = filename;
    job.start_time = start_time;
    job.total_duration = total_duration;
    return job;
}

} // namespace

TEST_CASE("history_sort_before is a strict weak ordering", "[history][sort]") {
    const auto column =
        GENERATE(HistorySortColumn::DATE, HistorySortColumn::DURATION, HistorySortColumn::FILENAME);
    const auto direction = GENERATE(HistorySortDirection::ASC, HistorySortDirection::DESC);

    const PrintHistoryJob low = row("a.gcode", 100.0, 10.0);
    const PrintHistoryJob high = row("b.gcode", 200.0, 20.0);
    const PrintHistoryJob low_again = row("a.gcode", 100.0, 10.0);

    for (const auto* x : {&low, &high}) {
        REQUIRE_FALSE(helix::history_sort_before(*x, *x, column, direction));
    }
    REQUIRE_FALSE(helix::history_sort_before(low, low_again, column, direction));
    REQUIRE_FALSE(helix::history_sort_before(low_again, low, column, direction));

    const bool low_first = helix::history_sort_before(low, high, column, direction);
    const bool high_first = helix::history_sort_before(high, low, column, direction);
    REQUIRE(low_first != high_first);
    REQUIRE(low_first == (direction == HistorySortDirection::ASC));
}

TEST_CASE_METHOD(XMLTestFixture, "History sort descending keeps every row of equal keys",
                 "[history][sort]") {
    const auto column =
        GENERATE(HistorySortColumn::DATE, HistorySortColumn::DURATION, HistorySortColumn::FILENAME);

    std::vector<PrintHistoryJob> jobs;
    for (int i = 0; i < 40; ++i) {
        PrintHistoryJob job = row("reprint.gcode", 1000.0, 0.0);
        job.job_id = std::to_string(i);
        jobs.push_back(job);
    }

    HistoryListPanel panel;
    helix::ui::HistoryListPanelTestAccess::apply_sort(panel, jobs, column,
                                                      HistorySortDirection::DESC);

    REQUIRE(jobs.size() == 40);
    std::vector<bool> seen(40, false);
    for (const auto& job : jobs) {
        REQUIRE(job.filename == "reprint.gcode");
        seen[static_cast<size_t>(std::stoi(job.job_id))] = true;
    }
    for (bool s : seen) {
        REQUIRE(s);
    }
}
