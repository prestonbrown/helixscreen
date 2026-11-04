// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_offer_preparing.cpp
 * @brief Power-loss recovery must not ambush a start the user already committed to.
 *
 * Run with: ./build/bin/helix-tests "[plr][print_state]"
 *
 * PlrOfferController computed its `printer_idle` signal from
 * printer_has_job(print_stats.state), which counts only PRINTING and
 * PAUSED. During a host-side pre-print block the wire still reads standby, so
 * the controller considered the printer idle and offered "Resume interrupted
 * print?" on top of a start already under way - a modal ambush whose Resume
 * button starts a DIFFERENT file than the one the user just chose.
 *
 * The decision is asserted through evaluate_offer() + the one-shot latch rather
 * than by rendering the modal; test_plr_prompt.cpp explains why the rendering
 * itself is deliberately not covered here.
 *
 * The Qidi section below pins the same decision path for the other passive
 * backend: was_interrupted is ALSO true during every normal print, so the
 * idle gate is the only thing scoping the offer to a boot after power loss.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plr_offer_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "connection_state.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "plr_offer_controller.h"
#include "plr_plugin_rpc.h"
#include "plr_profile.h"
#include "print_lifecycle_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrintJobState;
using helix::PrintStartPhase;
using helix::ui::PlrOfferController;
using json = nlohmann::json;

