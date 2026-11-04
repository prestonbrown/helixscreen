// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "print_stop_reason.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

constexpr double kEnd = 1'700'000'000.0;

// What a Voron's AFC wrote when it paused on a lane it could not load.
constexpr const char* kAfcLoadFailure =
    "!! lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL\n"
    "To resolve set lane loaded with `SET_LANE_LOADED LANE=lane3` macro.\n"
    "Manually move filament with LANE_MOVE macro";

JournalEntry gcode_error(const char* text, double time) {
    JournalEntry e;
    e.kind = journal_kind::kGcodeError;
    e.text = text;
    e.time = time;
    return e;
}

json response(std::vector<std::string> lines) {
    return json{{"params", lines}};
}

/// Counts reads and keeps the last continuation so a test can answer it.
class StoreClient : public test::FakeMoonrakerClient {
  public:
    void get_gcode_store(int count,
                         std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                         helix::SharedCallback<void(const MoonrakerError&)>) override {
        ++reads;
        last_count = count;
        reply = std::move(on_success);
    }
    int reads = 0;
    int last_count = 0;
    std::function<void(const std::vector<GcodeStoreEntry>&)> reply;
};

class TrackerFixture : public LVGLTestFixture {
  public:
    TrackerFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        test::set_wire_state(ps, PrintJobState::PRINTING);
        tracker_ = std::make_unique<PrintStopReasonTracker>(client_, ps, [this] { return now_; });
        settle();
    }

    ~TrackerFixture() override {
        tracker_.reset();
        test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        settle();
    }

    static void settle() {
        ui::UpdateQueue::instance().drain_until_empty();
    }

    static std::string reason() {
        return lv_subject_get_string(
            get_printer_state().print_state().get_print_stop_reason_subject());
    }

    static int reason_visible() {
        return lv_subject_get_int(
            get_printer_state().print_state().get_print_stop_reason_visible_subject());
    }

    void end(const char* state, const char* message = nullptr) {
        json stats = {{"state", state}};
        if (message) {
            stats["message"] = message;
        }
        get_printer_state().update_from_status(json{{"print_stats", stats}});
        settle();
    }

    double now_ = kEnd;
    StoreClient client_;
    std::unique_ptr<PrintStopReasonTracker> tracker_;
};

} // namespace

TEST_CASE("an event is put down to a cause shortly before it", "[plugin_event_journal]") {
    CHECK(is_recent_cause(kEnd - kEventCauseWindowS, kEnd));
    CHECK_FALSE(is_recent_cause(kEnd - kEventCauseWindowS - 0.5, kEnd));
    CHECK(is_recent_cause(kEnd + kEventCauseLateS, kEnd));
    CHECK_FALSE(is_recent_cause(kEnd + kEventCauseLateS + 0.5, kEnd));
}

TEST_CASE("stop reason: Klipper's message wins over a gcode error",
          "[print_completion][stop_reason]") {
    const std::vector<JournalEntry> errors = {gcode_error("Probe triggered prior", kEnd - 1)};
    CHECK(decide_print_stop_reason(PrintJobState::ERROR, "MCU 'mcu' shutdown", errors, kEnd) ==
          "MCU 'mcu' shutdown");
}

TEST_CASE("stop reason: the newest gcode error in the window explains a pause, cancel or failure",
          "[print_completion][stop_reason]") {
    const std::vector<JournalEntry> errors = {
        gcode_error("Too far out of range", kEnd - 40),
        gcode_error("Retries exceeded", kEnd - 3),
        gcode_error("Quad gantry level failed", kEnd - 1),
    };
    CHECK(decide_print_stop_reason(PrintJobState::CANCELLED, "", errors, kEnd) ==
          "Quad gantry level failed");
    CHECK(decide_print_stop_reason(PrintJobState::ERROR, "", errors, kEnd) ==
          "Quad gantry level failed");
    CHECK(decide_print_stop_reason(PrintJobState::PAUSED, "", errors, kEnd) ==
          "Quad gantry level failed");
}

