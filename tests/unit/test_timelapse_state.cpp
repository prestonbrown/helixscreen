// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"
#include "timelapse_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// Helper to flush queued UI updates so subject values are readable
static void flush_queue() {
    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// Subject lifecycle
// ============================================================================

TEST_CASE("TimelapseState: init_subjects creates valid subjects", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    REQUIRE(state.get_render_progress_subject() != nullptr);
    REQUIRE(state.get_render_status_subject() != nullptr);
    REQUIRE(state.get_frame_count_subject() != nullptr);

    // Verify initial values
    REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "idle");
    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);

    state.deinit_subjects();
    flush_queue();
}

TEST_CASE("TimelapseState: deinit_subjects cleans up", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);
    lv_subject_set_int(state.get_frame_count_subject(), 7);

    state.deinit_subjects();
    // Double deinit is a no-op
    state.deinit_subjects();

    // Torn down for real: the next init starts the subjects over.
    state.init_subjects(false);
    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);
    state.deinit_subjects();
    flush_queue();
}

// ============================================================================
// newframe events
// ============================================================================

TEST_CASE("TimelapseState: newframe increments frame count", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    json event = {{"action", "newframe"}, {"framefile", "frame001.jpg"}, {"framenum", 1}};
    state.handle_timelapse_event(event);
    flush_queue();

    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 1);

    state.deinit_subjects();
}

TEST_CASE("TimelapseState: multiple newframe events increment correctly", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    for (int i = 1; i <= 5; i++) {
        json event = {{"action", "newframe"}, {"framefile", "frame.jpg"}, {"framenum", i}};
        state.handle_timelapse_event(event);
        flush_queue();
    }

    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 5);

    state.deinit_subjects();
}

// ============================================================================
// render events
// ============================================================================

TEST_CASE("TimelapseState: render running updates progress and status", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    json event = {{"action", "render"}, {"status", "running"}, {"progress", 45}};
    state.handle_timelapse_event(event);
    flush_queue();

    REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == 45);
    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "rendering");

    state.deinit_subjects();
}

TEST_CASE("TimelapseState: render success sets complete and resets progress", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    // First set some progress
    json running = {{"action", "render"}, {"status", "running"}, {"progress", 80}};
    state.handle_timelapse_event(running);
    flush_queue();

    // Then success
    json success = {{"action", "render"}, {"status", "success"}, {"filename", "vid.mp4"}};
    state.handle_timelapse_event(success);
    flush_queue();

    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "complete");
    REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == 0);

    state.deinit_subjects();
}

TEST_CASE("TimelapseState: render error sets error status", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    json event = {{"action", "render"}, {"status", "error"}, {"msg", "ffmpeg failed"}};
    state.handle_timelapse_event(event);
    flush_queue();

    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "error");

    state.deinit_subjects();
}

// ============================================================================
// reset
// ============================================================================

TEST_CASE("TimelapseState: reset clears all state", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    // Set some state
    json frame = {{"action", "newframe"}, {"framefile", "f.jpg"}, {"framenum", 1}};
    state.handle_timelapse_event(frame);
    flush_queue();

    json render = {{"action", "render"}, {"status", "running"}, {"progress", 50}};
    state.handle_timelapse_event(render);
    flush_queue();

    // Reset
    state.reset();
    flush_queue();

    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "idle");

    state.deinit_subjects();
}

// ============================================================================
// Render complete: filename storage and callback
// ============================================================================

TEST_CASE("TimelapseState: render success stores last rendered filename", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    REQUIRE(state.get_last_rendered_filename().empty());

    json success = {
        {"action", "render"}, {"status", "success"}, {"filename", "benchy_20260312.mp4"}};
    state.handle_timelapse_event(success);
    flush_queue();

    REQUIRE(state.get_last_rendered_filename() == "benchy_20260312.mp4");

    state.deinit_subjects();
}

TEST_CASE("TimelapseState: render success fires on_render_complete callback", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    std::string captured_filename;
    state.set_on_render_complete(
        [&captured_filename](const std::string& filename) { captured_filename = filename; });

    json success = {{"action", "render"}, {"status", "success"}, {"filename", "vase.mp4"}};
    state.handle_timelapse_event(success);
    flush_queue();

    REQUIRE(captured_filename == "vase.mp4");

    // Clean up callback to avoid dangling references
    state.set_on_render_complete(nullptr);
    state.deinit_subjects();
}

TEST_CASE("TimelapseState: reset clears last rendered filename", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    json success = {{"action", "render"}, {"status", "success"}, {"filename", "test.mp4"}};
    state.handle_timelapse_event(success);
    flush_queue();

    REQUIRE_FALSE(state.get_last_rendered_filename().empty());

    state.reset();
    flush_queue();

    REQUIRE(state.get_last_rendered_filename().empty());

    state.deinit_subjects();
}

// ============================================================================
// Edge cases: malformed/unknown events
// ============================================================================

TEST_CASE("TimelapseState: unknown action does not crash or change state", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    json event = {{"action", "unknown_action"}};
    state.handle_timelapse_event(event);
    flush_queue();

    // State unchanged from defaults
    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.get_render_status_subject())) == "idle");

    state.deinit_subjects();
}

TEST_CASE("TimelapseState: malformed JSON with no action field", "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    // Empty object
    json event = json::object();
    state.handle_timelapse_event(event);
    flush_queue();

    // State unchanged
    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);

    // Non-string action
    json bad_action = {{"action", 42}};
    state.handle_timelapse_event(bad_action);
    flush_queue();

    REQUIRE(lv_subject_get_int(state.get_frame_count_subject()) == 0);

    state.deinit_subjects();
}

