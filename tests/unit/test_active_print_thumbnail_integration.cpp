// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_thumbnail_integration.cpp
 * @brief Both active-print thumbnail consumers driven against ONE PrinterState.
 *
 * Every other test in this area drives a single producer or a single consumer in
 * isolation, which is why a wrong-thumbnail bug survived a green suite: the
 * defect only exists in the wiring. `ActivePrintMediaManager` publishes the
 * shared `print_thumbnail_path` subject, `PrintStatusPanel` subscribes to it,
 * and both also observe `print_filename` — so the ordering between them is part
 * of the behaviour, not an implementation detail.
 *
 * These cases run the configuration that actually ships (one manager + one panel
 * + one PrinterState) across print A -> idle -> print B, and pin:
 *   - both consumers show A while A is printing;
 *   - the display is deliberately PRESERVED when the filename goes empty
 *     (post-cancel UX — the user should still see what was printing);
 *   - A's image never survives into B, on either consumer;
 *   - a leftover path from a print this manager never processed is not adopted.
 *
 * Assertions read the panel's real widget src via PrintStatusPanelTestAccess,
 * not a screenshot, and not the panel's own bookkeeping alone.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "active_print_media_manager.h"
#include "printer_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// Real, distinct assets so lv_image_set_src resolves a decoder instead of
// logging a miss. Deliberately NOT benchy_thumbnail_white.png, which is the
// no-thumbnail placeholder — a test that used it could not tell "print A's
// image" apart from "nothing to show".
constexpr const char* THUMB_A = "A:assets/images/printer.png";
constexpr const char* THUMB_B = "A:assets/images/folder.png";

/// One PrinterState, one ActivePrintMediaManager, one PrintStatusPanel.
///
/// The state is OWNED here rather than shared with the process-wide instance:
/// the panel's observers are registered against whatever reference it is handed,
/// and a global whose subjects were never init_subjects()-ed reads "" for every
/// string, which makes every assertion below pass vacuously.
///
/// Consumers are brought up by start_consumers() rather than in the constructor
/// so a case can seed the subject BEFORE the manager exists — that is the
/// reconnect-mid-print shape, and it is a different bug from A -> B.
struct ActivePrintThumbnailFixture : public LVGLTestFixture {
    ActivePrintThumbnailFixture() {
        // register_xml=false keeps these subjects out of the process-wide XML
        // registry, which outlives this stack frame.
        state_.init_subjects(false);
    }

