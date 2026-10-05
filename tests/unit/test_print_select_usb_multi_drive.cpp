// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_usb_multi_drive.cpp
 * @brief The USB print source lists every mounted drive, not only the first.
 *
 * Run with: ./build/bin/helix-tests "[usb][multi_drive]"
 *
 * A second stick is a drive the user plugged in on purpose; a source that only
 * ever scanned drives[0] made it invisible with no message, and a removal event
 * dropped the whole source even when the other stick was still mounted
 * (prestonbrown/helixscreen#1373).
 */

#include "ui_print_select_usb_source.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/usb_scan_wait.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_file_data.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

int usb_present() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "print_source_usb_present");
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

UsbDrive drive(const std::string& mount, const std::string& label) {
    return UsbDrive(mount, "/dev/" + label, label);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrintSelectUsbSource scans every mounted drive and survives losing one",
                 "[usb][multi_drive]") {
    helix::ui::PrintSelectUsbSource::init_subjects();

    UsbManager manager(true); // force mock
    REQUIRE(manager.start());
    auto* backend = static_cast<UsbBackendMock*>(manager.get_backend());
    REQUIRE(backend != nullptr);

    backend->simulate_drive_insert(drive("/media/usb0", "FIRST"));
    backend->simulate_drive_insert(drive("/media/usb1", "SECOND"));
    backend->set_mock_files("/media/usb0", {{"/media/usb0/a.gcode", "a.gcode", 100, 1000}});
    backend->set_mock_files("/media/usb1", {{"/media/usb1/b.gcode", "b.gcode", 100, 1000},
                                            {"/media/usb1/c.gcode", "c.gcode", 100, 1000}});

    helix::ui::PrintSelectUsbSource usb_source;
    std::vector<std::string> listed;
    usb_source.set_on_files_ready([&](std::vector<PrintFileData>&& files) {
        listed.clear();
        for (const auto& f : files) {
            listed.push_back(f.filename);
        }
    });
    usb_source.set_usb_manager(&manager);
    REQUIRE(usb_present() == 1);

    usb_source.select_usb_source();
    helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
    REQUIRE(listed.size() == 3);
    CHECK(listed[0] == "a.gcode");
    CHECK(listed[1] == "b.gcode");
    CHECK(listed[2] == "c.gcode");

    // Pulling the first stick leaves the second one's files on the USB tab.
    backend->simulate_drive_remove("/media/usb0");
    usb_source.on_drive_removed();
    helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
    CHECK(usb_source.get_current_source() == FileSource::USB);
    CHECK(usb_present() == 1);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0] == "b.gcode");

    // Pulling the last one is what retires the source.
    backend->simulate_drive_remove("/media/usb1");
    usb_source.on_drive_removed();
    CHECK(usb_source.get_current_source() == FileSource::PRINTER);
    CHECK(usb_present() == 0);

    manager.stop();
}

