// SPDX-License-Identifier: GPL-3.0-or-later

// KlipperConfigEditor and the HelixPrint plugin's config_edit feature: the
// fixture tree moonraker-plugin's pytest also reads (both parsers attribute
// every section and option to the same file and line), the route over every
// plugin_feature state, the edit list the plugin path sends, and the editor
// taking the plugin path when the feature is ready and the file API otherwise.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "helix_plugin_features.h"
#include "klipper_config_editor.h"
#include "klipper_config_includes.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::system;
using json = nlohmann::json;

namespace {

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/klipper_config_tree";
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Every config file of the fixture tree, keyed by its path under the tree.
std::map<std::string, std::string> fixture_files() {
    std::map<std::string, std::string> files;
    const std::filesystem::path root = fixture_dir();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cfg") {
            files[std::filesystem::relative(entry.path(), root).generic_string()] =
                read_file(entry.path());
        }
    }
    return files;
}

json fixture_expected() {
    return json::parse(read_file(fixture_dir() + "/expected.json"), nullptr, false);
}

PluginFeatureMap config_edit_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features[kConfigEditFeature] = PluginFeatureEntry{version, available, enabled, {}, ""};
    return features;
}

/// A server.helix.config result holding one section.
json read_result(const std::string& section, json options, bool regular = true) {
    json defined = json::array();
    defined.push_back({{"file", "printer.cfg"}, {"line", 1}, {"autosave", !regular}});
    return {{"sections", {{section, {{"options", options}, {"defined_in", defined}}}}}};
}

json option(const std::string& value, bool autosave = false) {
    return {{"value", value}, {"file", "printer.cfg"}, {"line", 2}, {"autosave", autosave}};
}

/// Answers the plugin's config endpoints from test-controlled replies and
/// records every call; everything else goes to the stock mock.
class ConfigPluginMockClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    json config_reply;
    json edit_reply = {{"dry_run", false}, {"files", json::array()}, {"backups", json::array()}};
    bool fail_read = false;

    std::vector<std::pair<std::string, json>> calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }
    std::vector<json> params_of(const std::string& method) const {
        std::vector<json> out;
        for (const auto& [m, p] : calls()) {
            if (m == method)
                out.push_back(p);
        }
        return out;
    }

    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb,
                 uint32_t timeout_ms = 0, bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls_.emplace_back(method, params);
        }
        if (method == "server.helix.config") {
            if (fail_read) {
                if (error_cb)
                    error_cb(MoonrakerError::unknown("Method not found", method));
            } else if (success_cb) {
                success_cb({{"result", config_reply}});
            }
            return 0;
        }
        if (method == "server.helix.config_edit") {
            if (success_cb)
                success_cb({{"result", edit_reply}});
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, json>> calls_;
};

} // namespace

// ============================================================================
// One fixture tree, two parsers
// ============================================================================

TEST_CASE("Config fixture tree: the app's include resolution matches the plugin's",
          "[plugin_config_edit][config][includes]") {
    const auto files = fixture_files();
    const json expected = fixture_expected();
    REQUIRE(expected.is_object());

    SECTION("the same files are active") {
        std::set<std::string> want;
        for (const auto& f : expected["files"])
            want.insert(f.get<std::string>());
        CHECK(resolve_active_files(files, "printer.cfg") == want);
    }

    SECTION("each section belongs to the file of its last definition in read order") {
        KlipperConfigEditor editor;
        auto sections = editor.resolve_includes(files, "printer.cfg");
        std::map<std::string, std::string> got;
        for (const auto& [name, loc] : sections)
            got[name] = loc.file_path;
        std::map<std::string, std::string> want;
        for (const auto& [name, file] : expected["sections"].items())
            want[name] = file.get<std::string>();
        CHECK(got == want);
    }

    SECTION("each regular option is on the line the plugin names") {
        int checked = 0;
        for (const auto& [section, options] : expected["options"].items()) {
            for (const auto& [name, where] : options.items()) {
                if (where["autosave"].get<bool>())
                    continue;
                const std::string file = where["file"].get<std::string>();
                INFO("[" << section << "] " << name << " in " << file);
                REQUIRE(files.count(file) == 1);
                auto key =
                    KlipperConfigEditor::parse_structure(files.at(file)).find_key(section, name);
                REQUIRE(key.has_value());
                CHECK(key->line_number + 1 == where["line"].get<int>());
                ++checked;
            }
        }
        CHECK(checked >= 15);
    }
}