    ~ActivePrintThumbnailFixture() override {
        panel_.reset();
        media_.reset();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void start_consumers() {
        media_ = std::make_unique<ActivePrintMediaManager>(state_);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        // Stands in for the XML build. The panel's observer only touches the
        // image when this pointer is non-null, so leaving it null would skip
        // the very code path under test.
        PrintStatusPanelTestAccess::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
        drain();
    }

    /// Deliver a print_stats.filename update the way Moonraker does.
    void set_print_filename(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    /// Async work (the panel's deferred filename observer, queued subject
    /// writes) only lands on a queue tick [L048].
    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    std::string subject_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

    std::string panel_src() const {
        return PrintStatusPanelTestAccess::displayed_src(*panel_);
    }

    PrinterState& state() {
        return state_;
    }
    ActivePrintMediaManager& media() {
        return *media_;
    }
    PrintStatusPanel& panel() {
        return *panel_;
    }

    PrinterState state_;
    std::unique_ptr<ActivePrintMediaManager> media_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: print B never displays print A's image",
                 "[print_status][thumbnail][integration]") {
    start_consumers();

    // --- Print A starts and its thumbnail resolves -------------------------
    set_print_filename("model_a.gcode");
    // Stands in for the resolved thumbnail (PrintStartController's USB pre-set
    // takes this exact route; a Moonraker fetch lands on the same subject).
    media().set_thumbnail_path("model_a.gcode", THUMB_A);
    drain();

    REQUIRE(state().get_print_thumbnail_file() == "model_a.gcode");
    REQUIRE(subject_path() == THUMB_A);
    REQUIRE(panel_src() == THUMB_A);
    REQUIRE(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_a.gcode");

    // --- Print A ends: the display is deliberately preserved ---------------
    // Klipper reports an empty filename on cancel/complete. Both consumers must
    // keep showing what was printing; clearing here is what made a cancelled
    // print flash to a blank card before the user could read it.
    set_print_filename("");

    CHECK(subject_path() == THUMB_A);
    CHECK(panel_src() == THUMB_A);

    // --- Print B starts, thumbnail not resolved yet ------------------------
    set_print_filename("model_b.gcode");

    // The subject must no longer attribute anything to A: identity moves to B,
    // and A's path is gone. Whatever stands in for "nothing yet" (empty string
    // or an explicit placeholder), it is not A's image.
    CHECK(state().get_print_thumbnail_file() == "model_b.gcode");
    CHECK(subject_path() != THUMB_A);
    // "Nothing yet" is the placeholder, published explicitly, so the panel
    // actually repaints instead of leaving A's pixels on B's card.
    CHECK(subject_path() == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(panel_src() == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) != THUMB_A);

    // --- Print B's thumbnail resolves --------------------------------------
    media().set_thumbnail_path("model_b.gcode", THUMB_B);
    drain();

    CHECK(subject_path() == THUMB_B);
    CHECK(panel_src() == THUMB_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_b.gcode");

    // --- A stale in-flight result for A lands late -------------------------
    // A fetch started for A can still complete after B took over. The panel
    // compares the identity the subject carries against the file it is showing,
    // so this must not repaint B's card with A's image.
    state().set_print_thumbnail("model_a.gcode", THUMB_A);
    drain();

    CHECK(panel_src() == THUMB_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_b.gcode");
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a leftover from a print the manager never saw is dropped",
                 "[print_status][thumbnail][integration]") {
    // Reconnect / restart mid-print: the subject already holds the previous
    // print's path and identity, but this manager has no history of its own.
    // The clear must key off the path's identity, not off whether THIS manager
    // has loaded anything before.
    state().set_print_thumbnail("model_a.gcode", THUMB_A);

    start_consumers();
    REQUIRE(subject_path() == THUMB_A);

    set_print_filename("model_b.gcode");

    CHECK(state().get_print_thumbnail_file() == "model_b.gcode");
    CHECK(subject_path() != THUMB_A);
    CHECK(panel_src() != THUMB_A);
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a file with no thumbnail shows the placeholder on "
                 "every consumer",
                 "[print_status][thumbnail][integration]") {
    start_consumers();

    set_print_filename("model_a.gcode");
    media().set_thumbnail_path("model_a.gcode", THUMB_A);
    drain();
    REQUIRE(panel_src() == THUMB_A);

    // Print B has no thumbnail: nothing pre-set, and no API to fetch one.
    set_print_filename("no_thumb.gcode");

    const std::string shown = subject_path();
    CHECK_FALSE(shown.empty());
    CHECK(shown == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(panel_src() == shown);
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: the shared subject is never the empty string",
                 "[print_status][thumbnail][integration]") {
    // This is the invariant that lets all three consumers drop their
    // empty-string branches. It is not cosmetic: lv_image_set_src("") has a
    // first byte of 0x00, which lv_image_src_get_type classifies as
    // LV_IMAGE_SRC_VARIABLE, so LVGL dereferences the one-byte literal as an
    // lv_image_dsc_t. A consumer without a guard is only safe if the subject
    // genuinely never carries "".
    CHECK(subject_path() == ActivePrintMediaManager::no_thumbnail_placeholder());

    start_consumers();
    CHECK_FALSE(subject_path().empty());

    // Print starts, nothing resolved yet.
    set_print_filename("model_a.gcode");
    CHECK_FALSE(subject_path().empty());

    // Print ends.
    set_print_filename("");
    CHECK_FALSE(subject_path().empty());
}
