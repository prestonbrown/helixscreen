// SPDX-License-Identifier: GPL-3.0-or-later

// The print_edited edit grammar: the golden fixtures moonraker-plugin's pytest
// suite also applies, the app's producers against them, and the choice
// between the plugin and the download-rewrite-upload path.

#include "gcode_edit_list.h"
#include "gcode_file_modifier.h"
#include "gcode_ops_detector.h"
#include "gcode_tool_remapper.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::gcode;
using json = nlohmann::json;

namespace {

std::string golden_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/gcode_edits";
}

std::string read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

json read_spec(const std::string& name) {
    return json::parse(read_bytes(golden_dir() + "/" + name + "/edits.json"), nullptr, false);
}

std::vector<std::string> golden_cases() {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(golden_dir())) {
        if (entry.is_directory()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// What the app's rewriters make of @p input under @p list: the output bytes,
/// or nullopt for a refusal. @p streaming picks apply_streaming over apply().
std::optional<std::string> apply_list(const std::string& input, const EditList& list,
                                      bool streaming) {
    if (!list.remap.empty()) {
        const std::string out = "/tmp/helix_gcode_edit_list_" + std::to_string(getpid());
        const auto changed = GcodeToolRemapper::apply_to_file(input, out, list.remap);
        REQUIRE(changed.has_value());
        std::string bytes = read_bytes(out);
        std::filesystem::remove(out);
        return bytes;
    }
    GCodeFileModifier modifier;
    for (const auto& mod : list.modifications) {
        modifier.add_modification(mod);
    }
    const auto result = streaming ? modifier.apply_streaming(input) : modifier.apply(input);
    if (!result.success) {
        if (!result.modified_path.empty()) {
            std::filesystem::remove(result.modified_path);
        }
        return std::nullopt;
    }
    std::string bytes = read_bytes(result.modified_path);
    std::filesystem::remove(result.modified_path);
    return bytes;
}

} // namespace

TEST_CASE("gcode edit list: every golden case is byte-identical to its expected output",
          "[plugin_server_edits][gcode][modifier]") {
    const auto names = golden_cases();
    // An empty directory would pass by running nothing.
    REQUIRE(names.size() >= 10);

    for (const auto& name : names) {
        CAPTURE(name);
        const json spec = read_spec(name);
        REQUIRE(spec.is_object());
        const auto list = from_server_edits(spec["edits"]);
        REQUIRE(list.has_value());

        // The app writes exactly the list the plugin is handed.
        const auto round_trip = to_server_edits(*list);
        REQUIRE(round_trip.has_value());
        CHECK(*round_trip == spec["edits"]);

        const std::string input = golden_dir() + "/" + name + "/input.gcode";
        const bool refused =
            spec.contains("refused") && spec["refused"].is_boolean() && spec["refused"].get<bool>();
        for (const bool streaming : {true, false}) {
            CAPTURE(streaming);
            const auto out = apply_list(input, *list, streaming);
            if (refused) {
                CHECK_FALSE(out.has_value());
            } else {
                REQUIRE(out.has_value());
                CHECK(*out == read_bytes(golden_dir() + "/" + name + "/expected.gcode"));
            }
        }
    }
}

TEST_CASE("gcode edit list: the app's producers write the fixtures' lists",
          "[plugin_server_edits][gcode][modifier]") {
    auto scan_of = [](const std::string& name) {
        return GCodeOpsDetector{}.scan_content(
            read_bytes(golden_dir() + "/" + name + "/input.gcode"));
    };

    SECTION("a detected command commented out") {
        const auto scan = scan_of("comment_out");
        GCodeFileModifier modifier;
        modifier.disable_operations(scan, {OperationType::BED_MESH});
        CHECK(to_server_edits({modifier.modifications(), {}}) == read_spec("comment_out")["edits"]);
    }

    SECTION("skip parameters on the start macro") {
        const auto scan = scan_of("print_start_skip_params");
        GCodeFileModifier modifier;
        REQUIRE(modifier.add_print_start_skip_params(scan,
                                                     {{"SKIP_BED_MESH", "1"}, {"SKIP_QGL", "1"}}));
        CHECK(to_server_edits({modifier.modifications(), {}}) ==
              read_spec("print_start_skip_params")["edits"]);
    }

    SECTION("a macro parameter switched off") {
        const auto scan = scan_of("macro_param_rewrite");
        const auto op = scan.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        GCodeFileModifier modifier;
        REQUIRE(modifier.disable_operation(*op));
        CHECK(to_server_edits({modifier.modifications(), {}}) ==
              read_spec("macro_param_rewrite")["edits"]);
    }

    SECTION("a tool remap") {
        CHECK(to_server_edits({{}, {{0, 1}, {1, 0}}}) == read_spec("tool_remap")["edits"]);
    }
}

TEST_CASE("gcode edit list: a list that cannot travel stays on the screen",
          "[plugin_server_edits][gcode]") {
    const auto replace = Modification::replace(2, "NEW").expecting("OLD");

    CHECK_FALSE(to_server_edits({}).has_value());
    CHECK_FALSE(to_server_edits({{replace}, {{0, 1}}}).has_value());
    CHECK_FALSE(to_server_edits({{Modification::replace(0, "NEW")}, {}}).has_value());
    CHECK_FALSE(to_server_edits({{Modification::replace(2, "NE\xffW")}, {}}).has_value());
    CHECK_FALSE(
        to_server_edits({{Modification::comment_out(2).expecting("G29 \xc3")}, {}}).has_value());
    CHECK_FALSE(to_server_edits({{Modification::comment_out(2, "\xed\xa0\x80")}, {}}).has_value());
    CHECK_FALSE(to_server_edits({{Modification::comment_out_range(4, 3)}, {}}).has_value());
    Modification ranged_replace = Modification::replace(2, "NEW");
    ranged_replace.end_line_number = 3;
    CHECK_FALSE(to_server_edits({{ranged_replace}, {}}).has_value());
    CHECK_FALSE(to_server_edits({{}, {{-1, 0}}}).has_value());

    // UTF-8 text travels as it is.
    const auto ok = to_server_edits({{Modification::replace(2, "M117 Düse")}, {}});
    REQUIRE(ok.has_value());
    CHECK((*ok)[0]["args"]["gcode"] == "M117 Düse");

    // Text an edit kind never writes is left out rather than refused.
    const auto inject = to_server_edits({{Modification::inject_after(2, "M84", "why")}, {}});
    REQUIRE(inject.has_value());
    CHECK((*inject)[0]["args"] == json{{"gcode", "M84"}});
}

TEST_CASE("gcode edit list: a malformed list is refused as the plugin refuses it",
          "[plugin_server_edits][gcode]") {
    auto edit = [](json line, const char* action, json args) {
        return json{{"line_number", std::move(line)},
                    {"expect_raw_line", ""},
                    {"action", action},
                    {"args", std::move(args)}};
    };
    const json remap = edit(0, "tool_remap", {{"map", {{"0", 1}}}});

    CHECK_FALSE(from_server_edits(json::object()).has_value());
    CHECK_FALSE(from_server_edits(json::array({"x"})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(1, "explode", json::object())})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(1, "replace", json::object())})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(1, "delete", {{"gcode", "X"}})})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(0, "delete", json::object())})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(true, "delete", json::object())})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit("1", "delete", json::object())})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(3, "delete", {{"end_line", 2}})})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(1, "replace", {{"gcode", 7}})})).has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(0, "tool_remap", {{"map", {{"T0", 1}}}})}))
                    .has_value());
    CHECK_FALSE(from_server_edits(json::array({edit(0, "tool_remap", {{"map", {{"0", -1}}}})}))
                    .has_value());
    CHECK_FALSE(
        from_server_edits(json::array({edit(1, "tool_remap", {{"map", {{"0", 1}}}})})).has_value());
    CHECK_FALSE(
        from_server_edits(json::array({remap, edit(1, "delete", json::object())})).has_value());

    const auto parsed = from_server_edits(json::array({remap}));
    REQUIRE(parsed.has_value());
    CHECK(parsed->remap == std::map<int, int>{{0, 1}});
    CHECK(parsed->modifications.empty());
}

