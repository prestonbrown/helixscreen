// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_file_analysis.cpp
 * @brief The plugin's file analysis record as the screen's first source.
 *
 * Run with: ./build/bin/helix-tests "[plugin_file_analysis]"
 *
 * The decision is pinned over every state plugin_feature() can be in; the
 * reply reader over each status; the merge field by field; and the loader end
 * to end against the mock, where the record must win wherever it answers and
 * the file read must fill whatever it leaves open.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/scoped_cache_dir.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "core/update_queue.h"
#include "file_analysis.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_inputs.h"
#include "printer_state.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::FileAnalysisReply;
using helix::FileAnalysisSource;
using helix::FileStartFacts;
using helix::PluginFeatureEntry;
using helix::PluginFeatureMap;
using helix::PluginFeatureState;
using json = nlohmann::json;

namespace {

PluginFeatureMap feature_map(int version, bool available, bool enabled) {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    return {{helix::kFileAnalysisFeature, e}};
}

FileAnalysisSource source_for(bool plugin, const PluginFeatureMap& features,
                              const std::string& file = "part.gcode") {
    return helix::file_analysis_source(helix::plugin_feature_state(plugin, features,
                                                                   helix::kFileAnalysisFeature,
                                                                   helix::kFileAnalysisMinVersion),
                                       file);
}

json record_json() {
    return json{{"schema", 1},
                {"filename", "part.gcode"},
                {"file_size", 1234},
                {"file_mtime", 1760000000.5},
                {"tools", json::array({json{{"tool", 2},
                                            {"mm", 40.0},
                                            {"grams", 0.12},
                                            {"color", "#0000FF"},
                                            {"material", "PETG"}},
                                       json{{"tool", 0},
                                            {"mm", 120.5},
                                            {"grams", 0.36},
                                            {"color", "#FF8800"},
                                            {"material", "PLA"}}})},
                {"objects",
                 {{"source", "label"},
                  {"list", json::array({json{{"name", "Box A"},
                                             {"center", {12.5, 12.5}},
                                             {"polygon", {{10, 10}, {15, 10}, {15, 15}}}}})}}},
                {"layers", {{"count", 3}, {"index", ".helix_index/part.gcode.layers.jsonl"}}},
                {"pauses", 1}};
}

json ready(json record) {
    return json{{"filename", "part.gcode"}, {"status", "ready"}, {"analysis", std::move(record)}};
}

} // namespace

// ============================================================================
// Decision
// ============================================================================

TEST_CASE("file analysis is asked of the plugin only while the feature is ready",
          "[plugin_file_analysis]") {
    SECTION("no plugin") {
        CHECK(source_for(false, feature_map(1, true, true)) == FileAnalysisSource::Fallback);
    }
    SECTION("a plugin without the features map") {
        CHECK(source_for(true, {}) == FileAnalysisSource::Fallback);
    }
    SECTION("switched off") {
        CHECK(source_for(true, feature_map(1, true, false)) == FileAnalysisSource::Fallback);
    }
    SECTION("ready") {
        CHECK(source_for(true, feature_map(1, true, true)) == FileAnalysisSource::Plugin);
    }
    SECTION("a contract below the minimum") {
        CHECK(source_for(true, feature_map(0, true, true)) == FileAnalysisSource::Fallback);
    }
    SECTION("unavailable") {
        CHECK(source_for(true, feature_map(1, false, true)) == FileAnalysisSource::Fallback);
    }
    SECTION("a .3mf has no record, even when ready") {
        CHECK(source_for(true, feature_map(1, true, true), "part.gcode.3mf") ==
              FileAnalysisSource::Fallback);
        CHECK(source_for(true, feature_map(1, true, true), "") == FileAnalysisSource::Fallback);
    }
}

// ============================================================================
// Reply
// ============================================================================

