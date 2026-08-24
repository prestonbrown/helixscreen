// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_print_status.h"

#include <string>

// Friend access to PrintStatusPanel internals. `ui_panel_print_status.h`
// declares `friend class PrintStatusPanelTestAccess;` in the GLOBAL namespace,
// so the definition must live there too — and in ONE place: two test
// translation units each defining their own version of the class would be an
// ODR violation.
//
//  - recompute_aux_composites(): the measurement-only entry point for the fan
//    row's composite visibility, which otherwise needs a laid-out widget tree.
//  - set_thumbnail_widget(): stands in for the XML build, which is what
//    normally assigns print_thumbnail_. The shared-subject observer only
//    touches the image when that pointer is non-null, so a test that leaves it
//    null silently skips the code it means to exercise.
//  - displayed_file() / cached_thumbnail_path(): the two markers the panel uses
//    to decide whether the preview is current.
//  - thumbnail_widget(): the image itself, so a test can read the src actually
//    on screen rather than the panel's belief about it. The two diverge — the
//    panel clears its markers on a filename change without touching the widget,
//    so only the widget says what a user would see.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
class PrintStatusPanelTestAccess {
  public:
    static void recompute_aux_composites(PrintStatusPanel& panel, int density, bool aux_present) {
        panel.recompute_aux_composites_for_measurement(density, aux_present);
    }

    static void set_thumbnail_widget(PrintStatusPanel& panel, lv_obj_t* image) {
        panel.print_thumbnail_ = image;
    }

    static const std::string& displayed_file(const PrintStatusPanel& panel) {
        return panel.displayed_file_;
    }

    static const std::string& cached_thumbnail_path(const PrintStatusPanel& panel) {
        return panel.cached_thumbnail_path_;
    }

    /// The panel's own copy of the thumbnail source override. Distinct from the
    /// media manager's: both go stale independently (#1339).
    /// The identity override for the current print, now owned by PrinterState
    /// rather than by the panel. Still reached through the panel so the cases
    /// that assert it keep reading it from the object under test.
    static const std::string& identity_override(const PrintStatusPanel& panel) {
        return panel.printer_state_.get_print_identity_override();
    }

    static const std::string& current_print_filename(const PrintStatusPanel& panel) {
        return panel.current_print_filename_;
    }

    static void set_filename(PrintStatusPanel& panel, const char* filename) {
        panel.set_filename(filename);
    }

    static lv_obj_t* thumbnail_widget(const PrintStatusPanel& panel) {
        return panel.print_thumbnail_;
    }

    /// The image source actually set on the panel's thumbnail widget, or "" when
    /// no widget is attached or nothing has been set on it yet.
    static std::string displayed_src(const PrintStatusPanel& panel) {
        lv_obj_t* image = thumbnail_widget(panel);
        if (!image) {
            return {};
        }
        const void* src = lv_image_get_src(image);
        if (!src || lv_image_src_get_type(src) != LV_IMAGE_SRC_FILE) {
            return {};
        }
        return static_cast<const char*>(src);
    }
};
