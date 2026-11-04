// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"
#include "ui_panel_print_status.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/archive_transfers.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_snapmaker.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "print_start_checks.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"
#include "three_mf_reader.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::string fixture_dir() {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    return pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/three_mf/"
                                    : "tests/fixtures/three_mf/";
}

std::string disk_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void drain_until_quiet() {
    UpdateQueue::instance().drain_until_empty(16);
}

constexpr const char* kArchive = "Door.gcode.3mf";

struct ArchiveStart : HelixTestFixture {
    ArchiveStart() {
        lv_init_safe();
        // The success path names the print on the GLOBAL state.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        manager.set_dependencies(&printer.api, &printer.state);
        printer.state.print_state().begin_preparing(PrintJobRef{kArchive, "", ""});
    }

    void plant(const char* fixture) {
        planted.emplace(kArchive, "", disk_bytes(fixture_dir() + fixture));
    }

    void start() {
        manager.start_print(
            kArchive, "", [this]() { ++navigated; },
            [this](bool ok, const std::string&) {
                completed = true;
                succeeded = ok;
            });
        drain_until_quiet();
    }

    MockPrinter printer;
    PrintPreparationManager manager;
    std::optional<PlantedGcode> planted;
    int navigated = 0;
    bool completed = false;
    bool succeeded = false;
};

} // namespace

TEST_CASE_METHOD(ArchiveStart, "a .3mf plate prints as staged G-code that maps back to the archive",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9);
    start();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].root == "gcodes");
    const std::string& staged = uploads[0].dest_path;
    INFO(staged);
    CHECK(staged.compare(staged.size() - 6, 6, ".gcode") == 0);
    CHECK(gcode::is_uploaded_rewrite_path(staged));
    CHECK(gcode::trusted_original_path(staged) == kArchive);
    CHECK(gcode::rewritten_archive_plate(staged) == 9);
    // What left the machine is the plate, not the archive.
    CHECK(uploads[0].content == disk_bytes(fixture_dir() + "src/plate.gcode"));

    // Klipper is asked for the staged copy, never through the plugin.
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == staged);
    CHECK(printer.api.job_mock().modified_prints().empty());

    // Neither the archive nor the extracted plate outlives the flow.
    const auto& downloads = printer.api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
    CHECK_FALSE(std::filesystem::exists(uploads[0].local_path));
}

TEST_CASE_METHOD(ArchiveStart, "a .3mf the printer reads natively starts as itself",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    SECTION("never marked") {}
    SECTION("marked, then forgotten") {
        manager.set_archive_plate(kArchive, 9);
        manager.set_archive_plate(kArchive, 0);
    }
    SECTION("another file marked") {
        manager.set_archive_plate("Other.gcode.3mf", 2);
    }
    start();

    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.transfers_mock().download_destinations().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == kArchive);
}

TEST_CASE_METHOD(ArchiveStart, "a plate whose checksum fails is not uploaded or started",
                 "[print_preparation][three_mf]") {
    plant("bad_md5.gcode.3mf");
    manager.set_archive_plate(kArchive, 9);
    start();

    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK(completed);
    CHECK_FALSE(succeeded);
    CHECK_FALSE(printer.state.print_state().has_preparing_job());
    const auto& downloads = printer.api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
}

