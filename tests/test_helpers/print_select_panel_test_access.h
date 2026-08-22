// tests/test_helpers/print_select_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_print_select.h"

#include <string>

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

    /// Whether the detail-view overlay is currently pushed (OverlayBase's
    /// is_visible, driven by NavigationManager activate/deactivate).
    static bool detail_view_visible(const PrintSelectPanel& panel) {
        return panel.detail_view_ && panel.detail_view_->is_visible();
    }
};
