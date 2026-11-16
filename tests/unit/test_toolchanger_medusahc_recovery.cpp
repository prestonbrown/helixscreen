// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_medusahc_recovery.cpp
 * @brief A swap that fails mid-print pauses the job; Resume needs the target hot.
 *
 * Irbis3D's controller answers a failed swap during a print by pausing it
 * (MHC_ERROR), with no gcode error. A RESUME that runs MHC_RESUME (the upstream
 * example's) picks the target tool again and refuses while that tool's nozzle
 * is at or below 190C. The screen says so while the print is held, and its
 * Resume asks first. A RESUME without MHC_RESUME has no such rule.
 */

#include "../busy_guard_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_toolchanger.h"
#include "core/update_queue.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// A stock MedusaHC: the controller and its docks, no klipper-toolchanger.
PrinterDiscovery stock_medusahc() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"pin_watch io", "medusahc", "toolhead", "extruder", "extruder1",
                                  "extruder2", "extruder3", "gcode_macro T0", "gcode_macro T1",
                                  "gcode_macro T2", "gcode_macro T3", "gcode_macro DROP_TOOL"}));
    return hw;
}

class RecoveryChanger : public AmsBackendToolChanger {
  public:
    RecoveryChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
        : AmsBackendToolChanger(api, client) {
        set_discovery(stock_medusahc());
        running_ = true;
    }
    ~RecoveryChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }
};

/// The printer's RESUME as configfile reports it.
void resume_macro(const std::string& gcode) {
    MacroParamCache::instance().populate_from_configfile(
        json{{"gcode_macro resume", {{"gcode", gcode}}}}, {"RESUME"});
}

/// Upstream's macros_examples.cfg RESUME.
constexpr const char* kUpstreamResume =
    "{% if printer.pause_resume.is_paused %}\n"
    "    {% set tgt = printer[\"gcode_macro _GLOBAL_STATE\"].target_tool|int %}\n"
    "    MHC_RESUME T={tgt}\n"
    "{% else %}\n"
    "    RESPOND MSG=\"RESUME (no print, passthrough)\"\n"
    "{% endif %}";

class RecoveryFixture : public BusyGuardFixture {
  public:
    RecoveryFixture() {
        state.temperature_state().init_extruders(
            {"extruder", "extruder1", "extruder2", "extruder3"});
        resume_macro(kUpstreamResume);
    }
    ~RecoveryFixture() override {
        MacroParamCache::instance().clear();
    }

    void nozzle(const char* heater, double celsius) {
        state.update_from_status(json{{heater, {{"temperature", celsius}, {"target", 0.0}}}});
    }

    /// What the controller publishes once MHC_SET T=2 could not confirm the pick.
    static json failed_pick_of_t2() {
        return json{{"medusahc",
                     {{"operation", "idle"},
                      {"current_tool", -1},
                      {"target_tool", 2},
                      {"sensor_error", true},
                      {"last_error", "MHC_SET: sensors did not confirm T2"}}}};
    }

    /// prepare_for_resume()'s answer, which the contract delivers synchronously
    /// or on the main thread.
    static std::optional<AmsError> resume_answer(AmsBackend& backend) {
        std::optional<AmsError> answer;
        backend.prepare_for_resume(-1, [&](const AmsError& err) { answer = err; });
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
        return answer;
    }
};

} // namespace

TEST_CASE("MedusaHC: Resume after a failed swap needs the target nozzle above 190C",
          "[toolchanger][medusahc][resume]") {
    const auto rule = toolchanger_addon::resume_rule(stock_medusahc());
    CHECK(rule.min_nozzle_c == 190);
    CHECK(rule.command == "MHC_RESUME");

    PrinterDiscovery plain;
    plain.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder"}));
    CHECK(toolchanger_addon::resume_rule(plain).min_nozzle_c == 0);
}

TEST_CASE("MedusaHC: the 190C rule binds only where RESUME runs MHC_RESUME",
          "[toolchanger][medusahc][resume]") {
    const auto rule = toolchanger_addon::resume_rule(stock_medusahc());
    resume_macro(kUpstreamResume);
    CHECK(toolchanger_addon::resume_rule_in_force(rule));
    // A RESUME that reaches it through a macro of its own still runs it.
    MacroParamCache::instance().populate_from_configfile(
        json{{"gcode_macro resume", {{"gcode", "_MY_RESUME"}}},
             {"gcode_macro _my_resume", {{"gcode", "MHC_RESUME T=0"}}}},
        {"RESUME", "_MY_RESUME"});
    CHECK(toolchanger_addon::resume_rule_in_force(rule));
    resume_macro("RESTORE_GCODE_STATE\nBASE_RESUME");
    CHECK_FALSE(toolchanger_addon::resume_rule_in_force(rule));
    MacroParamCache::instance().clear();
    CHECK_FALSE(toolchanger_addon::resume_rule_in_force(rule));
}

