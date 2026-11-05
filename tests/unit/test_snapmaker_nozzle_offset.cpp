// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_snapmaker_nozzle_offset.cpp
 * @brief The U1 multi-toolhead offset calibration plan and its runner
 *
 * The invariant the runner tests pin is the exit discipline: every command
 * after PRESTART refuses outside the firmware's XYZ_OFFSET_CALIBRATE state,
 * and EXIT is the only way out, so a finished, failed or cancelled run sends
 * it exactly once and a run that never started sends nothing.
 */

#include "ui_snapmaker_nozzle_offset_overlay.h"

#include "printer_discovery.h"
#include "snapmaker_nozzle_offset.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::snapmaker::nozzle_offset;

namespace {

nlohmann::json stock_u1_objects() {
    nlohmann::json objects = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder_offset_calibration",
         "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART", "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT",
         "gcode_macro EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL"});
    for (int t = 0; t < kHeads; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    return objects;
}

nlohmann::json without(nlohmann::json objects, const std::string& name) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& o : objects) {
        if (o.get<std::string>() != name) {
            out.push_back(o);
        }
    }
    return out;
}

/// Records every command the runner sends and lets the test finish them.
struct Harness {
    struct Sent {
        std::string gcode;
        uint32_t timeout_ms;
        Runner::Completion done;
    };
    std::vector<Sent> sent;
    int aborts = 0;
    int changes = 0;
    Runner runner{Runner::Hooks{
        [this](const std::string& gcode, uint32_t timeout_ms, Runner::Completion done) {
            sent.push_back({gcode, timeout_ms, std::move(done)});
        },
        [this]() { ++aborts; }, [this]() { ++changes; }}};

    /// Completes the most recent command.
    void finish(bool ok = true, const std::string& error = {}) {
        REQUIRE_FALSE(sent.empty());
        auto done = sent.back().done;
        done(ok, error);
    }

    /// Completes commands and confirms prompts until the runner is waiting on
    /// @p gcode (sent, not yet finished) or leaves the run.
    void advance_to(const std::string& gcode) {
        for (int guard = 0; guard < 100; ++guard) {
            if (runner.phase() == Runner::Phase::AwaitingUser) {
                runner.confirm();
                continue;
            }
            if (runner.phase() != Runner::Phase::Running) {
                return;
            }
            if (sent.back().gcode == gcode) {
                return;
            }
            finish();
        }
        FAIL("runner did not reach " << gcode);
    }

    [[nodiscard]] int exits() const {
        int n = 0;
        for (const auto& s : sent) {
            n += s.gcode == kExitGcode ? 1 : 0;
        }
        return n;
    }
};

} // namespace

TEST_CASE("nozzle offset calibration is offered only with the firmware's macros",
          "[snapmaker][nozzle_offset]") {
    SECTION("the stock U1 set") {
        helix::PrinterDiscovery d;
        d.parse_objects(stock_u1_objects());
        REQUIRE(supported(d));
    }
    SECTION("no PRESTART hook") {
        helix::PrinterDiscovery d;
        d.parse_objects(
            without(stock_u1_objects(), "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART"));
        REQUIRE_FALSE(supported(d));
    }
    SECTION("no EXIT hook") {
        helix::PrinterDiscovery d;
        d.parse_objects(without(stock_u1_objects(), "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"));
        REQUIRE_FALSE(supported(d));
    }
    SECTION("one head's step missing") {
        helix::PrinterDiscovery d;
        d.parse_objects(
            without(stock_u1_objects(), "gcode_macro _EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_T3"));
        REQUIRE_FALSE(supported(d));
    }
    SECTION("a printer with none of it") {
        helix::PrinterDiscovery d;
        d.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "toolhead"}));
        REQUIRE_FALSE(supported(d));
    }
}