TEST_CASE("a ready reply carries the record", "[plugin_file_analysis]") {
    const FileAnalysisReply reply = helix::parse_file_analysis_reply(ready(record_json()));
    REQUIRE(reply.status == FileAnalysisReply::Status::Ready);
    REQUIRE(reply.record.has_value());
    const auto& rec = *reply.record;
    CHECK(rec.filename == "part.gcode");
    CHECK(rec.file_size == 1234);
    REQUIRE(rec.tools.size() == 2);
    CHECK(rec.tools[0].tool == 0); // ascending whatever the order sent
    CHECK(rec.tools[1].tool == 2);
    CHECK(rec.tools[1].grams == std::optional<double>(0.12));
    CHECK(rec.tools[1].color == "#0000FF");
    CHECK(rec.objects_source == "label");
    REQUIRE(rec.objects.size() == 1);
    CHECK(rec.objects[0].polygon.size() == 3);
    REQUIRE(rec.objects[0].center.has_value());
    CHECK(rec.objects[0].center->first == 12.5);
    CHECK(rec.layer_count == 3);
    CHECK(rec.layer_index == ".helix_index/part.gcode.layers.jsonl");
    CHECK(rec.pauses == 1);
}

TEST_CASE("a record's unknown values stay unknown", "[plugin_file_analysis]") {
    json rec = record_json();
    rec["tools"] = json::array(
        {json{{"tool", 0}, {"mm", nullptr}, {"grams", "3"}, {"color", nullptr}, {"material", 5}}});
    const auto reply = helix::parse_file_analysis_reply(ready(rec));
    REQUIRE(reply.record.has_value());
    const auto& t = reply.record->tools.at(0);
    CHECK_FALSE(t.mm.has_value());
    CHECK_FALSE(t.grams.has_value());
    CHECK(t.color.empty());
    CHECK(t.material.empty());
}

TEST_CASE("pending, disabled and unreadable replies carry no record", "[plugin_file_analysis]") {
    CHECK(helix::parse_file_analysis_reply(json{{"status", "pending"}}).status ==
          FileAnalysisReply::Status::Pending);
    CHECK(helix::parse_file_analysis_reply(json{{"status", "disabled"}}).status ==
          FileAnalysisReply::Status::Disabled);
    json other_schema = record_json();
    other_schema["schema"] = 2;
    const auto newer = helix::parse_file_analysis_reply(ready(other_schema));
    CHECK(newer.status == FileAnalysisReply::Status::Invalid);
    CHECK_FALSE(newer.record.has_value());
    CHECK(helix::parse_file_analysis_reply(json::array()).status ==
          FileAnalysisReply::Status::Invalid);
    CHECK(helix::parse_file_analysis_reply(json{{"status", "ready"}}).status ==
          FileAnalysisReply::Status::Invalid);
}

// ============================================================================
// Merge
// ============================================================================

TEST_CASE("the record answers per field; what it leaves open keeps the metadata's",
          "[plugin_file_analysis]") {
    FileStartFacts facts;
    facts.palette.colors = {"#111111", "#222222", "#333333"};
    facts.palette.materials = {"ABS", "ABS", "ABS"};
    facts.tools_used = {1};

    json rec = record_json();
    rec["tools"][0]["color"] = nullptr; // tool 2 has no color in the record
    const auto reply = helix::parse_file_analysis_reply(ready(rec));
    REQUIRE(reply.record.has_value());
    helix::apply_file_analysis(facts, *reply.record);

    CHECK(facts.tools_used == std::set<int>{0, 2});
    CHECK(facts.palette.colors == std::vector<std::string>{"#FF8800", "#222222", "#333333"});
    CHECK(facts.palette.materials == std::vector<std::string>{"PLA", "ABS", "PETG"});
    CHECK(facts.tool_grams == std::vector<double>{0.36, 0.0, 0.12});
}

TEST_CASE("grams are taken only when every printing tool has them", "[plugin_file_analysis]") {
    FileStartFacts facts;
    json rec = record_json();
    rec["tools"][0]["grams"] = nullptr;
    helix::apply_file_analysis(facts, *helix::parse_file_analysis_reply(ready(rec)).record);
    CHECK(facts.tool_grams.empty());
    CHECK(facts.tools_used == std::set<int>{0, 2});
}

