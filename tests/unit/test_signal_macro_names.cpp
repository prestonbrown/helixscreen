// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_signal_macro_names.cpp
 * @brief The signal macro names and the forwarders that keep a visible name callable.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix_plugin_features.h"
#include "macro_manager.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "signal_macro_names.h"
#include "sliced_signal_names.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace sm = helix::signal_macros;
using nlohmann::json;
using sm::SignalCaller;
using sm::SignalNameDrift;

namespace {

/// Names of every [gcode_macro _HELIX_*] section in column 0 of the content.
std::vector<std::string> hidden_sections(const std::string& content) {
    std::vector<std::string> names;
    std::istringstream in(content);
    std::string line;
    const std::string open = "[gcode_macro _HELIX_";
    while (std::getline(in, line)) {
        if (line.rfind(open, 0) == 0 && line.back() == ']') {
            names.push_back(line.substr(13, line.size() - 14));
        }
    }
    return names;
}

json macro(const std::string& gcode, const std::string& description = "G-Code macro") {
    return {{"gcode", gcode}, {"description", description}};
}

/// The configfile.settings entry Klipper reports for one of our forwarders.
json forwarder(const std::string& visible) {
    return macro(sm::hidden_name(visible),
                 std::string(sm::kAliasDescriptionPrefix) + sm::hidden_name(visible));
}

/// Every hidden signal macro, as a printer on the current pack reports them.
json hidden_pack() {
    json s = json::object();
    for (auto visible : sm::kVisibleNames) {
        std::string key = "gcode_macro _" + std::string(visible);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        s[key] = macro("SET_GCODE_VARIABLE MACRO=_HELIX_STATE VARIABLE=phase VALUE='\"X\"'");
    }
    s["gcode_macro helix_start_print"] = macro("_HELIX_PHASE_HOMING\nG28\n_HELIX_READY");
    return s;
}

/// The description line under `[gcode_macro <section>]` in the bundled pack.
std::string pack_description(const std::string& section) {
    std::istringstream in(helix::MacroManager::get_macro_content());
    std::string line;
    const std::string header = "[gcode_macro " + section + "]";
    const std::string prefix = "description: ";
    while (std::getline(in, line)) {
        if (line == header && std::getline(in, line) && line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }
    return {};
}

/// The pack an earlier HelixScreen staged: every signal under its visible name,
/// with the descriptions the bundled pack gives the hidden ones.
json visible_pack() {
    json s = json::object();
    for (auto visible : sm::kVisibleNames) {
        std::string key = "gcode_macro " + std::string(visible);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        s[key] = macro("SET_GCODE_VARIABLE MACRO=_HELIX_STATE VARIABLE=phase VALUE='\"X\"'",
                       pack_description(sm::hidden_name(visible)));
    }
    s["gcode_macro helix_start_print"] = macro("HELIX_PHASE_HOMING\nG28\nHELIX_READY");
    return s;
}

sm::SignalMacroScan scan_of(const json& settings) {
    return sm::scan(settings, helix::MacroManager::pack_section_names());
}

std::vector<std::string> all_names() {
    return {sm::kVisibleNames.begin(), sm::kVisibleNames.end()};
}

std::vector<std::string> all_but(std::string_view name) {
    std::vector<std::string> names;
    for (auto visible : sm::kVisibleNames) {
        if (visible != name) {
            names.emplace_back(visible);
        }
    }
    return names;
}

/// A forwarder for every signal name, as a printer staged with all eleven reports them.
json with_all_forwarders(json s) {
    for (auto visible : sm::kVisibleNames) {
        std::string key = "gcode_macro " + std::string(visible);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        s[key] = forwarder(std::string(visible));
    }
    return s;
}

std::vector<std::string> caller_names(const sm::SignalMacroScan& scan) {
    std::vector<std::string> names;
    for (const auto& c : scan.callers) {
        names.push_back(c.name);
    }
    return names;
}

} // namespace

TEST_CASE("kVisibleNames lists every signal macro the bundled pack defines", "[signal_macros]") {
    const std::string pack = helix::MacroManager::get_macro_content();
    REQUIRE_FALSE(pack.empty());

    std::vector<std::string> pack_signals;
    for (const auto& name : hidden_sections(pack)) {
        if (name != "_HELIX_STATE") {
            pack_signals.push_back(name);
        }
    }

    std::vector<std::string> listed;
    for (auto visible : sm::kVisibleNames) {
        listed.push_back(sm::hidden_name(visible));
    }

    std::sort(pack_signals.begin(), pack_signals.end());
    std::sort(listed.begin(), listed.end());
    CHECK(pack_signals == listed);
}

TEST_CASE("alias_block forwards one visible name to its hidden macro", "[signal_macros]") {
    CHECK(sm::alias_block({"HELIX_READY"}) == "[gcode_macro HELIX_READY]\n"
                                              "description: HelixScreen alias for _HELIX_READY\n"
                                              "gcode:\n"
                                              "    _HELIX_READY\n");
}

TEST_CASE("alias_block writes pack order and drops unknown names", "[signal_macros]") {
    CHECK(sm::alias_block({"HELIX_PHASE_QGL", "NOT_A_SIGNAL", "HELIX_ENDED"}) ==
          "[gcode_macro HELIX_ENDED]\n"
          "description: HelixScreen alias for _HELIX_ENDED\n"
          "gcode:\n"
          "    _HELIX_ENDED\n"
          "\n"
          "[gcode_macro HELIX_PHASE_QGL]\n"
          "description: HelixScreen alias for _HELIX_PHASE_QGL\n"
          "gcode:\n"
          "    _HELIX_PHASE_QGL\n");
}

TEST_CASE("alias_block of no names is empty", "[signal_macros]") {
    CHECK(sm::alias_block({}).empty());
    CHECK(sm::alias_block({"HELIX_START_PRINT"}).empty());
}

// ============================================================================
// scan(): which visible names the printer's own config calls
// ============================================================================

TEST_CASE("scan finds a PRINT_START that calls a visible name", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro print_start"] = macro("G28\nHELIX_READY");

    const auto result = scan_of(s);

    CHECK(result.callers ==
          std::vector<SignalCaller>{{"gcode_macro print_start", "gcode", "HELIX_READY"}});
}

TEST_CASE("scan matches a call in any letter case", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro print_start"] = macro("helix_ready");

    CHECK(caller_names(scan_of(s)) == std::vector<std::string>{"HELIX_READY"});
}

TEST_CASE("scan does not count a longer name that contains a visible one", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro print_start"] = macro("_HELIX_READY\nHELIX_READYX\nMY_HELIX_READY");

    CHECK(scan_of(s).callers.empty());
}

TEST_CASE("scan reads gcode outside gcode_macro sections", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_button x"] = {{"pin", "PA1"}, {"press_gcode", "HELIX_PHASE_HOMING"}};
    s["delayed_gcode later"] = {{"gcode", "HELIX_ENDED"}, {"initial_duration", 1.0}};
    s["idle_timeout"] = {{"gcode", "HELIX_RESET\nM84"}, {"timeout", 600.0}};

    CHECK(scan_of(s).callers ==
          std::vector<SignalCaller>{{"delayed_gcode later", "gcode", "HELIX_ENDED"},
                                    {"gcode_button x", "press_gcode", "HELIX_PHASE_HOMING"},
                                    {"idle_timeout", "gcode", "HELIX_RESET"}});
}

TEST_CASE("scan counts a mention inside a Jinja lookup", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro status"] = macro("{% set m = printer[\"gcode_macro HELIX_READY\"] %}");

    CHECK(caller_names(scan_of(s)) == std::vector<std::string>{"HELIX_READY"});
}

TEST_CASE("kPackDescriptions are the bundled pack's descriptions", "[signal_macros]") {
    for (size_t i = 0; i < sm::kVisibleNames.size(); ++i) {
        const std::string hidden = sm::hidden_name(sm::kVisibleNames[i]);
        INFO(hidden);
        CHECK(pack_description(hidden) == sm::kPackDescriptions[i]);
    }
}

TEST_CASE("scan skips the pack's own sections under either spelling", "[signal_macros]") {
    CHECK(scan_of(hidden_pack()).callers.empty());
    CHECK(scan_of(visible_pack()).callers.empty());
    CHECK(scan_of(visible_pack()).foreign.empty());
}

TEST_CASE("scan lists our forwarders as aliases, not callers", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro helix_ready"] = forwarder("HELIX_READY");
    s["gcode_macro helix_phase_qgl"] = forwarder("HELIX_PHASE_QGL");

    const auto result = scan_of(s);

    CHECK(result.aliases == std::vector<std::string>{"HELIX_READY", "HELIX_PHASE_QGL"});
    CHECK(result.callers.empty());
    CHECK(result.foreign.empty());
}

TEST_CASE("scan keeps a user's own macro of a signal name apart", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro helix_ready"] = macro("RESPOND MSG=\"mine\"", "My ready macro");
    s["gcode_macro print_start"] = macro("HELIX_READY");

    const auto result = scan_of(s);

    CHECK(result.foreign == std::vector<std::string>{"HELIX_READY"});
    CHECK(result.aliases.empty());
    CHECK(caller_names(result) == std::vector<std::string>{"HELIX_READY"});
    // Their macro wins, and a forwarder of the same name would merge into it.
    CHECK(sm::names_to_alias(result, {}) == all_but("HELIX_READY"));
}

TEST_CASE("scan reads a user's HELIX_READY that writes _HELIX_STATE as theirs", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro helix_ready"] =
        macro("SET_GCODE_VARIABLE MACRO=_HELIX_STATE VARIABLE=print_started VALUE=True\n"
              "M117 ready",
              "My own ready signal");

    const auto result = scan_of(s);

    CHECK(result.foreign == std::vector<std::string>{"HELIX_READY"});
    CHECK_FALSE(result.visible_loaded);
}

TEST_CASE("scan sees whether the visible names were loaded", "[signal_macros]") {
    CHECK(scan_of(visible_pack()).visible_loaded);
    CHECK_FALSE(scan_of(hidden_pack()).visible_loaded);

    json forwarded = hidden_pack();
    forwarded["gcode_macro helix_phase_qgl"] = forwarder("HELIX_PHASE_QGL");
    CHECK(scan_of(forwarded).visible_loaded);
}

TEST_CASE("scan sees the hidden names loaded only when all of them are", "[signal_macros]") {
    CHECK(scan_of(hidden_pack()).hidden_loaded);
    CHECK_FALSE(scan_of(visible_pack()).hidden_loaded);

    json partial = hidden_pack();
    partial.erase("gcode_macro _helix_phase_purging");
    CHECK_FALSE(scan_of(partial).hidden_loaded);
}

TEST_CASE("scan of malformed settings is empty", "[signal_macros]") {
    for (const json& bad : {json("oops"), json::array({1, 2}), json::object(), json(nullptr)}) {
        const auto result = scan_of(bad);
        CHECK(result.callers.empty());
        CHECK(result.aliases.empty());
        CHECK_FALSE(result.hidden_loaded);
    }
    json s = hidden_pack();
    s["gcode_macro odd"] = {{"gcode", 5}, {"variable_x", json::array({"HELIX_READY"})}};
    s["not_an_object"] = "HELIX_READY";
    CHECK(scan_of(s).callers.empty());
}

// ============================================================================
// drift(): what is loaded against what an update would stage
// ============================================================================

TEST_CASE("drift reads each loaded state", "[signal_macros]") {
    SECTION("the old visible pack") {
        json s = visible_pack();
        s["gcode_macro print_start"] = macro("HELIX_READY");
        CHECK(sm::drift(scan_of(s), {}) == SignalNameDrift::MoveToHidden);
    }
    SECTION("hidden only, but PRINT_START calls a visible name") {
        json s = hidden_pack();
        s["gcode_macro print_start"] = macro("HELIX_READY");
        CHECK(sm::drift(scan_of(s), {}) == SignalNameDrift::AddAliases);
    }
    SECTION("one forwarder nothing calls: the other ten join it") {
        json s = hidden_pack();
        s["gcode_macro helix_ready"] = forwarder("HELIX_READY");
        CHECK(sm::drift(scan_of(s), {}) == SignalNameDrift::AddAliases);
    }
    SECTION("all eleven forwarders, nothing calling them, stay") {
        CHECK(sm::drift(scan_of(with_all_forwarders(hidden_pack())), {}) == SignalNameDrift::None);
    }
    SECTION("all eleven forwarders and a caller") {
        json s = with_all_forwarders(hidden_pack());
        s["gcode_macro print_start"] = macro("HELIX_READY");
        CHECK(sm::drift(scan_of(s), {}) == SignalNameDrift::None);
    }
    SECTION("hidden only, nothing calls a visible name") {
        CHECK(sm::drift(scan_of(hidden_pack()), {}) == SignalNameDrift::None);
    }
}

TEST_CASE("names_to_alias forwards all eleven once any visible name is in play",
          "[signal_macros]") {
    SECTION("a caller of one name") {
        json s = hidden_pack();
        s["gcode_macro print_start"] = macro("HELIX_PHASE_QGL");
        CHECK(sm::names_to_alias(scan_of(s), {}) == all_names());
    }
    SECTION("the old visible pack, nothing calling it") {
        CHECK(sm::names_to_alias(scan_of(visible_pack()), {}) == all_names());
    }
    SECTION("an extra name alone") {
        CHECK(sm::names_to_alias(scan_of(hidden_pack()), {"HELIX_ENDED"}) == all_names());
    }
    SECTION("a printer that never had the visible names") {
        CHECK(sm::names_to_alias(scan_of(hidden_pack()), {}).empty());
        CHECK(sm::names_to_alias(scan_of(hidden_pack()), {"NOT_A_SIGNAL"}).empty());
    }
}

// ============================================================================
// What MacroManager stages and reports from the scan
// ============================================================================

namespace {

helix::PrinterDiscovery printer_with(const json& settings, const json& objects) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.set_signal_macro_scan(scan_of(settings));
    return hw;
}

const json kOldPackObjects =
    json::array({"gcode_macro HELIX_READY", "gcode_macro _HELIX_STATE",
                 "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_UNLOAD_FILAMENT"});

const json kCurrentPackObjects =
    json::array({"gcode_macro _HELIX_READY", "gcode_macro _HELIX_STATE",
                 "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_UNLOAD_FILAMENT"});

} // namespace