TEST_CASE_METHOD(ArchiveStart, "a plate the archive does not hold fails the start",
                 "[print_preparation][three_mf]") {
    plant("multi_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9); // plates 2 and 5 only
    start();
    CHECK(printer.api.transfers_mock().path_uploads().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK_FALSE(succeeded);
}

TEST_CASE_METHOD(ArchiveStart, "the chosen plate of several is the one that prints",
                 "[print_preparation][three_mf]") {
    plant("multi_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 5);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 5);
    CHECK(uploads[0].content.find("total layer number: 90") != std::string::npos);
}

TEST_CASE_METHOD(ArchiveStart, "a .3mf plate on a transport without local copies is refused",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    printer.api.transfers_mock().mock_no_local_copies();
    manager.set_dependencies(&printer.api, &printer.state);
    manager.set_archive_plate(kArchive, 9);
    start();

    CHECK(printer.api.transfers_mock().download_destinations().empty());
    CHECK(printer.api.job_mock().started_prints().empty());
    CHECK_FALSE(succeeded);
    CHECK_FALSE(printer.state.print_state().has_preparing_job());
}

TEST_CASE_METHOD(ArchiveStart, "a modified .3mf plate starts as itself even with the plugin",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    // The plugin would link the copy under the archive's name, which Klipper
    // does not print.
    printer.state.set_helix_plugin_installed(true);
    drain_until_quiet();
    REQUIRE(printer.state.plugin_status_state().service_has_helix_plugin());
    manager.set_archive_plate(kArchive, 9);
    manager.set_cached_scan_result(gcode::ScanResult{}, kArchive);
    PrintPreparationManagerTestAccess::modify_and_print(manager, kArchive);
    drain_until_quiet();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 9);
    CHECK(printer.api.job_mock().modified_prints().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
}

TEST_CASE_METHOD(ArchiveStart, "a plate the printer already converted prints from its output",
                 "[print_preparation][three_mf]") {
    plant("single_plate.gcode.3mf");
    manager.set_archive_plate(kArchive, 9, "Door_plate9.gcode");
    CHECK(manager.archive_plate_for(kArchive) == 0);
    CHECK(manager.printable_path(kArchive) == "Door_plate9.gcode");
    start();

    // Nothing is fetched, extracted or uploaded: the output starts as it is.
    CHECK(printer.api.transfers_mock().download_destinations().empty());
    CHECK(printer.api.transfers_mock().path_uploads().empty());
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == "Door_plate9.gcode");
}

TEST_CASE_METHOD(ArchiveStart, "staged copies go where the printer's Klipper can print them",
                 "[print_preparation][three_mf][staging]") {
    plant("single_plate.gcode.3mf");
    const bool forgex = GENERATE(true, false);
    CAPTURE(forgex);
    REQUIRE(printer.state.profile_state().set_printer_type(
        forgex ? "FlashForge Adventurer 5M (ForgeX)" : "FlashForge Adventurer 5M", true, false,
        false));
    const std::string root = forgex ? "helix_staging/modified_" : ".helix_temp/modified_";
    manager.set_archive_plate(kArchive, 9);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].dest_path.rfind(root, 0) == 0);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
}

TEST_CASE_METHOD(ArchiveStart,
                 "the plugin's own link directory decides the staging over the database",
                 "[print_preparation][three_mf][staging]") {
    plant("single_plate.gcode.3mf");
    // A stock entry in the database, but the plugin reports a visible link
    // directory: it found the dot-directory filter on this printer itself.
    REQUIRE(printer.state.profile_state().set_printer_type("FlashForge Adventurer 5M", true, false,
                                                           false));
    printer.state.set_helix_plugin_installed(true);
    printer.state.set_helix_plugin_link_dir("helix_print");
    drain_until_quiet();
    REQUIRE(printer.state.plugin_status_state().link_dir() == "helix_print");

    manager.set_archive_plate(kArchive, 9);
    start();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].dest_path.rfind("helix_staging/modified_", 0) == 0);
    // Its prints resolve back through that directory too.
    CHECK(gcode::resolve_gcode_filename("helix_print/full/Door.gcode.3mf") == "Door.gcode.3mf");
    printer.state.set_helix_plugin_link_dir("");
    drain_until_quiet();
}