namespace {

class PlrOfferPreparingFixture : public LVGLTestFixture {
  public:
    PlrOfferPreparingFixture() {
        auto& ps = get_printer_state();
        // The global PrinterState is shared across the shard: reset and
        // re-init, or a prior case's subjects decide this one's answers.
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        // A validated Snapmaker snapshot: the passive backend, so availability
        // needs no probe and the offer decision reduces to the idle signal.
        ps.update_from_status(
            json{{"print_stats", {{"state", "standby"}}},
                 {"virtual_sdcard",
                  {{"pl_env_valid", true}, {"file_path", "gcodes/interrupted.gcode"}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        REQUIRE(ps.print_state().is_plr_snapshot_valid());
        REQUIRE_FALSE(ps.print_state().pl_recovery_file().empty());
    }

    ~PlrOfferPreparingFixture() override {
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    /// set_print_start_state defers, and its callback republishes the lifecycle.
    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Put the app in the state it is in while running the user's own pre-start
    /// block: committed to a job, printer still reporting standby.
    static void enter_host_side_preparing(helix::PrinterState& ps) {
        ps.print_state().begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
        ps.print_state().set_print_start_state(PrintStartPhase::HOMING, "", 0);
        settle();
        REQUIRE(ps.print_state().get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Preparing);
    }
};

} // namespace

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR offers recovery when the printer is truly idle",
                 "[plr][print_state]") {
    // Non-vacuity baseline. Without this, every suppression assertion below
    // would also pass against a controller that never offers at all.
    PlrOfferController controller;
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer while a print is running",
                 "[plr][print_state]") {
    auto& ps = get_printer_state();

    SECTION("printing") {
        helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        helix::test::set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture, "PLR does not offer during a host-side pre-print block",
                 "[plr][print_state]") {
    // THE BUG. print_stats still says standby, so the wire-only idle test said
    // "idle" and the recovery prompt landed on top of the start.
    auto& ps = get_printer_state();
    enter_host_side_preparing(ps);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PlrOfferPreparingFixture,
                 "PLR does not offer during a firmware-side PRINT_START either",
                 "[plr][print_state]") {
    // The other half of Preparing: Klipper already reports printing because the
    // pre-print work lives inside PRINT_START. The wire caught this one already;
    // it must stay caught.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    ps.print_state().set_print_start_state(PrintStartPhase::HOMING, "", 0);
    settle();
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Preparing);

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

// ===========================================================================
// Qidi backend: was_interrupted is the availability signal, so the offer
// decision reduces to the idle signal exactly as on Snapmaker.
// ===========================================================================

namespace {

class QidiPlrOfferFixture : public LVGLTestFixture {
  public:
    QidiPlrOfferFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        // Discovery found the stock macros (set_hardware's half), then the
        // boot status carried the still-true was_interrupted a power loss
        // leaves behind (update_from_status's half).
        lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
        ps.update_from_status(
            json{{"print_stats", {{"state", "standby"}}},
                 {"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
        REQUIRE(ps.print_state().is_plr_interrupted_flag());
    }

    ~QidiPlrOfferFixture() override {
        auto& ps = get_printer_state();
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR offers recovery when the printer is idle",
                 "[plr][qidi]") {
    // Non-vacuity baseline: the Qidi path reaches the prompt at all.
    PlrOfferController controller;
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR does not offer while a print is running",
                 "[plr][qidi]") {
    // was_interrupted stays true through every normal print (only
    // CLEAR_LAST_FILE clears it), so the idle gate is the whole defence
    // against offering a resume on top of the print in progress.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR does not offer without the macro capability",
                 "[plr][qidi]") {
    // A was_interrupted variable on a printer without the stock recovery
    // macros: the variable alone must not select the backend (any Klipper user
    // can SAVE_VARIABLE that name).
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 0);
    settle();

    PlrOfferController controller;
    settle();

    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR capability is wired from discovery",
                 "[plr][qidi]") {
    // Drives the real set_hardware wire rather than poking the subject: the
    // discovery snapshot's RESUME_INTERRUPTED macro is what marks the printer
    // as running Qidi stock firmware.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 0);

    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"gcode_macro RESUME_INTERRUPTED"}));
    ps.set_hardware(std::move(hw));
    settle();

    CHECK(ps.print_state().is_plr_resume_macro_present());
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR: a disconnect reset lets a reconnect re-offer",
                 "[plr][qidi]") {
    auto& ps = get_printer_state();

    // CONNECTED before construction: the controller seeds its connection
    // baseline from the live subject, so only then does the drop below count
    // as a CONNECTED -> not-CONNECTED edge.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));

    PlrOfferController controller;
    settle();
    REQUIRE(PlrOfferControllerTestAccess::prompted(controller));

    // Printer drops: the controller must force the PLR subjects back to 0, or
    // the subjects' same-value guard swallows the reconnect's identical
    // status and no observer ever fires again.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    CHECK_FALSE(ps.print_state().is_plr_resume_macro_present());
    CHECK_FALSE(ps.print_state().is_plr_interrupted_flag());

    // Reconnect: discovery re-runs (the macro subject) and the boot status
    // re-arrives (was_interrupted), each a genuine 0 -> 1 edge back into the
    // observers, so the one-shot latch re-arms and the offer fires again.
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();

    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

// ===========================================================================
// HelixPrint plugin (GENERIC): the plugin's status reply is the availability
// signal, gated on the `plr` feature.
// ===========================================================================

namespace {

class PluginPlrOfferFixture : public LVGLTestFixture {
  public:
    PluginPlrOfferFixture() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.update_from_status(json{{"print_stats", {{"state", "standby"}}}});
        ps.print_state().set_print_start_state(PrintStartPhase::IDLE, "", 0);
        set_plr_feature(true, true);
        settle();
    }

    ~PluginPlrOfferFixture() override {
        auto& ps = get_printer_state();
        ps.plugin_status_state().set_features({});
        ps.plugin_status_state().set_installed(false);
        lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::DISCONNECTED));
        settle();
    }

    static void set_plr_feature(bool available, bool enabled) {
        auto& plugin = get_printer_state().plugin_status_state();
        helix::PluginFeatureEntry entry;
        entry.version = helix::kPlrFeatureMinVersion;
        entry.available = available;
        entry.enabled = enabled;
        plugin.set_features({{helix::kPlrFeatureName, entry}});
        plugin.set_installed(true);
    }

    static helix::PlrPluginStatus offering(helix::PlrPluginBackend backend) {
        helix::PlrPluginStatus s;
        s.backend = backend;
        s.tier = 2;
        s.offer_available = true;
        helix::PlrSnapshotInfo snap;
        snap.filename = "part.gcode";
        s.snapshot = snap;
        return s;
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR offers when its status offers a resume",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    REQUIRE_FALSE(PlrOfferControllerTestAccess::prompted(controller));

    PlrOfferControllerTestAccess::plugin_status_arrived(controller,
                                                        offering(helix::PlrPluginBackend::Generic));
    settle();
    CHECK(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR does not offer without a resume to offer",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    auto status = offering(helix::PlrPluginBackend::Generic);

    SECTION("the plugin blocks the offer") {
        status.offer_available = false;
        status.blocked_by = {"too_early"};
    }
    SECTION("the plugin has no backend for this printer") {
        status.backend = helix::PlrPluginBackend::None;
    }
    SECTION("the feature is switched off") {
        set_plr_feature(true, false);
    }
    SECTION("the feature cannot run") {
        set_plr_feature(false, false);
    }
    SECTION("a print is running") {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        settle();
    }
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
    helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR keeps asking while the plugin answered before Klipper was ready",
                 "[plr][generic]") {
    PlrOfferController controller;
    settle();
    auto status = offering(helix::PlrPluginBackend::Generic);
    status.offer_available = false;
    status.blocked_by = {"klippy_not_ready"};
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::has_plugin_status(controller));
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR: the legacy QIDI macros offer until the plugin answers",
                 "[plr][generic][qidi]") {
    auto& ps = get_printer_state();
    // The firmware layout both serve: the legacy macros and their flag.
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();

    PlrOfferController controller;
    settle();
    // Before the plugin answers, the legacy backend is all there is.
    REQUIRE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR print_state adapter outranks the legacy QIDI macros",
                 "[plr][generic][qidi]") {
    auto& ps = get_printer_state();
    PlrOfferController controller;
    settle();

    // The plugin answers first, with no resume to offer: the adapter owns this
    // layout, so the legacy macros' flag arriving afterwards offers nothing.
    auto status = offering(helix::PlrPluginBackend::QidiPrintState);
    status.offer_available = false;
    status.blocked_by = {"too_early"};
    PlrOfferControllerTestAccess::plugin_status_arrived(controller, status);
    lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 1);
    ps.update_from_status(json{{"save_variables", {{"variables", {{"was_interrupted", true}}}}}});
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR: a disconnect drops the plugin's answer",
                 "[plr][generic]") {
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    PlrOfferControllerTestAccess::plugin_status_arrived(controller,
                                                        offering(helix::PlrPluginBackend::Generic));
    settle();
    REQUIRE(PlrOfferControllerTestAccess::has_plugin_status(controller));

    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    CHECK_FALSE(PlrOfferControllerTestAccess::has_plugin_status(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR feature state reaches its subjects",
                 "[plr][generic][profile]") {
    auto& plugin = get_printer_state().plugin_status_state();
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Ready));
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 1);

    set_plr_feature(true, false);
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Disabled));
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 0);