TEST_CASE("macro_content_for forwards all eleven to a printer on the old visible pack",
          "[signal_macros]") {
    // Nothing in the config calls a visible name, but a sliced file's start
    // G-code may, and the scan cannot see it.
    const auto hw = printer_with(visible_pack(), kOldPackObjects);

    const std::string base = helix::MacroManager::get_macro_content();
    const std::string content = helix::MacroManager::macro_content_for(hw);

    REQUIRE(content.rfind(base, 0) == 0);
    CHECK(content.substr(base.size()).find(sm::alias_block(all_names())) != std::string::npos);
}

TEST_CASE("macro_content_for leaves out a name the user defined", "[signal_macros]") {
    json s = visible_pack();
    s["gcode_macro helix_ready"] = macro("M117 mine", "Mine");
    const auto hw = printer_with(s, kOldPackObjects);

    const std::string content = helix::MacroManager::macro_content_for(hw);

    CHECK(content.find("[gcode_macro HELIX_READY]") == std::string::npos);
    CHECK(content.find(sm::alias_block(all_but("HELIX_READY"))) != std::string::npos);
}

TEST_CASE("macro_content_for forwards nothing to a printer that never had the visible names",
          "[signal_macros]") {
    const auto hw = printer_with(hidden_pack(), kCurrentPackObjects);

    CHECK(helix::MacroManager::macro_content_for(hw) == helix::MacroManager::get_macro_content());
}

