// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_prep_start_waiter.cpp
 * @brief A start reads print-select's one prep manager only for its own file.
 *
 * Run with: ./build/bin/helix-tests "[start_waiter]"
 *
 * The manager keeps each file's operations scan under its own key, and
 * start_print() rewrites the G-code by that scan's line numbers. A start and
 * the file view never evict or replace each other's scan; when_ready_to_start()
 * answers only for its own path and only while the answer is current for the
 * printer's macros; start_print() uses only the scan kept for its own file and
 * refuses when it needs one and has none.
 */

#include "ui_print_preparation_manager.h"
#include "ui_update_queue.h"

#include "../test_helpers/delayed_file_transfers.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_start_grace.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/scoped_macros_read.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "gcode_ops_detector.h"
#include "printer_state.h"
#include "queued_job_start.h"

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlantedGcode;
using helix::PrinterStopCheck;
using helix::ui::PrePrintOptionState;
using helix::ui::PrintPreparationManager;
using Outcome = helix::queue::QueuedJobStarter::Outcome;

namespace {

constexpr const char* kPlainFile = "; filament_type = PLA\n"
                                   "G28\n"
                                   "; filament used [g] = 1.50\n";

class WaiterFixture : private helix::PrintSelectGlobalStateReset, public LVGLUITestFixture {
  public:
    WaiterFixture() : client(MoonrakerClientMock::PrinterType::VORON_24, 100.0) {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<DelayedMoonrakerAPIMock>(client, get_printer_state(), transfers);
        manager = std::make_unique<PrintPreparationManager>();
        manager->set_dependencies(api.get(), &get_printer_state());
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        settle();
    }

    ~WaiterFixture() override {
        starter.reset();
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, helix::PrintJobState::STANDBY);
        settle();
        manager.reset();
        api.reset();
        client.disconnect();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty(6);
    }

    /// when_ready_to_start(@p name, @p dir, @p version), recording its answer.
    void wait(const std::string& name, const std::string& dir = {},
              helix::FileVersion version = {}) {
        answers = 0;
        answer.reset();
        manager->when_ready_to_start(name, dir, version,
                                     [this](std::optional<PrinterStopCheck> check) {
                                         ++answers;
                                         answer = check;
                                     });
    }

    void release_scans() {
        transfers.release_held_partials();
        settle();
    }

    /// A starter driving this fixture's manager, its start answered at once.
    void make_starter() {
        helix::test::wait_out_print_start_grace();
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.start",
            [](MoonrakerClientMock*, const json&,
               helix::SharedCallback<void(const json&)> success_cb,
               helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                success_cb(json{{"result", "ok"}});
                return true;
            });
        starter = std::make_unique<helix::queue::QueuedJobStarter>(get_printer_state(), api.get());
        QueuedJobStarterTestAccess::set_prep(*starter, manager.get());
    }

    helix::test::ScopedMacrosRead macros;
    MoonrakerClientMock client;
    DelayedFileTransfers transfers{client, ""};
    std::unique_ptr<DelayedMoonrakerAPIMock> api;
    std::unique_ptr<PrintPreparationManager> manager;
    std::unique_ptr<helix::queue::QueuedJobStarter> starter;
    int answers = 0;
    std::optional<PrinterStopCheck> answer;
};

} // namespace

TEST_CASE_METHOD(WaiterFixture, "a file whose scan is held answers at once",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    manager->scan_file_for_operations(a.name(), "");
    settle();
    REQUIRE(manager->has_printer_stop_answer_for(a.name()));
    const int reads = transfers.partial_read_count;

    wait(a.name());
    CHECK(answers == 1);
    REQUIRE(answer.has_value());
    CHECK(answer->state == PrinterStopCheck::State::Clean);
    CHECK(transfers.partial_read_count == reads); // no second read
}