TEST_CASE("gcode edit list: the plugin rewrites only when its feature is ready",
          "[plugin_server_edits][gcode]") {
    auto entry = [](int version, bool available, bool enabled) {
        PluginFeatureEntry e;
        e.version = version;
        e.available = available;
        e.enabled = enabled;
        return e;
    };
    auto state = [](bool plugin, const PluginFeatureMap& features) {
        return plugin_feature_state(plugin, features, kServerEditsFeature, kServerEditsMinVersion);
    };
    const PluginFeatureMap ready = {{kServerEditsFeature, entry(1, true, true)}};

    struct Case {
        const char* what;
        PluginFeatureState feature;
    };
    const Case cases[] = {
        {"no plugin", state(false, ready)},
        {"a plugin without the features map", state(true, {})},
        {"switched off", state(true, {{kServerEditsFeature, entry(1, true, false)}})},
        {"unavailable", state(true, {{kServerEditsFeature, entry(1, false, false)}})},
        {"contract below the minimum", state(true, {{kServerEditsFeature, entry(0, true, true)}})},
        {"ready", state(true, ready)},
    };
    for (const auto& c : cases) {
        CAPTURE(c.what);
        const bool is_ready = c.feature == PluginFeatureState::Ready;
        CHECK(is_ready == (std::string(c.what) == "ready"));
        CHECK(choose_edit_transport(c.feature, false, true) ==
              (is_ready ? EditTransport::Plugin : EditTransport::DownloadRewrite));
        CHECK(choose_edit_transport(c.feature, true, true) == EditTransport::DownloadRewrite);
        CHECK(choose_edit_transport(c.feature, false, false) == EditTransport::DownloadRewrite);
    }
}

