// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_message.cpp
 * @brief Tests for Klipper display message (M117 / display_status.message) (Issue #124)
 *
 * Klipper's display_status.message carries the LCD message set by M117 gcode
 * commands and macros. HelixScreen parses this into a string subject for UI display.
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "i_moonraker_client.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// ============================================================================
// Basic Message Parsing
// ============================================================================

TEST_CASE("Display message: parses string message from display_status",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("sets message from display_status.message string") {
        json status = {{"display_status", {{"progress", 0.0}, {"message", "Heating bed..."}}}};
        state.update_from_status(status);

        const char* msg = lv_subject_get_string(state.print_state().get_display_message_subject());
        REQUIRE(std::string(msg) == "Heating bed...");
    }

    SECTION("clears message when null") {
        // First set a message
        json set = {{"display_status", {{"message", "Purging nozzle"}}}};
        state.update_from_status(set);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "Purging nozzle");

        // Then clear it (null)
        json clear = {{"display_status", {{"message", nullptr}}}};
        state.update_from_status(clear);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "");
    }

    SECTION("clears message with empty string") {
        json set = {{"display_status", {{"message", "Layer 5/100"}}}};
        state.update_from_status(set);

        json clear = {{"display_status", {{"message", ""}}}};
        state.update_from_status(clear);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "");
    }

    SECTION("message updates independently of progress") {
        json status = {{"display_status", {{"message", "QGL in progress..."}}}};
        state.update_from_status(status);

        const char* msg = lv_subject_get_string(state.print_state().get_display_message_subject());
        REQUIRE(std::string(msg) == "QGL in progress...");
    }
}

// ============================================================================
// Clearing Semantics
// ============================================================================

TEST_CASE("Display message: survives the transition into PRINTING", "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Regression for the delta-clobber bug. A PRINT_START macro emits M117
    // before print_stats.state flips to "printing", so the message arrives in
    // an EARLIER notification. Moonraker sends deltas, so Klipper will never
    // re-send this value. If the PRINTING transition clears it, it is gone for
    // the whole print — which is exactly what users reported.
    json m117 = {{"display_status", {{"message", "Heating bed to 60"}}}};
    state.update_from_status(m117);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Heating bed to 60");

    // Separate notification, no display_status key at all (a true delta).
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Heating bed to 60");
    REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);
}

TEST_CASE("Display message: cleared at print end", "[print][display_message]") {
    lv_init_safe();

    auto run_end_state = [](const char* end_state) {
        PrinterState& state = get_printer_state();
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);

        json printing = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing);

        json msg = {{"display_status", {{"message", "Layer 47/120"}}}};
        state.update_from_status(msg);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "Layer 47/120");

        json done = {{"print_stats", {{"state", end_state}}}};
        state.update_from_status(done);

        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "");
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 0);
    };

    SECTION("complete") {
        run_end_state("complete");
    }
    SECTION("cancelled") {
        run_end_state("cancelled");
    }
    SECTION("error") {
        run_end_state("error");
    }
}

TEST_CASE("Display message: END_PRINT M117 survives the print-end clear",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Print ends first...
    json done = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(done);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "");

    // ...then an END_PRINT macro's M117 lands in a later notification.
    // It must display, not be swallowed.
    json farewell = {{"display_status", {{"message", "Print complete - remove part"}}}};
    state.update_from_status(farewell);

    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Print complete - remove part");
    REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);

    // A later frame repeating the terminal state must NOT re-clear. This is
    // what discriminates edge- from level-triggered: a level-triggered clear
    // would fire again here and swallow the farewell message.
    json still_complete = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(still_complete);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Print complete - remove part");
    REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);
}

TEST_CASE("Display message: cleared on abnormal exit to standby (no terminal state)",
          "[print][display_message]") {
    lv_init_safe();

    auto run_abnormal_exit = [](const char* active_state) {
        PrinterState& state = get_printer_state();
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);

        json active = {{"print_stats", {{"state", active_state}}}};
        state.update_from_status(active);

        json msg = {{"display_status", {{"message", "Layer 47/120"}}}};
        state.update_from_status(msg);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "Layer 47/120");

        // Klipper restart / SDCARD_RESET_FILE / firmware cancel: jumps straight
        // to standby WITHOUT passing through complete/cancelled/error.
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "");
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 0);
    };

    SECTION("printing -> standby") {
        run_abnormal_exit("printing");
    }
    SECTION("paused -> standby") {
        run_abnormal_exit("paused");
    }
}