// ============================================================================
// The route, over every plugin_feature state
// ============================================================================

TEST_CASE("config_edit_route over every plugin_feature state", "[plugin_config_edit][config]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        ConfigEditRoute expected;
    };
    const Row rows[] = {
        {"no plugin", false, config_edit_map(1, true, true), ConfigEditRoute::FileApi},
        {"old plugin without a features map", true, {}, ConfigEditRoute::FileApi},
        {"switched off", true, config_edit_map(1, true, false), ConfigEditRoute::FileApi},
        {"unavailable", true, config_edit_map(1, false, false), ConfigEditRoute::FileApi},
        {"contract below the minimum", true, config_edit_map(0, true, true),
         ConfigEditRoute::FileApi},
        {"ready", true, config_edit_map(1, true, true), ConfigEditRoute::Plugin},
        {"ready at a newer contract", true, config_edit_map(2, true, true),
         ConfigEditRoute::Plugin},
    };
    for (const auto& row : rows) {
        INFO(row.what);
        CHECK(config_edit_route(
                  plugin_feature_state(row.plugin_available, row.features, kConfigEditFeature,
                                       kConfigEditFeatureMinVersion)) == row.expected);
    }
}

// ============================================================================
// The edit list the plugin path sends
// ============================================================================

TEST_CASE("plan_plugin_config_edits carries each option's current value",
          "[plugin_config_edit][config]") {
    using Kind = PluginConfigEditPlan::Kind;
    const json config =
        read_result("extruder", {{"control", option("pid")}, {"pid_kp", option("26.2")}});

    SECTION("SET_VALUE expects the value it replaces") {
        auto plan = plan_plugin_config_edits(
            "extruder", {{ConfigEdit::Type::SET_VALUE, "control", "mpc"}}, config);
        REQUIRE(plan.kind == Kind::Edits);
        CHECK(plan.edits == json::array({{{"section", "extruder"},
                                          {"option", "control"},
                                          {"action", "set"},
                                          {"value", "mpc"},
                                          {"expect", "pid"}}}));
    }

    SECTION("ADD_KEY of an absent key expects it absent") {
        auto plan = plan_plugin_config_edits(
            "extruder", {{ConfigEdit::Type::ADD_KEY, "heater_power", "40"}}, config);
        REQUIRE(plan.kind == Kind::Edits);
        CHECK(plan.edits[0]["action"] == "set");
        CHECK(plan.edits[0]["expect"].is_null());
    }

    SECTION("REMOVE_KEY comments out a present key and skips an absent one") {
        auto plan = plan_plugin_config_edits("extruder",
                                             {{ConfigEdit::Type::REMOVE_KEY, "pid_kp", ""},
                                              {ConfigEdit::Type::REMOVE_KEY, "pid_ki", ""}},
                                             config);
        REQUIRE(plan.kind == Kind::Edits);
        REQUIRE(plan.edits.size() == 1);
        CHECK(plan.edits[0] == json{{"section", "extruder"},
                                    {"option", "pid_kp"},
                                    {"action", "remove"},
                                    {"expect", "26.2"}});
    }

    SECTION("a key edited twice expects its own earlier edit") {
        auto plan = plan_plugin_config_edits("extruder",
                                             {{ConfigEdit::Type::SET_VALUE, "control", "mpc"},
                                              {ConfigEdit::Type::SET_VALUE, "control", "pid"}},
                                             config);
        REQUIRE(plan.kind == Kind::Edits);
        CHECK(plan.edits[1]["expect"] == "mpc");
    }

    SECTION("SET_VALUE of an absent key is an error") {
        auto plan = plan_plugin_config_edits(
            "extruder", {{ConfigEdit::Type::SET_VALUE, "heater_power", "40"}}, config);
        CHECK(plan.kind == Kind::Error);
    }

    SECTION("a missing section is an error unless every edit is ADD_KEY") {
        const json empty = json{{"sections", json::object()}};
        auto set = plan_plugin_config_edits(
            "extrudr", {{ConfigEdit::Type::SET_VALUE, "control", "mpc"}}, empty);
        CHECK(set.kind == Kind::Error);
        CHECK(set.error.find("not found") != std::string::npos);
        auto add = plan_plugin_config_edits(
            "input_shaper", {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}, empty);
        CHECK(add.kind == Kind::Edits);
    }

    SECTION("a section only the SAVE_CONFIG block has counts as missing") {
        const json autosave_only =
            read_result("input_shaper", {{"shaper_type_x", option("mzv", true)}}, false);
        auto set = plan_plugin_config_edits(
            "input_shaper", {{ConfigEdit::Type::SET_VALUE, "shaper_freq_y", "40"}}, autosave_only);
        CHECK(set.kind == Kind::Error);
    }

    SECTION("an option in the SAVE_CONFIG block goes to the file API") {
        const json autosave =
            read_result("input_shaper", {{"shaper_freq_x", option("48.2", true)}}, false);
        auto plan = plan_plugin_config_edits(
            "input_shaper", {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}, autosave);
        CHECK(plan.kind == Kind::FileApi);
    }

    SECTION("an empty list goes to the file API") {
        CHECK(plan_plugin_config_edits("extruder", {}, config).kind == Kind::FileApi);
    }
}