    plugin.set_installed(false);
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Absent));
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR feature state follows a features update",
                 "[plr][generic][profile]") {
    // The plugin stays installed; only its features map changes.
    auto& plugin = get_printer_state().plugin_status_state();
    REQUIRE(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
            static_cast<int>(helix::PluginFeatureState::Ready));
    helix::PluginFeatureEntry entry;
    entry.version = helix::kPlrFeatureMinVersion;
    entry.available = true;
    entry.enabled = false;
    plugin.set_features({{helix::kPlrFeatureName, entry}});
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Disabled));
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 0);
    plugin.set_features({});
    CHECK(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
          static_cast<int>(helix::PluginFeatureState::Absent));
}

namespace {

/// The connection and Klipper state the plugin status request waits for.
void set_link(helix::PrinterState& ps, ConnectionState conn, helix::KlippyState klippy) {
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(conn));
    lv_subject_set_int(ps.network_state().get_klippy_state_subject(), static_cast<int>(klippy));
}

} // namespace

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR asks the plugin once Klipper is ready",
                 "[plr][generic]") {
    // The profile write re-reads the plugin's features; the mock answers with
    // the plr feature, as a real plugin does.
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    set_link(ps, ConnectionState::CONNECTED, helix::KlippyState::STARTUP);
    PlrOfferController controller;
    settle();
    const int before = client.call_count(helix::kPlrStatusRpc);

    lv_subject_set_int(ps.network_state().get_klippy_state_subject(),
                       static_cast<int>(helix::KlippyState::READY));
    settle();
    const int after = client.call_count(helix::kPlrStatusRpc);
    set_moonraker_api(nullptr);
    CHECK(before == 0);
    CHECK(after == 1);
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR asks the plugin when the printer connects",
                 "[plr][generic]") {
    // The profile write re-reads the plugin's features; the mock answers with
    // the plr feature, as a real plugin does.
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    set_link(ps, ConnectionState::DISCONNECTED, helix::KlippyState::READY);
    PlrOfferController controller;
    settle();
    const int before = client.call_count(helix::kPlrStatusRpc);

    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    settle();
    const int after = client.call_count(helix::kPlrStatusRpc);
    set_moonraker_api(nullptr);
    CHECK(before == 0);
    CHECK(after == 1);
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR asks the plugin once its plr feature is ready",
                 "[plr][generic]") {
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    // Saving off and no snapshot waiting, on the plugin's side too, so its
    // features answer agrees.
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrUserKey, helix::plr_user_json(false));
    client.send_jsonrpc(
        helix::kPlrDiscardRpc, json::object(), [](const json&) {}, [](const MoonrakerError&) {});
    REQUIRE_FALSE(client.mock_plr_interrupted());
    set_plr_feature(true, false);
    set_link(ps, ConnectionState::CONNECTED, helix::KlippyState::READY);
    PlrOfferController controller;
    settle();
    const int before = client.call_count(helix::kPlrStatusRpc);

    set_plr_feature(true, true);
    settle();
    const int after = client.call_count(helix::kPlrStatusRpc);
    set_moonraker_api(nullptr);
    CHECK(before == 0);
    CHECK(after == 1);
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR rewrites the profile when discovery changes the printer type",
                 "[plr][generic][profile]") {
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.set_printer_type_sync("Qidi Q2");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    REQUIRE(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey)["tier"] == 2);

    ps.set_printer_type_sync("Voron 2.4");
    settle();
    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    CHECK(profile["tier"] == 0);
    CHECK(profile["backend"] == "generic");
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR does not offer without room to lift",
                 "[plr][generic]") {
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    helix::ScopedEnv fail_env{"HELIX_MOCK_PLR_FAIL", "no_lift_room"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    set_link(ps, ConnectionState::DISCONNECTED, helix::KlippyState::READY);
    PlrOfferController controller;
    settle();
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    settle();
    const int asks = client.call_count(helix::kPlrStatusRpc);
    // A Klipper edge after the answer: the answer stands, nothing asks again.
    lv_subject_set_int(ps.network_state().get_klippy_state_subject(),
                       static_cast<int>(helix::KlippyState::STARTUP));
    lv_subject_set_int(ps.network_state().get_klippy_state_subject(),
                       static_cast<int>(helix::KlippyState::READY));
    settle();
    const int asks_after = client.call_count(helix::kPlrStatusRpc);
    set_moonraker_api(nullptr);
    CHECK(asks == 1);
    CHECK(asks_after == 1);
    CHECK(PlrOfferControllerTestAccess::has_plugin_status(controller));
    CHECK_FALSE(PlrOfferControllerTestAccess::prompted(controller));
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR offers a waiting snapshot with saving switched off",
                 "[plr][generic]") {
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrUserKey, helix::plr_user_json(false));
    REQUIRE(client.mock_plr_interrupted());
    // The features answer the plugin gives in that state.
    helix::plr_rpc::refresh_features(api);
    settle();
    auto& plugin = ps.plugin_status_state();
    CHECK(lv_subject_get_int(plugin.get_plr_saving_enabled_subject()) == 0);
    REQUIRE(lv_subject_get_int(plugin.get_plr_feature_state_subject()) ==
            static_cast<int>(helix::PluginFeatureState::Ready));

    set_link(ps, ConnectionState::CONNECTED, helix::KlippyState::READY);
    PlrOfferController controller;
    settle();
    const bool prompted = PlrOfferControllerTestAccess::prompted(controller);
    set_moonraker_api(nullptr);
    CHECK(prompted);
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR writes the resolved profile while saving is still off",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.set_printer_type_sync("Qidi Q2");
    // Disabled: the plugin can run but is not saving, which is what a missing
    // profile (tier 0) leaves it at.
    set_plr_feature(true, false);
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    settle();

    PlrOfferController controller;
    settle();

    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    REQUIRE(profile.is_object());
    CHECK(profile["tier"] == 2);
    CHECK(profile["backend"] == "qidi_print_state");
    CHECK(profile["storage_hint"] == "emmc");
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR never writes the previous printer's profile after a switch",
                 "[plr][generic][profile]") {
    // The profile write re-reads the plugin's features; the mock must answer
    // with the plr feature for the map to survive that, as a real plugin does.
    helix::ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr_env{"HELIX_MOCK_PLR", "generic"};
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.set_printer_type_sync("Qidi Q2");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    REQUIRE(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey)["tier"] == 2);

    // A different printer connects. Until its discovery has run, the type
    // still names the Q2.
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrProfileKey, nullptr);
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    settle();
    // The last connection's features map is still in place: the feature reads
    // available while the type still names the Q2.
    REQUIRE(ps.plugin_status_state().plugin_feature(helix::kPlrFeatureName,
                                                    helix::kPlrFeatureMinVersion) !=
            helix::PluginFeatureState::Absent);
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey).is_null());

    // Discovery resolves the new type, then the plugin probe answers.
    ps.set_printer_type_sync("Voron 2.4");
    settle();
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey).is_null());
    set_plr_feature(true, false);
    settle();
    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    REQUIRE(profile.is_object());
    CHECK(profile["tier"] == 0);
    CHECK(profile["backend"] == "generic");
    ps.set_printer_type_sync("");
}