TEST_CASE("stop reason: an error outside the window, or a completed print, says nothing",
          "[print_completion][stop_reason]") {
    const std::vector<JournalEntry> old = {gcode_error("Must home axis first", kEnd - 60)};
    CHECK(decide_print_stop_reason(PrintJobState::CANCELLED, "", old, kEnd).empty());
    CHECK(decide_print_stop_reason(PrintJobState::CANCELLED, "", {}, kEnd).empty());

    const std::vector<JournalEntry> recent = {gcode_error("Must home axis first", kEnd - 1)};
    CHECK(decide_print_stop_reason(PrintJobState::COMPLETE, "", recent, kEnd).empty());
    CHECK(decide_print_stop_reason(PrintJobState::PRINTING, "Runout", recent, kEnd).empty());
}

TEST_CASE("stop reason tap: only !! lines, and the last one of a frame wins",
          "[print_completion][stop_reason]") {
    CHECK_FALSE(last_gcode_error_text(response({"// QGL probing", "ok"})).has_value());
    CHECK_FALSE(last_gcode_error_text(response({"Error: unknown command"})).has_value());
    CHECK(last_gcode_error_text(response({"!! First failure", "// note", "!! Second failure"})) ==
          "Second failure");
}

TEST_CASE("stop reason text: only the first line of a multi-line !! response",
          "[print_completion][stop_reason]") {
    CHECK(
        gcode_error_reason_text(kAfcLoadFailure) ==
        "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH");
    CHECK(gcode_error_reason_text("!! Move out of range: 1 2 3") == "Move out of range: 1 2 3");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterState: an observer of a terminal outcome sees the same frame's message",
                 "[state][stop_reason]") {
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    test::set_wire_state(state, PrintJobState::PRINTING);

    std::string seen;
    lv_observer_t* obs = lv_subject_add_observer(
        state.print_state().get_print_outcome_subject(),
        [](lv_observer_t* o, lv_subject_t* s) {
            if (lv_subject_get_int(s) == static_cast<int>(PrintOutcome::ERROR)) {
                *static_cast<std::string*>(lv_observer_get_user_data(o)) = lv_subject_get_string(
                    get_printer_state().print_state().get_print_message_subject());
            }
        },
        &seen);
    state.update_from_status(
        json{{"print_stats", {{"state", "error"}, {"message", "Heater extruder not heating"}}}});
    lv_observer_remove(obs);

    CHECK(seen == "Heater extruder not heating");
    test::set_wire_state(state, PrintJobState::STANDBY);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterState: a new print starts with no stop reason",
                 "[state][stop_reason]") {
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.print_state().set_print_stop_reason("Quad gantry level failed");
    REQUIRE(lv_subject_get_int(state.print_state().get_print_stop_reason_visible_subject()) == 1);

    state.print_state().reset_for_new_print();

    CHECK(std::string(lv_subject_get_string(state.print_state().get_print_stop_reason_subject())) ==
          "");
    CHECK(lv_subject_get_int(state.print_state().get_print_stop_reason_visible_subject()) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterState: a print's first frame keeps its own message",
                 "[state][stop_reason]") {
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    test::set_wire_state(state, PrintJobState::STANDBY);
    auto message = [&state] {
        return std::string(lv_subject_get_string(state.print_state().get_print_message_subject()));
    };

    state.update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"message", "Waiting for chamber"}}}});
    CHECK(message() == "Waiting for chamber");

    // A start frame that carries no message clears the last print's.
    test::set_wire_state(state, PrintJobState::CANCELLED);
    state.update_from_status(json{{"print_stats", {{"state", "printing"}}}});
    CHECK(message().empty());
    test::set_wire_state(state, PrintJobState::STANDBY);
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: the tapped error explains a cancel",
                 "[print_completion][stop_reason]") {
    now_ = kEnd - 2;
    REQUIRE(client_.fire_notification("notify_gcode_response",
                                      response({"!! Quad gantry level failed"})));
    settle();
    CHECK(reason().empty());

    now_ = kEnd;
    end("cancelled");
    CHECK(reason() == "Quad gantry level failed");
    CHECK(reason_visible() == 1);
    CHECK(client_.reads == 0);

    // Klipper's reset to standby leaves the reason up; a new print clears it.
    end("standby");
    CHECK(reason() == "Quad gantry level failed");
    end("printing");
    CHECK(reason().empty());
    CHECK(reason_visible() == 0);
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: Klipper's message outlives its reset",
                 "[print_completion][stop_reason]") {
    end("error", "Heater bed not heating at expected rate");
    CHECK(reason() == "Heater bed not heating at expected rate");
    get_printer_state().update_from_status(json{{"print_stats", {{"message", ""}}}});
    settle();
    CHECK(reason() == "Heater bed not heating at expected rate");
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: reads the gcode store when the tap saw nothing",
                 "[print_completion][stop_reason]") {
    now_ = kEnd - 120;
    client_.fire_notification("notify_gcode_response", response({"!! An old failure"}));
    now_ = kEnd - 1;
    client_.fire_notification("notify_gcode_response", response({"// Canceling"}));
    settle();

    now_ = kEnd;
    end("cancelled");
    CHECK(reason().empty());
    REQUIRE(client_.reads == 1);
    CHECK(client_.last_count == PrintStopReasonTracker::kGcodeStoreCount);

    // Moonraker's clock runs 500s behind the screen's; the store places itself.
    const double host = kEnd - 500;
    std::vector<GcodeStoreEntry> store(4);
    store[0].type = "response";
    store[0].time = host - 30;
    store[0].message = "!! Too old to matter";
    store[1].type = "response";
    store[1].time = host - 2;
    store[1].message = "!! Quad gantry level failed";
    store[2].type = "response";
    store[2].time = host - 1;
    store[2].message = "// Canceling";
    store[3].type = "command";
    store[3].time = host;
    store[3].message = "CANCEL_PRINT";
    client_.reply(store);
    settle();
    CHECK(reason() == "Quad gantry level failed");
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: an error arriving just after the end fills in",
                 "[print_completion][stop_reason]") {
    end("cancelled");
    CHECK(reason().empty());
    now_ = kEnd + 1;
    client_.fire_notification("notify_gcode_response", response({"!! Probe triggered prior"}));
    settle();
    CHECK(reason() == "Probe triggered prior");
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: a completed print has no reason",
                 "[print_completion][stop_reason]") {
    client_.fire_notification("notify_gcode_response", response({"!! Unknown command"}));
    settle();
    end("complete");
    CHECK(reason().empty());
    CHECK(client_.reads == 0);
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: unregisters its tap",
                 "[print_completion][stop_reason]") {
    tracker_.reset();
    CHECK_FALSE(client_.fire_notification("notify_gcode_response", response({"!! x"})));
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: a pause shows the error that caused it",
                 "[print_completion][stop_reason]") {
    auto& ps = get_printer_state();
    ps.update_from_status(json{{"display_status", {{"message", "Print starting..."}}}});
    settle();
    REQUIRE(lv_subject_get_int(ps.print_state().get_display_message_visible_subject()) == 1);

    now_ = kEnd - 40 * 60;
    client_.fire_notification("notify_gcode_response",
                              response({"!! Error getting data from moonraker"}));
    now_ = kEnd - 1;
    client_.fire_notification("notify_gcode_response", response({kAfcLoadFailure}));
    settle();

    now_ = kEnd;
    end("paused");
    CHECK(
        reason() ==
        "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH");
    // The reason, not the macro's leftover M117, is the status while paused.
    CHECK(lv_subject_get_int(ps.print_state().get_display_message_visible_subject()) == 0);

    end("printing");
    CHECK(reason().empty());
    CHECK(lv_subject_get_int(ps.print_state().get_display_message_visible_subject()) == 1);
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: canceling a paused print keeps the pause's reason",
                 "[print_completion][stop_reason]") {
    now_ = kEnd - 1;
    client_.fire_notification("notify_gcode_response", response({kAfcLoadFailure}));
    settle();
    now_ = kEnd;
    end("paused");
    REQUIRE_FALSE(reason().empty());

    now_ = kEnd + 120;
    end("cancelled");
    CHECK(
        reason() ==
        "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH");
    CHECK(client_.reads == 0);
}