TEST_CASE("evaluate_status offers an update when a called name has no definition",
          "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro print_start"] = macro("HELIX_READY");

    CHECK(helix::MacroManager::evaluate_status(printer_with(s, kCurrentPackObjects)) ==
          helix::MacroInstallStatus::OUTDATED);
    CHECK(helix::MacroManager::evaluate_status(printer_with(hidden_pack(), kCurrentPackObjects)) ==
          helix::MacroInstallStatus::INSTALLED);

    helix::PrinterDiscovery unscanned;
    unscanned.parse_objects(kCurrentPackObjects);
    CHECK(helix::MacroManager::evaluate_status(unscanned) == helix::MacroInstallStatus::INSTALLED);
}

namespace {

/// Runs the real discovery sequence over the mock's handlers.
class ScanDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }
};

} // namespace

TEST_CASE("discovery records which signal names the config calls", "[signal_macros][discovery]") {
    LVGLTestFixture fixture;
    ScanDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_config_settings_section("gcode_macro my_start", macro("G28\nHELIX_READY"));

    REQUIRE_FALSE(client.hardware().signal_macro_scan().has_value());
    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    const auto scan = client.hardware().signal_macro_scan();
    REQUIRE(scan.has_value());
    const SignalCaller expected{"gcode_macro my_start", "gcode", "HELIX_READY"};
    CHECK(std::find(scan->callers.begin(), scan->callers.end(), expected) != scan->callers.end());
}

