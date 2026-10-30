// SPDX-License-Identifier: GPL-3.0-or-later

// Standing print options: the choice between the plugin and the screen at a
// job start, telling the screen's own starts from foreign ones, and the
// rendered record coming from the same backend function as the screen's start.

#include "../lvgl_test_fixture.h"
#include "ams_backend.h"
#include "ams_backend_mock.h"
#include "config.h"
#include "helix_plugin_features.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "plr_plugin_rpc.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "snapmaker_print_preferences.h"
#include "standing_print_options.h"

#if HELIX_HAS_SNAPMAKER
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#endif

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

PluginFeatureEntry entry(int version, bool available, bool enabled, std::string reason = "") {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    e.reason = std::move(reason);
    return e;
}

PluginFeatureMap hooks_map(int version, bool available, bool enabled) {
    PluginFeatureMap features;
    features[kPrintHooksFeature] = entry(version, available, enabled);
    return features;
}

/// A backend whose standing gcode is whatever the test says.
class FixedStandingBackend : public AmsBackendMock {
  public:
    explicit FixedStandingBackend(std::string gcode) : gcode_(std::move(gcode)) {}
    std::string standing_print_gcode() const override {
        return gcode_;
    }

  private:
    std::string gcode_;
};

} // namespace

TEST_CASE("standing options go to exactly one sender per plugin state", "[plugin_print_hooks]") {
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        bool opted_in;
        StandingOptionsSender expected;
    };
    const Row rows[] = {
        {"no plugin, opted in", false, hooks_map(1, true, true), true, StandingOptionsSender::App},
        {"no plugin, not opted in", false, {}, false, StandingOptionsSender::None},
        {"old plugin without a features map", true, {}, true, StandingOptionsSender::App},
        {"contract below the minimum", true, hooks_map(0, true, true), true,
         StandingOptionsSender::App},
        {"unavailable", true, hooks_map(1, false, false), true, StandingOptionsSender::App},
        {"switched off in the plugin", true, hooks_map(1, true, false), true,
         StandingOptionsSender::None},
        {"ready", true, hooks_map(1, true, true), true, StandingOptionsSender::Plugin},
        {"ready, stale opt-in read", true, hooks_map(1, true, true), false,
         StandingOptionsSender::Plugin},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        const PluginFeatureState state = plugin_feature_state(
            row.plugin_available, row.features, kPrintHooksFeature, kPrintHooksMinVersion);
        CHECK(standing_options_sender(state, row.opted_in, /*screen_started=*/false,
                                      /*has_gcode=*/true,
                                      FallbackSenderRole::Sender) == row.expected);
        INFO("another HelixScreen on the printer sends the fallback");
        CHECK(standing_options_sender(state, row.opted_in, false, true,
                                      FallbackSenderRole::Standby) ==
              (row.expected == StandingOptionsSender::App ? StandingOptionsSender::None
                                                          : row.expected));
        INFO("the screen's own start already sent its options");
        CHECK(standing_options_sender(state, row.opted_in, /*screen_started=*/true, true,
                                      FallbackSenderRole::Sender) == StandingOptionsSender::None);
        INFO("nothing rendered");
        CHECK(standing_options_sender(state, row.opted_in, false, /*has_gcode=*/false,
                                      FallbackSenderRole::Sender) == StandingOptionsSender::None);
    }
}

TEST_CASE("only a ready print_hooks feature hears of the screen's starts", "[plugin_print_hooks]") {
    CHECK(announce_screen_start(PluginFeatureState::Ready));
    CHECK_FALSE(announce_screen_start(PluginFeatureState::Disabled));
    CHECK_FALSE(announce_screen_start(PluginFeatureState::Absent));
}