TEST_CASE_METHOD(WaiterFixture, "a same-named file in another directory is not this file's scan",
                 "[start_waiter][queued_job_start]") {
    // The mock serves a partial read by basename, so one planted file answers
    // for both directories; the scan keys are what tell them apart.
    PlantedGcode part("part.gcode", "", kPlainFile);
    manager->scan_file_for_operations("part.gcode", "waiter_dir_one");
    settle();
    REQUIRE(manager->has_scan_result_for("waiter_dir_one/part.gcode"));
    CHECK_FALSE(manager->has_scan_result_for("waiter_dir_two/part.gcode"));

    transfers.hold_partials = true;
    const int reads = transfers.partial_read_count;
    wait("part.gcode", "waiter_dir_two");
    CHECK(answers == 0);
    CHECK(transfers.partial_read_count == reads + 1); // dir two read for itself

    release_scans();
    CHECK(answers == 1);
    CHECK(answer.has_value());
}

TEST_CASE_METHOD(WaiterFixture, "a start's scan and the file view's are kept side by side",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    PlantedGcode b("waiter_b.gcode", "", kPlainFile);
    transfers.hold_partials = true;

    wait(a.name());
    manager->scan_file_for_operations(b.name(), ""); // the file view opens B
    release_scans();

    // Each answer is kept under its own file: neither read was dropped or repeated.
    CHECK(transfers.partial_read_count == 2);
    CHECK(answers == 1);
    REQUIRE(answer.has_value());
    CHECK(manager->has_printer_stop_answer_for(a.name()));
    CHECK(manager->has_printer_stop_answer_for(b.name()));
    // The view's file stays the shown one.
    REQUIRE(manager->get_scan_result().has_value());
    CHECK(manager->scan_result_for(b.name()) == &*manager->get_scan_result());
}

TEST_CASE_METHOD(WaiterFixture, "the wait holds while the PRINT_START analysis runs",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("waiter_a.gcode", "", kPlainFile);
    manager->scan_file_for_operations(a.name(), "");
    settle();

    // A reconnect starts the analysis; its config listing is held.
    auto* connection = get_printer_state().network_state().get_printer_connection_state_subject();
    lv_subject_set_int(connection, static_cast<int>(ConnectionState::DISCONNECTED));
    settle();
    client.defer_next("server.files.list");
    lv_subject_set_int(connection, static_cast<int>(ConnectionState::CONNECTED));
    settle();
    REQUIRE(manager->is_macro_analysis_in_progress());

    wait(a.name());
    CHECK(answers == 0);
    const int reads = transfers.partial_read_count;

    bool reread = false;
    SECTION("the analysis landing ends it") {}
    SECTION("a macro re-read meanwhile scans the file again first") {
        helix::MacroParamCache::instance().populate_from_configfile(nlohmann::json::object(), {});
        REQUIRE_FALSE(manager->has_printer_stop_answer_for(a.name()));
        reread = true;
    }

    client.fire_deferred("server.files.list");
    // A failed read retries on a timer; let any retry run to its end.
    for (int i = 0; i < 4 && manager->is_macro_analysis_in_progress(); ++i) {
        settle();
        process_lvgl(2100);
    }
    settle();
    REQUIRE_FALSE(manager->is_macro_analysis_in_progress());
    CHECK(answers == 1);
    REQUIRE(answer.has_value());
    // Whatever it answered is current for the printer's macros.
    CHECK(manager->has_printer_stop_answer_for(a.name()));
    CHECK(answer->macro_generation == helix::MacroParamCache::instance().generation());
    if (reread) {
        CHECK(transfers.partial_read_count > reads);
    } else {
        CHECK(transfers.partial_read_count == reads);
    }
}

TEST_CASE_METHOD(WaiterFixture, "a start's option override comes before the attached provider",
                 "[start_waiter][queued_job_start]") {
    manager->set_option_state_provider([](const std::string& id) { return id == "qgl" ? 1 : -1; });
    manager->set_start_option_override([](const std::string& id) { return id == "qgl" ? 0 : -1; });
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::DISABLED);

    manager->set_start_option_override([](const std::string&) { return -1; });
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::ENABLED);

    manager->set_start_option_override(nullptr);
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::ENABLED);
}

TEST_CASE_METHOD(WaiterFixture, "start_print drops a start's option override when it returns",
                 "[start_waiter][queued_job_start]") {
    manager->set_option_state_provider([](const std::string& id) { return id == "qgl" ? 1 : -1; });
    manager->set_start_option_override([](const std::string& id) { return id == "qgl" ? 0 : -1; });
    manager->start_print("part.gcode", "", []() {}, [](bool, const std::string&) {});
    settle();
    CHECK_FALSE(PrintPreparationManagerTestAccess::has_start_option_override(*manager));
    // A later Print tap reads the view's own rows.
    CHECK(manager->get_option_state("qgl") == PrePrintOptionState::ENABLED);
}