TEST_CASE("the plan follows the stock screen's order", "[snapmaker][nozzle_offset]") {
    const auto steps = build_sequence();
    std::vector<std::string> gcode;
    for (const auto& s : steps) {
        gcode.push_back(s.gcode);
    }
    std::vector<std::string> expected = {"EXTRUDER_OFFSET_ACTION_PRESTART"};
    for (int t = 0; t < kHeads; ++t) {
        const std::string id = "T" + std::to_string(t);
        expected.push_back("EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=" + id);
        expected.push_back("EXTRUDER_OFFSET_ACTION_AUTO_CLEAN TOOL_ID=" + id +
                           " SAFE_MOVE_Y_POS=250");
        expected.push_back("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=" + id);
        expected.push_back("EXTRUDER_OFFSET_ACTION_WAIT_COOL TOOL_ID=" + id + " TEMP=140");
    }
    expected.push_back("EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0");
    for (int t = 0; t < kHeads; ++t) {
        expected.push_back("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T" + std::to_string(t));
    }
    REQUIRE(gcode == expected);

    SECTION("the user cleans each nozzle after it is parked, before it cools") {
        for (const auto& s : steps) {
            const bool cool = s.kind == StepKind::WaitCool;
            REQUIRE((s.prompt_before == Prompt::CleanNozzle) == cool);
        }
    }
    SECTION("the PEI sheet comes off before the plate check, and nowhere else") {
        int plate_prompts = 0;
        for (const auto& s : steps) {
            if (s.prompt_before == Prompt::RemovePlate) {
                ++plate_prompts;
                REQUIRE(s.kind == StepKind::DetectPlate);
            }
        }
        REQUIRE(plate_prompts == 1);
    }
    SECTION("per-head steps name their head") {
        for (const auto& s : steps) {
            const bool machine_wide =
                s.kind == StepKind::Prestart || s.kind == StepKind::DetectPlate;
            REQUIRE((s.tool == -1) == machine_wide);
        }
    }
}

TEST_CASE("a coded firmware fault reads as words", "[snapmaker][nozzle_offset]") {
    SECTION("the firmware's own msg when we have no wording for the code") {
        REQUIRE(
            failure_text(R"({"coded": "0003-0530-0000-0009", "msg":"Operation requires state"})") ==
            "Operation requires state");
    }
    SECTION("plain text passes through") {
        REQUIRE(failure_text("Probe samples exceed tolerance") == "Probe samples exceed tolerance");
    }
    SECTION("a malformed object passes through as it arrived") {
        REQUIRE(failure_text(R"({"coded": broken)") == R"({"coded": broken)");
    }
}

TEST_CASE("results come from each head's extruder_offset", "[snapmaker][nozzle_offset]") {
    nlohmann::json response = {{"result",
                                {{"status",
                                  {{"extruder", {{"extruder_offset", {0.07, -0.03, 0.0}}}},
                                   {"extruder1", {{"extruder_offset", {0.08, 0.03, 0.09}}}},
                                   {"extruder2", {{"extruder_offset", {0.01, 0.02, 0.27}}}},
                                   {"extruder3", {{"extruder_offset", {-0.08, -0.01, 0.06}}}}}}}}};
    const auto offsets = offsets_from_query(response);
    REQUIRE(offsets.has_value());
    REQUIRE(offsets->size() == 4);
    REQUIRE((*offsets)[0][0] == Catch::Approx(0.07));
    REQUIRE((*offsets)[2][2] == Catch::Approx(0.27));
    REQUIRE((*offsets)[3][1] == Catch::Approx(-0.01));

    SECTION("a missing head reads as no result") {
        response["result"]["status"].erase("extruder2");
        REQUIRE_FALSE(offsets_from_query(response).has_value());
    }
    SECTION("the query asks for exactly those fields") {
        const auto params = offsets_query_params();
        for (const char* name : {"extruder", "extruder1", "extruder2", "extruder3"}) {
            REQUIRE(params["objects"][name] == nlohmann::json::array({"extruder_offset"}));
        }
    }
}