TEST_CASE_METHOD(TrackerFixture, "stop reason tracker: a pause reads the gcode store too",
                 "[print_completion][stop_reason]") {
    // Moonraker's clock is its own; PAUSING, seen live, places the store on ours.
    client_.fire_notification("notify_gcode_response", response({"PAUSING"}));
    settle();
    end("paused");
    REQUIRE(client_.reads == 1);
    std::vector<GcodeStoreEntry> store(3);
    store[0].type = "response";
    store[0].time = 100.0;
    store[0].message = kAfcLoadFailure;
    store[1].type = "response";
    store[1].time = 100.2;
    store[1].message = "PAUSING";
    store[2].type = "response";
    store[2].time = 100.5;
    store[2].message = "// idle_timeout: Timeout set to 43200 s";
    client_.reply(store);
    settle();
    CHECK(
        reason() ==
        "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH");
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: an old error in a quiet printer's store explains nothing",
                 "[print_completion][stop_reason]") {
    // The last line the screen saw, twenty minutes before a pause from the
    // screen that printed nothing.
    now_ = kEnd - 20 * 60;
    client_.fire_notification("notify_gcode_response", response({"// Filament loaded"}));
    settle();
    now_ = kEnd;
    end("paused");
    REQUIRE(client_.reads == 1);

    const double host = 5000.0;
    std::vector<GcodeStoreEntry> store(2);
    store[0].type = "response";
    store[0].time = host - 20 * 60 - 3;
    store[0].message = "!! Heater extruder not heating at expected rate";
    store[1].type = "response";
    store[1].time = host - 20 * 60;
    store[1].message = "// Filament loaded";
    client_.reply(store);
    settle();
    CHECK(reason().empty());
    CHECK(reason_visible() == 0);
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: a store with no line seen live explains nothing",
                 "[print_completion][stop_reason]") {
    end("paused");
    REQUIRE(client_.reads == 1);
    std::vector<GcodeStoreEntry> store(2);
    store[0].type = "response";
    store[0].time = 100.0;
    store[0].message = kAfcLoadFailure;
    store[1].type = "response";
    store[1].time = 100.2;
    store[1].message = "PAUSING";
    client_.reply(store);
    settle();
    CHECK(reason().empty());
}