// ============================================================================
// Notification throttling
// ============================================================================

TEST_CASE("TimelapseState: render progress notifications throttled to 25% boundaries",
          "[timelapse_state]") {
    lv_init_safe();
    auto& state = TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);

    // Send progress events at 10%, 20%, 25%, 30%, 50%, 75%, 100%
    // Only 25%, 50%, 75%, 100% should trigger notifications
    // (We can't easily verify notifications in unit tests, but we verify
    // that the progress subject updates correctly for each event)
    int progress_values[] = {10, 20, 25, 30, 50, 75, 100};
    for (int p : progress_values) {
        json event = {{"action", "render"}, {"status", "running"}, {"progress", p}};
        state.handle_timelapse_event(event);
        flush_queue();

        REQUIRE(lv_subject_get_int(state.get_render_progress_subject()) == p);
    }

    state.deinit_subjects();
}

// ============================================================================
// New print resets the per-print capture state
// ============================================================================

namespace {

struct TimelapsePrintFixture {
    TimelapsePrintFixture() {
        lv_init_safe();
        auto& ps = get_printer_state();
        ps.init_subjects(false);
        test::set_wire_state(ps, PrintJobState::STANDBY);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        // Re-init so the print-state observer binds to this PrinterState's subjects.
        TimelapseState::instance().deinit_subjects();
        TimelapseState::instance().init_subjects(false);
    }
    ~TimelapsePrintFixture() {
        test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        TimelapseState::instance().deinit_subjects();
    }

    void wire(PrintJobState s) {
        test::set_wire_state(get_printer_state(), s);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            TimelapseState::instance().handle_timelapse_event(
                json{{"action", "newframe"}, {"framefile", "f.jpg"}});
            helix::ui::UpdateQueue::instance().drain_until_empty();
        }
    }
    int frame_count() {
        return lv_subject_get_int(TimelapseState::instance().get_frame_count_subject());
    }
    std::string capture_info() {
        return lv_subject_get_string(TimelapseState::instance().get_capture_info_subject());
    }
};

} // namespace

TEST_CASE_METHOD(TimelapsePrintFixture,
                 "TimelapseState: a new print starts the frame count over when nothing rendered",
                 "[timelapse_state][new_print]") {
    // Autorender off: no render event ever clears the previous print's frames.
    wire(PrintJobState::PRINTING);
    frames(3);
    REQUIRE(frame_count() == 3);
    REQUIRE_FALSE(capture_info().empty());
    wire(PrintJobState::COMPLETE);

    wire(PrintJobState::PRINTING);
    CHECK(frame_count() == 0);
    CHECK(capture_info().empty());
    frames(1);
    CHECK(frame_count() == 1);
}

TEST_CASE_METHOD(TimelapsePrintFixture,
                 "TimelapseState: a new print starts over after a failed render",
                 "[timelapse_state][new_print]") {
    wire(PrintJobState::PRINTING);
    frames(4);
    wire(PrintJobState::COMPLETE);
    TimelapseState::instance().handle_timelapse_event(
        json{{"action", "render"}, {"status", "error"}, {"msg", "ffmpeg failed"}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(frame_count() == 4);

    wire(PrintJobState::PRINTING);
    CHECK(frame_count() == 0);
    CHECK(capture_info().empty());
}

TEST_CASE_METHOD(TimelapsePrintFixture, "TimelapseState: resuming a paused print keeps its frames",
                 "[timelapse_state][new_print]") {
    wire(PrintJobState::PRINTING);
    frames(2);
    const std::string info = capture_info();
    REQUIRE_FALSE(info.empty());

    wire(PrintJobState::PAUSED);
    wire(PrintJobState::PRINTING);
    CHECK(frame_count() == 2);
    CHECK(capture_info() == info);
}

TEST_CASE_METHOD(TimelapsePrintFixture,
                 "TimelapseState: joining a print already running keeps its frames",
                 "[timelapse_state][new_print]") {
    // App start mid-print: frames arrive before the first status frame reports
    // PRINTING. Nothing has been seen to end, so this is not a new print.
    frames(2);
    const std::string info = capture_info();
    REQUIRE_FALSE(info.empty());

    wire(PrintJobState::PRINTING);
    CHECK(frame_count() == 2);
    CHECK(capture_info() == info);

    // A reconnect replays the same state: still the same print.
    wire(PrintJobState::PRINTING);
    frames(1);
    CHECK(frame_count() == 3);

    // The print that follows it does start over.
    wire(PrintJobState::COMPLETE);
    wire(PrintJobState::PRINTING);
    CHECK(frame_count() == 0);
}

TEST_CASE("TimelapseState: subscribing while a print runs keeps its frames",
          "[timelapse_state][new_print]") {
    // Subjects re-initialised mid-print: the observer's first value is PRINTING
    // with no prior state.
    lv_init_safe();
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    test::set_wire_state(ps, PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    auto& tl = TimelapseState::instance();
    tl.deinit_subjects();
    tl.init_subjects(false);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    tl.handle_timelapse_event(json{{"action", "newframe"}, {"framefile", "f.jpg"}});
    helix::ui::UpdateQueue::instance().drain_until_empty();

    test::set_wire_state(ps, PrintJobState::PAUSED);
    test::set_wire_state(ps, PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lv_subject_get_int(tl.get_frame_count_subject()) == 1);

    test::set_wire_state(ps, PrintJobState::STANDBY);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    tl.deinit_subjects();
}
