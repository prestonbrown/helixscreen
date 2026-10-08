// tests/test_helpers/print_select_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_print_select.h"

#include "print_start_controller_test_access.h"

#include <string>
#include <utility>
#include <vector>

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

    /// Whether the detail-view overlay is currently pushed (OverlayBase's
    /// is_visible, driven by NavigationManager activate/deactivate).
    static bool detail_view_visible(const PrintSelectPanel& panel) {
        return panel.detail_view_ && panel.detail_view_->is_visible();
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
};
