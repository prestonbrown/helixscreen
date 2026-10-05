// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/usb_scan_wait.h"
#include "config.h"
#include "http_executor.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#include "usb_printer_detector.h"

#include <future>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// UsbPrinterDetector static method tests (no USB hardware needed)
// ============================================================================

TEST_CASE("USB printer detector", "[label-printer][usb-detect]") {
    SECTION("known printer table includes Phomemo M110") {
        auto known = UsbPrinterDetector::known_printers();
        bool found = false;
        for (const auto& p : known) {
            if (p.vid == 0x0493 && p.pid == 0x8760) {
                found = true;
                REQUIRE(p.name == "Phomemo M110");
            }
        }
        REQUIRE(found);
    }

    SECTION("is_known_printer matches Phomemo VID:PID") {
        REQUIRE(UsbPrinterDetector::is_known_printer(0x0493, 0x8760));
    }

    SECTION("is_known_printer rejects unknown VID:PID") {
        REQUIRE_FALSE(UsbPrinterDetector::is_known_printer(0x1234, 0x5678));
    }

    SECTION("get_printer_name returns name for known device") {
        auto name = UsbPrinterDetector::get_printer_name(0x0493, 0x8760);
        REQUIRE(name == "Phomemo M110");
    }

    SECTION("get_printer_name returns empty for unknown device") {
        auto name = UsbPrinterDetector::get_printer_name(0x1234, 0x5678);
        REQUIRE(name.empty());
    }

    SECTION("known printer table is not empty") {
        REQUIRE_FALSE(UsbPrinterDetector::known_printers().empty());
    }

    SECTION("default state is not polling") {
        UsbPrinterDetector detector;
        REQUIRE_FALSE(detector.is_polling());
    }
}

TEST_CASE("USB printer detector ignores the generic STM32 CDC-ACM id",
          "[label-printer][usb-detect]") {
    // 0483:5740 is ST's stock virtual COM port id, shared by unrelated boards.
    // A CDC-ACM device gets a tty, never the /dev/usb/lpN node printing needs.
    CHECK_FALSE(UsbPrinterDetector::is_known_printer(0x0483, 0x5740));
}

TEST_CASE_METHOD(LVGLTestFixture, "USB printer polling enumerates the bus off the UI thread",
                 "[label-printer][usb-detect]") {
    UsbPrinterDetector detector;
    int calls = 0;

    SECTION("the first result arrives through the UI queue, not inside start_polling") {
        detector.start_polling([&](const std::vector<UsbPrinterInfo>&) { ++calls; });
        CHECK(calls == 0);
        helix::test::wait_for_usb_scan([&] { return detector.is_scanning(); });
        CHECK(calls == 1);
    }

    SECTION("a scan still running when the detector dies reports nothing") {
        {
            UsbPrinterDetector doomed;
            doomed.start_polling([&](const std::vector<UsbPrinterInfo>&) { ++calls; });
        }
        // The detector is gone, so only the lane can say the scan ended. An
        // idle lane cannot come before the scan's item finishes, and the item
        // queues its result before it does; other work only lengthens the wait.
        helix::test::wait_for_usb_scan(
            [] { return helix::http::HttpExecutor::fast().inflight() > 0; });
        CHECK(calls == 0);
    }

    SECTION("a scan still running when polling stops reports nothing") {
        detector.start_polling([&](const std::vector<UsbPrinterInfo>&) { ++calls; });
        detector.stop_polling();
        helix::test::wait_for_usb_scan([&] { return detector.is_scanning(); });
        CHECK(calls == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a USB spool label enumerates the bus off the UI thread",
                 "[label-printer][usb-detect]") {
    if (!UsbPrinterDetector::scan().empty()) {
        SKIP("a real USB label printer is attached; this test would print to it");
    }
    Config::get_instance();
    auto& settings = LabelPrinterSettingsManager::instance();
    settings.init_subjects();
    struct RestoreType {
        LabelPrinterSettingsManager& settings;
        std::string previous;
        ~RestoreType() {
            settings.set_printer_type(previous);
            settings.deinit_subjects();
        }
    } restore{settings, settings.get_printer_type()};
    settings.set_printer_type("usb");

    auto& lane = helix::http::HttpExecutor::fast();
    REQUIRE(lane.running());
    // Start from an idle lane so the count below is this test's alone.
    helix::test::wait_for_usb_scan([&] { return lane.inflight() > 0; });
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    for (int i = 0; i < 4; ++i) {
        lane.submit([open] { open.wait(); });
    }

    SpoolInfo spool;
    spool.id = 7;
    spool.material = "PLA";
    int answers = 0;
    print_spool_label(spool, [&](bool, const std::string&) { ++answers; });

    // Every worker is held, so a scan handed to the lane is still queued here.
    CHECK(lane.inflight() == 5);
    CHECK(answers == 0);

    gate.set_value();
    helix::test::wait_for_usb_scan([&] { return answers == 0; });
    CHECK(answers == 1);
}
