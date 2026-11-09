// Copyright (C) 2025-2026 356C LLC
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

    // The detail overlay's Delete button reaches handle_delete() with the row
    // the user opened already selected; seeding both here skips the overlay.
    static void select_job(HistoryListPanel& p, std::vector<PrintHistoryJob> jobs, size_t index) {
        p.filtered_jobs_ = std::move(jobs);
        p.selected_job_index_ = index;
    }

    static void handle_delete(HistoryListPanel& p) {
        p.handle_delete();
    }

    /// The delete dialog's confirm button.
    static void confirm_delete(HistoryListPanel& p) {
        p.confirm_delete();
    }

    static lv_obj_t* delete_dialog(HistoryListPanel& p) {
        return p.delete_confirmation_dialog_;
    }

    static void handle_view_timelapse(HistoryListPanel& p) {
        p.handle_view_timelapse();
    }

    static void handle_reprint(HistoryListPanel& p) {
        p.handle_reprint();
    }

    static const std::vector<PrintHistoryJob>& filtered_jobs(HistoryListPanel& p) {
        return p.filtered_jobs_;
    }

    /// Stand in for the XML search box: create() finds it by name, and its
    /// value_changed callback reaches on_search_changed().
    static void set_search_box(HistoryListPanel& p, lv_obj_t* textarea) {
        p.search_box_ = textarea;
    }

    static void on_search_changed(HistoryListPanel& p) {
        p.on_search_changed();
    }

    /// What show_detail_overlay() records, so the list's re-activation when the
    /// detail closes takes the no-refresh path it takes in production.
    static void set_detail_overlay_open(HistoryListPanel& p, bool open) {
        p.detail_overlay_open_ = open;
    }

    static void update_detail_subjects(HistoryListPanel& p, const PrintHistoryJob& job) {
        p.update_detail_subjects(job);
    }

    static void apply_sort(HistoryListPanel& p, std::vector<PrintHistoryJob>& jobs,
                           HistorySortColumn column, HistorySortDirection direction) {
        p.sort_column_ = column;
        p.sort_direction_ = direction;
        p.apply_sort(jobs);
    }

    /// Give the panel the two containers create() would find in its XML, so
    /// the virtual-scroll list can be driven without the whole overlay.
    static void set_list_containers(HistoryListPanel& p, lv_obj_t* content, lv_obj_t* rows) {
        p.list_content_ = content;
        p.list_rows_ = rows;
    }

    static void set_history_manager(HistoryListPanel& p, PrintHistoryManager* manager) {
        p.history_manager_ = manager;
    }

    static void refresh_from_manager(HistoryListPanel& p) {
        p.refresh_from_manager();
    }

    /// The per-open hook show() runs before the push.
    static void before_show(HistoryListPanel& p) {
        p.before_show();
    }

    // Production opens the detail overlay from a row click on a created panel.
    static void show_detail_overlay(HistoryListPanel& p, lv_obj_t* parent,
                                    const PrintHistoryJob& job) {
        p.parent_screen_ = parent;
        p.show_detail_overlay(job);
    }
};

} // namespace helix::ui
