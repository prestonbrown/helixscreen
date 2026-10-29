// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_inputs.cpp
 * @brief The file half of the pre-start checks, computed with no file view.
 *
 * Run with: ./build/bin/helix-tests "[print_start_inputs]"
 *
 * A queued job started from the home tile gets its tool info, mapping, remap
 * and empty-slot pre-flight from fill_file_context() and preflight_for(, false). The
 * parity case pins that they answer what an open PrintSelectDetailView answers
 * for the same file with no mapping edit; the loader cases pin the metadata,
 * footer and whole-file scan steps the file view shares.
 */

#include "ui_callback_helpers.h"
#include "ui_filament_mapping_card.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/scoped_cache_dir.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_snapmaker.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"
#include "tools_used_cache.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::FileStartFacts;
using helix::FileStartFactsLoader;

namespace {

void noop_cb(lv_event_t* /*e*/) {}

using CacheDir = helix::test::ScopedCacheDir;

FileStartFacts two_tool_facts() {
    FileStartFacts facts;
    facts.filename = "sub/two_tool.gcode";
    facts.palette.colors = {"#FF0000", "#0000FF"};
    facts.palette.materials = {"PLA", "PETG"};
    facts.tools_used = {0, 1};
    return facts;
}

void check_same_mappings(const std::vector<helix::ToolMapping>& a,
                         const std::vector<helix::ToolMapping>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].tool_index == b[i].tool_index);
        CHECK(a[i].mapped_slot == b[i].mapped_slot);
        CHECK(a[i].mapped_backend == b[i].mapped_backend);
        CHECK(a[i].is_auto == b[i].is_auto);
        CHECK(a[i].material_mismatch == b[i].material_mismatch);
    }
}

void check_same_tools(const std::vector<helix::GcodeToolInfo>& a,
                      const std::vector<helix::GcodeToolInfo>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].tool_index == b[i].tool_index);
        CHECK(a[i].color_rgb == b[i].color_rgb);
        CHECK(a[i].material == b[i].material);
    }
}

void drain() {
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

} // namespace

