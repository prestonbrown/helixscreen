// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queued_job_start.cpp
 * @brief The home tile's one-tap queued start: saved options, every check, no screen.
 *
 * Run with: ./build/bin/helix-tests "[queued_job_start]"
 *
 * QueuedJobStarter starts the queue's job through the same PrintStartController
 * gates and PrintPreparationManager start a file-view Print tap uses, with the
 * file half of the checks from FileStartFacts. A gate that warns shows its
 * dialog and nothing starts until Proceed; the empty-slot pre-flight offers
 * Print Anyway and Remap, and Remap is the one path that opens the file view.
 */

#include "ui_modal.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/scoped_macros_read.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preprint_skip_wrappers.h"
#include "printer_discovery.h"
#include "queued_job_start.h"
#include "static_panel_registry.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlantedGcode;
using helix::PrintJobState;
using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobStarter;
using helix::test::NotificationLog;
using helix::test::set_wire_state;
using helix::test::WarningLog;
using Outcome = QueuedJobStarter::Outcome;

TEST_CASE("saved_option_state answers from the saved entry", "[queued_job_start]") {
    const QueuedJobOptions saved{"part.gcode", {{"bed_mesh", false}, {"qgl", true}}};
    CHECK(helix::queue::saved_option_state(saved, "bed_mesh") == 0);
    CHECK(helix::queue::saved_option_state(saved, "qgl") == 1);
    CHECK(helix::queue::saved_option_state(saved, "z_tilt") == -1);
    CHECK(helix::queue::saved_option_state(QueuedJobOptions{}, "bed_mesh") == -1);
}

namespace {

/// One PETG tool, one footer line naming it: the file a PLA lane mismatches.
constexpr const char* kPetgFile = "; filament_type = PETG\n"
                                  "; filament_colour = #FF0000\n"
                                  "G28\n"
                                  "; filament used [g] = 1.50\n";

class StarterFixture : public helix::test::QueuedStartMockedFixture {
  public:
    StarterFixture() {
        starter = std::make_unique<QueuedJobStarter>(get_printer_state(), api_.get());
        // The fixture's panel is this test's print-select.
        QueuedJobStarterTestAccess::set_prep(*starter, panel_->print_preparation_manager());
    }

    ~StarterFixture() override {
        starter.reset();
    }

    void start(const helix::JobQueueEntry& job) {
        outcome.reset();
        starter->start(job, [this](Outcome o) { outcome = o; });
        settle();
    }

    /// Press @p button on the dialog on top of ModalStack.
    void press(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
        lv_timer_handler();
        settle();
    }

    std::unique_ptr<QueuedJobStarter> starter;
    std::optional<Outcome> outcome;
};

} // namespace

TEST_CASE_METHOD(StarterFixture, "a busy printer refuses before anything is sent",
                 "[queued_job_start]") {
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    settle();
    const int reads = mock_client_.call_count("server.database.get_item");

    WarningLog warnings;
    start(entry("0001", "3DBenchy.gcode"));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::Refused);
    CHECK(warnings.contains("3DBenchy.gcode"));
    CHECK(mock_client_.call_count("server.database.get_item") == reads);
    CHECK_FALSE(starter->in_flight());
    CHECK(queue_has("0001"));
}

TEST_CASE_METHOD(StarterFixture, "a second start while one is in flight is refused",
                 "[queued_job_start]") {
    mock_client_.defer_next("server.database.get_item");
    starter->start(entry("0001", "3DBenchy.gcode"));
    REQUIRE(starter->in_flight());

    std::optional<Outcome> second;
    starter->start(entry("0002", "xyz-10mm-calibration-cube.gcode"),
                   [&second](Outcome o) { second = o; });
    REQUIRE(second.has_value());
    CHECK(*second == Outcome::Refused);
    CHECK(starter->in_flight());

    // End the first start without a print.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    mock_client_.fire_deferred("server.database.get_item");
    settle();
    CHECK_FALSE(starter->in_flight());
}

TEST_CASE_METHOD(StarterFixture, "a controller refusal ends the start", "[queued_job_start]") {
    mock_client_.defer_next("server.database.get_item");
    start(entry("0001", "3DBenchy.gcode"));
    REQUIRE(starter->in_flight());

    // The printer took another job while the saved options were read.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    mock_client_.fire_deferred("server.database.get_item");
    settle();

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::NotStarted);
    CHECK(starts() == 0);
    CHECK(queue_has("0001"));
}

