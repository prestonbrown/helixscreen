// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_skip_toggles.cpp
 * @brief The leveling-skip toggles helix_skips.cfg makes possible, end to end
 *        through PrinterState and PrintPreparationManager.
 *
 * A toggle is offered for a step the loaded wrappers cover, and only while its
 * gate is open: a mesh loaded for bed mesh, the level applied for QGL. Turning
 * it off sends a flag line before START_PRINT; no plugin is involved. A skip
 * left set by an earlier print is reset before the next one and when a job
 * ends.
 */

#include "ui_print_preparation_manager.h"

#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "gcode_ops_detector.h"
#include "lvgl_test_fixture.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preprint_skip_wrappers.h"
#include "printer_state.h"
#include "queued_job_start.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::skip_wrappers::Op;
using nlohmann::json;
using Access = PrintPreparationManagerTestAccess;
using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobStarter;

namespace {

/// Records every gcode send and answers none of them.
struct RecordingAPI : public MoonrakerAPIMock {
    using MoonrakerAPIMock::MoonrakerAPIMock;

    void execute_gcode(const std::string& gcode, IMoonrakerAPI::SuccessCallback,
                       IMoonrakerAPI::ErrorCallback, uint32_t = 0, bool = false,
                       IMoonrakerAPI::SuccessCallback = nullptr, bool = true,
                       bool = false) override {
        sent.push_back(gcode);
    }

    std::vector<std::string> sent;
};

PrePrintOption macro_param_opt(const std::string& id) {
    PrePrintOption o;
    o.id = id;
    o.default_enabled = true;
    o.strategy_kind = PrePrintStrategyKind::MacroParam;
    PrePrintStrategyMacroParam param;
    param.param_name = "SKIP_" + id;
    param.enable_value = "0";
    param.skip_value = "1";
    o.strategy = param;
    return o;
}

} // namespace

class SkipToggleFixture : public LVGLTestFixture {
  public:
    SkipToggleFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<RecordingAPI>(client, state);
        manager.set_dependencies(api.get(), &state);
        helix::MacroParamCache::instance().populate_from_configfile(
            {{"gcode_macro _helix_prep", {{"gcode", "SET_GCODE_VARIABLE"}}}}, {});
    }

    ~SkipToggleFixture() override {
        helix::MacroParamCache::instance().clear();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// A Voron with the wrappers loaded for bed mesh and QGL.
    void wrappers_loaded(std::vector<Op> active = {Op::BedMesh, Op::Qgl}) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"bed_mesh", "quad_gantry_level", "gcode_macro _HELIX_PREP"}));
        hw.set_skip_wrappers({Op::BedMesh, Op::Qgl}, std::move(active));
        state.set_hardware(hw);
    }

    void frame(const json& status) {
        state.update_from_status(status);
        helix::ui::UpdateQueue::instance().drain();
    }

    void gates_open() {
        frame({{"bed_mesh", {{"probed_matrix", {{0.1, 0.2}}}}},
               {"quad_gantry_level", {{"applied", true}}}});
    }

    const PrePrintOption* option(const std::string& id) {
        return state.profile_state().pre_print_option_set().find(id);
    }

    bool offered(const std::string& id) {
        const PrePrintOption* opt = option(id);
        return opt != nullptr && helix::skip_wrappers::is_wrapper_option(*opt);
    }

    bool card_shown() {
        return lv_subject_get_int(
                   state.composite_visibility_state().get_has_any_preprint_options_subject()) == 1;
    }

    void set_job_holds(int v) {
        lv_subject_set_int(state.print_state().get_job_holds_machine_subject(), v);
        helix::ui::UpdateQueue::instance().drain();
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<RecordingAPI> api;
    helix::ui::PrintPreparationManager manager;
};

TEST_CASE_METHOD(SkipToggleFixture, "a skip toggle is offered only while its gate is open",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();
    CHECK_FALSE(offered("bed_mesh"));
    CHECK_FALSE(offered("qgl"));
    CHECK_FALSE(card_shown());

    frame({{"bed_mesh", {{"probed_matrix", {{0.1, 0.2}}}}}});
    CHECK(offered("bed_mesh"));
    CHECK_FALSE(offered("qgl"));
    CHECK(card_shown()); // no plugin: the toggle works without one

    frame({{"quad_gantry_level", {{"applied", true}}}});
    CHECK(offered("qgl"));

    SECTION("motors off clears applied and withdraws the QGL toggle") {
        frame({{"quad_gantry_level", {{"applied", false}}}});
        CHECK_FALSE(offered("qgl"));
        CHECK(offered("bed_mesh"));
    }
    SECTION("a cleared mesh withdraws the bed mesh toggle") {
        frame({{"bed_mesh", {{"probed_matrix", json::array({json::array()})}}}});
        CHECK_FALSE(offered("bed_mesh"));
    }
}