TEST_CASE("plugin_config_edit_backups reads the backups a config_edit names",
          "[plugin_config_edit][config]") {
    const json result = {{"backups",
                          {{{"file", "printer.cfg"}, {"backup", "printer.cfg.helixbak-1"}},
                           {{"file", ""}, {"backup", "x"}},
                           "junk"}}};
    auto backups = plugin_config_edit_backups(result);
    REQUIRE(backups.size() == 1);
    CHECK(backups[0] ==
          std::make_pair(std::string("printer.cfg.helixbak-1"), std::string("printer.cfg")));
    CHECK(plugin_config_edit_backups(json::object()).empty());
}

// ============================================================================
// The editor takes the plugin path when the feature is ready
// ============================================================================

namespace {

struct EditorRun {
    bool succeeded = false;
    std::string error;
};

void run_multi_edit(LVGLTestFixture& fixture, KlipperConfigEditor& editor, MoonrakerAPIMock& api,
                    const std::string& section, const std::vector<ConfigEdit>& edits,
                    EditorRun& run, int restart_timeout_ms = 300) {
    editor.set_restart_poll_interval(std::chrono::milliseconds(1));
    editor.safe_multi_edit(
        api, section, edits, [&run]() { run.succeeded = true; },
        [&run](const std::string& err) { run.error = err; }, restart_timeout_ms);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    // The health monitor holds `api` and `editor` by reference: never leave it running.
    REQUIRE(fixture.wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; },
                               15000));
    helix::ui::UpdateQueue::instance().drain();
    // The mock's FIRMWARE_RESTART arms a 1s timer; run it out inside this test.
    fixture.process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
}