TEST_CASE_METHOD(LVGLTestFixture, "USB thumbnails are cached per file path, not per filename",
                 "[usb][thumbnail]") {
    helix::ui::PrintSelectUsbSource::init_subjects();

    // Two sticks (or two folders on one) routinely hold a same-named file
    // with different models in it; each card must keep its own thumbnail.
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("helix_usb_thumb_key_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "a");
    fs::create_directories(root / "b");
    const fs::path src = "assets/test_gcodes/xyz-10mm-calibration-cube.gcode";
    REQUIRE(fs::exists(src));
    fs::copy_file(src, root / "a" / "part.gcode");
    fs::copy_file(src, root / "b" / "part.gcode");

    UsbManager manager(true);
    REQUIRE(manager.start());
    auto* backend = static_cast<UsbBackendMock*>(manager.get_backend());
    REQUIRE(backend != nullptr);
    backend->simulate_drive_insert(drive(root.string(), "STICK"));
    backend->set_mock_files(root.string(),
                            {{(root / "a" / "part.gcode").string(), "part.gcode", 100, 1000},
                             {(root / "b" / "part.gcode").string(), "part.gcode", 100, 1000}});

    helix::ui::PrintSelectUsbSource usb_source;
    std::vector<std::string> thumbs;
    usb_source.set_on_files_ready([&](std::vector<PrintFileData>&& files) {
        thumbs.clear();
        for (const auto& f : files) {
            thumbs.push_back(f.thumbnail_path);
        }
    });
    usb_source.set_usb_manager(&manager);
    usb_source.select_usb_source();
    helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });

    REQUIRE(thumbs.size() == 2);
    CHECK_FALSE(thumbs[0].empty());
    CHECK_FALSE(thumbs[1].empty());
    CHECK(thumbs[0] != thumbs[1]);

    manager.stop();
    fs::remove_all(root);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrintSelectUsbSource walks the stick off the UI thread",
                 "[usb][usb_async]") {
    helix::ui::PrintSelectUsbSource::init_subjects();

    UsbManager manager(true);
    REQUIRE(manager.start());
    auto* backend = static_cast<UsbBackendMock*>(manager.get_backend());
    REQUIRE(backend != nullptr);
    backend->simulate_drive_insert(drive("/media/usb0", "FIRST"));
    backend->set_mock_files("/media/usb0", {{"/media/usb0/a.gcode", "a.gcode", 100, 1000}});

    helix::ui::PrintSelectUsbSource usb_source;
    int deliveries = 0;
    usb_source.set_on_files_ready([&](std::vector<PrintFileData>&&) { ++deliveries; });
    usb_source.set_usb_manager(&manager);

    SECTION("the list arrives through the UI queue, not inside the call") {
        usb_source.select_usb_source();
        CHECK(deliveries == 0);
        helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
        CHECK(deliveries == 1);
    }

    SECTION("a scan that lands after a switch back to Printer is dropped") {
        usb_source.select_usb_source();
        usb_source.select_printer_source();
        helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
        CHECK(deliveries == 0);
    }

    SECTION("only the newest of two overlapping refreshes delivers") {
        usb_source.select_usb_source();
        usb_source.refresh_files();
        helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
        CHECK(deliveries == 1);
    }

    SECTION("refreshes during a walk do not each start a walk") {
        usb_source.select_usb_source();
        usb_source.refresh_files();
        usb_source.refresh_files();
        usb_source.refresh_files();
        helix::test::wait_for_usb_scan([&] { return usb_source.is_scanning(); });
        // The walk in flight, then one more for everything that came during it.
        CHECK(backend->scan_count() <= 2);
        CHECK(deliveries == 1);
    }

    manager.stop();
}

TEST_CASE("scan_usb_drives stops as soon as it is cancelled", "[usb][usb_async]") {
    UsbManager manager(true);
    REQUIRE(manager.start());
    auto* backend = static_cast<UsbBackendMock*>(manager.get_backend());
    REQUIRE(backend != nullptr);
    backend->simulate_drive_insert(drive("/media/usb0", "FIRST"));
    backend->simulate_drive_insert(drive("/media/usb1", "SECOND"));
    backend->set_mock_files("/media/usb0", {{"/media/usb0/a.gcode", "a.gcode", 100, 1000}});
    backend->set_mock_files("/media/usb1", {{"/media/usb1/b.gcode", "b.gcode", 100, 1000}});
    std::vector<UsbDrive> drives;
    REQUIRE(backend->get_connected_drives(drives).success());
    REQUIRE(drives.size() == 2);

    SECTION("cancelled before it starts, it reads nothing") {
        auto scan = helix::ui::scan_usb_drives(*backend, drives, [] { return true; });
        CHECK(backend->scan_count() == 0);
        CHECK(scan.files.empty());
    }

    SECTION("cancelled after the first drive, it skips the second") {
        int polls = 0;
        auto scan = helix::ui::scan_usb_drives(*backend, drives, [&] { return ++polls > 1; });
        CHECK(backend->scan_count() == 1);
        CHECK(scan.thumbnails.empty());
    }

    manager.stop();
}