TEST_CASE("a record with no tools changes nothing", "[plugin_file_analysis]") {
    FileStartFacts facts;
    facts.tools_used = {3};
    facts.palette.colors = {"#ABCDEF"};
    json rec = record_json();
    rec["tools"] = json::array();
    helix::apply_file_analysis(facts, *helix::parse_file_analysis_reply(ready(rec)).record);
    CHECK(facts.tools_used == std::set<int>{3});
    CHECK(facts.palette.colors == std::vector<std::string>{"#ABCDEF"});
}

// ============================================================================
// FileStartFactsLoader
// ============================================================================

namespace {

constexpr const char* kFooter = "; filament_type = ABS;ABS;ABS\n"
                                "; filament_colour = #111111;#222222;#333333\n"
                                "G28\n"
                                "T1\n"
                                "; filament used [g] = 0.00, 9.00, 0.00\n";

void drain() {
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

struct AnalysisLoaderFixture : LVGLUITestFixture {
    AnalysisLoaderFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
    }
    ~AnalysisLoaderFixture() override {
        get_printer_state().plugin_status_state().set_features({});
        get_printer_state().set_helix_plugin_installed(false);
        drain();
        api.reset();
        client.disconnect();
    }

    void plugin(bool installed, PluginFeatureMap features) {
        get_printer_state().set_helix_plugin_installed(installed);
        drain();
        get_printer_state().plugin_status_state().set_features(std::move(features));
    }

    std::optional<FileStartFacts> load(const std::string& name) {
        helix::FileStartFactsLoader loader(api.get());
        std::optional<FileStartFacts> answer;
        loader.load(
            name, [&answer](FileStartFacts facts) { answer = std::move(facts); },
            [](helix::SharedCallback<void(bool, const std::string&)> done) { done(false, {}); });
        drain();
        return answer;
    }

    helix::test::ScopedCacheDir cache;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

TEST_CASE_METHOD(AnalysisLoaderFixture, "the loader takes the record before the footer",
                 "[plugin_file_analysis][print_start_inputs][loader]") {
    helix::PlantedGcode file("analysis_first.gcode", "", kFooter);
    plugin(true, feature_map(1, true, true));
    json rec = record_json();
    rec["filename"] = file.name();
    client.set_mock_file_analysis(file.name(), ready(rec));

    const auto answer = load(file.name());
    REQUIRE(answer.has_value());
    CHECK(client.call_count(helix::kFileAnalysisRpc) == 1);
    CHECK(answer->tools_used == std::set<int>{0, 2});
    CHECK(answer->tool_grams == std::vector<double>{0.36, 0.0, 0.12});
    CHECK(answer->palette.colors.at(0) == "#FF8800");
    CHECK(answer->palette.materials.at(2) == "PETG");
}

TEST_CASE_METHOD(AnalysisLoaderFixture, "what the record leaves open the footer fills",
                 "[plugin_file_analysis][print_start_inputs][loader]") {
    helix::PlantedGcode file("analysis_partial.gcode", "", kFooter);
    plugin(true, feature_map(1, true, true));
    json rec = record_json();
    rec["tools"] = json::array({json{
        {"tool", 1}, {"mm", 30.0}, {"grams", nullptr}, {"color", nullptr}, {"material", nullptr}}});
    client.set_mock_file_analysis(file.name(), ready(rec));

    const auto answer = load(file.name());
    REQUIRE(answer.has_value());
    // Tools from the record; grams from the footer, which the record lacked.
    CHECK(answer->tools_used == std::set<int>{1});
    CHECK(answer->tool_grams == std::vector<double>{0.0, 9.0, 0.0});
}

TEST_CASE_METHOD(AnalysisLoaderFixture, "a pending record falls back to the file read",
                 "[plugin_file_analysis][print_start_inputs][loader]") {
    helix::PlantedGcode file("analysis_pending.gcode", "", kFooter);
    plugin(true, feature_map(1, true, true));

    const auto answer = load(file.name());
    REQUIRE(answer.has_value());
    CHECK(client.call_count(helix::kFileAnalysisRpc) == 1);
    CHECK(answer->tools_used == std::set<int>{1});
    CHECK(answer->tool_grams == std::vector<double>{0.0, 9.0, 0.0});
}

TEST_CASE_METHOD(AnalysisLoaderFixture, "without the feature the plugin is never asked",
                 "[plugin_file_analysis][print_start_inputs][loader]") {
    helix::PlantedGcode file("analysis_absent.gcode", "", kFooter);
    SECTION("no plugin") {
        plugin(false, feature_map(1, true, true));
    }
    SECTION("switched off") {
        plugin(true, feature_map(1, true, false));
    }
    client.set_mock_file_analysis(file.name(), ready(record_json()));

    const auto answer = load(file.name());
    REQUIRE(answer.has_value());
    CHECK(client.call_count(helix::kFileAnalysisRpc) == 0);
    CHECK(answer->tools_used == std::set<int>{1});
}

// ============================================================================
// Layer index sidecar fetch
// ============================================================================

TEST_CASE_METHOD(AnalysisLoaderFixture, "the sidecar the record names reaches the load",
                 "[plugin_file_analysis][layer_index]") {
    helix::PlantedGcode file("sidecar_owner.gcode", "", kFooter);
    // The mock serves a download by its basename from the planted root.
    helix::PlantedGcode sidecar("sidecar_owner.gcode.layers.jsonl", "", "{\"format\":1}\n");
    json rec = record_json();
    rec["layers"]["index"] = ".helix_index/sidecar_owner.gcode.layers.jsonl";
    client.set_mock_file_analysis(file.name(), ready(rec));
    helix::AsyncLifetimeGuard guard;

    std::optional<std::string> got;
    helix::with_layer_index_sidecar(api.get(), PluginFeatureState::Ready, file.name(),
                                    /*wanted=*/true, guard.token(),
                                    [&got](std::string s) { got = std::move(s); });
    for (int i = 0; i < 50 && !got; ++i) {
        drain();
        process_lvgl(10);
    }
    REQUIRE(got.has_value());
    CHECK(*got == "{\"format\":1}\n");
}

TEST_CASE_METHOD(AnalysisLoaderFixture, "no record, no sidecar",
                 "[plugin_file_analysis][layer_index]") {
    helix::PlantedGcode file("sidecar_none.gcode", "", kFooter);
    helix::AsyncLifetimeGuard guard;
    std::optional<std::string> got;

    SECTION("feature not ready: answered at once, plugin not asked") {
        helix::with_layer_index_sidecar(api.get(), PluginFeatureState::Disabled, file.name(),
                                        /*wanted=*/true, guard.token(),
                                        [&got](std::string s) { got = std::move(s); });
        REQUIRE(got.has_value());
        CHECK(client.call_count(helix::kFileAnalysisRpc) == 0);
    }
    SECTION("a load that will not stream: answered at once, plugin not asked") {
        helix::with_layer_index_sidecar(api.get(), PluginFeatureState::Ready, file.name(),
                                        /*wanted=*/false, guard.token(),
                                        [&got](std::string s) { got = std::move(s); });
        REQUIRE(got.has_value());
        CHECK(client.call_count(helix::kFileAnalysisRpc) == 0);
    }
    SECTION("pending") {
        helix::with_layer_index_sidecar(api.get(), PluginFeatureState::Ready, file.name(),
                                        /*wanted=*/true, guard.token(),
                                        [&got](std::string s) { got = std::move(s); });
        drain();
        REQUIRE(got.has_value());
        CHECK(client.call_count(helix::kFileAnalysisRpc) == 1);
    }
    CHECK(got->empty());
}