TEST_CASE("the ledger tells the screen's starts from foreign ones", "[plugin_print_hooks]") {
    ScreenStartLedger ledger;
    const auto t0 = ScreenStartLedger::Clock::time_point{} + 1000s;

    SECTION("a start the screen noted is its own, once") {
        ledger.note("benchy.gcode", t0);
        CHECK_FALSE(ledger.claim("cube.gcode", t0 + 1s));
        CHECK(ledger.claim("benchy.gcode", t0 + 1s));
        CHECK_FALSE(ledger.claim("benchy.gcode", t0 + 2s));
    }

    SECTION("a withdrawn note claims nothing; another file's note stays") {
        ledger.note("benchy.gcode", t0);
        ledger.note("cube.gcode", t0);
        CHECK(ledger.withdraw("benchy.gcode"));
        CHECK_FALSE(ledger.withdraw("benchy.gcode"));
        CHECK_FALSE(ledger.claim("benchy.gcode", t0 + 1s));
        CHECK(ledger.claim("cube.gcode", t0 + 1s));
    }

    SECTION("nothing noted is a foreign start") {
        CHECK_FALSE(ledger.claim("benchy.gcode", t0));
    }

    SECTION("a noted start lapses after its window") {
        ledger.note("benchy.gcode", t0);
        CHECK_FALSE(ledger.claim("benchy.gcode", t0 + ScreenStartLedger::kWindow + 1s));
    }

    SECTION("a start whose job name is unknown claims the next job") {
        ledger.note_any(t0);
        CHECK(ledger.claim(".helix_print/abcd_benchy.gcode", t0 + 1s));
        CHECK_FALSE(ledger.claim("cube.gcode", t0 + 2s));
    }

    SECTION("a slow start waits the window it asked for") {
        ledger.note_any(t0, 30min);
        CHECK(ledger.claim("benchy.gcode", t0 + 20min));
    }
}