TEST_CASE_METHOD(StarterFixture,
                 "a material mismatch from the tile warns and does not start until confirmed",
                 "[queued_job_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    PlantedGcode file("tile_mismatch.gcode", "", kPetgFile);
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    start(entry("0001", file.name()));

    auto& controller = QueuedJobStarterTestAccess::controller(*starter);
    REQUIRE(PrintStartControllerTestAccess::print_gate_modal(controller) != nullptr);
    CHECK(PrintStartControllerTestAccess::open_gate_name(controller) == "material_compatibility");
    CHECK(starts() == 0);
    CHECK(starter->in_flight());
    CHECK_FALSE(outcome.has_value());

    SECTION("cancel ends the start and keeps the job") {
        press("btn_secondary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::NotStarted);
        CHECK(starts() == 0);
        CHECK(queue_has("0001"));
        CHECK(read_store().count("0001") == 1);
    }
    SECTION("proceed starts it and the job leaves the queue") {
        press("btn_primary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::Started);
        CHECK(starts() == 1);
        CHECK_FALSE(queue_has("0001"));
        CHECK(read_store().count("0001") == 0);
    }
}

TEST_CASE_METHOD(StarterFixture,
                 "an empty-slot block offers Force and Remap; Remap opens the file view",
                 "[queued_job_start]") {
    // One lane, empty: tool 0 maps to it.
    helix::test::ScopedMockAms ams({{0xFF0000, "PETG"}});
    ams.load(0, 0xFF0000, "PETG", helix::SlotStatus::EMPTY);
    PlantedGcode file("tile_empty.gcode", "", kPetgFile);
    StaticPanelRegistry::instance().destroy_all();

    start(entry("0001", file.name()));

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(lv_obj_find_by_name(dialog, "btn_tertiary") != nullptr); // the pre-flight modal
    CHECK(starts() == 0);
    CHECK_FALSE(outcome.has_value());

    SECTION("Remap hands the job to the file view") {
        NotificationLog infos(NotificationLog::Kind::Info);
        press("btn_tertiary");
        CHECK_FALSE(infos.contains("could not be checked from Home")); // the user chose it
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::OpenedFileView);
        CHECK(starts() == 0);
        const std::string* pending = ::PrintSelectPanelTestAccess::pending_queued_job_id(
            *get_print_select_panel(get_printer_state(), api_.get()));
        REQUIRE(pending != nullptr);
        CHECK(*pending == "0001");
        CHECK(queue_has("0001"));
    }
    SECTION("Force runs the gates and starts") {
        press("btn_primary");
        // Any later gate that warns is confirmed the same way.
        for (int i = 0; i < 4 && !outcome; ++i) {
            if (PrintStartControllerTestAccess::print_gate_modal(
                    QueuedJobStarterTestAccess::controller(*starter)) == nullptr) {
                break;
            }
            press("btn_primary");
        }
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::Started);
        CHECK(starts() == 1);
    }
    SECTION("Cancel ends the start") {
        press("btn_secondary");
        REQUIRE(outcome.has_value());
        CHECK(*outcome == Outcome::NotStarted);
        CHECK(starts() == 0);
    }

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(StarterFixture, "the pre-flight dialog of an abandoned start drives nothing",
                 "[queued_job_start][teardown]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PETG"}});
    ams.load(0, 0xFF0000, "PETG", helix::SlotStatus::EMPTY);
    PlantedGcode file("tile_abandon_dialog.gcode", "", kPetgFile);
    StaticPanelRegistry::instance().destroy_all();
    start(entry("0001", file.name()));
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(lv_obj_find_by_name(dialog, "btn_tertiary") != nullptr); // the pre-flight modal

    starter->abandon();
    press("btn_primary"); // Print Anyway, after the printer went away

    CHECK(starts() == 0);
    CHECK_FALSE(starter->in_flight());
    CHECK_FALSE(outcome.has_value());

    StaticPanelRegistry::instance().destroy_all();
}

namespace {

/// The job the global print-select panel's file view holds, or "" for none.
std::string file_view_job(MoonrakerAPI* api) {
    const std::string* pending = ::PrintSelectPanelTestAccess::pending_queued_job_id(
        *get_print_select_panel(get_printer_state(), api));
    return pending ? *pending : std::string();
}

} // namespace