namespace {
helix::PluginFeatureMap not_listed_plr() {
    helix::PluginFeatureEntry entry;
    entry.version = helix::kPlrFeatureMinVersion;
    entry.available = false;
    entry.reason = "not_listed";
    return {{helix::kPlrFeatureName, entry}};
}
} // namespace

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR writes the profile while the plugin reports the printer unlisted",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    // No profile yet, so the plugin cannot run PLR: only the profile can list
    // the printer.
    ps.plugin_status_state().set_features(not_listed_plr());
    REQUIRE(ps.plugin_status_state().plugin_feature(helix::kPlrFeatureName,
                                                    helix::kPlrFeatureMinVersion) ==
            helix::PluginFeatureState::Absent);
    ps.set_printer_type_sync("Qidi Q2");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    REQUIRE(profile.is_object());
    CHECK(profile["tier"] == 2);
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR re-reads the features when the stored profile is already current",
                 "[plr][generic][profile]") {
    // A second screen on a printer another screen already listed: nothing to
    // write, but this screen's features map was read before the profile landed.
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.plugin_status_state().set_features(not_listed_plr());
    ps.set_printer_type_sync("Qidi Q2");
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrProfileKey,
                       helix::plr_profile_json(helix::plr_resolve_profile(
                           PrinterDetector::get_plr_block("Qidi Q2"), helix::PlrUserOverride{},
                           helix::PlrHomingCheck{})));
    const int status_reads = client.call_count("server.helix.status");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    const int posts = client.call_count("server.database.post_item");
    const int status_reads_after = client.call_count("server.helix.status");
    set_moonraker_api(nullptr);
    CHECK(posts == 0);
    CHECK(status_reads_after > status_reads);
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR writes the profile again after a failed write",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.plugin_status_state().set_features(not_listed_plr());
    ps.set_printer_type_sync("Qidi Q2");
    client.fail_next("server.database.post_item");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    REQUIRE(client.call_count("server.database.post_item") == 1);
    REQUIRE(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey).is_null());

    // The next plugin probe answer is the next opportunity.
    ps.plugin_status_state().set_features(not_listed_plr());
    settle();
    const json profile = client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey);
    set_moonraker_api(nullptr);
    REQUIRE(profile.is_object());
    CHECK(profile["tier"] == 2);
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR writes no profile without a plr entry",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    SECTION("a plugin that reports no plr feature") {
        ps.plugin_status_state().set_features({});
    }
    SECTION("no plugin") {
        ps.plugin_status_state().set_installed(false);
    }
    ps.set_printer_type_sync("Qidi Q2");
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    const int posts = client.call_count("server.database.post_item");
    const int gets = client.call_count("server.database.get_item");
    set_moonraker_api(nullptr);
    CHECK(posts == 0);
    CHECK(gets == 0);
    CHECK(client.mock_db_get(helix::kPlrDbNamespace, helix::kPlrProfileKey).is_null());
    ps.set_printer_type_sync("");
}

