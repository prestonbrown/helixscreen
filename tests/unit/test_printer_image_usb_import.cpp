// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_usb_import.cpp
 * @brief The printer image overlay lists a stick's images without walking it
 *        on the UI thread.
 */

#include "ui_nav_manager.h"
#include "ui_overlay_printer_image.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/usb_scan_wait.h"
#include "http_executor.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <filesystem>
#include <future>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

TEST_CASE_METHOD(LVGLUITestFixture, "printer image overlay walks the stick off the UI thread",
                 "[usb][usb_async][printer_image]") {
    const fs::path stick =
        fs::temp_directory_path() / ("helix_image_stick_" + std::to_string(::getpid()));
    fs::remove_all(stick);
    fs::create_directories(stick);
    REQUIRE(fs::exists("assets/images/filament_spool.png"));
    fs::copy_file("assets/images/filament_spool.png", stick / "printer.png");

    UsbManager manager(true);
    REQUIRE(manager.start());
    auto* backend = static_cast<UsbBackendMock*>(manager.get_backend());
    REQUIRE(backend != nullptr);
    backend->simulate_drive_insert(UsbDrive(stick.string(), "/dev/sda1", "STICK"));

    auto& overlay = helix::settings::get_printer_image_overlay();
    overlay.set_usb_manager(&manager);

    // Hold every fast-lane worker so a walk handed to the lane cannot finish.
    auto& lane = helix::http::HttpExecutor::fast();
    REQUIRE(lane.running());
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    for (int i = 0; i < 4; ++i) {
        lane.submit([gate] { gate.wait(); });
    }

    REQUIRE(overlay.show(test_screen()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* list = lv_obj_find_by_name(lv_screen_active(), "usb_images_list");
    REQUIRE(list != nullptr);
    CHECK(lv_obj_get_child_count(list) == 0);

    release.set_value();
    helix::test::wait_for_usb_scan([&] { return overlay.usb_scan_in_flight(); });
    CHECK(lv_obj_get_child_count(list) == 1);

    overlay.close();
    helix::ui::UpdateQueue::instance().drain();
    overlay.set_usb_manager(nullptr);
    manager.stop();
    fs::remove_all(stick);
}
