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
#include "printer_plugin_status_state.h"
#include "signal_macro_names.h"
#include "sliced_signal_names.h"

#include <algorithm>
#include <memory>
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
    return macro("RESPOND TYPE=command MSG=\"" + std::string(sm::kAliasAnnouncePrefix) + visible +
                     "\"\n" + sm::hidden_name(visible),
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
        // State, the plan and the two signals that were never spelled visibly have no forwarder.
        if (name != "_HELIX_STATE" && name != "_HELIX_PLAN" && name != "_HELIX_PHASE_SOAKING" &&
            name != "_HELIX_PHASE_HEATING_CHAMBER") {
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
                                              "    RESPOND TYPE=command "
                                              "MSG=\"HELIX:ALIAS:HELIX_READY\"\n"
                                              "    _HELIX_READY\n");
}

TEST_CASE("alias_block writes pack order and drops unknown names", "[signal_macros]") {
    CHECK(sm::alias_block({"HELIX_PHASE_QGL", "NOT_A_SIGNAL", "HELIX_ENDED"}) ==
          "[gcode_macro HELIX_ENDED]\n"
          "description: HelixScreen alias for _HELIX_ENDED\n"
          "gcode:\n"
          "    RESPOND TYPE=command MSG=\"HELIX:ALIAS:HELIX_ENDED\"\n"
          "    _HELIX_ENDED\n"
          "\n"
          "[gcode_macro HELIX_PHASE_QGL]\n"
          "description: HelixScreen alias for _HELIX_PHASE_QGL\n"
          "gcode:\n"
          "    RESPOND TYPE=command MSG=\"HELIX:ALIAS:HELIX_PHASE_QGL\"\n"
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
    json::array({"gcode_macro _HELIX_READY", "gcode_macro _HELIX_STATE", "gcode_macro _HELIX_PLAN",
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

namespace {

/// Restores the active printer's recorded names when it goes out of scope.
struct RecordedNamesScope {
    helix::Config* cfg = helix::Config::get_instance();
    std::string key = cfg->df() + "helix_macros/sliced_signal_names";
    std::string quiet_key = cfg->df() + "helix_macros/alias_quiet_prints";
    json saved = cfg->get<json>(key, json());
    json saved_quiet = cfg->get<json>(quiet_key, json());
    RecordedNamesScope() {
        cfg->set<json>(key, json::array());
        cfg->set<json>(quiet_key, json::object());
    }
    ~RecordedNamesScope() {
        cfg->set<json>(key, saved);
        cfg->set<json>(quiet_key, saved_quiet);
    }
};

} // namespace

TEST_CASE("the watcher records what a print called when it ends, with one notice",
          "[signal_macros]") {
    LVGLTestFixture fixture;
    RecordedNamesScope recorded;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    auto respond = [&](const std::string& line) {
        client.dispatch_method_callback("notify_gcode_response", {{"params", json::array({line})}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };

    std::vector<std::vector<std::string>> notices;
    helix::SlicedSignalWatcher watcher(
        &client, [&](const std::vector<std::string>& names) { notices.push_back(names); });

    // Typed in a console with no print running: neither recorded nor announced.
    respond("// Unknown command:\"HELIX_READY\"");
    watcher.on_print_active(true);
    watcher.on_print_active(false);
    CHECK(sm::recorded_signal_names().empty());
    CHECK(notices.empty());

    // One print calling two names over several frames: nothing until it ends,
    // then one record and one notice listing them in pack order.
    watcher.on_print_active(true);
    respond("// Unknown command:\"HELIX_READY\"");
    respond("// Unknown command:\"HELIX_PHASE_HOMING\"");
    respond("// Unknown command:\"HELIX_READY\"");
    respond("// Unknown command:\"FOO\"");
    CHECK(sm::recorded_signal_names().empty());
    CHECK(notices.empty());
    watcher.on_print_active(false);
    CHECK(sm::recorded_signal_names() ==
          std::vector<std::string>{"HELIX_READY", "HELIX_PHASE_HOMING"});
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] == std::vector<std::string>{"HELIX_READY", "HELIX_PHASE_HOMING"});

    // The next print calling the same names is not announced again.
    watcher.on_print_active(true);
    respond("// Unknown command:\"HELIX_READY\"");
    watcher.on_print_active(false);
    CHECK(notices.size() == 1);
}

TEST_CASE("the watcher's console handler queues only signal name lines", "[signal_macros]") {
    LVGLTestFixture fixture;
    RecordedNamesScope recorded;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain_until_empty();
    helix::SlicedSignalWatcher watcher(&client, [](const std::vector<std::string>&) {});

    const size_t before = queue.pending_count();
    client.dispatch_method_callback("notify_gcode_response",
                                    {{"params", json::array({"// ok", "B:60.0 /60.0"})}});
    CHECK(queue.pending_count() == before);
    client.dispatch_method_callback(
        "notify_gcode_response", {{"params", json::array({"// Unknown command:\"HELIX_READY\""})}});
    CHECK(queue.pending_count() == before + 1);
    queue.drain_until_empty();
}

TEST_CASE("record_signal_names saves only what is new", "[signal_macros]") {
    RecordedNamesScope recorded;
    CHECK(sm::record_signal_names({"HELIX_ENDED", "NOT_A_SIGNAL", "HELIX_READY"}) ==
          std::vector<std::string>{"HELIX_READY", "HELIX_ENDED"});
    CHECK(sm::record_signal_names({"HELIX_READY"}).empty());
    CHECK(sm::recorded_signal_names() == std::vector<std::string>{"HELIX_READY", "HELIX_ENDED"});
}

TEST_CASE("the sliced-file notice points at the update only when it is offered",
          "[signal_macros]") {
    const auto offered = sm::sliced_signal_notice({"HELIX_READY", "HELIX_ENDED"}, true);
    const auto plain = sm::sliced_signal_notice({"HELIX_READY"}, false);
    CHECK(offered.find("HELIX_READY, HELIX_ENDED") != std::string::npos);
    CHECK(offered.find("Update Helper Macros") != std::string::npos);
    CHECK(plain.find("HELIX_READY") != std::string::npos);
    CHECK(plain.find("Update Helper Macros") == std::string::npos);
}

// ============================================================================
// What the helper-macros row says about the names
// ============================================================================

namespace {

/// A hidden-pack printer whose PRINT_START still calls the visible names, as
/// a 1.0.x phase-tracking instrumentation leaves it.
json instrumented_print_start() {
    json s = hidden_pack();
    s["gcode_macro print_start"] = macro("G28\nHELIX_PHASE_HOMING\nM109 S200\nHELIX_READY");
    return s;
}

} // namespace

TEST_CASE("signal_names_line says what the macros row reports", "[signal_macros]") {
    using L = sm::SignalNamesLine;
    using P = helix::PluginFeatureState;
    using R = sm::SignalMigrationReport;
    json own_caller = hidden_pack();
    own_caller["gcode_macro my_start"] = macro("HELIX_READY");
    auto plugin = [](bool installed, bool reported, P feature, R report) {
        sm::SignalMigrationPlugin p;
        p.installed = installed;
        p.reported = reported;
        p.feature = feature;
        p.report = report;
        return p;
    };
    const auto none = plugin(false, false, P::Absent, R::Unknown);
    const auto old_plugin = plugin(true, false, P::Absent, R::Unknown);
    const auto renames = plugin(true, true, P::Ready, R::WillRename);
    const auto instrumented = scan_of(instrumented_print_start());

    CHECK(sm::signal_names_line(scan_of(visible_pack()), renames) == L::None);
    CHECK(sm::signal_names_line(scan_of(hidden_pack()), none) == L::AllHidden);
    CHECK(sm::signal_names_line(scan_of(with_all_forwarders(hidden_pack())), renames) ==
          L::AliasesKept);
    CHECK(sm::signal_names_line(scan_of(own_caller), none) == L::AliasesKept);
    CHECK(sm::signal_names_line(instrumented, renames) == L::PluginWillRename);
    CHECK(sm::signal_names_line(instrumented, old_plugin) == L::UpdatePluginToRename);
    // No plugin at all: nothing to update, the forwarders keep the calls working.
    CHECK(sm::signal_names_line(instrumented, none) == L::AliasesKept);
    // The plugin found no marker block: the call is hand-written and stays.
    CHECK(sm::signal_names_line(instrumented, plugin(true, true, P::Ready, R::NothingFound)) ==
          L::AliasesKept);
    CHECK(sm::signal_names_line(instrumented, plugin(true, true, P::Ready, R::Unknown)) ==
          L::AliasesKept);
    CHECK(sm::signal_names_line(instrumented, plugin(true, true, P::Ready, R::WontRename)) ==
          L::AliasesKept);
    // Switched off in moonraker.conf, or reported but unable to run.
    CHECK(sm::signal_names_line(instrumented, plugin(true, true, P::Disabled, R::WontRename)) ==
          L::AliasesKept);
    CHECK(sm::signal_names_line(instrumented, plugin(true, true, P::Absent, R::Unknown)) ==
          L::AliasesKept);
}

TEST_CASE("the plugin's migration report state reads as what it will do", "[signal_macros]") {
    using R = sm::SignalMigrationReport;
    CHECK(sm::signal_migration_report("nothing") == R::NothingFound);
    CHECK(sm::signal_migration_report("done") == R::WillRename);
    CHECK(sm::signal_migration_report("waiting_for_pack") == R::WillRename);
    for (const char* state : {"disabled", "firmware_managed", "needs_attention", "failed"}) {
        CHECK(sm::signal_migration_report(state) == R::WontRename);
    }
    CHECK(sm::signal_migration_report("") == R::Unknown);
}

TEST_CASE("the macros row's signal line follows the scan and the plugin", "[signal_macros]") {
    LVGLTestFixture fixture;
    helix::PrinterPluginStatusState status;
    status.init_subjects(false);
    auto line = [&status] {
        return static_cast<sm::SignalNamesLine>(
            lv_subject_get_int(status.get_helix_macros_signal_names_subject()));
    };

    CHECK(line() == sm::SignalNamesLine::None);
    status.set_signal_macro_scan(scan_of(instrumented_print_start()));

    helix::PluginFeatureEntry ready;
    ready.version = 1;
    ready.available = true;
    ready.enabled = true;
    CHECK(line() == sm::SignalNamesLine::AliasesKept);
    status.set_installed(true);
    CHECK(line() == sm::SignalNamesLine::UpdatePluginToRename);
    status.set_features({{"signal_macro_migration", ready}});
    CHECK(line() == sm::SignalNamesLine::AliasesKept);
    status.set_signal_migration_report(sm::SignalMigrationReport::WillRename);
    CHECK(line() == sm::SignalNamesLine::PluginWillRename);
    status.set_signal_migration_report(sm::SignalMigrationReport::NothingFound);
    CHECK(line() == sm::SignalNamesLine::AliasesKept);

    status.set_signal_macro_scan(std::nullopt);
    CHECK(line() == sm::SignalNamesLine::None);
    status.deinit_subjects();
}

// ============================================================================
// Narrowing the forwarders to the names something calls (#2006)
// ============================================================================

TEST_CASE("announced_alias reads a forwarder line and nothing else", "[signal_macros]") {
    CHECK(sm::announced_alias("// HELIX:ALIAS:HELIX_READY") == "HELIX_READY");
    CHECK(sm::announced_alias("HELIX:ALIAS:HELIX_PHASE_QGL") == "HELIX_PHASE_QGL");
    CHECK_FALSE(sm::announced_alias("// HELIX:READY"));
    CHECK_FALSE(sm::announced_alias("// HELIX:ALIAS:NOT_A_SIGNAL"));
    CHECK_FALSE(sm::announced_alias("// HELIX:ALIAS:HELIX_READYX"));
    CHECK_FALSE(sm::announced_alias("// HELIX:ALIAS:"));
}

TEST_CASE("scan lists a forwarder as announcing only when it can be heard", "[signal_macros]") {
    json s = hidden_pack();
    s["gcode_macro helix_ready"] = forwarder("HELIX_READY");
    s["gcode_macro helix_ended"] =
        macro("_HELIX_ENDED", std::string(sm::kAliasDescriptionPrefix) + "_HELIX_ENDED");

    SECTION("printer has [respond]") {
        s["respond"] = json::object();
        CHECK(scan_of(s).announcing == std::vector<std::string>{"HELIX_READY"});
        CHECK(scan_of(s).aliases == std::vector<std::string>{"HELIX_READY", "HELIX_ENDED"});
    }
    SECTION("printer lacks [respond]: its forwarders cannot speak") {
        CHECK(scan_of(s).announcing.empty());
    }
}

TEST_CASE("a retired name leaves the pack unless the config calls it", "[signal_macros]") {
    json s = with_all_forwarders(hidden_pack());
    SECTION("nothing calls it") {
        CHECK(sm::names_to_alias(scan_of(s), {}, {"HELIX_ENDED"}) == all_but("HELIX_ENDED"));
        CHECK(sm::drift(scan_of(s), {}, {"HELIX_ENDED"}) == SignalNameDrift::DropAliases);
    }
    SECTION("a config macro calls it") {
        s["gcode_macro print_end"] = macro("HELIX_ENDED");
        CHECK(sm::names_to_alias(scan_of(s), {}, {"HELIX_ENDED"}) == all_names());
        CHECK(sm::drift(scan_of(s), {}, {"HELIX_ENDED"}) == SignalNameDrift::None);
    }
    SECTION("already dropped: nothing left to change") {
        s.erase("gcode_macro helix_ended");
        CHECK(sm::drift(scan_of(s), {}, {"HELIX_ENDED"}) == SignalNameDrift::None);
    }
}

namespace {

/// A watcher whose printer reports `loaded` forwarders at each print start.
struct AliasWatcherRig {
    LVGLTestFixture fixture;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::vector<std::string> loaded{"HELIX_READY", "HELIX_ENDED"};
    bool fresh = true;
    bool completed = true;
    int retired_calls = 0;
    std::unique_ptr<helix::SlicedSignalWatcher> watcher;

    AliasWatcherRig() {
        restart_app();
    }
    /// A new watcher holds no counts: only what was saved in the settings carries over.
    void restart_app() {
        watcher.reset();
        watcher = std::make_unique<helix::SlicedSignalWatcher>(
            &client, [](const std::vector<std::string>&) {});
        watcher->set_print_probes({[this] { return loaded; }, [this] { return fresh; },
                                   [this] { return completed; }, [this] { ++retired_calls; }});
    }
    void say(const std::string& line) {
        client.dispatch_method_callback("notify_gcode_response", {{"params", json::array({line})}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    void print(const std::vector<std::string>& lines = {}) {
        watcher->on_print_active(true);
        for (const auto& line : lines) {
            say(line);
        }
        watcher->on_print_active(false);
    }
};

const std::string kReadyHit = "// HELIX:ALIAS:HELIX_READY";

} // namespace

TEST_CASE("five completed prints without a hit retire a forwarder", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints - 1; ++i) {
        rig.print();
    }
    CHECK(sm::retired_signal_names().empty());
    CHECK(rig.retired_calls == 0);
    rig.print();
    CHECK(sm::retired_signal_names() == std::vector<std::string>{"HELIX_READY", "HELIX_ENDED"});
    CHECK(rig.retired_calls == 1);
}

TEST_CASE("a name heard on the last counted print keeps its forwarder", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints - 1; ++i) {
        rig.print();
    }
    rig.print({kReadyHit});
    CHECK(sm::retired_signal_names() == std::vector<std::string>{"HELIX_ENDED"});
}

TEST_CASE("a hit clears a name's quiet count", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints - 1; ++i) {
        rig.print();
    }
    rig.print({kReadyHit});
    for (int i = 0; i < sm::kAliasDropAfterPrints - 1; ++i) {
        rig.print();
    }
    CHECK(sm::retired_signal_names() == std::vector<std::string>{"HELIX_ENDED"});
}

TEST_CASE("a print that proves nothing leaves the counts alone", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    SECTION("cancelled or failed") {
        rig.completed = false;
    }
    SECTION("joined part-way through") {
        rig.fresh = false;
    }
    SECTION("no announcing forwarders loaded when it started") {
        rig.loaded.clear();
    }
    for (int i = 0; i < sm::kAliasDropAfterPrints + 1; ++i) {
        rig.print();
    }
    CHECK(sm::retired_signal_names().empty());
    CHECK(rig.retired_calls == 0);
}

TEST_CASE("a hit during a cancelled print still clears the count", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints - 1; ++i) {
        rig.print();
    }
    rig.completed = false;
    rig.print({kReadyHit});
    rig.completed = true;
    rig.print();
    CHECK(sm::retired_signal_names() == std::vector<std::string>{"HELIX_ENDED"});
}

TEST_CASE("the quiet count survives an app restart", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints - 2; ++i) {
        rig.print();
    }
    rig.restart_app();
    rig.print();
    CHECK(sm::retired_signal_names().empty());
    rig.print();
    CHECK(sm::retired_signal_names().size() == 2);
}

TEST_CASE("a dropped name a later file calls comes back", "[signal_macros]") {
    RecordedNamesScope recorded;
    AliasWatcherRig rig;
    for (int i = 0; i < sm::kAliasDropAfterPrints; ++i) {
        rig.print();
    }
    REQUIRE(sm::retired_signal_names().size() == 2);

    rig.loaded.clear();
    rig.print({"// Unknown command:\"HELIX_READY\""});

    CHECK(sm::retired_signal_names() == std::vector<std::string>{"HELIX_ENDED"});
    CHECK(sm::recorded_signal_names() == std::vector<std::string>{"HELIX_READY"});
    CHECK(sm::names_to_alias(scan_of(hidden_pack()), sm::recorded_signal_names(),
                             sm::retired_signal_names()) == all_but("HELIX_ENDED"));
}

TEST_CASE("the macro status sees a retired name through the settings", "[signal_macros]") {
    RecordedNamesScope recorded;
    const auto hw = printer_with(with_all_forwarders(hidden_pack()), kCurrentPackObjects);
    CHECK(helix::MacroManager::evaluate_status(hw) == helix::MacroInstallStatus::INSTALLED);
    for (int i = 0; i < sm::kAliasDropAfterPrints; ++i) {
        sm::note_alias_print({}, {"HELIX_ENDED"}, true);
    }
    CHECK(helix::MacroManager::evaluate_status(hw) == helix::MacroInstallStatus::OUTDATED);
    CHECK(helix::MacroManager::macro_content_for(hw).find("[gcode_macro HELIX_ENDED]") ==
          std::string::npos);
}