TEST_CASE("file_palette_from_metadata splits materials and copies colors", "[print_start_inputs]") {
    FileMetadata metadata;
    metadata.filament_type = "PLA;PLA;PETG";
    metadata.filament_colors = {"#FF0000", "#00FF00", "#0000FF"};

    const auto palette = helix::file_palette_from_metadata(metadata);
    CHECK(palette.materials == std::vector<std::string>{"PLA", "PLA", "PETG"});
    CHECK(palette.colors == metadata.filament_colors);

    SECTION("an empty type gives no materials") {
        metadata.filament_type.clear();
        CHECK(helix::file_palette_from_metadata(metadata).materials.empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context matches the file view",
                 "[print_start_inputs][detail_view]") {
    CacheDir cache;
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x0000FF, "PETG"}});

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", noop_cb},
        {"on_print_select_print_button", noop_cb},
        {"on_print_select_delete_button", noop_cb},
        {"on_print_select_more_button", noop_cb},
        {"on_print_detail_back_clicked", noop_cb},
        {"on_toggle_sliced_colors", noop_cb},
        {"on_print_select_detail_objects", noop_cb},
    });
    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);
    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    const FileStartFacts facts = two_tool_facts();
    constexpr size_t kSize = 2468;
    constexpr time_t kMtime = 1357;
    // A warmed cache renders the authoritative state inside show().
    helix::tools_used_cache().store(facts.filename, {kSize, kMtime}, facts.tools_used);
    view.show("two_tool.gcode", "sub", "PLA", facts.palette.colors, facts.palette.materials, kSize,
              kMtime);
    view.recompute_preflight();
    REQUIRE(view.get_tools_used() == facts.tools_used);

    helix::PrintStartContext ctx;
    helix::fill_file_context(ctx, facts);

    CHECK(ctx.has_file_facts);
    // The card is showing, so both the card's copy and the effective one agree.
    REQUIRE_FALSE(view.get_filament_mappings().empty());
    check_same_mappings(ctx.mappings, view.get_filament_mappings());
    check_same_mappings(ctx.mappings, view.effective_mappings());
    check_same_tools(ctx.tool_info, view.get_filament_tool_info());
    check_same_tools(ctx.tool_info, view.get_used_tool_info());
    CHECK(ctx.effective_remap == view.get_effective_remap());
    CHECK(ctx.filament_materials == view.get_filament_materials());
    CHECK(ctx.available_slots.size() == view.get_available_slots().size());

    const auto ours = helix::preflight_for(facts, false);
    const auto theirs = view.preflight_result();
    REQUIRE(ours.checks.size() == theirs.checks.size());
    REQUIRE_FALSE(ours.checks.empty());
    for (size_t i = 0; i < ours.checks.size(); ++i) {
        CHECK(ours.checks[i].tool_index == theirs.checks[i].tool_index);
        CHECK(ours.checks[i].mapped_slot == theirs.checks[i].mapped_slot);
        CHECK(ours.checks[i].severity == theirs.checks[i].severity);
    }
    CHECK(ours.has_block() == theirs.has_block());
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context feeds the material mismatch gate",
                 "[print_start_inputs]") {
    // Two lanes so the card would show: a mapping exists to be checked.
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});

    FileStartFacts facts;
    facts.filename = "petg.gcode";
    facts.palette.colors = {"#FF0000"};
    facts.palette.materials = {"PETG"};
    facts.tools_used = {0};

    helix::PrintStartContext ctx;
    ctx.ams_available = true;
    ctx.has_active_backend = true;
    ctx.ams_manages_filament = true;
    ctx.filament_color_count = facts.palette.colors.size();
    helix::fill_file_context(ctx, facts);

    REQUIRE(ctx.has_file_facts);
    CHECK_FALSE(helix::material_mismatches_in(ctx).empty());

    SECTION("without file facts the gate has nothing to compare") {
        ctx.has_file_facts = false;
        CHECK(helix::material_mismatches_in(ctx).empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "with an AMS the mismatch comes from the tool materials and the mapping flag",
                 "[print_start_inputs]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});

    FileStartFacts facts;
    facts.filename = "petg.gcode";
    facts.palette.colors = {"#FF0000"};
    facts.palette.materials = {"PETG"};
    facts.tools_used = {0};

    helix::PrintStartContext ctx;
    ctx.ams_available = true;
    ctx.has_active_backend = true;
    ctx.ams_manages_filament = true;
    ctx.filament_color_count = facts.palette.colors.size();
    helix::fill_file_context(ctx, facts);
    // The AMS path never reads the palette's material list.
    ctx.filament_materials.clear();

    const auto* tool = helix::ui::FilamentMappingCard::find_by_tool_index(ctx.tool_info, 0);
    REQUIRE(tool != nullptr);
    CHECK(tool->material == "PETG");
    REQUIRE(ctx.mappings.size() == 1);
    CHECK(ctx.mappings[0].material_mismatch);

    const auto mismatches = helix::material_mismatches_in(ctx);
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0].tool_index == 0);
    CHECK(mismatches[0].expected_material == "PETG");
    CHECK(mismatches[0].loaded_material == "PLA");
    CHECK_FALSE(mismatches[0].grade_only);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fill_file_context leaves no mapping on a printer with no AMS",
                 "[print_start_inputs]") {
    // A full-profile palette on a plain printer: the file view's card is
    // hidden, so the unresolved-tools gate must find no mapping to judge.
    FileStartFacts facts;
    facts.filename = "plain.gcode";
    facts.palette.colors = {"#FF0000", "#00FF00"};
    facts.palette.materials = {"PLA", "PLA"};
    facts.tools_used = {0};

    helix::PrintStartContext ctx;
    ctx.filament_color_count = facts.palette.colors.size();
    helix::fill_file_context(ctx, facts);
    CHECK(ctx.mappings.empty());
    CHECK(helix::unresolved_tools_in(ctx).empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fill_file_context takes the file's tools from its weights when none were scanned",
                 "[print_start_inputs][reprint]") {
    FileStartFacts facts;
    facts.filename = "weights_only.gcode";
    facts.metadata = FileMetadata{};
    facts.metadata->filament_weights = {1.5, 0.0, 2.25};
    facts.metadata->filament_colors = {"#FF0000", "#00FF00", "#0000FF"};

    helix::PrintStartContext ctx;
    helix::fill_file_context(ctx, facts);
    CHECK(ctx.tools_used == std::set<int>{0, 2});
}

// ============================================================================
// FileStartFactsLoader
// ============================================================================

namespace {

constexpr const char* kTwoToolFooter = "; filament_type = PLA;PETG\n"
                                       "; filament_colour = #FF0000;#0000FF\n"
                                       "G28\n"
                                       "T0\n"
                                       "; filament used [g] = 1.50, 2.25\n";

struct LoaderFixture : LVGLUITestFixture {
    LoaderFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
    }
    ~LoaderFixture() override {
        drain();
        api.reset();
        client.disconnect();
    }

    CacheDir cache;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

TEST_CASE_METHOD(LoaderFixture, "a footer that names its tools answers without a download",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode file("loader_footer.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());

    int downloads = 0;
    std::optional<FileStartFacts> answer;
    loader.load(
        file.name(), [&answer](FileStartFacts facts) { answer = std::move(facts); },
        [&downloads](std::function<void(bool, const std::string&)> done) {
            ++downloads;
            done(false, {});
        });
    drain();

    REQUIRE(answer.has_value());
    CHECK(answer->filename == file.name());
    CHECK(answer->metadata.has_value());
    CHECK(answer->tools_used == std::set<int>{0, 1});
    CHECK(answer->palette.colors.size() == 2);
    CHECK(answer->palette.materials == std::vector<std::string>{"PLA", "PETG"});
    CHECK(answer->tool_grams == std::vector<double>{1.50, 2.25});
    CHECK(downloads == 0);
}

TEST_CASE_METHOD(LoaderFixture, "a failed metadata read still answers",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode file("loader_nometa.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());
    client.fail_next("server.files.metadata");

    std::optional<FileStartFacts> answer;
    loader.load(
        file.name(), [&answer](FileStartFacts facts) { answer = std::move(facts); },
        [](std::function<void(bool, const std::string&)> done) { done(false, {}); });
    drain();

    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->metadata.has_value());
    // The footer still names the tools and their colors.
    CHECK(answer->tools_used == std::set<int>{0, 1});
    CHECK(answer->palette.colors.size() == 2);
}

TEST_CASE_METHOD(LoaderFixture, "a metadata-only load reads nothing of the file",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode file("loader_metaonly.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());
    // Without the metadata only the file's own footer could name its tools.
    client.fail_next("server.files.metadata");

    int downloads = 0;
    std::optional<FileStartFacts> answer;
    loader.load(
        file.name(), [&answer](FileStartFacts facts) { answer = std::move(facts); },
        [&downloads](std::function<void(bool, const std::string&)> done) {
            ++downloads;
            done(false, {});
        },
        nullptr, helix::FileRead::MetadataOnly);
    drain();

    REQUIRE(answer.has_value());
    CHECK(answer->tools_used.empty());
    CHECK(downloads == 0);
}

namespace {
/// The footer of an Orca job for the Snapmaker U1 ("BMCU 370C_ASA_18.4g_54m6s.gcode",
/// read off a printer), whose Moonraker metadata carries no per-tool weights.
constexpr const char* kOrcaU1Footer = "G28\n"
                                      "T0\n"
                                      "T1\n"
                                      "; CONFIG_BLOCK_END\n"
                                      "\n"
                                      "; filament used [mm] = 7140.18, 245.42, 0.00, 0.00\n"
                                      "; filament used [cm3] = 17.17, 0.59, 0.00, 0.00\n"
                                      "; filament used [g] = 17.86, 0.63, 0.00, 0.00\n"
                                      "; filament cost = 0.36, 0.01, 0.00, 0.00\n"
                                      "; total filament used [g] = 18.49\n"
                                      "; total filament cost = 0.37\n"
                                      "; total filament change = 14\n"
                                      "; total layers count = 91\n";
} // namespace

TEST_CASE_METHOD(LoaderFixture, "a footer-bounded load reads the footer and never downloads",
                 "[print_start_inputs][loader]") {
    FileStartFactsLoader loader(api.get());
    int downloads = 0;
    auto download = [&downloads](std::function<void(bool, const std::string&)> done) {
        ++downloads;
        done(false, {});
    };
    std::optional<FileStartFacts> answer;
    auto record = [&answer](FileStartFacts facts) { answer = std::move(facts); };

    SECTION("an Orca U1 footer names its tools from the per-filament grams") {
        helix::PlantedGcode file("loader_orca_u1.gcode", "", kOrcaU1Footer);
        client.fail_next("server.files.metadata"); // only the footer can answer
        loader.load(file.name(), record, download, nullptr, helix::FileRead::Footer);
        drain();
        REQUIRE(answer.has_value());
        CHECK(answer->tools_used == std::set<int>{0, 1});
        CHECK(downloads == 0);
    }
    SECTION("a footer that does not answer ends the read") {
        helix::PlantedGcode file("loader_no_usage.gcode", "", "G28\nT0\nT1\nG1 X10\n");
        client.fail_next("server.files.metadata");
        loader.load(file.name(), record, download, nullptr, helix::FileRead::Footer);
        drain();
        REQUIRE(answer.has_value());
        CHECK(answer->tools_used.empty());
        CHECK(downloads == 0);
    }
}

TEST_CASE_METHOD(LoaderFixture, "a later load supersedes an unanswered one",
                 "[print_start_inputs][loader]") {
    helix::PlantedGcode first("loader_first.gcode", "", kTwoToolFooter);
    helix::PlantedGcode second("loader_second.gcode", "", kTwoToolFooter);
    FileStartFactsLoader loader(api.get());

    std::vector<std::string> answered;
    auto record = [&answered](FileStartFacts facts) { answered.push_back(facts.filename); };
    client.defer_next("server.files.metadata");
    loader.load(first.name(), record);
    loader.load(second.name(), record);
    drain();
    client.fire_deferred("server.files.metadata");
    drain();

    CHECK(answered == std::vector<std::string>{second.name()});
}

// ============================================================================
// The file's tools, as the pre-print step and the slicer-start check read them
// ============================================================================

TEST_CASE("file_tools_used reads scanned tools, then per-tool weights",
          "[print_start_inputs][reprint]") {
    FileStartFacts facts;
    CHECK(helix::file_tools_used(facts).empty());
    facts.metadata = FileMetadata{};
    facts.metadata->filament_weights = {1.5, 0.0, 2.25};
    facts.metadata->filament_colors = {"#FF0000", "#00FF00", "#0000FF"};
    CHECK(helix::file_tools_used(facts) == std::set<int>{0, 2});
    facts.tools_used = {1};
    CHECK(helix::file_tools_used(facts) == std::set<int>{1});
}

namespace {
constexpr const char* kT0T2Footer = "; filament_type = PLA;PLA;PETG\n"
                                    "; filament_colour = #FF0000;#00FF00;#0000FF\n"
                                    "G28\n"
                                    "T0\n"
                                    "T2\n"
                                    "; filament used [g] = 1.50, 0.00, 2.25\n";
} // namespace

TEST_CASE_METHOD(LoaderFixture,
                 "a reprint with no tools in hand reads them from the file for the pre-print step",
                 "[print_start_inputs][reprint]") {
    get_printer_state().update_from_status(nlohmann::json{{"webhooks", {{"state", "ready"}}}});
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend_reg(api.get(), nullptr);
    // The last print ran configured (T0, T2 at their own heads), so a reprint
    // has its routing; only the tools are missing.
    helix::SnapmakerTestAccess::handle_status(
        *backend_reg, nlohmann::json{{"print_task_config",
                                      {{"extruders_used", {true, false, true, false}},
                                       {"extruder_map_table", {0, 1, 2, 3, 0, 0, 0, 0}}}}});
    helix::PlantedGcode file("reprint_t0t2.gcode", "", kT0T2Footer);
    helix::ui::PrintStartController controller(get_printer_state(), api.get());
    client.clear_gcode_script_history();

    bool started = false;
    controller.initiate_reprint(
        file.name(), "", /*tools_used=*/{}, [&started]() { started = true; }, []() {});
    drain();

    const auto history = client.gcode_script_history();
    bool used_heads = false;
    for (const auto& script : history) {
        if (script.find("SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2") != std::string::npos) {
            used_heads = true;
        }
    }
    INFO("gcode sent: " << history.size() << " script(s)");
    CHECK(used_heads);
    CHECK(started);
}

namespace {
/// A U1 backend whose firmware is not known to reset its routing.
class NoResetSnapmaker : public helix::AmsBackendSnapmaker {
  public:
    using AmsBackendSnapmaker::AmsBackendSnapmaker;
    [[nodiscard]] bool print_routing_resets_to_identity() const override {
        return false;
    }
};

std::vector<std::string> scripts_containing(MoonrakerClientMock& client, const std::string& what) {
    std::vector<std::string> out;
    for (const auto& script : client.gcode_script_history()) {
        if (script.find(what) != std::string::npos) {
            out.push_back(script);
        }
    }
    return out;
}

/// No configured task seen: the firmware map as @p map reads, nothing used.
nlohmann::json unconfigured_task(const std::vector<int>& map) {
    return nlohmann::json{
        {"print_task_config",
         {{"extruders_used", {false, false, false, false}}, {"extruder_map_table", map}}}};
}
} // namespace

TEST_CASE_METHOD(LoaderFixture, "an unrouted U1 reprint names the file's heads and writes no map",
                 "[print_start_inputs][reprint]") {
    get_printer_state().update_from_status(nlohmann::json{{"webhooks", {{"state", "ready"}}}});
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend_reg(api.get(), nullptr);
    helix::PlantedGcode file("reprint_unrouted.gcode", "", kT0T2Footer);
    helix::ui::PrintStartController controller(get_printer_state(), api.get());

    SECTION("the firmware map at its default: the heads go, no map") {
        helix::SnapmakerTestAccess::handle_status(*backend_reg,
                                                  unconfigured_task({0, 1, 2, 3, 0, 0, 0, 0}));
        client.clear_gcode_script_history();
        controller.initiate_reprint(file.name(), "", {}, [] {}, [] {});
        drain();
        CHECK(scripts_containing(client, "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2").size() == 1);
        CHECK(scripts_containing(client, "SET_PRINT_EXTRUDER_MAP").empty());
    }
    SECTION("a map left pending routes elsewhere: no heads") {
        helix::SnapmakerTestAccess::handle_status(*backend_reg,
                                                  unconfigured_task({2, 1, 0, 3, 0, 0, 0, 0}));
        client.clear_gcode_script_history();
        controller.initiate_reprint(file.name(), "", {}, [] {}, [] {});
        drain();
        CHECK(scripts_containing(client, "SET_PRINT_USED_EXTRUDERS").empty());
        CHECK(scripts_containing(client, "SET_PRINT_EXTRUDER_MAP").empty());
    }
    SECTION("a file whose tools cannot be read: no heads") {
        helix::SnapmakerTestAccess::handle_status(*backend_reg,
                                                  unconfigured_task({0, 1, 2, 3, 0, 0, 0, 0}));
        client.clear_gcode_script_history();
        controller.initiate_reprint("not_on_the_printer.gcode", "", {}, [] {}, [] {});
        drain();
        CHECK(scripts_containing(client, "SET_PRINT_USED_EXTRUDERS").empty());
    }
}

TEST_CASE_METHOD(LoaderFixture,
                 "an unrouted reprint on firmware that keeps its routing sends no heads",
                 "[print_start_inputs][reprint]") {
    get_printer_state().update_from_status(nlohmann::json{{"webhooks", {{"state", "ready"}}}});
    helix::test::RegisteredBackend<NoResetSnapmaker> backend_reg(api.get(), nullptr);
    helix::SnapmakerTestAccess::handle_status(*backend_reg,
                                              unconfigured_task({0, 1, 2, 3, 0, 0, 0, 0}));
    helix::PlantedGcode file("reprint_noreset.gcode", "", kT0T2Footer);
    helix::ui::PrintStartController controller(get_printer_state(), api.get());
    client.clear_gcode_script_history();

    bool started = false;
    controller.initiate_reprint(file.name(), "", {}, [&started] { started = true; }, [] {});
    drain();
    CHECK(scripts_containing(client, "SET_PRINT_USED_EXTRUDERS").empty());
    CHECK(scripts_containing(client, "SET_PRINT_EXTRUDER_MAP").empty());
    CHECK(started);
}