TEST_CASE("the plugin's status carries its link directory", "[print_preparation][staging][api]") {
    MockPrinter printer;
    ScopedEnv installed("HELIX_MOCK_HELIX_PLUGIN", "1");
    std::optional<HelixPluginInfo> info;
    SECTION("a visible one") {
        ScopedEnv dir("HELIX_MOCK_HELIX_PLUGIN_LINK_DIR", "helix_print");
        printer.api.job().check_helix_plugin([&info](const HelixPluginInfo& i) { info = i; },
                                             [](const MoonrakerError&) {});
        REQUIRE(info);
        CHECK(info->available);
        CHECK(info->symlink_dir == "helix_print");
    }
    SECTION("the default") {
        printer.api.job().check_helix_plugin([&info](const HelixPluginInfo& i) { info = i; },
                                             [](const MoonrakerError&) {});
        REQUIRE(info);
        CHECK(info->symlink_dir == ".helix_print");
    }
}

TEST_CASE("the mock plugin's status carries a features map", "[api][mock][plugin_feature]") {
    MockPrinter printer;
    std::optional<HelixPluginInfo> info;
    auto check = [&printer, &info]() {
        info.reset();
        printer.api.job().check_helix_plugin([&info](const HelixPluginInfo& i) { info = i; },
                                             [](const MoonrakerError&) {});
        REQUIRE(info);
        return plugin_feature_state(info->available, info->features, "convert_3mf", 1);
    };
    SECTION("installed") {
        ScopedEnv installed("HELIX_MOCK_HELIX_PLUGIN", "1");
        CHECK(check() == PluginFeatureState::Ready);
        REQUIRE(info->features.count("convert_3mf") == 1);
        CHECK(info->features.at("convert_3mf").reason.empty());
        CHECK(info->features.count("plr") == 0);
    }
    SECTION("set to enabled: False") {
        ScopedEnv disabled("HELIX_MOCK_HELIX_PLUGIN", "disabled");
        CHECK(check() == PluginFeatureState::Absent);
        CHECK_FALSE(info->features.at("convert_3mf").available);
        CHECK(info->features.at("convert_3mf").reason == "plugin_disabled");
    }
    SECTION("absent") {
        ScopedEnv absent("HELIX_MOCK_HELIX_PLUGIN", nullptr);
        CHECK(check() == PluginFeatureState::Absent);
        CHECK(info->features.empty());
    }
}

TEST_CASE("an extracted plate's archive is gone before the rewrite runs",
          "[print_preparation][three_mf][disk]") {
    const std::string dir = std::filesystem::temp_directory_path().string();
    const std::string zip = dir + "/helix_rewrite_archive_" + std::to_string(::getpid()) + ".3mf";
    const std::string plate = disk_bytes(fixture_dir() + "src/plate.gcode");
    auto copy_archive = [&zip]() {
        std::ofstream out(zip, std::ios::binary);
        out << disk_bytes(fixture_dir() + "single_plate.gcode.3mf");
    };

    SECTION("a rewrite that changes the plate") {
        copy_archive();
        bool archive_during_rewrite = true;
        const std::string out = PrintPreparationManagerTestAccess::rewrite_archive_plate(
            "Door.gcode.3mf", zip, 9, [&](const std::string&) {
                archive_during_rewrite = std::filesystem::exists(zip);
                return true;
            });
        CHECK_FALSE(archive_during_rewrite);
        REQUIRE_FALSE(out.empty());
        CHECK(disk_bytes(out) == plate);
        CHECK_FALSE(std::filesystem::exists(out + ".keep"));
        std::remove(out.c_str());
    }
    SECTION("a rewrite that changes nothing still uploads the plate") {
        copy_archive();
        const std::string out = PrintPreparationManagerTestAccess::rewrite_archive_plate(
            "Door.gcode.3mf", zip, 9, [](const std::string&) { return false; });
        REQUIRE_FALSE(out.empty());
        CHECK(disk_bytes(out) == plate);
        CHECK_FALSE(std::filesystem::exists(zip));
        std::remove(out.c_str());
    }
}

TEST_CASE_METHOD(ArchiveStart, "a reprint of an archive plate extracts it again",
                 "[print_preparation][three_mf][reprint]") {
    plant("multi_plate.gcode.3mf");
    std::optional<bool> ended;
    manager.start_archive_reprint(kArchive, 5,
                                  [&ended](bool ok, const std::string&) { ended = ok; });
    drain_until_quiet();
    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::rewritten_archive_plate(uploads[0].dest_path) == 5);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
    CHECK(printer.api.job_mock().started_prints()[0] != kArchive);
}