namespace {

/// A head with a bed mesh the file runs itself, at line 2.
constexpr const char* kMeshFile = "G28\n"
                                  "BED_MESH_CALIBRATE\n"
                                  "G1 X10\n";

/// The line @p scan found the bed mesh on, or 0 when it found none.
size_t mesh_line(const gcode::ScanResult* scan) {
    if (scan) {
        for (const auto& op : scan->operations) {
            if (op.type == gcode::OperationType::BED_MESH) {
                return op.line_number;
            }
        }
    }
    return 0;
}

gcode::ScanResult mesh_scan() {
    gcode::ScanResult scan;
    gcode::DetectedOperation op;
    op.type = gcode::OperationType::BED_MESH;
    op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
    op.line_number = 2;
    scan.operations.push_back(op);
    return scan;
}

} // namespace

TEST_CASE_METHOD(WaiterFixture, "start_print uses only the scan kept for its own file",
                 "[start_waiter][queued_job_start]") {
    // bed_mesh turned off: a start must know whether the file runs one.
    manager->set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    std::optional<bool> ok;
    std::string error;
    auto start = [&](const std::string& name, const std::string& expected_key = {}) {
        ok.reset();
        error.clear();
        manager->start_print(
            name, "", []() {},
            [&ok, &error](bool success, const std::string& e) {
                ok = success;
                error = e;
            },
            expected_key);
        settle();
    };
    auto refused = [&]() {
        return ok.has_value() && !*ok && error.find("could not be checked") != std::string::npos;
    };

    SECTION("another file's scan is never used") {
        manager->set_cached_scan_result(mesh_scan(), "other.gcode");
        start("part.gcode");
        CHECK(refused());
        CHECK(client.call_count("printer.print.start") == 0);
        CHECK(client.call_count("printer.gcode.script") == 0);
    }
    SECTION("a stick file's scan stands only for that file's copy") {
        PlantedGcode stick("stick_x.gcode", "", kMeshFile);
        manager->scan_file_for_operations(stick.name(), "", stick.path());
        // A stick file is read on the fast lane, off this thread.
        REQUIRE(wait_until(
            [&]() {
                settle();
                return manager->has_scan_result_for(stick.path());
            },
            5000));

        start("other_y.gcode"); // a printer file of another name
        CHECK(refused());

        start("stick_x.gcode", stick.path()); // the copy, naming its scan
        CHECK_FALSE(refused());
    }
    SECTION("its own file's scan is used") {
        manager->set_cached_scan_result(mesh_scan(), "part.gcode");
        start("part.gcode");
        CHECK_FALSE(refused());
    }
    SECTION("nothing turned off needs no scan") {
        manager->set_option_state_provider(nullptr);
        start("part.gcode");
        CHECK_FALSE(refused());
        CHECK(client.call_count("printer.print.start") == 1);
    }
}

TEST_CASE_METHOD(WaiterFixture,
                 "a tile start then the file view opening another file: both keep their scans",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("tile_a.gcode", "", kPlainFile);
    PlantedGcode b("tile_b.gcode", "", kMeshFile);
    make_starter();
    transfers.hold_partials = true;

    std::optional<Outcome> outcome;
    starter->start_with_options(helix::JobQueueEntry{"0001", a.name(), 0.0, 0.0}, {},
                                [&outcome](Outcome o) { outcome = o; });
    settle();
    REQUIRE(starter->in_flight());

    manager->scan_file_for_operations(b.name(), ""); // the file view opens B
    release_scans();

    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::Started);
    CHECK(client.call_count("printer.print.start") == 1);
    // Both scans are kept, and the view's is still B's, shown.
    CHECK(manager->has_scan_result_for(a.name()));
    CHECK(manager->has_printer_stop_answer_for(b.name()));
    REQUIRE(manager->get_scan_result().has_value());
    CHECK(manager->scan_result_for(b.name()) == &*manager->get_scan_result());

    // B then starts from the view, bed mesh off, with its own scan: the file's
    // mesh is found and stripped, which only B's scan can say.
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
    manager->set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    get_printer_state().set_helix_plugin_installed(true); // a rewrite may run
    settle();
    const int downloads = transfers.download_count;
    std::optional<bool> b_ok;
    manager->start_print(
        b.name(), "", []() {}, [&b_ok](bool success, const std::string&) { b_ok = success; });
    settle();
    CHECK_FALSE((b_ok.has_value() && !*b_ok));
    CHECK(transfers.download_count == downloads + 1); // the modify path read the whole file
    get_printer_state().set_helix_plugin_installed(false);
    settle();
}