TEST_CASE("gcode store clock shift: the newest response matching the line seen live",
          "[stop_reason]") {
    std::vector<GcodeStoreEntry> store(3);
    store[0] = {"ok", 10.0, "response"};
    store[1] = {"ok", 20.0, "response"};
    store[2] = {"ok", 30.0, "command"};
    const auto shift = gcode_store_clock_shift(store, "ok", 1000.0);
    REQUIRE(shift.has_value());
    CHECK(*shift == 980.0);
    CHECK_FALSE(gcode_store_clock_shift(store, "missing", 1000.0).has_value());
    CHECK_FALSE(gcode_store_clock_shift({}, "ok", 1000.0).has_value());
}

TEST_CASE_METHOD(TrackerFixture,
                 "stop reason tracker: a tap delivered after the tracker is gone does nothing",
                 "[print_completion][stop_reason]") {
    auto it = client_.method_callbacks.find("notify_gcode_response");
    REQUIRE(it != client_.method_callbacks.end());
    REQUIRE(it->second.size() == 1);
    // The client invokes a copy taken under its lock, after releasing it, so
    // unregistering does not stop a delivery already on its way.
    auto in_flight = it->second.front().callback;

    tracker_.reset();
    int clock_reads = 0;
    tracker_ =
        std::make_unique<PrintStopReasonTracker>(client_, get_printer_state(), [&clock_reads] {
            ++clock_reads;
            return kEnd;
        });
    REQUIRE(client_.method_callbacks["notify_gcode_response"].size() == 1);
    auto counted = client_.method_callbacks["notify_gcode_response"].front().callback;
    tracker_.reset();
    settle();
    REQUIRE(clock_reads == 0);
    const std::string error_before = get_printer_state().print_state().last_gcode_error();

    counted(response({"!! Quad gantry level failed"}));
    in_flight(response({"!! Quad gantry level failed"}));
    CHECK(clock_reads == 0);
    CHECK(ui::UpdateQueueTestAccess::pending_count(ui::UpdateQueue::instance()) == 0);
    settle();
    CHECK(get_printer_state().print_state().last_gcode_error() == error_before);
}
