// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_offer_preparing.cpp
 * @brief Power-loss recovery must not ambush a start the user already committed to.
 *
 * Run with: ./build/bin/helix-tests "[plr][print_state]"
 *
 * PlrOfferController computed its `printer_idle` signal from
 * is_active_print_state(print_stats.state), which counts only PRINTING and
 * PAUSED. During a host-side pre-print block the wire still reads standby, so
 * the controller considered the printer idle and offered "Resume interrupted
 * print?" on top of a start already under way - a modal ambush whose Resume
 * button starts a DIFFERENT file than the one the user just chose.
 *
 * The decision is asserted through evaluate_offer() + the one-shot latch rather
 * than by rendering the modal; test_plr_prompt.cpp explains why the rendering
 * itself is deliberately not covered here.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plr_offer_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "plr_offer_controller.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrintJobState;
using helix::PrintStartPhase;
using helix::ui::PlrOfferController;
using json = nlohmann::json;

namespace {

class PlrOfferPreparingFixture : public LVGLTestFixture {
  public:
    PlrOfferPreparingFixture() {
        auto& ps = get_printer_state();
        // The global PrinterState is shared across the shard: reset and
        // re-init, or a prior case's subjects decide this one's answers.
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        // A validated Snapmaker snapshot: the passive backend, so availability
        // needs no probe and the offer decision reduces to the idle signal.
        ps.update_from_status(
            json{{"print_stats", {{"state", "standby"}}},
                 {"virtual_sdcard",
                  {{"pl_env_valid", true}, {"file_path", "gcodes/interrupted.gcode"}}}});
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        REQUIRE(ps.is_pl_env_valid());
        REQUIRE_FALSE(ps.pl_recovery_file().empty());
    }

    ~PlrOfferPreparingFixture() override {
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    /// set_print_start_state defers, and its callback republishes the lifecycle.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Put the app in the state it is in while running the user's own pre-start
    /// block: committed to a job, printer still reporting standby.
    static void enter_host_side_preparing(helix::PrinterState& ps) {
        ps.begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
        ps.set_print_start_state(PrintStartPhase::HOMING, "", 0);
        settle();
        REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.get_print_lifecycle() == PrintState::Preparing);
    }
};

} // namespace

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR offers recovery when the printer is truly idle",
                 "[plr][print_state]") {
    // Non-vacuity baseline. Without this, every suppression assertion below
    // would also pass against a controller that never offers at all.
    PlrOfferController controller;
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer while a print is running",
                 "[plr][print_state]") {
    auto& ps = get_printer_state();

    SECTION("printing") {
        helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        helix::test::set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer during a host-side pre-print block",
                 "[plr][print_state]") {
    // THE BUG. print_stats still says standby, so the wire-only idle test said
    // "idle" and the recovery prompt landed on top of the start.
    auto& ps = get_printer_state();
    enter_host_side_preparing(ps);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture,
                 "PLR does not offer during a firmware-side PRINT_START either",
                 "[plr][print_state]") {
    // The other half of Preparing: Klipper already reports printing because the
    // pre-print work lives inside PRINT_START. The wire caught this one already;
    // it must stay caught.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    ps.set_print_start_state(PrintStartPhase::HOMING, "", 0);
    settle();
    REQUIRE(ps.get_print_lifecycle() == PrintState::Preparing);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}