const std::string kShaperRoot =
    "[printer]\nkinematics: corexy\n[input_shaper]\nshaper_freq_x: 40\n";

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "safe_multi_edit writes through the plugin when it is ready",
                 "[plugin_config_edit][config][editor]") {
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", kShaperRoot}});
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);
    client.config_reply = read_result("input_shaper", {{"shaper_freq_x", option("40")}});

    KlipperConfigEditor editor;
    editor.set_feature_state_source([]() { return PluginFeatureState::Ready; });
    EditorRun run;
    run_multi_edit(*this, editor, api, "input_shaper",
                   {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"},
                    {ConfigEdit::Type::ADD_KEY, "shaper_type_x", "mzv"}},
                   run);

    CHECK(run.error.empty());
    CHECK(run.succeeded);
    auto reads = client.params_of("server.helix.config");
    REQUIRE(reads.size() == 1);
    CHECK(reads[0] == json{{"section", "input_shaper"}});
    auto writes = client.params_of("server.helix.config_edit");
    REQUIRE(writes.size() == 1);
    CHECK(writes[0]["dry_run"] == false);
    CHECK(writes[0]["edits"] == json::array({{{"section", "input_shaper"},
                                              {"option", "shaper_freq_x"},
                                              {"action", "set"},
                                              {"value", "41.6"},
                                              {"expect", "40"}},
                                             {{"section", "input_shaper"},
                                              {"option", "shaper_type_x"},
                                              {"action", "set"},
                                              {"value", "mzv"},
                                              {"expect", nullptr}}}));
    // The screen wrote nothing itself: no upload, no .helix_backup copy.
    CHECK(api.get_uploaded_config("printer.cfg") == kShaperRoot);
    CHECK(client.params_of("server.files.copy").empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_multi_edit keeps the file API when the plugin is not ready",
                 "[plugin_config_edit][config][editor]") {
    const PluginFeatureState state_under_test =
        GENERATE(PluginFeatureState::Absent, PluginFeatureState::Disabled);
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", kShaperRoot}});
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);

    KlipperConfigEditor editor;
    editor.set_feature_state_source([state_under_test]() { return state_under_test; });
    EditorRun run;
    run_multi_edit(*this, editor, api, "input_shaper",
                   {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}, run);

    CHECK(run.succeeded);
    CHECK(client.params_of("server.helix.config").empty());
    CHECK(client.params_of("server.helix.config_edit").empty());
    auto written = api.get_uploaded_config("printer.cfg");
    REQUIRE(written.has_value());
    CHECK(written->find("shaper_freq_x: 41.6") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "plugin path falls back to the file API for a SAVE_CONFIG option or a failed read",
                 "[plugin_config_edit][config][editor]") {
    const bool fail_read = GENERATE(false, true);
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", kShaperRoot}});
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);
    client.fail_read = fail_read;
    client.config_reply =
        read_result("input_shaper", {{"shaper_freq_x", option("48.2", true)}}, false);

    KlipperConfigEditor editor;
    editor.set_feature_state_source([]() { return PluginFeatureState::Ready; });
    EditorRun run;
    run_multi_edit(*this, editor, api, "input_shaper",
                   {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}, run);

    CHECK(run.succeeded);
    CHECK(client.params_of("server.helix.config").size() == 1);
    CHECK(client.params_of("server.helix.config_edit").empty());
    auto written = api.get_uploaded_config("printer.cfg");
    REQUIRE(written.has_value());
    CHECK(written->find("shaper_freq_x: 41.6") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "plugin path restores the plugin's backups when Klipper does not come back",
                 "[plugin_config_edit][config][editor]") {
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", kShaperRoot}});
    // Disconnected and never back: the monitor times out and reverts.
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::DISCONNECTED);
    client.config_reply = read_result("input_shaper", {{"shaper_freq_x", option("40")}});
    client.edit_reply = {
        {"dry_run", false},
        {"files", {"printer.cfg"}},
        {"backups", {{{"file", "printer.cfg"}, {"backup", "printer.cfg.helixbak-20261022-1"}}}}};

    KlipperConfigEditor editor;
    editor.set_feature_state_source([]() { return PluginFeatureState::Ready; });
    EditorRun run;
    run_multi_edit(*this, editor, api, "input_shaper",
                   {{ConfigEdit::Type::SET_VALUE, "shaper_freq_x", "41.6"}}, run);

    CHECK_FALSE(run.succeeded);
    CHECK(run.error.find("Original config restored") != std::string::npos);
    auto copies = client.params_of("server.files.copy");
    REQUIRE(copies.size() == 1);
    CHECK(copies[0]["source"] == "config/printer.cfg.helixbak-20261022-1");
    CHECK(copies[0]["dest"] == "config/printer.cfg");
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_edit_value takes the plugin path when it is ready",
                 "[plugin_config_edit][config][editor]") {
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", "[probe]\nz_offset: 1.2\n"}});
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);
    client.config_reply = read_result("probe", {{"z_offset", option("1.2")}});

    KlipperConfigEditor editor;
    editor.set_feature_state_source([]() { return PluginFeatureState::Ready; });
    editor.set_restart_poll_interval(std::chrono::milliseconds(1));
    bool succeeded = false;
    editor.safe_edit_value(
        api, "probe", "z_offset", "1.3", [&]() { succeeded = true; }, [](const std::string&) {},
        300);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 15000));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(succeeded);
    auto writes = client.params_of("server.helix.config_edit");
    REQUIRE(writes.size() == 1);
    CHECK(writes[0]["edits"][0]["expect"] == "1.2");
    CHECK(writes[0]["edits"][0]["value"] == "1.3");
}

// ============================================================================
// Backups and the SAVE_CONFIG block on the file-API path
// ============================================================================

TEST_CASE("config backups live under .helix_backups at the file's own path",
          "[plugin_config_edit][config]") {
    CHECK(config_backup_path("printer.cfg") == ".helix_backups/printer.cfg.helix_backup");
    CHECK(config_backup_path("conf.d/a.cfg") == ".helix_backups/conf.d/a.cfg.helix_backup");
    CHECK(config_backup_original(".helix_backups/conf.d/a.cfg.helix_backup") == "conf.d/a.cfg");
    CHECK_FALSE(config_backup_original("printer.cfg.helix_backup"));
    CHECK_FALSE(config_backup_original(".helix_backups/printer.cfg.helixbak-20261022-101500"));
    CHECK_FALSE(config_backup_original(".helix_backups/.helix_backup"));
}

