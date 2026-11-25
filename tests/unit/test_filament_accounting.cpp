// SPDX-License-Identifier: GPL-3.0-or-later

// Filament accounting: who switches the Spoolman active spool, the spool map
// the plugin follows, the choice between the plugin and this screen, and the
// per-spool record the print history shows. The ownership, key and switch
// rules come from the case table the plugin's suite reads too.

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "filament_accounting.h"
#include "filament_accounting_controller.h"
#include "helix_plugin_features.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "printer_plugin_status_state.h"
#include "printer_state.h"
#include "tool_state.h"

#include <algorithm>
#include <ctime>
#include <memory>
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

TEST_CASE("filament accounting: on unless plugin_features turns it off", "[plugin_filament]") {
    INFO("no record, or no key in it: on");
    CHECK(filament_accounting_opted_in(json::object()));
    CHECK(filament_accounting_opted_in(json{{"photos", false}}));
    INFO("a key that is not a bool is the default");
    CHECK(filament_accounting_opted_in(json{{kFilamentAccountingFeature, "no"}}));
    INFO("the escape hatch");
    CHECK_FALSE(filament_accounting_opted_in(json{{kFilamentAccountingFeature, false}}));
    CHECK(filament_accounting_opted_in(json{{kFilamentAccountingFeature, true}}));
}

TEST_CASE("filament accounting: no settings switch, a status line in the overview",
          "[plugin_filament]") {
    CHECK(std::none_of(kPluginSwitchFeatures.begin(), kPluginSwitchFeatures.end(),
                       [](const PluginSwitchFeature& f) {
                           return std::string(f.name) == kFilamentAccountingFeature;
                       }));

    PluginFeatureMap features;
    features[kFilamentAccountingFeature] = PluginFeatureEntry{1, true, true, {}, ""};
    const auto lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].home == PluginFeatureHome::None);
    CHECK(lines[0].label != kFilamentAccountingFeature);

    INFO("a printer without Moonraker's [spoolman] has nothing to switch: no line");
    features[kFilamentAccountingFeature] = PluginFeatureEntry{1, false, false, {}, "no_spoolman"};
    CHECK(plugin_feature_lines(features).empty());
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

namespace {

/// The get_item reply that holds @p record under the opt-ins key.
json plugin_features_reply(const json& record) {
    return json{{"result",
                 {{"namespace", kPluginFeaturesNamespace},
                  {"key", kPluginFeaturesKey},
                  {"value", record}}}};
}

/// The controller against a fake Moonraker published as the app's.
struct OptInsFx : LVGLUITestFixture {
    helix::test::FakeMoonrakerClient client;
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = nullptr;
    std::unique_ptr<FilamentAccountingController> controller;

    OptInsFx() {
        previous_api = get_moonraker_api();
        set_moonraker_api(&api);
        controller = std::make_unique<FilamentAccountingController>();
    }
    ~OptInsFx() override {
        controller.reset();
        drain();
        set_moonraker_api(previous_api);
    }

    static void drain() {
        for (int i = 0; i < 5; ++i) {
            ui::UpdateQueue::instance().drain();
        }
    }

    void read(const json& record) {
        client.respond_to("server.database.get_item", plugin_features_reply(record));
        controller->read_opt_ins();
        drain();
    }

    FilamentAccountingSender sender() const {
        return controller->fallback_sender(PluginFeatureState::Absent, ActiveSpoolOwner::Helix,
                                           FallbackSenderRole::Sender);
    }
};

} // namespace

TEST_CASE_METHOD(OptInsFx, "filament accounting: a failed first read leaves the fallback off",
                 "[plugin_filament]") {
    MoonrakerError lost;
    lost.type = MoonrakerErrorType::TIMEOUT;
    client.fail_on("server.database.get_item", lost);
    controller->read_opt_ins();
    drain();
    CHECK(sender() == FilamentAccountingSender::None);
}

TEST_CASE_METHOD(OptInsFx, "filament accounting: the fallback acts only on a record it read",
                 "[plugin_filament]") {
    INFO("nothing read yet: no switching");
    CHECK(sender() == FilamentAccountingSender::None);

    read(json::object());
    INFO("an empty record, read: on");
    CHECK(sender() == FilamentAccountingSender::App);

    read(json{{kFilamentAccountingFeature, false}});
    INFO("the user's false, read at the next job start: off");
    CHECK(sender() == FilamentAccountingSender::None);

    read(json::object());
    REQUIRE(sender() == FilamentAccountingSender::App);
    MoonrakerError lost;
    lost.type = MoonrakerErrorType::TIMEOUT;
    client.fail_on("server.database.get_item", lost);
    controller->read_opt_ins();
    drain();
    INFO("a failed read keeps the last good one");
    CHECK(sender() == FilamentAccountingSender::App);

    MoonrakerError missing;
    missing.code = 404;
    client.fail_on("server.database.get_item", missing);
    controller->read_opt_ins();
    drain();
    INFO("no record at all: on");
    CHECK(sender() == FilamentAccountingSender::App);
}