TEST_CASE("the mock plugin reports the signal macro migration and its report", "[signal_macros]") {
    helix::ScopedEnv plugin{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    json status;
    json report;
    client.send_jsonrpc(
        "server.helix.status", json::object(),
        [&](const json& r) { status = r.value("result", json()); }, [](const MoonrakerError&) {});
    client.send_jsonrpc(
        "server.helix.signal_migration", json::object(),
        [&](const json& r) { report = r.value("result", json()); }, [](const MoonrakerError&) {});

    const json& entry = status.at("features").at("signal_macro_migration");
    CHECK(entry.at("available") == true);
    CHECK(entry.at("enabled") == true);
    CHECK(report.at("state") == "nothing");

    // The overview names it rather than showing its key.
    helix::PluginFeatureEntry parsed;
    parsed.version = 1;
    parsed.available = true;
    const auto lines = helix::plugin_feature_lines({{"signal_macro_migration", parsed}});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].label != lines[0].name);
}

// ============================================================================
// Names a sliced file calls
// ============================================================================

TEST_CASE("unknown_signal_command reads the signal name Klipper did not know", "[signal_macros]") {
    CHECK(sm::unknown_signal_command("// Unknown command:\"HELIX_READY\"") == "HELIX_READY");
    CHECK(sm::unknown_signal_command("!! Unknown command:\"helix_phase_qgl\"") ==
          "HELIX_PHASE_QGL");
    CHECK_FALSE(sm::unknown_signal_command("Unknown command:\"_HELIX_READY\""));
    CHECK_FALSE(sm::unknown_signal_command("Unknown command:\"FOO\""));
    CHECK_FALSE(sm::unknown_signal_command("Unknown command:\"HELIX_READY"));
    CHECK_FALSE(sm::unknown_signal_command("HELIX:READY"));
}