TEST_CASE_METHOD(LVGLTestFixture, "cleanup_backups deletes only the editor's own backups",
                 "[plugin_config_edit][config]") {
    PrinterState state;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api(client, state);
    // A .helix_backup outside .helix_backups is a file the user named, not
    // one restore_backups() would ever restore.
    api.set_config_files({{"printer.cfg", "[printer]\n"},
                          {"printer.cfg.helix_backup", "[printer]\n"},
                          {".helix_backups/printer.cfg.helix_backup", "[printer]\n"},
                          {".helix_backups/conf.d/a.cfg.helix_backup", "[a]\n"}});

    KlipperConfigEditor editor;
    bool completed = false;
    editor.cleanup_backups(api, [&completed]() { completed = true; });

    CHECK(completed);
    CHECK(api.files_mock().deleted_files() ==
          std::vector<std::string>{"config/.helix_backups/conf.d/a.cfg.helix_backup",
                                   "config/.helix_backups/printer.cfg.helix_backup"});
}

namespace {

const std::string kRootWithAutosave = "[include shaper.cfg]\n"
                                      "[printer]\n"
                                      "kinematics: corexy\n"
                                      "\n"
                                      "#*# <---------------------- SAVE_CONFIG "
                                      "---------------------->\n"
                                      "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are "
                                      "auto-generated.\n"
                                      "#*#\n"
                                      "#*# [input_shaper]\n"
                                      "#*# shaper_freq_x = 48.2\n";
const std::string kShaperInclude = "[input_shaper]\nshaper_type_y: ei\n";

} // namespace

TEST_CASE("config_edit_target_file sends a SAVE_CONFIG-only option to printer.cfg",
          "[plugin_config_edit][config]") {
    KlipperConfigEditor editor;
    const std::map<std::string, std::string> files = {{"printer.cfg", kRootWithAutosave},
                                                      {"shaper.cfg", kShaperInclude}};
    const auto sections = editor.resolve_includes(files, "printer.cfg");
    REQUIRE(sections.at("input_shaper").file_path == "shaper.cfg");

    CHECK(config_edit_target_file(sections, kRootWithAutosave, "input_shaper",
                                  {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}) ==
          "printer.cfg");
    CHECK(config_edit_target_file(sections, kRootWithAutosave, "input_shaper",
                                  {{ConfigEdit::Type::ADD_KEY, "shaper_type_y", "mzv"}}) ==
          "shaper.cfg");
    CHECK(config_edit_target_file(sections, kRootWithAutosave, "input_shaper",
                                  {{ConfigEdit::Type::ADD_KEY, "damping_ratio_x", "0.1"}}) ==
          "shaper.cfg");
    CHECK(config_edit_target_file(sections, kRootWithAutosave, "firmware_retraction",
                                  {{ConfigEdit::Type::ADD_KEY, "retract_length", "1"}}) ==
          "printer.cfg");
    CHECK(config_edit_target_file(sections, kRootWithAutosave, "firmware_retraction",
                                  {{ConfigEdit::Type::SET_VALUE, "retract_length", "1"}})
              .empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "file API writes a SAVE_CONFIG-only option into printer.cfg behind a backup",
                 "[plugin_config_edit][config][editor]") {
    PrinterState state;
    ConfigPluginMockClient client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({{"printer.cfg", kRootWithAutosave}, {"shaper.cfg", kShaperInclude}});
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);

    KlipperConfigEditor editor;
    editor.set_feature_state_source([]() { return PluginFeatureState::Absent; });
    EditorRun run;
    run_multi_edit(*this, editor, api, "input_shaper",
                   {{ConfigEdit::Type::ADD_KEY, "shaper_freq_x", "41.6"}}, run);

    CHECK(run.succeeded);
    auto root = api.get_uploaded_config("printer.cfg");
    REQUIRE(root.has_value());
    CHECK(root->find("[input_shaper]\nshaper_freq_x: 41.6") < root->find("#*# <---"));
    CHECK(api.get_uploaded_config("shaper.cfg") == kShaperInclude);

    auto dirs = client.params_of("server.files.post_directory");
    REQUIRE(dirs.size() == 1);
    CHECK(dirs[0]["path"] == "config/.helix_backups");
    auto copies = client.params_of("server.files.copy");
    REQUIRE(copies.size() == 1);
    CHECK(copies[0]["source"] == "config/printer.cfg");
    CHECK(copies[0]["dest"] == "config/.helix_backups/printer.cfg.helix_backup");
}