TEST_CASE("a full run walks every step and exits once", "[snapmaker][nozzle_offset]") {
    Harness h;
    h.runner.start();
    REQUIRE(h.runner.phase() == Runner::Phase::Running);
    REQUIRE(h.sent.size() == 1);
    REQUIRE(h.sent[0].gcode == "EXTRUDER_OFFSET_ACTION_PRESTART");
    REQUIRE(h.sent[0].timeout_ms == kStepTimeoutMs);

    SECTION("a parked nozzle waits for the user before it cools") {
        h.advance_to("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T0");
        const size_t before = h.sent.size();
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::AwaitingUser);
        REQUIRE(h.runner.prompt() == Prompt::CleanNozzle);
        REQUIRE(h.runner.current_step()->tool == 0);
        REQUIRE(h.sent.size() == before);
        h.runner.confirm();
        REQUIRE(h.runner.phase() == Runner::Phase::Running);
        REQUIRE(h.sent.back().gcode == "EXTRUDER_OFFSET_ACTION_WAIT_COOL TOOL_ID=T0 TEMP=140");
    }

    SECTION("the last probe ends in EXIT, then Done") {
        h.advance_to("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T3");
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::Finishing);
        REQUIRE(h.sent.back().gcode == kExitGcode);
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::Done);
        REQUIRE(h.exits() == 1);
        REQUIRE(h.aborts == 0);
    }
}

TEST_CASE("a failed step exits and reports why", "[snapmaker][nozzle_offset]") {
    Harness h;
    h.runner.start();
    h.advance_to("EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=T1");
    h.finish(false, R"({"coded": "0003-0530-0000-0009", "msg":"Heater extruder1 not heating"})");
    REQUIRE(h.runner.phase() == Runner::Phase::Failed);
    REQUIRE(h.runner.error() == "Heater extruder1 not heating");
    REQUIRE(h.sent.back().gcode == kExitGcode);
    REQUIRE(h.exits() == 1);

    SECTION("the EXIT's own completion leaves the failure standing") {
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::Failed);
        REQUIRE(h.runner.error() == "Heater extruder1 not heating");
    }
    SECTION("cancelling a failed run sends nothing more") {
        h.runner.cancel();
        REQUIRE(h.exits() == 1);
    }
}

TEST_CASE("a PEI sheet still on the bed asks again instead of failing",
          "[snapmaker][nozzle_offset]") {
    Harness h;
    h.runner.start();
    h.advance_to("EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0");
    h.finish(false, R"({"coded": "0003-0530-0000-0011", "msg":"The plate has not been removed"})");
    REQUIRE(h.runner.phase() == Runner::Phase::AwaitingUser);
    REQUIRE(h.runner.prompt() == Prompt::RemovePlate);
    REQUIRE_FALSE(h.runner.error().empty());
    REQUIRE(h.exits() == 0);

    h.runner.confirm();
    REQUIRE(h.sent.back().gcode == "EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0");
    h.finish();
    REQUIRE(h.runner.error().empty());
    REQUIRE(h.sent.back().gcode == "EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T0");

    SECTION("any other plate-check failure ends the run") {
        Harness other;
        other.runner.start();
        other.advance_to("EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0");
        other.finish(false, "Probe triggered prior to movement");
        REQUIRE(other.runner.phase() == Runner::Phase::Failed);
        REQUIRE(other.exits() == 1);
    }
}

