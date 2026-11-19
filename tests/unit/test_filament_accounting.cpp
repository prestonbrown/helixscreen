// SPDX-License-Identifier: GPL-3.0-or-later

// Filament accounting: who switches the Spoolman active spool, the spool map
// the plugin follows, the choice between the plugin and this screen, and the
// per-spool record the print history shows. The ownership, key and switch
// rules come from the case table the plugin's suite reads too.

#include "../test_helpers/filament_accounting_cases.h"
#include "../ui_test_utils.h"
#include "filament_accounting.h"
#include "filament_accounting_controller.h"
#include "helix_plugin_features.h"
#include "printer_discovery.h"
#include "printer_plugin_status_state.h"
#include "tool_state.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json cases() {
    json parsed = helix::test::filament_accounting_cases();
    REQUIRE_FALSE(parsed.is_discarded());
    return parsed;
}

PluginFeatureMap accounting_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features[kFilamentAccountingFeature] =
        PluginFeatureEntry{version, available, enabled, {}, available ? "" : "why"};
    return features;
}

FeedEntry feed(int tool, int lane, int spool, float gpm = 0.0f) {
    FeedEntry f;
    f.tool = tool;
    f.lane = lane;
    f.spool_id = spool;
    f.grams_per_mm = gpm;
    return f;
}

} // namespace

TEST_CASE("filament accounting: ownership follows the shared case table", "[plugin_filament]") {
    const json table = cases()["ownership"];
    REQUIRE(table.size() >= 10);
    for (const json& row : table) {
        INFO(row["name"].get<std::string>());
        const ActiveSpoolOwner owner =
            active_spool_owner(row["spoolman"].get<bool>(), row["manages_active_spool"].get<bool>(),
                               row["printer_switches_active_spool"].get<bool>(),
                               row["macros_set_active_spool"].get<bool>());
        CHECK(std::string(active_spool_owner_name(owner)) == row["owner"].get<std::string>());
    }
}

TEST_CASE("filament accounting: status values become keys as the case table says",
          "[plugin_filament]") {
    const json table = cases()["keys"];
    REQUIRE_FALSE(table.empty());
    for (const json& row : table) {
        INFO(row["value"].dump());
        const std::string expected = row["key"].is_null() ? "" : row["key"].get<std::string>();
        CHECK(feed_signal_key(row["value"]) == expected);
    }
}

TEST_CASE("filament accounting: the switch rule follows the shared case table",
          "[plugin_filament]") {
    const json table = cases()["switch"];
    REQUIRE_FALSE(table.empty());
    for (const json& row : table) {
        INFO(row["name"].get<std::string>());
        FeedEntry f;
        const FeedEntry* fp = nullptr;
        if (!row["feed"].is_null()) {
            const json& id = row["feed"]["spool_id"];
            f.spool_id = id.is_null() ? 0 : id.get<int>();
            fp = &f;
        }
        const int active = row["active"].is_null() ? 0 : row["active"].get<int>();
        const std::vector<int> mapped = row["mapped"].get<std::vector<int>>();
        const SpoolSwitch got = decide_spool_switch(fp, active, mapped);
        const json& expect = row["expect"];
        if (expect.is_null()) {
            CHECK(got.action == SpoolSwitch::Action::None);
        } else if (expect["action"] == "set") {
            CHECK(got.action == SpoolSwitch::Action::Set);
            CHECK(got.spool_id == expect["spool_id"].get<int>());
        } else {
            CHECK(got.action == SpoolSwitch::Action::Clear);
        }
    }
}