TEST_CASE_METHOD(RecoveryFixture, "MedusaHC: a failed swap mid-print says how to resume",
                 "[toolchanger][medusahc][resume]") {
    RecoveryChanger tc(api.get(), &mock_client);
    helix::test::set_wire_state(state, PrintJobState::PAUSED);

    tc.feed(failed_pick_of_t2());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Tool change to Tool 3 (T2) failed: MHC_SET: sensors did not confirm T2. Seat the "
          "hot ends, heat Tool 3 (T2) above 190°C, then tap Resume.");
}

TEST_CASE_METHOD(RecoveryFixture, "MedusaHC: a failed swap with no print says nothing of Resume",
                 "[toolchanger][medusahc][resume]") {
    RecoveryChanger tc(api.get(), &mock_client);

    tc.feed(failed_pick_of_t2());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Tool change to Tool 3 (T2) failed: MHC_SET: sensors did not confirm T2");
}

TEST_CASE_METHOD(RecoveryFixture, "MedusaHC: Resume waits for the target tool to heat",
                 "[toolchanger][medusahc][resume]") {
    RecoveryChanger tc(api.get(), &mock_client);
    helix::test::set_wire_state(state, PrintJobState::PAUSED);
    tc.feed(failed_pick_of_t2());

    nozzle("extruder2", 150.0);
    nozzle("extruder", 230.0); // another tool being hot is no help
    auto cold = resume_answer(tc);
    REQUIRE(cold.has_value());
    CHECK(cold->result == AmsResult::WRONG_STATE);
    CHECK(cold->user_msg == "Heat Tool 3 (T2) above 190°C before resuming");

    nozzle("extruder2", 190.0); // the controller wants it ABOVE 190
    REQUIRE(resume_answer(tc).has_value());
    CHECK_FALSE(resume_answer(tc)->success());

    nozzle("extruder2", 205.0);
    auto hot = resume_answer(tc);
    REQUIRE(hot.has_value());
    CHECK(hot->success());
}

TEST_CASE_METHOD(RecoveryFixture, "MedusaHC: a pause with no failed swap resumes as usual",
                 "[toolchanger][medusahc][resume]") {
    RecoveryChanger tc(api.get(), &mock_client);
    helix::test::set_wire_state(state, PrintJobState::PAUSED);
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 2},
                   {"target_tool", 2},
                   {"sensor_error", false},
                   {"last_error", ""}}}});
    nozzle("extruder2", 25.0);

    auto answer = resume_answer(tc);
    REQUIRE(answer.has_value());
    CHECK(answer->success());
}

TEST_CASE_METHOD(RecoveryFixture,
                 "MedusaHC: a RESUME without MHC_RESUME gets no 190C wording and no gate",
                 "[toolchanger][medusahc][resume]") {
    resume_macro("RESTORE_GCODE_STATE\nBASE_RESUME");
    RecoveryChanger tc(api.get(), &mock_client);
    helix::test::set_wire_state(state, PrintJobState::PAUSED);

    tc.feed(failed_pick_of_t2());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Tool change to Tool 3 (T2) failed: MHC_SET: sensors did not confirm T2");
    nozzle("extruder2", 25.0);
    auto answer = resume_answer(tc);
    REQUIRE(answer.has_value());
    CHECK(answer->success());
}

TEST_CASE_METHOD(RecoveryFixture,
                 "MedusaHC: a pause with another tool mounted waits for the target to heat",
                 "[toolchanger][medusahc][resume]") {
    RecoveryChanger tc(api.get(), &mock_client);
    helix::test::set_wire_state(state, PrintJobState::PAUSED);
    // A manual pause; someone swapped the hot end on the carriage since.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 1},
                   {"target_tool", 2},
                   {"sensor_error", false},
                   {"last_error", ""}}}});
    nozzle("extruder2", 25.0);

    auto cold = resume_answer(tc);
    REQUIRE(cold.has_value());
    CHECK(cold->user_msg == "Heat Tool 3 (T2) above 190°C before resuming");
}