TEST_CASE_METHOD(PluginPlrOfferFixture, "Plugin PLR leaves an identical stored profile alone",
                 "[plr][generic][profile]") {
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    ps.set_printer_type_sync("Qidi Q2");
    // Another screen on this printer already wrote the same profile.
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrProfileKey,
                       helix::plr_profile_json(helix::plr_resolve_profile(
                           PrinterDetector::get_plr_block("Qidi Q2"), helix::PlrUserOverride{},
                           helix::PlrHomingCheck{})));
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    PlrOfferController controller;
    settle();
    const int gets = client.call_count("server.database.get_item");
    const int posts = client.call_count("server.database.post_item");
    set_moonraker_api(nullptr);
    CHECK(gets >= 1);
    CHECK(posts == 0);
    ps.set_printer_type_sync("");
}

// ===========================================================================
// A host-stack restart under a running print: the plugin can push its offering
// status before print_stats reports standby, so the lifecycle still reads the
// dead job when the offer is first evaluated.
// ===========================================================================

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Plugin PLR offers once the dead job's lifecycle drops to idle",
                 "[plr][generic][print_state]") {
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();
    PlrOfferController controller;
    settle();

    PlrOfferControllerTestAccess::plugin_status_arrived(controller,
                                                        offering(helix::PlrPluginBackend::Generic));
    settle();
    CHECK(PlrOfferControllerTestAccess::offers_shown(controller) == 0);

    helix::test::set_wire_state(ps, PrintJobState::STANDBY);
    settle();
    CHECK(PlrOfferControllerTestAccess::offers_shown(controller) == 1);

    // The user answered the prompt. A later job dropping to idle on the same
    // connection does not ask again.
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();
    helix::test::set_wire_state(ps, PrintJobState::STANDBY);
    settle();
    CHECK(PlrOfferControllerTestAccess::offers_shown(controller) == 1);
}

