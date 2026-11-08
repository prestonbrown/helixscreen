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
    /// The installer the install prompt runs, for pointing it at a stub install.sh.
    static helix::HelixPluginInstaller& plugin_installer(PrintSelectPanel& panel) {
        return panel.plugin_installer_;
    }

    /// Whether the install prompt is waiting for the screen, with its recheck timer armed.
    static bool plugin_prompt_waiting_for_screen(const PrintSelectPanel& panel) {
        return panel.plugin_prompt_wait_ == helix::PluginTurnStep::WaitForScreen &&
               static_cast<bool>(panel.plugin_prompt_timer_);
    }

    /// What the recheck timer runs on each tick.
    static void recheck_plugin_prompt(PrintSelectPanel& panel) {
        panel.maybe_prompt_plugin_install();
    }

    /// The startup check of the installed plugin.
    static helix::PluginSelfHeal& plugin_self_heal(PrintSelectPanel& panel) {
        return panel.plugin_self_heal_;
    }

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

    /// Replace the listed files outright, as a finished listing would.
    static void set_files(PrintSelectPanel& panel, std::vector<PrintFileData> files) {
        panel.file_list_ = std::move(files);
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

    /// Request metadata for one listed file, as the card window does.
    static void fetch_metadata(PrintSelectPanel& panel, const std::string& filename) {
        for (size_t i = 0; i < panel.file_list_.size(); ++i) {
            if (panel.file_list_[i].filename == filename) {
                panel.fetch_metadata_range(i, i + 1);
                return;
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

    /// Select a listed file and show it in the detail view, as a card tap does.
    static void select_file(PrintSelectPanel& panel, const std::string& filename) {
        for (const auto& file : panel.file_list_) {
            if (!file.is_dir && file.filename == filename) {
                panel.show_file_in_detail(file);
                return;
            }
        }
        FAIL("no listed file " << filename);
    }

    /// The detail view's plate stepper buttons.
    static void step_plate(PrintSelectPanel& panel, int step) {
        panel.step_archive_plate(step);
    }

    /// What Print hands the preparation manager for the selected file, and
    /// whether it lets the start go ahead.
    static bool prepare_archive_start(PrintSelectPanel& panel) {
        return panel.prepare_archive_start();
    }

    static std::optional<std::string> queueable_path(PrintSelectPanel& panel) {
        return panel.queueable_path();
    }

    static int plate_count(PrintSelectPanel& panel) {
        return lv_subject_get_int(&panel.selected_plate_count_subject_);
    }

    static std::string plate_text(PrintSelectPanel& panel) {
        return lv_subject_get_string(&panel.selected_plate_text_subject_);
    }

    static std::string plate_position(PrintSelectPanel& panel) {
        return lv_subject_get_string(&panel.selected_plate_position_subject_);
    }

    static const std::vector<std::string>& selected_colors(const PrintSelectPanel& panel) {
        return panel.selected_filament_colors_;
    }

    static std::string selected_print_time(PrintSelectPanel& panel) {
        return lv_subject_get_string(&panel.selected_print_time_subject_);
    }

    /// Build the detail view the way the first file open does.
    static void build_detail_view(PrintSelectPanel& panel) {
        panel.create_detail_view();
    }

    /// Whether the detail view's widget tree has been built (a file was opened).
    static bool detail_view_built(const PrintSelectPanel& panel) {
        return panel.detail_view_built_;
    }

    static const helix::ui::PrintSelectCardView* card_view(const PrintSelectPanel& panel) {
        return panel.card_view_.get();
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

    static helix::ui::PrintSelectDetailView* detail_view(PrintSelectPanel& panel) {
        return panel.detail_view_.get();
    }

    static helix::ui::PrintStartController* print_controller(PrintSelectPanel& panel) {
        return panel.print_controller_.get();
    }

    /// Apply a remap the way the remap modal's confirm does.
    static void apply_remap(PrintSelectPanel& panel, const std::vector<helix::ToolMapping>& m) {
        panel.apply_remap(m);
    }
};