TEST_CASE("filament accounting: exactly one sender per plugin state", "[plugin_filament]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        bool opted_in;
        FilamentAccountingSender expected;
    };
    const Row rows[] = {
        {"no plugin, opted in", false, accounting_map(1, true, true), true,
         FilamentAccountingSender::App},
        {"no plugin, not opted in", false, {}, false, FilamentAccountingSender::None},
        {"old plugin without a features map", true, {}, true, FilamentAccountingSender::App},
        {"contract below the minimum", true, accounting_map(0, true, true), true,
         FilamentAccountingSender::App},
        {"unavailable", true, accounting_map(1, false, false), true, FilamentAccountingSender::App},
        {"switched off in the plugin", true, accounting_map(1, true, false), true,
         FilamentAccountingSender::None},
        {"ready", true, accounting_map(1, true, true), true, FilamentAccountingSender::Plugin},
        {"ready, stale opt-in read", true, accounting_map(1, true, true), false,
         FilamentAccountingSender::Plugin},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        const PluginFeatureState state =
            plugin_feature_state(row.plugin_available, row.features, kFilamentAccountingFeature,
                                 kFilamentAccountingMinVersion);
        CHECK(filament_accounting_sender(state, row.opted_in, ActiveSpoolOwner::Helix,
                                         FallbackSenderRole::Sender) == row.expected);
        INFO("another HelixScreen on the printer switches it");
        CHECK(filament_accounting_sender(state, row.opted_in, ActiveSpoolOwner::Helix,
                                         FallbackSenderRole::Standby) ==
              (row.expected == FilamentAccountingSender::App ? FilamentAccountingSender::None
                                                             : row.expected));
        INFO("a printer that switches the spool itself gets no switch from anyone");
        for (ActiveSpoolOwner owner :
             {ActiveSpoolOwner::NoSpoolman, ActiveSpoolOwner::Backend, ActiveSpoolOwner::Macros}) {
            CHECK(filament_accounting_sender(state, row.opted_in, owner,
                                             FallbackSenderRole::Sender) ==
                  FilamentAccountingSender::None);
        }
    }
}

TEST_CASE("filament accounting: the extruder signal leaves out shared extruders",
          "[plugin_filament]") {
    const std::vector<FeedEntry> feeds = {feed(0, -1, 11), feed(1, -1, 12), feed(2, 4, 13),
                                          feed(3, 5, 14)};
    auto sig = extruder_feed_signal({"extruder", "extruder1", "extruder2", "extruder2"}, feeds);
    REQUIRE(sig);
    CHECK(sig->object == "toolhead");
    CHECK(sig->path == std::vector<std::string>{"extruder"});
    REQUIRE(sig->values.size() == 2);
    CHECK(sig->values[0].first == "extruder");
    CHECK(sig->values[0].second.spool_id == 11);
    CHECK(sig->values[1].first == "extruder1");

    INFO("every tool on one extruder: nothing to tell them apart by");
    CHECK_FALSE(extruder_feed_signal({"extruder", "extruder"}, {feed(0, 0, 1), feed(1, 1, 2)}));
    CHECK_FALSE(extruder_feed_signal({"", ""}, {feed(0, 0, 1), feed(1, 1, 2)}));
}

TEST_CASE("filament accounting: a printer with two tools publishes each one's extruder",
          "[plugin_filament]") {
    lv_init_safe();
    ToolState& tools = ToolState::instance();
    tools.deinit_subjects();
    tools.init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1"}));
    tools.init_tools(hw);

    const auto signals = FilamentAccountingController::signals_now();
    tools.deinit_subjects();
    const auto toolhead = std::find_if(signals.begin(), signals.end(),
                                       [](const FeedSignal& s) { return s.object == "toolhead"; });
    REQUIRE(toolhead != signals.end());
    REQUIRE(toolhead->values.size() == 2);
    CHECK(toolhead->values[1].first == "extruder1");
    CHECK(toolhead->values[1].second.tool == 1);
}