TEST_CASE_METHOD(OptInsFx,
                 "filament accounting: each plugin probe reads the opt-ins as they are now",
                 "[plugin_filament]") {
    auto& status = get_printer_state().plugin_status_state();
    const auto probe = [&status] {
        lv_subject_set_int(status.get_helix_plugin_probe_seq_subject(),
                           lv_subject_get_int(status.get_helix_plugin_probe_seq_subject()) + 1);
    };

    client.respond_to("server.database.get_item",
                      plugin_features_reply(json{{kFilamentAccountingFeature, true}}));
    probe();
    drain();
    INFO("a record switched on, read at the probe");
    CHECK(sender() == FilamentAccountingSender::App);

    client.respond_to("server.database.get_item",
                      plugin_features_reply(json{{kFilamentAccountingFeature, false}}));
    probe();
    drain();
    INFO("the user's false, set since the last probe");
    CHECK(sender() == FilamentAccountingSender::None);
}

TEST_CASE_METHOD(OptInsFx,
                 "filament accounting: a job start reads the opt-ins and a job's end does not",
                 "[plugin_filament]") {
    auto& print_active = get_printer_state().print_state();
    const auto job = [&](bool running) {
        lv_subject_set_int(print_active.get_print_active_subject(), running ? 1 : 0);
        drain();
    };
    const auto reads = [&] { return client.rpc_count("server.database.get_item"); };

    // Attaching hands the controller the idle printer's state.
    drain();
    INFO("no job running: nothing is read");
    CHECK(reads() == 0);

    client.respond_to("server.database.get_item",
                      plugin_features_reply(json{{kFilamentAccountingFeature, true}}));
    job(true);
    INFO("a job starts: the record is read");
    CHECK(sender() == FilamentAccountingSender::App);

    client.respond_to("server.database.get_item",
                      plugin_features_reply(json{{kFilamentAccountingFeature, false}}));
    job(false);
    INFO("the job ends: nothing is read, and the last read stands");
    CHECK(sender() == FilamentAccountingSender::App);
    CHECK(reads() == 1);
}

TEST_CASE_METHOD(OptInsFx, "filament accounting: a disconnect forgets the opt-ins read",
                 "[plugin_filament]") {
    auto& ps = get_printer_state();
    const auto link = [&](ConnectionState state) {
        ps.set_printer_connection_state(static_cast<int>(state), "");
        drain();
    };
    link(ConnectionState::CONNECTED);
    read(json{{kFilamentAccountingFeature, true}});
    REQUIRE(sender() == FilamentAccountingSender::App);

    link(ConnectionState::DISCONNECTED);
    INFO("the next connection's print reads the record again before the fallback acts");
    CHECK(sender() == FilamentAccountingSender::None);
}

namespace {

constexpr const char* kToolExtruders[] = {"extruder", "extruder1"};

/// A printer with two tools, each on its own extruder, whose lanes hold the
/// tools' spools: lane 0 maps to tool 0 with spool 11, lane 1 to tool 1 with
/// spool 12. Spoolman's active spool is 11 and tool 0 is mounted. The
/// controller is armed by a probe, which reads an opt-in record, and a job is
/// running. @p remote makes this a screen off the printer's host, where a
/// HelixScreen on the host announces itself.
struct ToolChangeFx : OptInsFx {
    helix::AmsBackendMock* backend = nullptr;