TEST_CASE_METHOD(StarterFixture,
                 "a printer-stop check that did not run opens the file view, never starts",
                 "[queued_job_start]") {
    PlantedGcode file("tile_unchecked.gcode", "", kPetgFile);
    StaticPanelRegistry::instance().destroy_all();
    // Unread macros leave the file's printer-stopping command check NotRun.
    helix::MacroParamCache::instance().clear();

    start(entry("0001", file.name()));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::OpenedFileView);
    CHECK(file_view_job(api_.get()) == "0001");
    CHECK(starts() == 0);
    CHECK(queue_has("0001"));
    CHECK_FALSE(starter->in_flight());

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(StarterFixture, "a queued .3mf opens in the file view and starts nothing",
                 "[queued_job_start]") {
    PlantedGcode file("tile_project.gcode.3mf", "", "PK\x03\x04");
    StaticPanelRegistry::instance().destroy_all();
    NotificationLog infos(NotificationLog::Kind::Info);

    start(entry("0001", file.name()));
    CHECK(infos.contains(file.name() + " could not be checked from Home"));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::OpenedFileView);
    CHECK(file_view_job(api_.get()) == "0001");
    CHECK(starts() == 0);
    CHECK(queue_has("0001"));

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(StarterFixture,
                 "a rewrite that fails ends the start and hands the file view its own options",
                 "[queued_job_start]") {
    // The file runs its own bed mesh and the job saved bed mesh off, so with
    // the plugin present the start rewrites the file.
    PlantedGcode file("tile_rewrite.gcode", "",
                      "; filament_type = PLA\nG28\nBED_MESH_CALIBRATE\nG1 X10\n"
                      "; filament used [g] = 1.50\n");
    seed_store("0001", file.name(), {{"bed_mesh", false}});
    get_printer_state().set_helix_plugin_installed(true);
    settle();
    auto& job_api = static_cast<MoonrakerAPIMock&>(*api_).job_mock();
    job_api.mock_fail_modified_prints();
    auto* prep = panel_->print_preparation_manager();

    start(entry("0001", file.name()));

    REQUIRE(job_api.modified_prints().size() == 1); // the rewrite ran and its start failed
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::NotStarted);
    CHECK_FALSE(starter->in_flight());
    CHECK_FALSE(PrintPreparationManagerTestAccess::has_start_option_override(*prep));
    CHECK(queue_has("0001"));
    CHECK(read_store().count("0001") == 1);

    // A later tap is not refused as a start in flight.
    job_api.mock_fail_modified_prints(false);
    start(entry("0001", file.name()));
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::Started);

    get_printer_state().set_helix_plugin_installed(false);
    settle();
}

TEST_CASE_METHOD(StarterFixture, "a stalled start gives up, and a later tap starts again",
                 "[queued_job_start]") {
    mock_client_.defer_next("server.database.get_item");
    start(entry("0001", "3DBenchy.gcode"));
    REQUIRE(starter->in_flight());
    lv_timer_t* watchdog = QueuedJobStarterTestAccess::stall_watchdog(*starter);
    REQUIRE(watchdog != nullptr);

    WarningLog warnings;
    lv_timer_ready(watchdog);
    lv_timer_handler();
    settle();

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::NotStarted);
    CHECK(warnings.contains("Could not check"));
    CHECK_FALSE(starter->in_flight());
    CHECK(queue_has("0001"));

    // A later tap starts again; its metadata read is held.
    const int metadata_reads = mock_client_.call_count("server.files.metadata");
    mock_client_.defer_next("server.files.metadata");
    std::optional<Outcome> second;
    starter->start_with_options(entry("0002", "3DBenchy.gcode"), {},
                                [&second](Outcome o) { second = o; });
    settle();
    REQUIRE(starter->in_flight());
    REQUIRE(mock_client_.call_count("server.files.metadata") == metadata_reads + 1);

    // The first start's options answering now must not act on the second.
    mock_client_.fire_deferred("server.database.get_item");
    settle();
    CHECK(mock_client_.call_count("server.files.metadata") == metadata_reads + 1);
    CHECK(starter->in_flight());
    CHECK_FALSE(second.has_value());

    mock_client_.fire_deferred("server.files.metadata");
    settle();
}