TEST_CASE_METHOD(ArchiveStart,
                 "Reprint of an archive plate goes through the extraction, not a direct start",
                 "[print_preparation][three_mf][reprint]") {
    plant("single_plate.gcode.3mf");
    PrintStartController controller{printer.state, &printer.api};
    controller.set_prep_manager(&manager);
    bool started = false;
    bool errored = false;
    controller.initiate_reprint(
        kArchive, "", {}, [&started] { started = true; }, [&errored] { errored = true; }, 9);
    drain_until_quiet();

    const auto& uploads = printer.api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);
    CHECK(printer.api.job_mock().started_prints()[0] == uploads[0].dest_path);
    CHECK_FALSE(errored);

    SECTION("with nothing to extract it, a plate is refused, never started as the .3mf") {
        PrintStartController bare{printer.state, &printer.api};
        bool bare_errored = false;
        bare.initiate_reprint(kArchive, "", {}, [] {}, [&bare_errored] { bare_errored = true; }, 9);
        drain_until_quiet();
        CHECK(bare_errored);
        CHECK(printer.api.job_mock().started_prints().size() == 1);
    }
    SECTION("without a plate it starts the file as it is") {
        PrintStartController plain{printer.state, &printer.api};
        plain.set_prep_manager(&manager);
        plain.initiate_reprint(kArchive, "", {}, [] {}, [] {});
        drain_until_quiet();
        REQUIRE(printer.api.job_mock().started_prints().size() == 2);
        CHECK(printer.api.job_mock().started_prints()[1] == kArchive);
    }
}

namespace {

/// Plate @p plate's G-code entry in the fixture archive @p name.
helix::zip::ZipEntry plate_entry(const char* name, int plate) {
    const std::string zip = disk_bytes(fixture_dir() + name);
    std::vector<helix::zip::ZipEntry> entries;
    helix::zip::ZipError err = helix::zip::ZipError::None;
    REQUIRE(helix::zip::parse_central_directory(reinterpret_cast<const uint8_t*>(zip.data()),
                                                zip.size(), zip.size(), entries, err));
    const auto* e = helix::three_mf::find_archive_entry(
        entries, "Metadata/plate_" + std::to_string(plate) + ".gcode");
    REQUIRE(e);
    return *e;
}

} // namespace

TEST_CASE_METHOD(ArchiveStart, "an archive's pre-print scan reads the G-code that will print",
                 "[print_preparation][three_mf][scan]") {
    plant("single_plate.gcode.3mf");
    const FileVersion version{static_cast<uint64_t>(std::filesystem::file_size(planted->path())),
                              1};
    const std::string key = PrintPreparationManager::scan_key(kArchive, "");

    SECTION("nothing named: no G-code to scan, as before") {
        manager.scan_file_for_operations(kArchive, "", "", version);
        drain_until_quiet();
        REQUIRE(manager.get_scan_result().has_value());
        CHECK_FALSE(manager.get_scan_result()->print_start.found);
        CHECK(manager.printer_stop_check_for(key).state == PrinterStopCheck::State::NotRun);
    }
    SECTION("the shown plate, read out of the archive") {
        manager.scan_file_for_operations(kArchive, "", "", version);
        drain_until_quiet();
        PrintPreparationManager::ArchiveScanSource source;
        source.plate_gcode = plate_entry("single_plate.gcode.3mf", 9);
        manager.set_archive_scan_source(kArchive, "", version, source);
        drain_until_quiet();
        REQUIRE(manager.get_scan_result().has_value());
        // START_PRINT sits at line 312 of the plate's G-code.
        CHECK(manager.get_scan_result()->print_start.found);
        CHECK(manager.printer_stop_check_for(key).state != PrinterStopCheck::State::NotRun);
    }
    SECTION("the printer's conversion, read like any G-code") {
        PlantedGcode output("Door_plate9.gcode", "", disk_bytes(fixture_dir() + "src/plate.gcode"));
        manager.scan_file_for_operations(kArchive, "", "", version);
        PrintPreparationManager::ArchiveScanSource source;
        source.plate_gcode = plate_entry("single_plate.gcode.3mf", 9);
        source.converted_output = "Door_plate9.gcode";
        manager.set_archive_scan_source(kArchive, "", version, source);
        drain_until_quiet();
        REQUIRE(manager.get_scan_result().has_value());
        CHECK(manager.get_scan_result()->print_start.found);
    }
}