TEST_CASE("the stored opt-in follows the plugin's rule", "[plugin_print_hooks]") {
    CHECK(plugin_feature_opted_in(json{{"print_hooks", true}}, "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json{{"print_hooks", false}}, "print_hooks", false));
    INFO("absent or not a bool: the default, off for a feature that acts");
    CHECK_FALSE(plugin_feature_opted_in(json::object(), "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json{{"print_hooks", 1}}, "print_hooks", true));
    CHECK_FALSE(plugin_feature_opted_in(json(), "print_hooks", true));
    CHECK(plugin_feature_opted_in(json::object(), "journal", false));
}

TEST_CASE_METHOD(LVGLTestFixture, "the record joins every backend's standing gcode",
                 "[plugin_print_hooks]") {
    const FixedStandingBackend first{"M117 one"};
    const FixedStandingBackend silent{""};
    const FixedStandingBackend second{"M117 two"};

    CHECK(render_standing_gcode({&first, nullptr, &silent, &second}) == "M117 one\nM117 two");
    CHECK(render_standing_gcode({&silent}).empty());
    CHECK(render_standing_gcode({}).empty());
    CHECK(standing_options_record("M117 one") == json{{"version", 1}, {"gcode", "M117 one"}});
}

TEST_CASE("a forced preference write carries FORCE=1 last", "[plugin_print_hooks]") {
    snapmaker::PrintPreferences changes;
    changes.end_unload_filament = {true, false};
    changes.force = true;
    CHECK(snapmaker::write_print_preferences_gcode(changes) ==
          "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0] FORCE=1");

    INFO("nothing to write stays nothing");
    snapmaker::PrintPreferences empty;
    empty.force = true;
    CHECK(snapmaker::write_print_preferences_gcode(empty).empty());
}

namespace {

/// The mock, plus every JSON-RPC method sent through it, in order.
struct MethodRecordingClient : public MoonrakerClientMock {
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;
    std::vector<std::pair<std::string, json>> sent;

    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb,
                 uint32_t timeout_ms = 0, bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        sent.emplace_back(method, params);
        if (method == "printer.print.start" && fail_print_start) {
            if (error_cb) {
                error_cb(*fail_print_start);
            }
            return 0;
        }
        if (method == "server.helix.print_edited" && fail_print_edited) {
            if (error_cb) {
                error_cb(*fail_print_edited);
            }
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    /// When set, printer.print.start answers with this error.
    std::optional<MoonrakerError> fail_print_start;
    /// When set, server.helix.print_edited answers with this error.
    std::optional<MoonrakerError> fail_print_edited;
};

struct JobStartFx : LVGLTestFixture {
    MethodRecordingClient client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;

    JobStartFx() {
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
    }
    ~JobStartFx() override {
        client.stop_temperature_simulation();
        client.disconnect();
    }

    std::vector<std::string> methods() const {
        std::vector<std::string> out;
        for (const auto& s : client.sent)
            out.push_back(s.first);
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(JobStartFx, "a start from this screen is noted before it is sent",
                 "[plugin_print_hooks]") {
    MoonrakerAPI api{client, ps};

    SECTION("the job history adds for it is the screen's own") {
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim("benchy.gcode"));
        CHECK_FALSE(screen_start_ledger().claim("cube.gcode"));
    }

    SECTION("a ready print_hooks feature hears of it first") {
        screen_start_ledger().set_announce(true);
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        const auto sent = methods();
        const auto announce = std::find(sent.begin(), sent.end(), kScreenStartRpc);
        const auto start = std::find(sent.begin(), sent.end(), "printer.print.start");
        REQUIRE(announce != sent.end());
        REQUIRE(start != sent.end());
        CHECK(announce < start);
        CHECK(client.sent[static_cast<size_t>(announce - sent.begin())].second ==
              json{{"filename", "benchy.gcode"}});
    }

    SECTION("a server-edited start claims the next job, whatever its link name") {
        api.job().start_edited_print(
            "benchy.gcode", ".helix_temp/benchy.gcode", nlohmann::json::array(), {}, 0,
            [](const ModifiedPrintResult&) {}, [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim(".helix_print/abcd_benchy.gcode"));
    }

    SECTION("a start through the plugin's link claims the next job, whatever its name") {
        api.job().start_modified_print(
            "benchy.gcode", ".helix_temp/benchy.gcode", {}, [](const ModifiedPrintResult&) {},
            [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim(".helix_print/abcd_benchy.gcode"));
    }

    SECTION("a PLR resume the plugin starts after heating is the screen's own") {
        plr_rpc::commit(api, "plan-1", {}, 1, [] {}, [](const MoonrakerError&) {});
        // The plugin heats and purges first: the note waits as long as the commit may.
        CHECK(screen_start_ledger().claim(
            "benchy.gcode", ScreenStartLedger::Clock::now() +
                                std::chrono::milliseconds(plr_rpc::kCommitTimeoutMs) - 1min));
    }

    SECTION("an edited start's note outlasts the plugin's rewrite of a large file") {
        constexpr uint64_t kSize = 200ull * 1024 * 1024;
        api.job().start_edited_print(
            "benchy.gcode", ".helix_temp/benchy.gcode", nlohmann::json::array(), {}, kSize,
            [](const ModifiedPrintResult&) {}, [](const MoonrakerError&) {});
        // The plugin's worker alone may take 600 s + 60 s/MiB for this file.
        CHECK(screen_start_ledger().claim("benchy.gcode",
                                          ScreenStartLedger::Clock::now() + std::chrono::hours(3)));
    }

    SECTION("a refused edited start takes its note back; a lost answer keeps it") {
        MoonrakerError refused = MoonrakerError::from_json_rpc(
            {{"code", 400}, {"message", "Original file changed since the screen read it"}},
            "server.helix.print_edited");
        client.fail_print_edited = refused;
        api.job().start_edited_print(
            "benchy.gcode", ".helix_temp/benchy.gcode", nlohmann::json::array(), {}, 0,
            [](const ModifiedPrintResult&) {}, [](const MoonrakerError&) {});
        CHECK_FALSE(screen_start_ledger().claim("benchy.gcode"));

        MoonrakerError lost;
        lost.type = MoonrakerErrorType::TIMEOUT;
        client.fail_print_edited = lost;
        api.job().start_edited_print(
            "benchy.gcode", ".helix_temp/benchy.gcode", nlohmann::json::array(), {}, 0,
            [](const ModifiedPrintResult&) {}, [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim("benchy.gcode"));
    }

    SECTION("a refused start takes back its note and its announcement") {
        screen_start_ledger().set_announce(true);
        MoonrakerError refused;
        refused.type = MoonrakerErrorType::JSON_RPC_ERROR;
        refused.message = "File not found";
        client.fail_print_start = refused;
        bool failed = false;
        api.job().start_print(
            "benchy.gcode", [] {}, [&failed](const MoonrakerError&) { failed = true; });
        CHECK(failed);
        CHECK_FALSE(screen_start_ledger().claim("benchy.gcode"));
        const json* last = nullptr;
        for (const auto& [method, params] : client.sent) {
            if (method == kScreenStartRpc) {
                last = &params;
            }
        }
        REQUIRE(last != nullptr);
        CHECK(*last == json{{"filename", "benchy.gcode"}, {"withdraw", true}});
    }

    SECTION("a lost answer keeps them: the print may be running") {
        screen_start_ledger().set_announce(true);
        MoonrakerError lost;
        lost.type = MoonrakerErrorType::TIMEOUT;
        client.fail_print_start = lost;
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        CHECK(screen_start_ledger().claim("benchy.gcode"));
        int announcements = 0;
        for (const auto& [method, params] : client.sent) {
            if (method == kScreenStartRpc) {
                ++announcements;
                CHECK_FALSE(params.contains("withdraw"));
            }
        }
        CHECK(announcements == 1);
    }

    SECTION("a refused start without the plugin sends no withdraw") {
        MoonrakerError refused;
        refused.type = MoonrakerErrorType::JSON_RPC_ERROR;
        client.fail_print_start = refused;
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        CHECK_FALSE(screen_start_ledger().claim("benchy.gcode"));
        const auto sent = methods();
        CHECK(std::find(sent.begin(), sent.end(), kScreenStartRpc) == sent.end());
    }

    SECTION("otherwise the plugin is not told") {
        api.job().start_print("benchy.gcode", [] {}, [](const MoonrakerError&) {});
        const auto sent = methods();
        CHECK(std::find(sent.begin(), sent.end(), kScreenStartRpc) == sent.end());
    }
}

TEST_CASE("a plugin switch row's mode over every plugin state", "[plugin_feature]") {
    using M = PluginSwitchMode;
    struct Row {
        const char* what;
        bool plugin_available;
        PluginFeatureMap features;
        // Expected mode for: plugin-only, fallback on the printer's host,
        // fallback on a remote screen
        M plugin_only;
        M fallback_local;
        M fallback_remote;
    };
    const Row rows[] = {
        {"no plugin", false, {}, M::NeedsPlugin, M::Switch, M::FallbackInstallHint},
        {"no plugin, a stale map", false, hooks_map(1, true, true), M::NeedsPlugin, M::Switch,
         M::FallbackInstallHint},
        {"old plugin without a features map",
         true,
         {},
         M::NeedsUpdate,
         M::Switch,
         M::FallbackUpdateHint},
        {"contract below the minimum", true, hooks_map(0, true, true), M::NeedsUpdate, M::Switch,
         M::FallbackUpdateHint},
        {"below the minimum and unavailable", true, hooks_map(0, false, false), M::NeedsUpdate,
         M::Switch, M::FallbackUpdateHint},
        // The fallback still acts, so its switch must stay where the user can see it.
        {"unavailable on this printer", true, hooks_map(1, false, false), M::Hidden, M::Switch,
         M::FallbackUpdateHint},
        {"switched off", true, hooks_map(1, true, false), M::Switch, M::Switch, M::Switch},
        {"ready", true, hooks_map(1, true, true), M::Switch, M::Switch, M::Switch},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        auto mode = [&row](bool has_fallback, bool is_local) {
            return plugin_switch_mode(row.plugin_available, row.features, kPrintHooksFeature,
                                      kPrintHooksMinVersion, has_fallback, is_local);
        };
        CHECK(mode(false, true) == row.plugin_only);
        CHECK(mode(false, false) == row.plugin_only);
        CHECK(mode(true, true) == row.fallback_local);
        CHECK(mode(true, false) == row.fallback_remote);
    }
    INFO("another feature's entry does not stand in");
    PluginFeatureMap other;
    other["plr"] = entry(1, true, true);
    CHECK(plugin_switch_mode(true, other, kPrintHooksFeature, kPrintHooksMinVersion, false,
                             false) == M::NeedsUpdate);
}

TEST_CASE("only the fallback's sender writes the shared record, never an empty one",
          "[plugin_print_hooks]") {
    CHECK(writes_standing_record(FallbackSenderRole::Sender, "M117 one"));
    CHECK_FALSE(writes_standing_record(FallbackSenderRole::Standby, "M117 one"));
    INFO("before the filament backend reports, nothing is written");
    CHECK_FALSE(writes_standing_record(FallbackSenderRole::Sender, ""));
}

TEST_CASE("a failed opt-in read keeps the last record unless there is none", "[plugin_feature]") {
    const auto missing = MoonrakerError::from_json_rpc(
        {{"code", -32601},
         {"message", "Key 'plugin_features' in namespace 'helix-screen' not found"}},
        "server.database.get_item");
    REQUIRE(missing.is_not_found());
    CHECK(opt_ins_after_failed_read(missing) == json::object());

    MoonrakerError timeout;
    timeout.type = MoonrakerErrorType::TIMEOUT;
    timeout.message = "timed out";
    REQUIRE_FALSE(timeout.is_not_found());
    CHECK(opt_ins_after_failed_read(timeout) == std::nullopt);
}

TEST_CASE("one HelixScreen per printer sends a fallback", "[plugin_feature]") {
    INFO("the screen on the printer's host always sends");
    CHECK(fallback_sender_role(true, std::nullopt) == FallbackSenderRole::Sender);
    CHECK(fallback_sender_role(true, true) == FallbackSenderRole::Sender);
    CHECK(fallback_sender_role(true, false) == FallbackSenderRole::Sender);
    INFO("a remote screen sends only when no local one is connected");
    CHECK(fallback_sender_role(false, false) == FallbackSenderRole::Sender);
    CHECK(fallback_sender_role(false, true) == FallbackSenderRole::Standby);
    INFO("and stands by when that cannot be told");
    CHECK(fallback_sender_role(false, std::nullopt) == FallbackSenderRole::Standby);
}

TEST_CASE("a local screen's presence record says whether it is connected", "[plugin_feature]") {
    constexpr int64_t t0 = 1700000000;
    const json record = local_screen_presence_record(t0);
    CHECK(record == json{{"seen_at", t0}});
    CHECK(local_screen_present(record, t0) == true);
    CHECK(local_screen_present(record, t0 + 2 * kLocalScreenPresenceIntervalS) == true);
    CHECK(local_screen_present(record, t0 + 2 * kLocalScreenPresenceIntervalS + 1) == false);
    INFO("a writer whose clock runs ahead still reads as present");
    CHECK(local_screen_present(record, t0 - 300) == true);
    INFO("a record that is not one cannot be judged");
    CHECK(local_screen_present(json::object(), t0) == std::nullopt);
    CHECK(local_screen_present(json{{"seen_at", "soon"}}, t0) == std::nullopt);
    CHECK(local_screen_present(json(), t0) == std::nullopt);
}

TEST_CASE("the plugin overview lists every reported feature with its status", "[plugin_feature]") {
    PluginFeatureMap features;
    features["plr"] = entry(1, true, false);
    features["convert_3mf"] = entry(1, true, true);
    features["print_hooks"] = entry(1, true, true);
    features["zz_future"] = entry(2, true, true);
    features["aa_broken"] = entry(1, false, false, "load_failed");

    const auto lines = plugin_feature_lines(features);

    REQUIRE(lines.size() == 5);
    INFO("known features in table order, with their settings home");
    CHECK(lines[0].name == "print_hooks");
    CHECK(lines[0].label == "Start options for prints from other apps");
    CHECK(lines[0].status == PluginFeatureStatus::Active);
    CHECK(lines[0].home == PluginFeatureHome::Printing);
    CHECK(lines[1].name == "plr");
    CHECK(lines[1].status == PluginFeatureStatus::Off);
    CHECK(lines[1].home == PluginFeatureHome::Printing);
    CHECK(lines[2].name == "convert_3mf");
    CHECK(lines[2].home == PluginFeatureHome::None);

    INFO("unknown features follow by name, with no setting to lead to");
    CHECK(lines[3].name == "aa_broken");
    CHECK(lines[3].label == "aa_broken");
    CHECK(lines[3].status == PluginFeatureStatus::Unavailable);
    CHECK(lines[3].reason == "load_failed");
    CHECK(lines[3].home == PluginFeatureHome::None);
    CHECK(lines[3].icon == kUnknownPluginFeatureIcon);
    CHECK(lines[4].name == "zz_future");

    CHECK(plugin_feature_lines({}).empty());
}

TEST_CASE("the overview hides a vendor push row only while its vendor is absent",
          "[plugin_feature][plugin_vendor]") {
    PluginFeatureMap features;
    features["vendor_ad5x"] = entry(1, false, false, "no_adventurer_json");
    features["vendor_ace"] = entry(1, true, true);
    features["event_journal"] = entry(1, false, false, "load_failed");
    auto lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].name == "event_journal");
    CHECK(lines[0].status == PluginFeatureStatus::Unavailable);
    CHECK(lines[0].reason == "load_failed");
    CHECK(lines[1].name == "vendor_ace");
    CHECK(lines[1].status == PluginFeatureStatus::Active);

    features["vendor_ad5x"] = entry(1, true, false);
    features["vendor_ace"] = entry(1, false, false, "ace_objects_carry_slots");
    lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].name == "vendor_ad5x");
    CHECK(lines[1].status == PluginFeatureStatus::Off);

    INFO("a vendor module that failed to load still shows why");
    features["vendor_ace"] = entry(1, false, false, "load_failed: SyntaxError");
    features["vendor_ad5x"] = entry(1, false, false, "no_ace_bridge");
    lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].name == "vendor_ad5x");
    CHECK(lines[1].reason == "no_ace_bridge");
    CHECK(lines[2].name == "vendor_ace");
    CHECK(lines[2].status == PluginFeatureStatus::Unavailable);
    CHECK(lines[2].reason == "load_failed: SyntaxError");
}

TEST_CASE("the overview names every batch feature and gives the switchless ones no home",
          "[plugin_feature]") {
    PluginFeatureMap features;
    for (const char* name : {"file_analysis", "event_journal", "server_edits", "config_edit",
                             "vendor_ad5x", "vendor_ace"}) {
        features[name] = entry(1, true, true);
    }
    const auto lines = plugin_feature_lines(features);
    REQUIRE(lines.size() == 6);
    for (const PluginFeatureLine& line : lines) {
        INFO(line.name);
        CHECK(line.label != line.name);
        CHECK(line.icon != kUnknownPluginFeatureIcon);
        CHECK(line.home == PluginFeatureHome::None);
    }
}

TEST_CASE("plugin_feature_reported counts an unavailable entry", "[plugin_feature][plr]") {
    PluginFeatureMap features;
    features["plr"] = entry(1, false, false, "not_listed");
    CHECK(plugin_feature_reported(true, features, "plr", 1));
    CHECK_FALSE(plugin_feature_reported(false, features, "plr", 1));
    CHECK_FALSE(plugin_feature_reported(true, features, "plr", 2));
    CHECK_FALSE(plugin_feature_reported(true, features, "file_analysis", 1));
}

#if HELIX_HAS_SNAPMAKER

namespace {
struct StandingU1Fx : LVGLTestFixture {
    StandingU1Fx() {
        helix::Config::get_instance();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_end_of_print_unload_tools(0b0101);
    }
    ~StandingU1Fx() override {
        SettingsManager::instance().set_end_of_print_unload_tools(0);
    }
};
} // namespace

TEST_CASE_METHOD(StandingU1Fx, "the record carries the unload list the screen's own start sends",
                 "[plugin_print_hooks][end_unload]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SECTION("nothing before the firmware reports its heads") {
        CHECK(backend.standing_print_gcode().empty());
        CHECK(render_standing_gcode({&backend}).empty());
    }

    SECTION("the same list, forced for a print already running") {
        SnapmakerTestAccess::handle_status(
            backend,
            json{{"print_task_config", {{"end_unload_filament", {false, false, false, false}}}}});
        const std::string own_start = backend.build_preprint_gcode({}, {});
        REQUIRE(own_start == "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,1,0]");
        CHECK(backend.standing_print_gcode() == own_start + " FORCE=1");
        CHECK(render_standing_gcode({&backend}) == own_start + " FORCE=1");

        INFO("a later setting edit changes both alike");
        SettingsManager::instance().set_end_of_print_unload_tools(0b0010);
        CHECK(backend.standing_print_gcode() == backend.build_preprint_gcode({}, {}) + " FORCE=1");
    }

    SECTION("no routing rides along: it is refused once a print runs") {
        SnapmakerTestAccess::handle_status(
            backend,
            json{{"print_task_config", {{"end_unload_filament", {false, false, false, false}}}}});
        const std::string standing = backend.standing_print_gcode();
        CHECK(standing.find("SET_PRINT_EXTRUDER_MAP") == std::string::npos);
        CHECK(standing.find("SET_PRINT_USED_EXTRUDERS") == std::string::npos);
    }
}

#endif // HELIX_HAS_SNAPMAKER