TEST_CASE("cancel exits once and drops late completions", "[snapmaker][nozzle_offset]") {
    SECTION("before start nothing is sent") {
        Harness h;
        h.runner.cancel();
        REQUIRE(h.sent.empty());
        REQUIRE(h.runner.phase() == Runner::Phase::Idle);
    }
    SECTION("mid-step: EXIT goes out and the step's late answer is ignored") {
        Harness h;
        h.runner.start();
        h.advance_to("EXTRUDER_OFFSET_ACTION_AUTO_CLEAN TOOL_ID=T0 SAFE_MOVE_Y_POS=250");
        auto in_flight = h.sent.back().done;
        h.runner.cancel();
        REQUIRE(h.runner.phase() == Runner::Phase::Cancelled);
        REQUIRE(h.sent.back().gcode == kExitGcode);
        REQUIRE(h.aborts == 0);
        const size_t count = h.sent.size();
        in_flight(true, {});
        REQUIRE(h.runner.phase() == Runner::Phase::Cancelled);
        REQUIRE(h.sent.size() == count);
    }
    SECTION("while waiting on the user") {
        Harness h;
        h.runner.start();
        h.advance_to("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T0");
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::AwaitingUser);
        h.runner.cancel();
        REQUIRE(h.exits() == 1);
        h.runner.confirm();
        REQUIRE(h.exits() == 1);
        REQUIRE(h.sent.back().gcode == kExitGcode);
    }
    SECTION("during a probe the pass is aborted before EXIT") {
        Harness h;
        h.runner.start();
        h.advance_to("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T1");
        h.runner.cancel();
        REQUIRE(h.aborts == 1);
        REQUIRE(h.exits() == 1);
    }
    SECTION("twice is still one EXIT") {
        Harness h;
        h.runner.start();
        h.runner.cancel();
        h.runner.cancel();
        REQUIRE(h.exits() == 1);
    }
    SECTION("after Done it sends nothing") {
        Harness h;
        h.runner.start();
        h.advance_to("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T3");
        h.finish();
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::Done);
        h.runner.cancel();
        REQUIRE(h.exits() == 1);
        REQUIRE(h.runner.phase() == Runner::Phase::Done);
    }
    SECTION("reset forgets an ended run but never a live one") {
        Harness h;
        h.runner.start();
        h.runner.reset();
        REQUIRE(h.runner.phase() == Runner::Phase::Running);
        h.advance_to("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T0");
        h.finish();
        h.runner.reset();
        REQUIRE(h.runner.phase() == Runner::Phase::AwaitingUser);
        h.runner.cancel();
        h.runner.reset();
        REQUIRE(h.runner.phase() == Runner::Phase::Idle);
        REQUIRE(h.runner.current_step() == nullptr);
    }
    SECTION("a fresh start after a cancel runs from the top") {
        Harness h;
        h.runner.start();
        h.runner.cancel();
        h.runner.start();
        REQUIRE(h.runner.phase() == Runner::Phase::Running);
        REQUIRE(h.runner.step_index() == 0);
        REQUIRE(h.sent.back().gcode == "EXTRUDER_OFFSET_ACTION_PRESTART");
    }
}

TEST_CASE("each phase maps to one screen", "[snapmaker][nozzle_offset]") {
    using helix::ui::nozzle_offset_screen;
    using Screen = helix::ui::SnapmakerNozzleOffsetOverlay::Screen;
    using P = Runner::Phase;
    REQUIRE(nozzle_offset_screen(P::Idle, Prompt::None) == Screen::Intro);
    REQUIRE(nozzle_offset_screen(P::Cancelled, Prompt::None) == Screen::Intro);
    REQUIRE(nozzle_offset_screen(P::Running, Prompt::None) == Screen::Working);
    REQUIRE(nozzle_offset_screen(P::Finishing, Prompt::None) == Screen::Working);
    REQUIRE(nozzle_offset_screen(P::AwaitingUser, Prompt::CleanNozzle) == Screen::CleanNozzle);
    REQUIRE(nozzle_offset_screen(P::AwaitingUser, Prompt::RemovePlate) == Screen::RemovePlate);
    REQUIRE(nozzle_offset_screen(P::Done, Prompt::None) == Screen::Done);
    REQUIRE(nozzle_offset_screen(P::Failed, Prompt::None) == Screen::Failed);
}

