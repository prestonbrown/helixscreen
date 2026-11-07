// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_print_status.h"

#include <set>
#include <string>

// Friend access to PrintStatusPanel internals. `ui_panel_print_status.h`
// declares `friend class PrintStatusPanelTestAccess;` in the GLOBAL namespace,
// so the definition must live there too — and in ONE place: two test
// translation units each defining their own version of the class would be an
// ODR violation.
//
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
//    per-tool colors come from — shared with PrintSelectDetailView, so it needs
//    a test on THIS side too or the two can drift apart unnoticed.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
class PrintStatusPanelTestAccess {
  public:
    /// Stand in the memory reading the close-time decision samples. Pass
    /// helix::get_system_memory_info to put the real one back.
    static void set_memory_info_source(helix::MemoryInfo (*source)()) {
        PrintStatusPanel::memory_info_source_ = source;
    }

    /// Queue what the kept tree's job watch queues when the job lets go of the
    /// machine. The release checks everything again when it lands.
    static void queue_job_end_release() {
        PrintStatusPanel::release_kept_tree_after_job();
    }

    /// True between on_activate() and the next on_deactivate().
    static bool is_active(const PrintStatusPanel& panel) {
        return panel.is_active_;
    }

    /// Open and close the exclude-object side list, as the objects button does.
    static void show_exclude(PrintStatusPanel& panel) {
        panel.show_exclude_map_view();
    }
    static void hide_exclude(PrintStatusPanel& panel) {
        panel.hide_exclude_map_view();
    }
    static bool exclude_open(const PrintStatusPanel& panel) {
        return panel.exclude_mode_.is_open();
    }

    /// The G-code viewer the XML build found, or null once the tree is gone.
    static lv_obj_t* gcode_viewer(const PrintStatusPanel& panel) {
        return panel.gcode_viewer_;
    }

    /// The printer tile's chips and the job tile's checklist.
    static helix::ui::PrintStatusTiles& tiles(PrintStatusPanel& panel) {
        return panel.tiles_;
    }

    /// The preview card's camera stream exists (it may still be connecting).
    static bool camera_streaming(const PrintStatusPanel& panel) {
#if HELIX_HAS_CAMERA
        return panel.camera_view_.has_stream();
#else
        (void)panel;
        return false;
#endif
    }

    /// Start the preview card's camera as an active, showing panel does.
    static void start_preview_camera(PrintStatusPanel& panel) {
        panel.start_preview_camera();
    }

    /// A More menu row's tap, by its PrintStatusMoreMenu action.
    static void more_action(PrintStatusPanel& panel, int action) {
        panel.handle_more_action(action);
    }

    /// What the preview controller calls to put the G-code render up or down.
    static void show_gcode_viewer(PrintStatusPanel& panel, bool show) {
        panel.show_gcode_viewer(show);
    }

    /// The More button's tap, raising the menu below @p anchor.
    static void open_more_menu(PrintStatusPanel& panel, lv_obj_t* anchor) {
        panel.handle_more_clicked(anchor);
    }
    static helix::ui::PrintStatusMoreMenu& more_menu(PrintStatusPanel& panel) {
        return panel.more_menu_;
    }

    static void set_thumbnail_widget(PrintStatusPanel& panel, lv_obj_t* image) {
        panel.print_thumbnail_ = image;
        panel.preview_.attach_widgets(panel.print_thumbnail_, panel.gcode_viewer_);
    }

    static const std::string& displayed_file(const PrintStatusPanel& panel) {
        return panel.preview_.displayed_file();
    }

    static const std::string& cached_thumbnail_path(const PrintStatusPanel& panel) {
        return panel.preview_.cached_thumbnail_path();
    }

    /// The panel's own copy of the thumbnail source override. Distinct from the
    /// media manager's: both go stale independently (#1339).
    /// The identity override for the current print, now owned by PrinterState
    /// rather than by the panel. Still reached through the panel so the cases
    /// that assert it keep reading it from the object under test.
    static const std::string& identity_override(const PrintStatusPanel& panel) {
        return panel.printer_state_.print_state().get_print_identity_override();
    }

    static std::string reprint_filename(const PrintStatusPanel& panel) {
        return panel.reprint_filename();
    }
    static const std::string& current_print_filename(const PrintStatusPanel& panel) {
        return panel.current_print_filename_;
    }

    /// Reprint through @p controller, standing in for the one
    /// print_start_controller_for() hands out.
    static void reprint_through(PrintStatusPanel& panel,
                                helix::ui::PrintStartController& controller,
                                const helix::gcode::ReprintTarget& target) {
        panel.reprint_through(controller, target);
    }

    static void set_filename(PrintStatusPanel& panel, const char* filename) {
        panel.set_filename(filename);
    }

    static lv_obj_t* thumbnail_widget(const PrintStatusPanel& panel) {
        return panel.print_thumbnail_;
    }

    /// The preparing-phase progress bar, whose cached pointer is the one a
    /// queued print-start-progress apply dereferences (the teardown-uaf tests).
    static lv_obj_t* preparing_progress_widget(const PrintStatusPanel& panel) {
        return panel.preparing_progress_bar_;
    }

    /// What on_print_start_progress_changed() published to its own subject —
    /// proves a drained handler ran its body rather than early-outing.
    /// Non-const because lv_subject_get_int() takes a mutable subject.
    static int preparing_progress_subject_value(PrintStatusPanel& panel) {
        return lv_subject_get_int(&panel.preparing_progress_subject_);
    }

    /// The two subjects on_print_state_changed() writes without a
    /// subjects_initialized_ guard.
    static lv_subject_t* print_controls_enabled_subject(PrintStatusPanel& panel) {
        return &panel.print_controls_enabled_subject_;
    }
    static lv_subject_t* gcode_viewer_mode_subject(PrintStatusPanel& panel) {
        return &panel.gcode_viewer_mode_subject_;
    }

    /// Stands in for the XML build, which is what normally assigns
    /// gcode_viewer_. get_tools_used() reads nothing else, so this is the whole
    /// wiring a tools-used test needs.
    static void set_gcode_viewer(PrintStatusPanel& panel, lv_obj_t* viewer) {
        panel.gcode_viewer_ = viewer;
        panel.preview_.attach_widgets(panel.print_thumbnail_, viewer);
    }

    /// The tool set the panel answers with - what the print-scoped runout badge
    /// and the Snapmaker reprint preamble are both built from.
    static std::set<int> tools_used(const PrintStatusPanel& panel) {
        return panel.get_tools_used();
    }

    /// The runout-sensor-edge and AMS-slots-version observers' entry point, so a
    /// test can drive it directly instead of threading a subject update through
    /// FilamentSensorManager or AmsState.
    static void recompute_scoped_runout(PrintStatusPanel& panel) {
        panel.recompute_scoped_runout();
    }

    /// The deferred gcode load's entry point: fetch @p filename's gcode and
    /// load it into the viewer.
    static void load_gcode_for_viewing(PrintStatusPanel& panel, const std::string& filename) {
        panel.preview_.load_for_viewing(filename);
    }

    /// The tree teardown the overlay's destroy-on-close runs.
    static void ui_destroyed(PrintStatusPanel& panel) {
        panel.on_ui_destroyed();
    }

    /// The file whose geometry the panel records as loaded in the viewer.
    static const std::string& gcode_displayed_file(const PrintStatusPanel& panel) {
        return panel.preview_.gcode_displayed_file();
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