TEST_CASE("a recorded name is forwarded and marks the macros outdated", "[signal_macros]") {
    const auto hw = printer_with(hidden_pack(), kCurrentPackObjects);

    CHECK(sm::drift(scan_of(hidden_pack()), {"HELIX_READY"}) == SignalNameDrift::AddAliases);
    CHECK(helix::MacroManager::macro_content_for(hw, {"HELIX_READY"})
              .find(sm::alias_block(all_names())) != std::string::npos);
    CHECK(helix::MacroManager::evaluate_status(hw, {"HELIX_READY"}) ==
          helix::MacroInstallStatus::OUTDATED);
    CHECK(helix::MacroManager::evaluate_status(hw, {}) == helix::MacroInstallStatus::INSTALLED);
}

TEST_CASE("the watcher records a signal name Klipper did not know, once", "[signal_macros]") {
    LVGLTestFixture fixture;
    helix::Config* cfg = helix::Config::get_instance();
    const std::string key = cfg->df() + "helix_macros/sliced_signal_names";
    const json saved = cfg->get<json>(key, json());
    cfg->set<json>(key, json::array());
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    auto respond = [&](const std::string& line) {
        client.dispatch_method_callback("notify_gcode_response", {{"params", json::array({line})}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };

    std::vector<std::string> notices;
    {
        helix::SlicedSignalWatcher watcher(
            &client, [&](const std::string& name) { notices.push_back(name); });
        respond("// Unknown command:\"HELIX_READY\"");
        respond("// Unknown command:\"HELIX_READY\"");
        respond("// Unknown command:\"FOO\"");
    }
    respond("// Unknown command:\"HELIX_ENDED\"");

    CHECK(notices == std::vector<std::string>{"HELIX_READY"});
    CHECK(sm::recorded_signal_names() == std::vector<std::string>{"HELIX_READY"});
    CHECK_FALSE(sm::record_signal_name("HELIX_READY"));
    CHECK_FALSE(sm::record_signal_name("NOT_A_SIGNAL"));
    cfg->set<json>(key, saved);
}