TEST_CASE("a held calibration state is ended only when a run here owned it",
          "[snapmaker][nozzle_offset][reconcile]") {
    const nlohmann::json held = {{"machine_state_manager", {{"main_state", 2}}}};
    REQUIRE(held_state_action(held, false, true) == HeldState::Exit);
    REQUIRE(held_state_action(held, false, false) == HeldState::AskUser);
    REQUIRE(held_state_action(held, true, true) == HeldState::None);
    REQUIRE(held_state_action(held, true, false) == HeldState::None);
    REQUIRE(held_state_action({{"result", {{"status", held}}}}, false, false) ==
            HeldState::AskUser);

    SECTION("any other main state is not ours to leave") {
        REQUIRE(held_state_action({{"machine_state_manager", {{"main_state", 0}}}}, false, true) ==
                HeldState::None);
        // SCREWS_TILT_ADJUST belongs to the screws-tilt reconcile.
        REQUIRE(held_state_action({{"machine_state_manager", {{"main_state", 8}}}}, false, true) ==
                HeldState::None);
    }
    SECTION("no state reported reads as nothing to clear") {
        REQUIRE(held_state_action(nlohmann::json::object(), false, true) == HeldState::None);
        REQUIRE(held_state_action({{"machine_state_manager", {{"main_state", "2"}}}}, false,
                                  true) == HeldState::None);
    }
}

TEST_CASE("an EXIT that never lands leaves the exit pending",
          "[snapmaker][nozzle_offset][reconcile]") {
    clear_exit_pending();
    Harness h;
    h.runner.start();
    h.advance_to("EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=T1");

    SECTION("a step lost to a disconnect, then its EXIT too") {
        h.finish(false, "Websocket disconnected");
        REQUIRE(h.sent.back().gcode == kExitGcode);
        REQUIRE_FALSE(exit_pending());
        h.finish(false, "Not connected to the printer");
        REQUIRE(exit_pending());
    }
    SECTION("a cancel whose EXIT fails") {
        h.runner.cancel();
        h.finish(false, "Not connected to the printer");
        REQUIRE(exit_pending());
        SECTION("the next run owns the state and forgets it") {
            h.runner.start();
            REQUIRE_FALSE(exit_pending());
        }
    }
    SECTION("an EXIT that lands clears it") {
        mark_exit_pending();
        h.runner.cancel();
        h.finish();
        REQUIRE_FALSE(exit_pending());
    }
    clear_exit_pending();
}

TEST_CASE("Runner::probing names only a probe pass in flight", "[snapmaker][nozzle_offset]") {
    Harness h;
    REQUIRE_FALSE(h.runner.probing());
    h.runner.start();
    h.advance_to("EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0");
    REQUIRE_FALSE(h.runner.probing());
    h.advance_to("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T0");
    REQUIRE(h.runner.probing());
    h.runner.cancel();
    REQUIRE_FALSE(h.runner.probing());
    clear_exit_pending();
}

TEST_CASE("a run is live from start until it ends", "[snapmaker][nozzle_offset][reconcile]") {
    REQUIRE_FALSE(run_live());
    {
        Harness h;
        h.runner.start();
        REQUIRE(run_live());
        h.advance_to("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T0");
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::AwaitingUser);
        REQUIRE(run_live());
        h.runner.cancel();
        REQUIRE_FALSE(run_live());

        h.runner.start();
        h.advance_to("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T3");
        h.finish();
        REQUIRE(h.runner.phase() == Runner::Phase::Finishing);
        REQUIRE(run_live());
        h.finish();
        REQUIRE_FALSE(run_live());

        h.runner.start();
        REQUIRE(run_live());
    }
    // A runner destroyed mid-run stops counting.
    REQUIRE_FALSE(run_live());
}

TEST_CASE("a run refused at start sends nothing and says why", "[snapmaker][nozzle_offset]") {
    std::vector<std::string> sent;
    std::string refusal = "Printer is busy with a print";
    Runner runner{Runner::Hooks{
        [&](const std::string& gcode, uint32_t, Runner::Completion) { sent.push_back(gcode); },
        nullptr, nullptr, [&]() { return refusal; }}};
    runner.start();
    CHECK(runner.phase() == Runner::Phase::Failed);
    CHECK(runner.error() == "Printer is busy with a print");
    CHECK(sent.empty());

    refusal.clear();
    runner.reset();
    runner.start();
    CHECK(runner.phase() == Runner::Phase::Running);
    CHECK_FALSE(sent.empty());
    runner.cancel();
}