TEST_CASE_METHOD(StarterFixture,
                 "a macro re-read before the gates run sends the job to the file view there",
                 "[queued_job_start]") {
    // An empty lane for tool 0 holds the start on the pre-flight dialog.
    helix::test::ScopedMockAms ams({{0xFF0000, "PETG"}});
    ams.load(0, 0xFF0000, "PETG", helix::SlotStatus::EMPTY);
    PlantedGcode file("tile_reread.gcode", "", kPetgFile);
    StaticPanelRegistry::instance().destroy_all();
    start(entry("0001", file.name()));
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(lv_obj_find_by_name(dialog, "btn_tertiary") != nullptr); // the pre-flight modal

    // The printer's macros are read again while the dialog waits, so the
    // file's printer-stopping command check no longer describes them.
    helix::MacroParamCache::instance().populate_from_configfile(nlohmann::json::object(), {});

    press("btn_primary"); // Print Anyway
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::OpenedFileView);
    CHECK(file_view_job(api_.get()) == "0001");
    CHECK(starts() == 0);
    CHECK(queue_has("0001"));

    StaticPanelRegistry::instance().destroy_all();
}

// ============================================================================
// One PrintPreparationManager for the app
// ============================================================================

namespace {

/// The app's own wiring: the global print-select panel and a starter left
/// to find its prep manager the production way. No fixture panel, so the
/// panel's manager is the only one observing the printer.
class OneManagerFixture : private helix::PrintSelectGlobalStateReset, public LVGLUITestFixture {
  public:
    OneManagerFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        StaticPanelRegistry::instance().destroy_all();
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        set_connection(ConnectionState::CONNECTED);
        helix::test::wait_out_print_start_grace();
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.start",
            [](MoonrakerClientMock*, const json&,
               helix::SharedCallback<void(const json&)> success_cb,
               helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                success_cb(json{{"result", "ok"}});
                return true;
            });
        panel = get_print_select_panel(get_printer_state(), api.get());
        // setup() builds the detail-view model, and with it the manager.
        REQUIRE(panel->print_preparation_manager() != nullptr);
        settle();
    }

    ~OneManagerFixture() override {
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        settle();
        StaticPanelRegistry::instance().destroy_all();
        set_moonraker_api(previous_api_);
        api.reset();
        client.stop_temperature_simulation();
        client.disconnect();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty(6);
    }

    static void set_connection(ConnectionState state) {
        lv_subject_set_int(
            get_printer_state().network_state().get_printer_connection_state_subject(),
            static_cast<int>(state));
        settle();
    }

    /// PRINT_START analyses started by one reconnect: each reads the config
    /// file list once.
    int analyses_per_reconnect() {
        const int before = client.call_count("server.files.list");
        set_connection(ConnectionState::DISCONNECTED);
        set_connection(ConnectionState::CONNECTED);
        return client.call_count("server.files.list") - before;
    }

    /// `_HELIX_PREP` resets sent when a job ends with a leveling skip pending.
    int skip_resets_at_job_end() {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"bed_mesh", "quad_gantry_level", "gcode_macro _HELIX_PREP"}));
        hw.set_skip_wrappers({helix::skip_wrappers::Op::BedMesh, helix::skip_wrappers::Op::Qgl},
                             {helix::skip_wrappers::Op::BedMesh, helix::skip_wrappers::Op::Qgl});
        auto& ps = get_printer_state();
        ps.set_hardware(hw);
        lv_subject_set_int(ps.print_state().get_job_holds_machine_subject(), 1);
        settle();
        ps.update_from_status({{"gcode_macro _HELIX_PREP", {{"run_qgl", 0}}}});
        settle();
        const int before = client.call_count("printer.gcode.script");
        lv_subject_set_int(ps.print_state().get_job_holds_machine_subject(), 0);
        settle();
        return client.call_count("printer.gcode.script") - before;
    }

    helix::test::ScopedMacrosRead macros;
    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
    PrintSelectPanel* panel = nullptr;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(OneManagerFixture, "the tile drives print-select's own prep manager",
                 "[queued_job_start][one_prep_manager]") {
    const int analyses_before = analyses_per_reconnect();
    REQUIRE(analyses_before == 1);

    QueuedJobStarter starter(get_printer_state(), api.get());
    CHECK(QueuedJobStarterTestAccess::prep(starter) == panel->print_preparation_manager());

    PlantedGcode file("one_manager.gcode", "", kPetgFile);
    std::optional<Outcome> outcome;
    starter.start(helix::JobQueueEntry{"0001", file.name(), 0.0, 0.0},
                  [&outcome](Outcome o) { outcome = o; });
    settle();
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == Outcome::Started);

    SECTION("one PRINT_START analysis per connect") {
        CHECK(analyses_per_reconnect() == 1);
    }
    SECTION("one owner resets a pending skip") {
        CHECK(skip_resets_at_job_end() == 1);
    }
}