TEST_CASE("Display message: normal end-of-print sequence leaves the END_PRINT "
          "farewell message intact through the complete->standby transition",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // This is the negative case for the abnormal-exit clear above: the normal
    // path is printing -> complete -> standby, and an END_PRINT macro's M117
    // lands between the complete and standby transitions. If STANDBY cleared
    // unconditionally, this farewell message would be wiped a second time.
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    json done = {{"print_stats", {{"state", "complete"}}}};
    state.update_from_status(done);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "");

    json farewell = {{"display_status", {{"message", "Print complete - remove part"}}}};
    state.update_from_status(farewell);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Print complete - remove part");

    // Printer settles back to standby after the operator clears the bed / the
    // idle timeout fires. This must NOT clear the farewell message.
    json standby = {{"print_stats", {{"state", "standby"}}}};
    state.update_from_status(standby);

    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_display_message_subject())) ==
            "Print complete - remove part");
    REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);
}

// ============================================================================
// Subscription responses for an ended job
// ============================================================================

namespace {
std::string shown_message(PrinterState& state) {
    return lv_subject_get_string(state.print_state().get_display_message_subject());
}
int message_visible(PrinterState& state) {
    return lv_subject_get_int(state.print_state().get_display_message_visible_subject());
}
json ended_frame(const char* end_state, const char* message) {
    return {{"print_stats", {{"state", end_state}, {"filename", "cover_ASA_7h44m.gcode"}}},
            {"display_status", {{"progress", 0.0}, {"message", message}}}};
}

enum class Subscription { DiscoveryReplay, Refresh };

/// Delivers @p status the way a dispatched printer.objects.subscribe response
/// reaches PrinterState: as a notification carrying the provenance markers.
void apply_subscription(PrinterState& state, const json& status, Subscription kind) {
    json notification = {{"method", "notify_status_update"},
                         {"params", json::array({status, 0.0})},
                         {kWholeObjectsMarker, true}};
    if (kind == Subscription::DiscoveryReplay) {
        notification[kCachedSnapshotMarker] = true;
    }
    auto frame = parse_status_notification(notification);
    REQUIRE(frame);
    state.update_from_status(*frame->status, frame->eventtime, frame->from_cached_snapshot,
                             std::nullopt, frame->whole_objects);
}
} // namespace

TEST_CASE("Display message: a subscription response for an ended job does not show its "
          "leftover M117",
          "[print][display_message]") {
    lv_init_safe();

    auto run = [](const char* end_state, Subscription kind) {
        PrinterState& state = get_printer_state();
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);

        // Klipper still holds PRINT_START's M117 after the job ended.
        apply_subscription(state, ended_frame(end_state, "Print starting..."), kind);

        REQUIRE(shown_message(state).empty());
        REQUIRE(message_visible(state) == 0);

        // A message the printer sets afterwards is live and shows.
        state.update_from_status({{"display_status", {{"message", "Bed cooled"}}}});
        REQUIRE(shown_message(state) == "Bed cooled");
        REQUIRE(message_visible(state) == 1);
    };

    SECTION("cancelled, discovery replay") {
        run("cancelled", Subscription::DiscoveryReplay);
    }
    SECTION("complete, discovery replay") {
        run("complete", Subscription::DiscoveryReplay);
    }
    SECTION("error, discovery replay") {
        run("error", Subscription::DiscoveryReplay);
    }
    SECTION("cancelled, subscription refresh") {
        run("cancelled", Subscription::Refresh);
    }
}

TEST_CASE("Display message: a subscription refresh after connect keeps the leftover hidden",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    apply_subscription(state, ended_frame("cancelled", "Print starting..."),
                       Subscription::DiscoveryReplay);
    REQUIRE(shown_message(state).empty());

    // A plugin subscribing an extra object re-sends every app object in full.
    json refresh = ended_frame("cancelled", "Print starting...");
    refresh["temperature_sensor plugin_x"] = {{"temperature", 24.0}};
    apply_subscription(state, refresh, Subscription::Refresh);

    REQUIRE(shown_message(state).empty());
    REQUIRE(message_visible(state) == 0);
}

TEST_CASE("Display message: a reconnect replay keeps the message shown live",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    state.update_from_status({{"print_stats", {{"state", "cancelled"}}}});
    state.update_from_status({{"display_status", {{"message", "Print cancelled"}}}});
    REQUIRE(shown_message(state) == "Print cancelled");

    SECTION("replay carrying the same message") {
        apply_subscription(state, ended_frame("cancelled", "Print cancelled"),
                           Subscription::DiscoveryReplay);
    }
    SECTION("replay captured before the live message landed") {
        apply_subscription(state, ended_frame("cancelled", "Print starting..."),
                           Subscription::DiscoveryReplay);
    }

    REQUIRE(shown_message(state) == "Print cancelled");
    REQUIRE(message_visible(state) == 1);
}