TEST_CASE("gcode edit list: the wait for print_edited grows with the file",
          "[plugin_server_edits][gcode]") {
    constexpr uint64_t kMiB = 1024 * 1024;
    CHECK(print_edited_timeout_ms(1) == kPrintEditedBaseTimeoutMs + kPrintEditedTimeoutMsPerMiB);
    CHECK(print_edited_timeout_ms(100 * kMiB) ==
          kPrintEditedBaseTimeoutMs + 100 * kPrintEditedTimeoutMsPerMiB);
    // Never shorter than the plugin's worker limit (600 s + 60 s/MiB) plus the start
    // it answers after, or the app gives up on a rewrite the plugin is still doing.
    for (const uint64_t size : {uint64_t{1}, 3 * kMiB / 2, 100 * kMiB, 900 * kMiB}) {
        INFO(size);
        const double plugin_limit_ms =
            1000.0 * (600.0 + 60.0 * static_cast<double>(size) / static_cast<double>(kMiB));
        CHECK(print_edited_timeout_ms(size) >= plugin_limit_ms + kPrintEditedStartMarginMs);
    }
    CHECK(print_edited_timeout_ms(0) == print_edited_timeout_ms(kPrintEditedAssumedBytes));
    CHECK(print_edited_timeout_ms(UINT64_MAX / 2) == UINT32_MAX);
}

TEST_CASE("gcode edit list: only a refusal before anything ran falls back",
          "[plugin_server_edits][gcode]") {
    auto rpc = [](int code, const char* message) {
        return MoonrakerError::from_json_rpc({{"code", code}, {"message", message}},
                                             "server.helix.print_edited");
    };
    MoonrakerError lost;
    lost.type = MoonrakerErrorType::CONNECTION_LOST;

    CHECK(classify_print_edited_failure(MoonrakerError::timeout(
              "server.helix.print_edited", 1000)) == EditedPrintFailure::Unanswered);
    CHECK(classify_print_edited_failure(lost) == EditedPrintFailure::Unanswered);

    CHECK(classify_print_edited_failure(rpc(-32601, "Method not found")) ==
          EditedPrintFailure::Unsupported);
    CHECK(classify_print_edited_failure(rpc(400, "Invalid edit list: edit 0: unknown action")) ==
          EditedPrintFailure::Unsupported);
    CHECK(classify_print_edited_failure(rpc(503, "Server-side edits are unavailable")) ==
          EditedPrintFailure::Unsupported);

    CHECK(classify_print_edited_failure(
              rpc(409, "Refusing to edit a.gcode: line 3 is not the line the scan found")) ==
          EditedPrintFailure::Refused);
    CHECK(classify_print_edited_failure(rpc(400, "Original file not found: a.gcode")) ==
          EditedPrintFailure::Refused);
    CHECK(classify_print_edited_failure(rpc(500, "Failed to write the edited copy")) ==
          EditedPrintFailure::Refused);
    CHECK(classify_print_edited_failure(rpc(-32601, "Namespace x not found")) ==
          EditedPrintFailure::Refused);
}
