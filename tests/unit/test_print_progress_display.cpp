// SPDX-License-Identifier: GPL-3.0-or-later
//
// print_progress_display / print_progress_text are the pair the print-status
// bar and its percentage label bind to. They are written by a single call, so
// the two can never show different numbers — the defect this pins is a bar at
// 0% sitting under the text "100%" the instant a print finishes.

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// Read the pair back the way XML bindings do.
int display_pct(PrinterState& ps) {
    return lv_subject_get_int(ps.get_print_progress_display_subject());
}
std::string display_text(PrinterState& ps) {
    return lv_subject_get_string(ps.get_print_progress_text_subject());
}

// Feed a Moonraker status payload through the real parser.
void push_status(PrinterState& ps, const std::string& state, double progress) {
    nlohmann::json status;
    status["print_stats"]["state"] = state;
    status["virtual_sdcard"]["progress"] = progress;
    ps.update_from_status(status);
}

struct ProgressFixture : public HelixTestFixture {
    ProgressFixture() : ps(get_printer_state()) {
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
    }
    PrinterState& ps;
};

} // namespace

TEST_CASE_METHOD(ProgressFixture, "progress display tracks progress during a print",
                 "[print_status][progress]") {
    push_status(ps, "printing", 0.47);

    REQUIRE(display_pct(ps) == 47);
    REQUIRE(display_text(ps) == "47%");
}

TEST_CASE_METHOD(ProgressFixture, "progress display pins 100% on completion",
                 "[print_status][progress]") {
    push_status(ps, "printing", 0.98);
    push_status(ps, "complete", 0.98);

    // A print that ended is 100% even though the last sample was 98%.
    REQUIRE(display_pct(ps) == 100);
    REQUIRE(display_text(ps) == "100%");
}

TEST_CASE_METHOD(ProgressFixture, "bar and text agree through the standby that follows a print",
                 "[print_status][progress]") {
    push_status(ps, "printing", 0.98);
    push_status(ps, "complete", 0.98);

    // Moonraker zeroes virtual_sdcard.progress in the same batch as STANDBY.
    // The raw subject follows it down; the display pair must not.
    push_status(ps, "standby", 0.0);

    REQUIRE(lv_subject_get_int(ps.get_print_progress_subject()) == 0);
    REQUIRE(display_pct(ps) == 100);
    REQUIRE(display_text(ps) == "100%");
}

TEST_CASE_METHOD(ProgressFixture, "cancelled print holds its progress rather than jumping to 100",
                 "[print_status][progress]") {
    push_status(ps, "printing", 0.32);
    push_status(ps, "cancelled", 0.32);
    push_status(ps, "standby", 0.0);

    REQUIRE(display_pct(ps) == 32);
    REQUIRE(display_text(ps) == "32%");
}

TEST_CASE_METHOD(ProgressFixture, "a new print releases the frozen progress",
                 "[print_status][progress]") {
    push_status(ps, "printing", 0.98);
    push_status(ps, "complete", 0.98);
    push_status(ps, "standby", 0.0);
    REQUIRE(display_pct(ps) == 100);

    push_status(ps, "printing", 0.05);

    REQUIRE(display_pct(ps) == 5);
    REQUIRE(display_text(ps) == "5%");
}

TEST_CASE_METHOD(ProgressFixture, "the raw progress subject stays zero before a print",
                 "[print_status][progress]") {
    // Time-estimate seeding keys off print_progress being 0, so the freeze must
    // not leak into the raw subject.
    push_status(ps, "printing", 0.98);
    push_status(ps, "complete", 0.98);
    push_status(ps, "standby", 0.0);

    REQUIRE(lv_subject_get_int(ps.get_print_progress_subject()) == 0);
}