TEST_CASE_METHOD(WaiterFixture,
                 "an abandoned start never reaches the prep manager its printer freed",
                 "[start_waiter][queued_job_start][teardown][uaf]") {
    PlantedGcode a("tile_abandoned.gcode", "", kPlainFile);
    make_starter();
    transfers.hold_partials = true; // the start waits on its scan
    std::optional<Outcome> outcome;
    starter->start_with_options(helix::JobQueueEntry{"0001", a.name(), 0.0, 0.0}, {},
                                [&outcome](Outcome o) { outcome = o; });
    settle();
    REQUIRE(starter->in_flight());
    REQUIRE(QueuedJobStarterTestAccess::stall_watchdog(*starter) != nullptr);

    // A printer switch: the starter lets go, then print-select's manager is freed.
    starter->abandon();
    manager.reset();
    CHECK_FALSE(starter->in_flight());
    CHECK(QueuedJobStarterTestAccess::stall_watchdog(*starter) == nullptr);

    // Past the stall window, and with the held read answering, nothing runs.
    lv_tick_inc(helix::app_constants::transfer::STALL_MS * 2);
    lv_timer_handler();
    release_scans();
    CHECK_FALSE(outcome.has_value());
    CHECK(client.call_count("printer.print.start") == 0);

    // The next printer's start binds afresh.
    manager = std::make_unique<PrintPreparationManager>();
    manager->set_dependencies(api.get(), &get_printer_state());
    QueuedJobStarterTestAccess::set_prep(*starter, manager.get());
    starter->set_api(api.get());
    transfers.hold_partials = false;
    starter->start_with_options(helix::JobQueueEntry{"0002", a.name(), 0.0, 0.0}, {},
                                [&outcome](Outcome o) { outcome = o; });
    settle();
    REQUIRE(outcome.has_value());
    CHECK(*outcome == Outcome::Started);
}

TEST_CASE_METHOD(WaiterFixture, "a refused start reads a failed file again, so a retry starts",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode part("retry_part.gcode", "", kMeshFile);
    REQUIRE(part.remove_from_disk()); // the read fails
    manager->scan_file_for_operations(part.name(), "");
    settle();
    REQUIRE_FALSE(manager->has_scan_result_for(part.name()));
    manager->set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    REQUIRE_FALSE(manager->print_waits_on_scan(part.name())); // the failed read answered
    std::optional<bool> ok;
    std::string error;
    auto tap_print = [&]() {
        ok.reset();
        error.clear();
        manager->start_print(
            part.name(), "", []() {},
            [&ok, &error](bool success, const std::string& e) {
                ok = success;
                error = e;
            });
        settle();
    };

    {
        std::ofstream out(part.path(), std::ios::trunc | std::ios::binary);
        out << kMeshFile;
        REQUIRE(out.good());
    }
    const int reads = transfers.partial_read_count;
    transfers.hold_partials = true;
    tap_print();
    REQUIRE(ok.has_value());
    CHECK_FALSE(*ok);
    CHECK(error.find("Try again") != std::string::npos);
    CHECK(transfers.partial_read_count == reads + 1); // read again
    CHECK(manager->print_waits_on_scan(part.name()));

    release_scans();
    REQUIRE(manager->has_scan_result_for(part.name()));
    tap_print();
    CHECK_FALSE((ok.has_value() && !*ok));
}