TEST_CASE("filament accounting: the spool map record", "[plugin_filament]") {
    FeedSignal sig{"save_variables",
                   {"variables", "active_tool"},
                   {{"0", feed(0, 0, 21, 0.003f)}, {"1", feed(1, 1, 0)}}};
    const json record = filament_spools_record(false, true, true, {sig});
    CHECK(record["version"] == kFilamentSpoolsVersion);
    CHECK(record["manages_active_spool"] == false);
    CHECK(record["printer_switches_active_spool"] == true);
    CHECK(record["macros_set_active_spool"] == true);
    REQUIRE(record["signals"].size() == 1);
    const json& s = record["signals"][0];
    CHECK(s["object"] == "save_variables");
    CHECK(s["path"] == json::array({"variables", "active_tool"}));
    CHECK(s["values"]["0"]["spool_id"] == 21);
    CHECK(s["values"]["0"]["grams_per_mm"].get<double>() == Catch::Approx(0.003));
    INFO("an unlinked feed names no spool and no weight");
    CHECK(s["values"]["1"]["spool_id"].is_null());
    CHECK_FALSE(s["values"]["1"].contains("grams_per_mm"));

    INFO("only the fallback's sender writes it, and only once it says something");
    CHECK(writes_filament_spools_record(FallbackSenderRole::Sender, {sig}));
    CHECK_FALSE(writes_filament_spools_record(FallbackSenderRole::Standby, {sig}));
    CHECK_FALSE(writes_filament_spools_record(FallbackSenderRole::Sender, {}));
}

TEST_CASE("filament accounting: SpoolLink firmware switches the spool itself",
          "[plugin_filament]") {
    PrinterDiscovery stock;
    stock.parse_objects(json::array({"toolhead", "extruder", "extruder1", "print_stats"}));
    CHECK_FALSE(printer_switches_active_spool(stock));

    PrinterDiscovery listed;
    listed.parse_objects(json::array({"toolhead", "extruder", "spoollink", "print_stats"}));
    CHECK(printer_switches_active_spool(listed));

    INFO("a [spoollink] section the object list does not show still counts");
    PrinterDiscovery configured;
    configured.parse_objects(json::array({"toolhead", "extruder"}));
    configured.parse_config_keys(json{{"spoollink", json::object()}});
    CHECK(printer_switches_active_spool(configured));

    INFO("a later object list without it forgets it");
    listed.parse_objects(json::array({"toolhead", "extruder"}));
    CHECK_FALSE(printer_switches_active_spool(listed));
}

TEST_CASE("filament accounting: tool-select macros", "[plugin_filament]") {
    CHECK(is_tool_select_macro("T0"));
    CHECK(is_tool_select_macro("T12"));
    CHECK_FALSE(is_tool_select_macro("T"));
    CHECK_FALSE(is_tool_select_macro("TOOL0"));
    CHECK_FALSE(is_tool_select_macro("T1A"));
    CHECK_FALSE(is_tool_select_macro("CHANGE_TOOL"));

    const std::vector<std::string> macros = {"PRINT_START", "T0", "T1", "TOOL_SETUP"};
    CHECK(
        tool_macros_set_active_spool(macros, [](const std::string& name) { return name == "T1"; }));
    INFO("a non-tool macro that sets the spool does not count");
    CHECK_FALSE(tool_macros_set_active_spool(
        macros, [](const std::string& name) { return name == "PRINT_START"; }));
}

TEST_CASE("filament accounting: a job's per-spool record", "[plugin_filament]") {
    const json reply = {
        {"job_id", "00002A"},
        {"enabled", true},
        {"record",
         {{"spools",
           json::array(
               {{{"tool", 0}, {"lane", 2}, {"spool_id", 7}, {"mm", 4180.2}, {"grams", 12.47}},
                {{"tool", nullptr},
                 {"lane", nullptr},
                 {"spool_id", nullptr},
                 {"mm", 950.0},
                 {"grams", nullptr}},
                "not a spool"})}}}};
    const auto usage = parse_filament_usage(reply);
    REQUIRE(usage.size() == 2);
    CHECK(usage[0].tool == 0);
    CHECK(usage[0].lane == 2);
    CHECK(usage[0].spool_id == 7);
    CHECK(usage[0].mm == Catch::Approx(4180.2));
    REQUIRE(usage[0].grams);
    CHECK(*usage[0].grams == Catch::Approx(12.47));
    CHECK(usage[1].tool == -1);
    CHECK(usage[1].spool_id == 0);
    CHECK_FALSE(usage[1].grams);

    INFO("a job the plugin did not count");
    CHECK(parse_filament_usage(json{{"record", nullptr}}).empty());
    CHECK(parse_filament_usage(json::object()).empty());
    CHECK(parse_filament_usage(json{{"record", {{"spools", "x"}}}}).empty());

    INFO("asked only of a plugin that reports the feature");
    CHECK(filament_usage_available(PluginFeatureState::Ready));
    CHECK(filament_usage_available(PluginFeatureState::Disabled));
    CHECK_FALSE(filament_usage_available(PluginFeatureState::Absent));
}