TEST_CASE_METHOD(SkipToggleFixture, "a wrapper that is not loaded offers nothing",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded({Op::Qgl});
    gates_open();
    CHECK_FALSE(offered("bed_mesh"));
    CHECK(offered("qgl"));
}

TEST_CASE_METHOD(SkipToggleFixture, "a database option for the step wins over the toggle",
                 "[skip_wrappers][preprint]") {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(macro_param_opt("bed_mesh"));
    PrinterStateTestAccess::set_option_set(state, set);
    wrappers_loaded();
    gates_open();

    const PrePrintOption* mesh = option("bed_mesh");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->strategy_kind == PrePrintStrategyKind::MacroParam);
    CHECK(offered("qgl"));
    CHECK(std::count_if(state.profile_state().pre_print_option_set().options.begin(),
                        state.profile_state().pre_print_option_set().options.end(),
                        [](const PrePrintOption& o) { return o.id == "bed_mesh"; }) == 1);
}

TEST_CASE_METHOD(SkipToggleFixture, "toggle state becomes the flag line sent before START_PRINT",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();
    gates_open();
    manager.set_option_state_provider([](const std::string& id) { return id == "qgl" ? 0 : -1; });

    const auto lines = Access::get_pre_start_gcode_lines(manager);
    CHECK(std::find(lines.begin(), lines.end(),
                    "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=0") !=
          lines.end());
    CHECK(std::find(lines.begin(), lines.end(),
                    "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_bed_mesh VALUE=1") !=
          lines.end());
}

TEST_CASE_METHOD(SkipToggleFixture, "every print starts by resetting the skips",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();
    // No gate open, so no toggle: a leftover skip would otherwise ride along.
    manager.start_print("part.gcode", "", []() {}, [](bool, const std::string&) {});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(api->sent.empty());
    CHECK(api->sent.front().rfind("_HELIX_PREP", 0) == 0);
}

TEST_CASE_METHOD(SkipToggleFixture, "skip toggle lines do not make a macro skip look deliverable",
                 "[skip_wrappers][preprint][plugin_gate]") {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(macro_param_opt("nozzle_clean"));
    PrinterStateTestAccess::set_option_set(state, set);
    wrappers_loaded();
    gates_open();
    REQUIRE(offered("qgl"));

    const PrePrintOption* clean = option("nozzle_clean");
    REQUIRE(clean != nullptr);
    CHECK(manager.disabling_option_requires_plugin(*clean));
}

TEST_CASE_METHOD(SkipToggleFixture, "a skipped step embedded in the file is left in the file",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();
    gates_open();
    manager.set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });

    gcode::ScanResult scan;
    gcode::DetectedOperation op;
    op.type = gcode::OperationType::BED_MESH;
    op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
    op.line_number = 5;
    scan.operations.push_back(op);
    manager.set_cached_scan_result(scan, "part.gcode");

    CHECK(Access::get_ops_to_disable(manager).empty());
}

TEST_CASE_METHOD(SkipToggleFixture, "a skip left set with no job running is reset",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();

    SECTION("the job ends before reaching the step") {
        set_job_holds(1);
        frame({{"gcode_macro _HELIX_PREP", {{"run_qgl", 0}}}});
        CHECK(api->sent.empty()); // the job may still reach QUAD_GANTRY_LEVEL
        set_job_holds(0);
        CHECK(api->sent == std::vector<std::string>{"_HELIX_PREP"});
    }
    SECTION("the frame reporting it lands after the job ended") {
        set_job_holds(1);
        set_job_holds(0);
        frame({{"gcode_macro _HELIX_PREP", {{"run_qgl", 0}}}});
        CHECK(api->sent == std::vector<std::string>{"_HELIX_PREP"});
    }
    SECTION("found pending on connect with nothing running") {
        frame({{"gcode_macro _HELIX_PREP", {{"run_bed_mesh", 0}}}});
        CHECK(api->sent == std::vector<std::string>{"_HELIX_PREP"});
    }
    SECTION("every skip consumed") {
        set_job_holds(1);
        frame({{"gcode_macro _HELIX_PREP", {{"run_qgl", 1}}}});
        set_job_holds(0);
        CHECK(api->sent.empty());
    }
}