TEST_CASE_METHOD(WaiterFixture, "a re-sliced file of the same name is read again",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode part("waiter_resliced.gcode", "", kMeshFile);
    const helix::FileVersion sliced{100, 1000};
    manager->scan_file_for_operations(part.name(), "", "", sliced);
    settle();
    REQUIRE(mesh_line(manager->scan_result_for(part.name())) == 2);
    const int reads = transfers.partial_read_count;

    SECTION("the same version reuses its scan") {
        manager->scan_file_for_operations(part.name(), "", "", sliced);
        settle();
        CHECK(transfers.partial_read_count == reads);
    }

    // The same name uploaded again, its mesh now at line 4.
    const helix::FileVersion resliced{120, 2000};
    auto reslice = [&part]() {
        std::ofstream out(part.path(), std::ios::trunc | std::ios::binary);
        out << "G28\nG1 X10\nG1 Y10\nBED_MESH_CALIBRATE\n";
        REQUIRE(out.good());
    };
    auto check_new_lines = [&]() {
        CHECK(transfers.partial_read_count == reads + 1);
        CHECK(mesh_line(manager->scan_result_for(part.name())) == 4);
    };
    SECTION("the file view opening the new version reads it") {
        reslice();
        manager->scan_file_for_operations(part.name(), "", "", resliced);
        settle();
        check_new_lines();
    }
    SECTION("a start waiting on the new version reads it") {
        reslice();
        wait(part.name(), {}, resliced);
        settle();
        CHECK(answers == 1);
        check_new_lines();
    }
}

namespace {

/// Files planted for the eviction cases, each with its own name.
std::vector<std::unique_ptr<PlantedGcode>> plant(const std::string& prefix, int count) {
    std::vector<std::unique_ptr<PlantedGcode>> files;
    for (int i = 1; i <= count; ++i) {
        files.push_back(std::make_unique<PlantedGcode>(prefix + "_" + std::to_string(i) + ".gcode",
                                                       "", kPlainFile));
    }
    return files;
}

} // namespace

TEST_CASE_METHOD(WaiterFixture,
                 "kept scans stay bounded, and never evict the shown file or a start's",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode shown("evict_shown.gcode", "", kPlainFile);
    PlantedGcode started("evict_started.gcode", "", kPlainFile);
    manager->scan_file_for_operations(shown.name(), "");
    settle();
    wait(started.name());
    settle();
    REQUIRE(answers == 1);

    // Five more files, each read by neither the view nor the start.
    const auto others = plant("evict_other", 5);
    for (const auto& f : others) {
        PrintPreparationManagerTestAccess::request_scan(*manager, f->name());
        settle();
        REQUIRE(manager->has_printer_stop_answer_for(f->name()));
    }

    CHECK(PrintPreparationManagerTestAccess::kept_scan_count(*manager) ==
          PrintPreparationManager::kMaxKeptScans);
    CHECK(manager->has_scan_result_for(shown.name()));
    CHECK(manager->has_scan_result_for(started.name()));
    // The oldest unheld go first.
    CHECK_FALSE(manager->has_scan_result_for(others[0]->name()));
    CHECK_FALSE(manager->has_scan_result_for(others[2]->name()));
    CHECK(manager->has_scan_result_for(others[3]->name()));
    CHECK(manager->has_scan_result_for(others[4]->name()));
}

TEST_CASE_METHOD(WaiterFixture, "a start's file stays kept from its answer until start_print",
                 "[start_waiter][queued_job_start]") {
    PlantedGcode a("pin_a.gcode", "", kMeshFile);
    wait(a.name());
    settle();
    REQUIRE(answers == 1);

    // The file view opens five other files while the start's dialogs are up.
    const auto viewed = plant("pin_viewed", 5);
    for (const auto& f : viewed) {
        manager->scan_file_for_operations(f->name(), "");
        settle();
    }
    REQUIRE(manager->has_scan_result_for(a.name()));

    // The start still reads its own scan: bed mesh off is honoured, not refused.
    manager->set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    std::optional<bool> ok;
    std::string error;
    manager->start_print(
        a.name(), "", []() {},
        [&ok, &error](bool success, const std::string& e) {
            ok = success;
            error = e;
        });
    settle();
    CHECK_FALSE(
        (ok.has_value() && !*ok && error.find("could not be checked") != std::string::npos));

    // Taken by the start, the scan is no longer held.
    const auto later = plant("pin_later", PrintPreparationManager::kMaxKeptScans);
    for (const auto& f : later) {
        PrintPreparationManagerTestAccess::request_scan(*manager, f->name());
        settle();
    }
    CHECK_FALSE(manager->has_scan_result_for(a.name()));
}
