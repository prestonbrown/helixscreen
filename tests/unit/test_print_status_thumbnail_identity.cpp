// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_thumbnail_identity.cpp
 * @brief PrintStatusPanel adopts a shared thumbnail only when it was produced
 *        for the file the panel is showing.
 *
 * `print_thumbnail_path` is a shared subject with a single writer
 * (ActivePrintMediaManager) but no ordering relative to the panel's own view of
 * which file is printing. Before this, the panel applied whatever landed and
 * then stamped `displayed_file_` with its OWN current filename, without checking
 * that the path had been produced for that file. A late result for the previous
 * print therefore convinced ensure_preview_current() that the current file was
 * already on screen, so panel activation, re-entry and the next print start all
 * became no-ops — the wrong image stuck permanently rather than transiently.
 *
 * PrinterState::set_print_thumbnail() stores the source filename before it
 * publishes the path, so an observer can compare identity instead of guessing.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// A real asset, so lv_image_set_src resolves instead of logging a decoder miss.
// Deliberately NOT benchy_thumbnail_white.png: that is the no-thumbnail
// placeholder AND the subject's initial value, so publishing it would be a
// no-op write that never fires the observer these cases are about.
constexpr const char* THUMB_PATH = "A:assets/images/printer.png";

/// Owns a PrintStatusPanel with a thumbnail widget attached, which is what the
/// XML build normally supplies. Without it the observer's image branch — the
/// branch that carries the stamp — never runs and the test would pass for the
/// wrong reason.
struct PrintStatusThumbFixture : public LVGLTestFixture {
    PrintStatusThumbFixture() {
        // Own the PrinterState rather than sharing the global one: the panel's
        // observer is registered against whatever reference it is handed, and a
        // path left behind by an earlier test would be delivered to it the
        // moment it subscribes (observe_string_immediate fires on registration).
        // register_xml=false keeps these subjects out of the process-wide XML
        // registry, which outlives this stack frame.
        state_.init_subjects(false);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        PrintStatusPanelTestAccess::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
    }

    ~PrintStatusThumbFixture() override {
        panel_.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    PrinterState& state() {
        return state_;
    }
    PrintStatusPanel& panel() {
        return *panel_;
    }

    PrinterState state_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(PrintStatusThumbFixture,
                 "PrintStatusPanel: a thumbnail published for another file is ignored",
                 "[print_status][thumbnail]") {
    panel().set_filename("current.gcode");
    helix::ui::UpdateQueue::instance().drain();

    // A late publish belonging to the PREVIOUS print.
    state().set_print_thumbnail("previous.gcode", THUMB_PATH);
    helix::ui::UpdateQueue::instance().drain();

    // The stamp is the self-sealing half: claiming current.gcode is on screen
    // makes ensure_preview_current() a permanent no-op.
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) != "current.gcode");
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()).empty());
}

TEST_CASE_METHOD(PrintStatusThumbFixture,
                 "PrintStatusPanel: a thumbnail published for the current file is adopted",
                 "[print_status][thumbnail]") {
    panel().set_filename("current.gcode");
    helix::ui::UpdateQueue::instance().drain();

    state().set_print_thumbnail("current.gcode", THUMB_PATH);
    helix::ui::UpdateQueue::instance().drain();

    // The identity check must not degenerate into ignoring everything: the
    // matching publish still has to land, and displayed_file_ must record what
    // is ACTUALLY shown.
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) == THUMB_PATH);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "current.gcode");
}

TEST_CASE_METHOD(PrintStatusThumbFixture,
                 "PrintStatusPanel: a thumbnail arriving before any filename is adopted",
                 "[print_status][thumbnail]") {
    // PrintStartController pre-sets a USB thumbnail before the filename observer
    // fires. With no effective filename yet there is nothing to compare against,
    // so the value must still be taken.
    state().set_print_thumbnail("usb_model.gcode", THUMB_PATH);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) == THUMB_PATH);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "usb_model.gcode");
}

// --- Externally-started prints ------------------------------------------------
//
// A print started from the app routes through PrintStartController, which calls
// PrintStatusPanel::set_thumbnail_source() (ui_print_start_controller.cpp:241)
// before the printer ever reports the new name. A print started from Mainsail,
// Fluidd, or the printer's own screen does NOT: the panel learns the change only
// from the print_filename subject. That path carries the stale-preview fix
// (clear_gcode), so it needs its own coverage — the app-started path is not a
// proxy for it.

// A second real asset, so both publishes resolve and the two are distinguishable.
constexpr const char* THUMB_PATH_B = "A:assets/images/filament_spool.png";

TEST_CASE_METHOD(PrintStatusThumbFixture,
                 "PrintStatusPanel: an externally started print invalidates the preview marker",
                 "[print_status][thumbnail]") {
    panel().set_filename("printA.gcode");
    state().set_print_thumbnail("printA.gcode", THUMB_PATH);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(PrintStatusPanelTestAccess::displayed_file(panel()) == "printA.gcode");

    // No set_thumbnail_source(): the printer simply starts reporting a new file.
    panel().set_filename("printB.gcode");
    helix::ui::UpdateQueue::instance().drain();

    // The marker must no longer claim printA is on screen, or ensure_preview_current()
    // sees no mismatch and the previous print's content is never reconciled.
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) != "printA.gcode");

    // And the new print's thumbnail must be adopted once it lands.
    state().set_print_thumbnail("printB.gcode", THUMB_PATH_B);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) == THUMB_PATH_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "printB.gcode");
}

TEST_CASE_METHOD(PrintStatusThumbFixture,
                 "PrintStatusPanel: a thumbnail source that stops describing the print is retired",
                 "[print_status][thumbnail]") {
    // set_thumbnail_source() overrides the effective filename for BOTH the display
    // name and the thumbnail lookup, so an override left set across a print
    // boundary makes every later filename change compare against the OLD name.
    // The panel then never registers that a new print began - which stales the
    // thumbnail AND suppresses the stale-geometry clear in ensure_preview_current(),
    // because gcode_mismatch is computed off that same comparison. One override,
    // two symptoms.
    //
    // The panel's own print-ended retirement cannot be relied on to prevent it:
    // that path is gated on print_ended, which is going_idle only
    // (print_lifecycle_state.cpp:80), and any non-zero start phase forces
    // Preparing (:34), so the Complete->Idle edge is swallowed for any print
    // started from the app. The override must therefore be retired when it stops
    // describing the incoming filename.
    panel().set_thumbnail_source("printA.gcode");
    panel().set_filename("printA.gcode");
    state().set_print_thumbnail("printA.gcode", THUMB_PATH);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) == THUMB_PATH);

    // Next print starts externally, so nothing re-points the override at it.
    panel().set_filename("printB.gcode");
    state().set_print_thumbnail("printB.gcode", THUMB_PATH_B);
    helix::ui::UpdateQueue::instance().drain();

    // The override no longer describes the incoming file, so it must be dropped
    // rather than pinning the panel to the previous print.
    CHECK(PrintStatusPanelTestAccess::thumbnail_source(panel()).empty());
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) == THUMB_PATH_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "printB.gcode");
}