    explicit ToolChangeFx(bool remote) {
        auto& ps = get_printer_state();
        ToolState& tools = ToolState::instance();
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"extruder", "extruder1"}));
        tools.init_tools(hw);
        mount(0);

        auto& ams = AmsState::instance();
        ams.clear_backends();
        auto m = std::make_unique<helix::AmsBackendMock>(4);
        backend = m.get();
        REQUIRE(ams.add_backend(std::move(m)) == 0);
        for (int slot : {0, 1}) {
            helix::SlotInfo lane = backend->get_slot_info(slot);
            lane.material = "PLA";
            lane.mapped_tool = slot;
            lane.spoolman_id = 11 + slot;
            helix::test::apply_edit(*backend, slot, lane);
        }
        // The tools print from their own extruders: the lanes only hold their spools.
        REQUIRE(tools.tools().size() == 2);
        REQUIRE(tools.tools()[1].backend_index == -1);
        const auto lane = ams.lane_feeding_tool(1);
        REQUIRE(lane.has_value());
        REQUIRE(lane->slot == 1);
        REQUIRE(lane->info.spoolman_id == 12);

        ps.capabilities_state().set_spoolman_available(true);
        ps.set_moonraker_is_remote(remote);
        // The opt-ins are on, and a HelixScreen on the printer's host was seen just now.
        client.respond_to("server.database.get_item", [](const json& params, const auto& ok,
                                                         const auto&) {
            const std::string key = json_util::safe_string(params, "key");
            if (key == kPluginFeaturesKey) {
                ok(plugin_features_reply(json{{kFilamentAccountingFeature, true}}));
            } else if (key == kLocalScreenPresenceKey) {
                ok(json{
                    {"result",
                     {{"namespace", kPluginFeaturesNamespace},
                      {"key", kLocalScreenPresenceKey},
                      {"value", json{{"seen_at", static_cast<int64_t>(std::time(nullptr))}}}}}});
            }
        });
        client.respond_to("server.spoolman.status",
                          json{{"result", {{"spoolman_connected", true}, {"spool_id", 11}}}});

        auto& status = ps.plugin_status_state();
        lv_subject_set_int(status.get_helix_plugin_probe_seq_subject(),
                           lv_subject_get_int(status.get_helix_plugin_probe_seq_subject()) + 1);
        lv_subject_set_int(ps.print_state().get_print_active_subject(), 1);
        drain();
        // What lets a tool change through to the switch: no plugin feature to
        // do it, nobody else owning the switch, the opt-in read, this screen sending.
        REQUIRE(ps.plugin_status_state().plugin_feature(kFilamentAccountingFeature,
                                                        kFilamentAccountingMinVersion) ==
                PluginFeatureState::Absent);
        REQUIRE(
            active_spool_owner_now(ps.is_spoolman_available(),
                                   FilamentAccountingController::backends_manage_active_spool()) ==
            ActiveSpoolOwner::Helix);
        REQUIRE(sender() == FilamentAccountingSender::App);
    }

    ~ToolChangeFx() override {
        controller.reset();
        AmsState::instance().clear_backends();
        drain();
        // The tools this case mounted are process-wide: the next case's fixture brings its own up.
        ToolState::instance().deinit_subjects();
        auto& ps = get_printer_state();
        ps.capabilities_state().set_spoolman_available(false);
        ps.clear_moonraker_locality();
    }

    void mount(int tool) {
        ToolState::instance().update_from_status(
            {{"toolhead", {{"extruder", kToolExtruders[tool]}}}});
        REQUIRE(ToolState::instance().active_tool_index() == tool);
    }

    /// Mount tool 1, and let the Spoolman replies and the deferred switch land.
    void change_to_tool_1() {
        mount(1);
        drain();
    }

    /// How often this screen asked whether a HelixScreen is on the printer's host.
    size_t presence_reads() const {
        return static_cast<size_t>(std::count_if(
            client.rpc_calls.begin(), client.rpc_calls.end(),
            [](const helix::test::FakeMoonrakerClient::RpcCall& call) {
                return call.method == "server.database.get_item" &&
                       json_util::safe_string(call.params, "key") == kLocalScreenPresenceKey;
            }));
    }
};

struct SenderFx : ToolChangeFx {
    SenderFx() : ToolChangeFx(/*remote=*/false) {}
};

struct StandbyFx : ToolChangeFx {
    StandbyFx() : ToolChangeFx(/*remote=*/true) {}
};

} // namespace

TEST_CASE_METHOD(SenderFx,
                 "filament accounting: the fallback's sender switches to the new tool's spool",
                 "[plugin_filament]") {
    change_to_tool_1();

    const auto* sent = client.last_rpc("server.spoolman.post_spool_id");
    REQUIRE(sent != nullptr);
    CHECK(json_util::safe_int(sent->params, "spool_id") == 12);
}

TEST_CASE_METHOD(
    StandbyFx,
    "filament accounting: a standby screen leaves the spool to the screen on the printer",
    "[plugin_filament]") {
    const size_t asked_before = presence_reads();
    change_to_tool_1();
    // The tool change got as far as asking who else is on the printer.
    REQUIRE(presence_reads() > asked_before);

    CHECK(client.rpc_count("server.spoolman.post_spool_id") == 0);
}
