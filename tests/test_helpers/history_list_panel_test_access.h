// tests/test_helpers/history_list_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_history_list.h"

#include "print_history_data.h"

#include <vector>

namespace helix::ui {

// Test-only access to HistoryListPanel's job list.
//
// associate_timelapse_files() is public, but its RESULT lands in the private
// jobs_ vector (has_timelapse / timelapse_filename). Seeding jobs_ directly also
// side-steps set_jobs(), which drags in the filter/sort + virtual-scroll view.
struct HistoryListPanelTestAccess {
    static std::vector<PrintHistoryJob>& jobs(HistoryListPanel& p) {
        return p.jobs_;
    }

    static void set_jobs(HistoryListPanel& p, std::vector<PrintHistoryJob> jobs) {
        p.jobs_ = std::move(jobs);
    }
};

} // namespace helix::ui
