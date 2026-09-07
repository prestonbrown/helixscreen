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

#include "helix-xml/src/xml/lv_xml.h"
#include "print_file_data.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

int usb_present() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "print_source_usb_present");
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

UsbDrive drive(const std::string& mount, const std::string& label) {
    return UsbDrive(mount, "/dev/" + label, label, 1024, 512);
}

} // namespace

TEST_CASE("PrintSelectUsbSource scans every mounted drive and survives losing one",
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
    REQUIRE(listed.size() == 3);
    CHECK(listed[0] == "a.gcode");
    CHECK(listed[1] == "b.gcode");
    CHECK(listed[2] == "c.gcode");

    // Pulling the first stick leaves the second one's files on the USB tab.
    backend->simulate_drive_remove("/media/usb0");
    usb_source.on_drive_removed();
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