TEST_CASE("Display message: the subscription rule is limited to ended jobs",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("live frame ending the job together with a farewell M117") {
        state.update_from_status({{"print_stats", {{"state", "printing"}}}});
        state.update_from_status(ended_frame("cancelled", "Print cancelled"));
        REQUIRE(shown_message(state) == "Print cancelled");
        REQUIRE(message_visible(state) == 1);
    }
    SECTION("subscription response for an idle printer") {
        apply_subscription(
            state,
            {{"print_stats", {{"state", "standby"}}}, {"display_status", {{"message", "Ready"}}}},
            Subscription::DiscoveryReplay);
        REQUIRE(shown_message(state) == "Ready");
        REQUIRE(message_visible(state) == 1);
    }
    SECTION("subscription response for a running print") {
        apply_subscription(state,
                           {{"print_stats", {{"state", "printing"}}},
                            {"display_status", {{"message", "Layer 3/120"}}}},
                           Subscription::Refresh);
        REQUIRE(shown_message(state) == "Layer 3/120");
        REQUIRE(message_visible(state) == 1);
    }
}

// ============================================================================
// Initial State
// ============================================================================

TEST_CASE("Display message: initializes empty", "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    const char* msg = lv_subject_get_string(state.print_state().get_display_message_subject());
    REQUIRE(std::string(msg) == "");
}

// ============================================================================
// Visibility Subject
// ============================================================================

TEST_CASE("Display message: visibility subject tracks non-empty state",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("visible=0 initially") {
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 0);
    }

    SECTION("visible=1 when message set") {
        json status = {{"display_status", {{"message", "Heating..."}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);
    }

    SECTION("visible=0 when message cleared with null") {
        json set = {{"display_status", {{"message", "Hello"}}}};
        state.update_from_status(set);
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);

        json clear = {{"display_status", {{"message", nullptr}}}};
        state.update_from_status(clear);
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 0);
    }

    SECTION("visible=0 when message cleared with empty string") {
        json set = {{"display_status", {{"message", "Hello"}}}};
        state.update_from_status(set);

        json clear = {{"display_status", {{"message", ""}}}};
        state.update_from_status(clear);
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 0);
    }

    SECTION("visible=1 during print preparation (M117 shows through heating/QGL/purge)") {
        // Pre-print is where macro authors put the most M117 traffic. The
        // phase label lives in print_start_message; this row is the user's text.
        state.print_state().set_print_start_state(PrintStartPhase::HEATING_BED, "Heating Bed...",
                                                  30);
        helix::ui::UpdateQueue::instance().drain();

        json status = {{"display_status", {{"message", "Heating..."}}}};
        state.update_from_status(status);
        REQUIRE(std::string(lv_subject_get_string(
                    state.print_state().get_display_message_subject())) == "Heating...");
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);

        // Still visible at COMPLETE, which is itself a non-IDLE phase and
        // previously suppressed the row for an entire print.
        state.print_state().set_print_start_state(PrintStartPhase::COMPLETE, "Done", 100);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);

        // And after returning to IDLE.
        state.print_state().reset_print_start_state();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(state.print_state().get_display_message_visible_subject()) == 1);
    }
}

// ============================================================================
// Long Message Handling
// ============================================================================

TEST_CASE("Display message: truncates long messages safely", "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Send a message longer than the 128-byte buffer
    std::string long_msg(200, 'A');
    json status = {{"display_status", {{"message", long_msg}}}};
    state.update_from_status(status);

    const char* msg = lv_subject_get_string(state.print_state().get_display_message_subject());
    // Should not crash, and should contain some content
    REQUIRE(std::strlen(msg) > 0);
    REQUIRE(std::strlen(msg) < 200); // Truncated
}

// ============================================================================
// print_active parity (guards the declarative visibility binding)
// ============================================================================

TEST_CASE("print_active tracks PrintJobState across a full job lifecycle",
          "[print][display_message]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    struct Step {
        const char* moonraker_state;
        int expected_active;
    };
    const Step steps[] = {
        {"standby", 0},  {"printing", 1}, {"paused", 1},    {"printing", 1},
        {"complete", 0}, {"standby", 0},  {"cancelled", 0}, {"error", 0},
    };

    for (const auto& s : steps) {
        json status = {{"print_stats", {{"state", s.moonraker_state}}}};
        state.update_from_status(status);
        helix::ui::UpdateQueue::instance().drain();

        INFO("moonraker state: " << s.moonraker_state);
        REQUIRE(lv_subject_get_int(state.print_state().get_print_active_subject()) ==
                s.expected_active);
    }

    // Delta persistence: Moonraker sends deltas, so most frames carry no
    // print_stats at all. print_active must RETAIN its value across those —
    // recomputing it to 0 would hide the print card mid-print.
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(state.print_state().get_print_active_subject()) == 1);

    // A frame with no print_stats key whatsoever.
    state.update_from_status({{"virtual_sdcard", {{"progress", 0.5}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(state.print_state().get_print_active_subject()) == 1);

    // And an empty frame.
    state.update_from_status(json::object());
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(state.print_state().get_print_active_subject()) == 1);
}