TEST_CASE_METHOD(SkipToggleFixture, "an empty mesh as Klipper reports it offers no mesh skip",
                 "[skip_wrappers][preprint]") {
    wrappers_loaded();
    frame({{"bed_mesh", {{"probed_matrix", json::array({json::array()})}}},
           {"quad_gantry_level", {{"applied", true}}}});
    CHECK(offered("qgl"));
    CHECK_FALSE(offered("bed_mesh"));
}

// ============================================================================
// HELIX_MOCK_SKIP_WRAPPERS
// ============================================================================

TEST_CASE("the mock with HELIX_MOCK_SKIP_WRAPPERS has the wrappers loaded",
          "[skip_wrappers][mock]") {
    helix::ScopedEnv env("HELIX_MOCK_SKIP_WRAPPERS", "1");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("its configfile reads back as the loaded file") {
        CHECK(helix::skip_wrappers::active(mock.skip_wrapper_sections()) ==
              std::vector<Op>{Op::BedMesh, Op::Qgl});
    }
    SECTION("its objects list carries the wrapper macros") {
        const auto hw = mock.hardware();
        const auto& objects = hw.printer_objects();
        for (const char* name : {"gcode_macro _HELIX_PREP", "gcode_macro BED_MESH_CLEAR",
                                 "gcode_macro QUAD_GANTRY_LEVEL"}) {
            CHECK(std::find(objects.begin(), objects.end(), name) != objects.end());
        }
    }
    SECTION("the pre-start block sets a flag and a wrapped step consumes it") {
        mock.gcode_script("_HELIX_PREP\n"
                          "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=0\n"
                          "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_bed_mesh VALUE=1");
        CHECK(mock.skip_wrapper_status()["gcode_macro _HELIX_PREP"]["run_qgl"] == 0);
        CHECK(mock.skip_wrapper_status()["gcode_macro _HELIX_PREP"]["run_bed_mesh"] == 1);
        CHECK_FALSE(mock.consume_skip(Op::BedMesh));
        CHECK(mock.consume_skip(Op::Qgl));
        CHECK_FALSE(mock.consume_skip(Op::Qgl));
        CHECK(mock.skip_wrapper_status()["gcode_macro _HELIX_PREP"]["run_qgl"] == 1);
    }
}

TEST_CASE("the mock without the knob has no wrappers", "[skip_wrappers][mock]") {
    helix::ScopedEnv env("HELIX_MOCK_SKIP_WRAPPERS", nullptr);
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    CHECK(mock.skip_wrapper_sections().empty());
    CHECK(mock.skip_wrapper_status().empty());
    CHECK_FALSE(mock.consume_skip(Op::Qgl));
}

// ============================================================================
// A queued job started from the home tile
// ============================================================================

namespace {

/// Start part.gcode through a QueuedJobStarter driving the fixture's manager,
/// with @p saved answering its options. The planted file has one tool, no
/// palette and no printer-stopping command, so every file gate passes.
void start_queued_part(SkipToggleFixture& f, QueuedJobOptions saved) {
    helix::test::wait_out_print_start_grace();
    // Its footer names its one tool, so no whole-file read is needed.
    helix::PlantedGcode file("part.gcode", "", "G28\n; filament used [g] = 1.50\n");
    QueuedJobStarter starter(f.state, f.api.get());
    QueuedJobStarterTestAccess::set_prep(starter, &f.manager);
    starter.start_with_options(helix::JobQueueEntry{"0001", "part.gcode", 0.0, 0.0},
                               std::move(saved));
    for (int i = 0; i < 6; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE(starter.in_flight()); // the start is out, waiting on its gcode answers
}

/// Whether a line of any script sent so far is exactly @p line. The pre-start
/// block goes out as one multi-line script.
bool sent(const SkipToggleFixture& f, const std::string& line) {
    for (const auto& script : f.api->sent) {
        for (std::string_view l : helix::text_io::lines(script, '\n')) {
            if (l == line) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(SkipToggleFixture, "a queued job's saved options reach the start",
                 "[queued_job_start][skip_wrappers][preprint]") {
    wrappers_loaded();
    gates_open();

    start_queued_part(*this, QueuedJobOptions{"part.gcode", {{"qgl", false}}});

    CHECK(sent(*this, "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=0"));
    CHECK(sent(*this, "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_bed_mesh VALUE=1"));
}

TEST_CASE_METHOD(SkipToggleFixture, "a job with nothing saved starts with the defaults",
                 "[queued_job_start][skip_wrappers][preprint]") {
    wrappers_loaded();
    gates_open();

    start_queued_part(*this, QueuedJobOptions{});

    CHECK(sent(*this, "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=1"));
    CHECK_FALSE(sent(*this, "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=0"));
}