TEST_CASE_METHOD(QidiPlrOfferFixture, "Qidi PLR does not offer when a normal print ends",
                 "[plr][qidi][print_state]") {
    // PRINT_END runs CLEAR_LAST_FILE then SDCARD_RESET_FILE, so a finished
    // print drops straight from printing to standby with no Complete between.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();
    PlrOfferController controller;
    settle();
    REQUIRE(PlrOfferControllerTestAccess::offers_shown(controller) == 0);

    bool flag_after_end = false;
    SECTION("the flag clears in the standby frame") {
        flag_after_end = false;
    }
    SECTION("the flag lags the standby frame") {
        flag_after_end = true;
    }
    ps.update_from_status(
        json{{"print_stats", {{"state", "standby"}}},
             {"save_variables", {{"variables", {{"was_interrupted", flag_after_end}}}}}});
    settle();
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Idle);
    CHECK(PlrOfferControllerTestAccess::offers_shown(controller) == 0);
}

TEST_CASE_METHOD(PlrOfferPreparingFixture,
                 "Snapmaker PLR does not offer when a paused print is reset to idle",
                 "[plr][print_state]") {
    // The U1's restart-from-beginning sends SDCARD_RESET_FILE and
    // CANCEL_PRINT_BASE: Paused drops straight to standby while pl_env_valid
    // may still read true.
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PAUSED);
    settle();
    PlrOfferController controller;
    settle();
    REQUIRE(PlrOfferControllerTestAccess::offers_shown(controller) == 0);

    helix::test::set_wire_state(ps, PrintJobState::STANDBY);
    settle();
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Idle);
    CHECK(PlrOfferControllerTestAccess::offers_shown(controller) == 0);
}

TEST_CASE_METHOD(PluginPlrOfferFixture,
                 "Creality PLR probes once the dead job's lifecycle drops to idle",
                 "[plr][print_state]") {
    // The probe is refused outside standby, so a capability edge that lands
    // while the lifecycle still reads the dead job must be retried.
    auto& ps = get_printer_state();
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ps};
    set_moonraker_api(&api);
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();
    PlrOfferController controller;
    settle();

    ps.update_from_status(json{{"print_stats", {{"power_loss", 0}}}});
    settle();
    REQUIRE(lv_subject_get_int(ps.print_state().get_plr_power_loss_signal_subject()) == 1);
    const int while_printing = client.call_count(helix::kCrealityDetectRpc);

    helix::test::set_wire_state(ps, PrintJobState::STANDBY);
    settle();
    const int after_idle = client.call_count(helix::kCrealityDetectRpc);
    set_moonraker_api(nullptr);
    CHECK(while_printing == 0);
    CHECK(after_idle == 1);
}