TEST_CASE("filament accounting: history lines name who fed what", "[plugin_filament]") {
    std::vector<SpoolUsage> usage(3);
    usage[0].tool = 0;
    usage[0].spool_id = 7;
    usage[0].mm = 4180.0;
    usage[0].grams = 12.47;
    usage[1].tool = 1;
    usage[1].spool_id = 9;
    usage[1].mm = 1530.0;
    usage[2].lane = 3;
    usage[2].mm = 200.0;
    const auto names = [](int id) {
        return id == 7 ? std::string("Polymaker PLA") : std::string();
    };
    const std::string text = describe_spool_usage(usage, names, ui::LaneNoun::Slot);
    CHECK(text ==
          "T0 · Polymaker PLA · 12 g\nT1 · Spool #9 · 1.5m\nSlot 4 · No spool linked · 200mm");
    INFO("a lane is named in the printer's own word for one");
    CHECK(describe_spool_usage({usage[2]}, names, ui::LaneNoun::Gate) ==
          "Gate 4 · No spool linked · 200mm");
    CHECK(describe_spool_usage({}, nullptr, ui::LaneNoun::Slot).empty());
}

TEST_CASE("filament accounting: its switch lives on the Spoolman page", "[plugin_filament]") {
    const auto row = std::find_if(kPluginSwitchFeatures.begin(), kPluginSwitchFeatures.end(),
                                  [](const PluginSwitchFeature& f) {
                                      return std::string(f.name) == kFilamentAccountingFeature;
                                  });
    REQUIRE(row != kPluginSwitchFeatures.end());
    CHECK(row->has_fallback);
    CHECK(row->acts);
    CHECK(std::string(row->on_subject) == "plugin_feature_on_filament_accounting");

    PluginFeatureMap features;
    features[kFilamentAccountingFeature] = PluginFeatureEntry{1, true, false, {}, ""};
    const auto lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].home == PluginFeatureHome::Spoolman);
    CHECK(lines[0].label != kFilamentAccountingFeature);

    INFO("acts on spools: off until opted into");
    CHECK_FALSE(plugin_feature_opted_in(json::object(), kFilamentAccountingFeature, true));
}

TEST_CASE("filament accounting: who sets the spool for an explicit link of the loaded lane",
          "[plugin_filament]") {
    CHECK(app_sets_spool_on_edit(ActiveSpoolOwner::Helix, false));
    INFO("a backend that switches only on a load never sees the edit");
    CHECK(app_sets_spool_on_edit(ActiveSpoolOwner::Backend, false));
    INFO("one that takes the link itself (AFC) is left to it");
    CHECK_FALSE(app_sets_spool_on_edit(ActiveSpoolOwner::Backend, true));
    INFO("T<n> macros run only at the next tool change");
    CHECK(app_sets_spool_on_edit(ActiveSpoolOwner::Macros, false));
    INFO("firmware keeping its own assignment (SpoolLink) is left to it");
    CHECK_FALSE(app_sets_spool_on_edit(ActiveSpoolOwner::Printer, false));
    CHECK_FALSE(app_sets_spool_on_edit(ActiveSpoolOwner::NoSpoolman, false));
}