TEST_CASE_METHOD(ArchiveStart, "a plate step keeps the scan of the plate now shown",
                 "[print_preparation][three_mf][scan]") {
    plant("single_plate.gcode.3mf");
    PlantedGcode first("Door_first.gcode", "", "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n");
    PlantedGcode second("Door_second.gcode", "", "G28\nPRINT_START BED=60\nG1 X0 Y0\n");
    const FileVersion version{static_cast<uint64_t>(std::filesystem::file_size(planted->path())),
                              1};
    manager.scan_file_for_operations(kArchive, "", "", version);
    drain_until_quiet();

    auto& transfers = printer.api.transfers_mock();
    transfers.mock_hold_range_reads();
    PrintPreparationManager::ArchiveScanSource source;
    source.plate_gcode = plate_entry("single_plate.gcode.3mf", 9);
    source.converted_output = "Door_first.gcode";
    manager.set_archive_scan_source(kArchive, "", version, source);
    // A step while the first plate's read is still under way.
    source.converted_output = "Door_second.gcode";
    manager.set_archive_scan_source(kArchive, "", version, source);

    transfers.mock_hold_range_reads(false);
    transfers.release_held_range_reads();
    drain_until_quiet();

    const auto& scan = manager.get_scan_result();
    REQUIRE(scan.has_value());
    CHECK(scan->print_start.found);
    CHECK_FALSE(scan->has_operation(gcode::OperationType::BED_MESH));
}

TEST_CASE("read_entry_head inflates just the start of a plate", "[three_mf][reader]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    ArchiveTransfers transfers{client, ""};
    transfers.put("a.gcode.3mf", disk_bytes(fixture_dir() + "single_plate.gcode.3mf"));
    const std::string plate = disk_bytes(fixture_dir() + "src/plate.gcode");
    const auto entry = plate_entry("single_plate.gcode.3mf", 9);
    for (size_t n : {size_t{1000}, size_t{1} << 20}) {
        std::string got;
        helix::three_mf::ThreeMfReader::read_entry_head(
            transfers, "a.gcode.3mf", entry, n, [&got](const std::string& b) { got = b; },
            [](const std::string&) {});
        CHECK(got == plate.substr(0, n));
    }
    CHECK(transfers.ranges == 2);
}

