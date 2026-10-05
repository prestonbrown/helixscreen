// tests/test_helpers/print_select_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_print_select.h"
#include "ui_print_select_detail_view.h"

#include "print_start_controller_test_access.h"

#include <map>
#include <string>

#include "../catch_amalgamated.hpp"

// Test-only read access to PrintSelectPanel's file list.
//
// The panel exposes no public reader for file_list_ (production consumers all
// read it through the card/list views), but the delete-guard tests need to
// assert which files the panel currently holds without driving widget-level
// scroll state. Same pattern as BedMeshPanelTestAccess.
struct PrintSelectPanelTestAccess {
    static bool list_contains(const PrintSelectPanel& panel, const std::string& filename) {
        for (const auto& file : panel.file_list_) {
            if (!file.is_dir && file.filename == filename) {
                return true;
            }
        }
        return false;
    }

    static size_t list_size(const PrintSelectPanel& panel) {
        return panel.file_list_.size();
    }

    /// The listed file named @p filename, or null.
    static const PrintFileData* find_file(const PrintSelectPanel& panel,
                                          const std::string& filename) {
        for (const auto& file : panel.file_list_) {
            if (!file.is_dir && file.filename == filename) {
                return &file;
            }
        }
        return nullptr;
    }

    /// Return a listed file to its state before its metadata arrived.
    static void forget_metadata(PrintSelectPanel& panel, const std::string& filename) {
        for (auto& file : panel.file_list_) {
            if (file.filename == filename) {
                file.metadata_fetched = false;
                file.layer_count_str.clear();
                file.print_height_str.clear();
            }
        }
    }

    /// Feed @p metadata through the panel's metadata apply for a listed file.
    static void apply_metadata(PrintSelectPanel& panel, const std::string& filename,
                               const FileMetadata& metadata) {
        for (size_t i = 0; i < panel.file_list_.size(); ++i) {
            if (panel.file_list_[i].filename == filename) {
                panel.process_metadata_result(i, filename, metadata);
                return;
            }
        }
    }

    /// Build the detail view the way the first file open does.
    static void build_detail_view(PrintSelectPanel& panel) {
        panel.create_detail_view();
    }

    /// Whether the detail view's widget tree has been built (a file was opened).
    static bool detail_view_built(const PrintSelectPanel& panel) {
        return panel.detail_view_built_;
    }

    /// Whether the panel's USB source has a walk in flight.
    static bool usb_scanning(const PrintSelectPanel& panel) {
        return panel.usb_source_ && panel.usb_source_->is_scanning();
    }

    /// Whether the detail-view overlay is currently pushed (OverlayBase's
    /// is_visible, driven by NavigationManager activate/deactivate).
    static bool detail_view_visible(const PrintSelectPanel& panel) {
        return panel.detail_view_ && panel.detail_view_->is_visible();
    }

    /// Drive the panel's own show/hide the way the file-click and back-out
    /// paths do, without going through widget events.
    static void show_detail_view(PrintSelectPanel& panel) {
        panel.show_detail_view();
    }

    static void hide_detail_view(PrintSelectPanel& panel) {
        panel.hide_detail_view();
    }

    /// The job id of the queued start this panel is holding open, or null
    /// when no queued job is pending.
    static const std::string* pending_queued_job_id(const PrintSelectPanel& panel) {
        return panel.pending_queued_start_ ? &panel.pending_queued_start_->job_id : nullptr;
    }

    /// Mark that a Print tap for the pending queued file is in flight. The
    /// real writer is start_print(); tests set it to drive the failed-start
    /// bookkeeping without running the whole start pipeline.
    static void set_pending_start_attempted(PrintSelectPanel& panel, bool attempted) {
        if (panel.pending_queued_start_) {
            panel.pending_queued_start_->start_attempted = attempted;
        }
    }

    /// Fire the print-start-success callback the way the start pipeline does
    /// once Moonraker confirms the print — proving the panel wired
    /// set_on_print_started, not just that its consume method exists.
    static void fire_print_started(PrintSelectPanel& panel) {
        REQUIRE(panel.print_controller_ != nullptr);
        PrintStartControllerTestAccess::fire_print_started(*panel.print_controller_);
    }

    /// The file the print controller was last told to start: {filename, dir}.
    static std::pair<std::string, std::string> controller_file(const PrintSelectPanel& panel) {
        REQUIRE(panel.print_controller_ != nullptr);
        return PrintStartControllerTestAccess::file(*panel.print_controller_);
    }

    /// The tool colors the print controller was last handed.
    static std::vector<std::string> controller_colors(const PrintSelectPanel& panel) {
        REQUIRE(panel.print_controller_ != nullptr);
        return PrintStartControllerTestAccess::filament_colors(*panel.print_controller_);
    }

    /// Overwrite the selected file's tool colors, as opening another file does.
    static void set_selected_colors(PrintSelectPanel& panel, std::vector<std::string> colors) {
        panel.selected_filament_colors_ = std::move(colors);
    }

    /// The detail view's current option-row states (id -> on).
    static std::map<std::string, bool> collect_option_states(const PrintSelectPanel& panel) {
        if (!panel.detail_view_) {
            return {};
        }
        return panel.detail_view_->collect_option_states();
    }

    /// Queue the shown file the way the detail-view button's tap handler does.
    static void add_to_queue(PrintSelectPanel& panel) {
        panel.add_to_queue();
    }

    /// Whether an add_job request is on the wire (button disabled for it).
    static bool queue_add_in_flight(const PrintSelectPanel& panel) {
        return panel.queue_add_in_flight_;
    }

    /// current_path_ joined with the selected filename — the path Moonraker
    /// is addressed by, and the identity a queued start must reproduce.
    static std::string composed_selected_filename(const PrintSelectPanel& panel) {
        return panel.composed_selected_filename();
    }

    /// Drop any pending queued start; cleanup for the process-global panel,
    /// whose pending state would otherwise leak into later tests.
    static void clear_pending_queued_start(PrintSelectPanel& panel) {
        panel.pending_queued_start_.reset();
    }

    /// The detail view's print preparation manager, or null before one exists.
    static helix::ui::PrintPreparationManager* prep_manager(const PrintSelectPanel& panel) {
        return panel.detail_view_ ? panel.detail_view_->get_prep_manager() : nullptr;
    }
};