TEST_CASE("read_entry_head refuses bytes it cannot inflate as the entry",
          "[three_mf][reader][scan]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    ArchiveTransfers transfers{client, ""};
    std::string zip = disk_bytes(fixture_dir() + "single_plate.gcode.3mf");
    const auto entry = plate_entry("single_plate.gcode.3mf", 9);
    REQUIRE(entry.method == 8);
    const size_t lh = static_cast<size_t>(entry.lh_offset);
    const size_t extra = static_cast<uint8_t>(zip[lh + 28]) |
                         static_cast<size_t>(static_cast<uint8_t>(zip[lh + 29])) << 8;
    const size_t data = lh + helix::zip::kZipLocalHeaderBytes + entry.name.size() + extra;
    // Every byte asked for, so a whole entry is checked as it ends.
    size_t max_bytes = static_cast<size_t>(entry.usize) + 1;

    SECTION("a local extra field longer than the read allows for") {
        // The entry's own bytes, intact, behind a longer extra field.
        const size_t grown = extra + helix::zip::kZipLocalExtraSlack + 500;
        zip[lh + 28] = static_cast<char>(grown & 0xff);
        zip[lh + 29] = static_cast<char>(grown >> 8);
        zip.insert(data, grown - extra, '\0');
        // Just the head, which nothing at the entry's end checks.
        max_bytes = 8000;
    }
    SECTION("compressed bytes that inflate to other G-code") {
        // One flipped bit that still inflates, to the entry's length: only
        // its checksum tells.
        zip[data + 6000] = static_cast<char>(zip[data + 6000] ^ 0x01);
    }
    transfers.put("a.gcode.3mf", zip);
    bool done = false;
    std::string error;
    helix::three_mf::ThreeMfReader::read_entry_head(
        transfers, "a.gcode.3mf", entry, max_bytes, [&done](const std::string&) { done = true; },
        [&error](const std::string& e) { error = e; });
    CHECK_FALSE(done);
    CHECK_FALSE(error.empty());
}

TEST_CASE("a cancelled read_entry_head answers nothing", "[three_mf][reader][scan]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    ArchiveTransfers transfers{client, ""};
    transfers.put("a.gcode.3mf", disk_bytes(fixture_dir() + "single_plate.gcode.3mf"));
    const auto entry = plate_entry("single_plate.gcode.3mf", 9);
    const bool fail = GENERATE(false, true);
    transfers.hold_from = 1;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    bool answered = false;
    helix::three_mf::ThreeMfReader::read_entry_head(
        transfers, "a.gcode.3mf", entry, 1000, [&answered](const std::string&) { answered = true; },
        [&answered](const std::string&) { answered = true; }, cancelled);
    cancelled->store(true);
    transfers.release_held(fail);
    CHECK_FALSE(answered);
}

TEST_CASE_METHOD(ArchiveStart, "a reprint before Print Files is built runs the pre-print step",
                 "[print_status][reprint]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&printer.api, nullptr);
    helix::PlantedGcode file("reprint_fallback_t0t2.gcode", "",
                             "; filament_type = PLA;PLA;PETG\n"
                             "G28\nT0\nT2\n"
                             "; filament used [g] = 1.50, 0.00, 2.25\n");
    printer.client.clear_gcode_script_history();

    PrintStatusPanel panel{printer.state, &printer.api};
    panel.init_subjects();
    helix::ui::PrintStartController controller(printer.state, &printer.api);
    PrintStatusPanelTestAccess::reprint_through(panel, controller,
                                                helix::gcode::ReprintTarget{file.name(), 0});
    drain_until_quiet();
    REQUIRE(printer.api.job_mock().started_prints().size() == 1);

    // The pre-print step named the file's heads before the start.
    bool used_heads = false;
    for (const auto& script : printer.client.gcode_script_history()) {
        used_heads = used_heads ||
                     script.find("SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2") != std::string::npos;
    }
    CHECK(used_heads);
    panel.deinit_subjects();
}

TEST_CASE_METHOD(ArchiveStart, "a plate reprint before Print Files is built is refused",
                 "[print_status][reprint][three_mf]") {
    PrintStatusPanel panel{printer.state, &printer.api};
    panel.init_subjects();
    helix::ui::PrintStartController controller(printer.state, &printer.api);
    PrintStatusPanelTestAccess::reprint_through(panel, controller,
                                                helix::gcode::ReprintTarget{kArchive, 9});
    drain_until_quiet();
    CHECK(printer.api.job_mock().started_prints().empty());

    SECTION("a plain file still starts") {
        PrintStatusPanelTestAccess::reprint_through(panel, controller,
                                                    helix::gcode::ReprintTarget{"benchy.gcode", 0});
        drain_until_quiet();
        REQUIRE(printer.api.job_mock().started_prints().size() == 1);
        CHECK(printer.api.job_mock().started_prints()[0] == "benchy.gcode");
    }
    panel.deinit_subjects();
}
