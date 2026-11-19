// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "display_settings_manager.h"
#include "lvgl.h"
#include "moonraker_types.h"
#include "print_file_data.h"
#include "translation_loader.h"
#include "usb_backend.h"

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

// ============================================================================
// PrintFileData::from_moonraker_file() Tests
// ============================================================================

TEST_CASE("from_moonraker_file creates PrintFileData from FileInfo", "[print_file_data]") {
    FileInfo file;
    file.filename = "test_model.gcode";
    file.is_dir = false;
    file.size = 1024 * 1024; // 1 MB
    file.modified = 1735000000.0;

    PrintFileData data = PrintFileData::from_moonraker_file(file, "A:/placeholder.bin");

    SECTION("copies basic fields correctly") {
        REQUIRE(data.filename == "test_model.gcode");
        REQUIRE(data.is_dir == false);
        REQUIRE(data.file_size_bytes == 1024 * 1024);
        REQUIRE(data.modified_timestamp == 1735000000);
        REQUIRE(data.thumbnail_path == "A:/placeholder.bin");
    }

    SECTION("initializes metadata fields to defaults") {
        REQUIRE(data.print_time_minutes == 0);
        REQUIRE(data.filament_grams == 0.0f);
        REQUIRE(data.metadata_fetched == false);
    }

    SECTION("formats only the display strings that do not translate") {
        REQUIRE(data.size_str == "1.0 MB");
        REQUIRE(data.print_time_str.empty());
        REQUIRE(data.filament_str == "0.0 g");
    }

    SECTION("initializes optional fields to empty") {
        REQUIRE(data.layer_count_str.empty());
        REQUIRE(data.print_height_str.empty());
        REQUIRE(data.original_thumbnail_url.empty());
    }
}

TEST_CASE("from_moonraker_file handles directory", "[print_file_data]") {
    FileInfo dir;
    dir.filename = "subfolder";
    dir.is_dir = true;
    dir.size = 0;
    dir.modified = 1735000000.0;

    PrintFileData data = PrintFileData::from_moonraker_file(dir, "A:/folder.bin");

    REQUIRE(data.filename == "subfolder");
    REQUIRE(data.is_dir == true);
    REQUIRE(data.thumbnail_path == "A:/folder.bin");
}

TEST_CASE("from_moonraker_file handles edge cases", "[print_file_data]") {
    SECTION("zero-size file") {
        FileInfo file;
        file.filename = "empty.gcode";
        file.is_dir = false;
        file.size = 0;
        file.modified = 0;

        PrintFileData data = PrintFileData::from_moonraker_file(file, "");

        REQUIRE(data.file_size_bytes == 0);
        REQUIRE(data.size_str == "0 B");
    }
}

// ============================================================================
// PrintFileData::from_usb_file() Tests
// ============================================================================

TEST_CASE("from_usb_file creates PrintFileData from UsbGcodeFile", "[print_file_data]") {
    UsbGcodeFile file("/mnt/usb/model.gcode", "model.gcode", 512 * 1024, 1735000000);

    PrintFileData data = PrintFileData::from_usb_file(file, "A:/usb_placeholder.bin");

    SECTION("copies basic fields correctly") {
        REQUIRE(data.filename == "model.gcode");
        REQUIRE(data.is_dir == false);
        REQUIRE(data.file_size_bytes == 512 * 1024);
        REQUIRE(data.modified_timestamp == 1735000000);
        REQUIRE(data.thumbnail_path == "A:/usb_placeholder.bin");
    }

    SECTION("formats the size") {
        REQUIRE(data.size_str == "512.0 KB");
    }

    SECTION("uses placeholder for unavailable metadata") {
        REQUIRE(data.print_time_str == "--");
        REQUIRE(data.filament_str == "--");
        REQUIRE(data.layer_count_str == "--");
        REQUIRE(data.print_height_str == "--");
    }

    SECTION("initializes other fields") {
        REQUIRE(data.print_time_minutes == 0);
        REQUIRE(data.filament_grams == 0.0f);
        REQUIRE(data.metadata_fetched == false);
        REQUIRE(data.original_thumbnail_url.empty());
    }
}

// ============================================================================
// PrintFileData::make_directory() Tests
// ============================================================================

TEST_CASE("make_directory creates directory entry", "[print_file_data]") {
    SECTION("normal directory") {
        PrintFileData data = PrintFileData::make_directory("subfolder", "A:/folder.bin", false);

        REQUIRE(data.filename == "subfolder");
        REQUIRE(data.is_dir == true);
        REQUIRE(data.thumbnail_path == "A:/folder.bin");
        REQUIRE(data.size_str == "Folder");
        REQUIRE(data.metadata_fetched == true);
    }

    SECTION("parent directory") {
        PrintFileData data = PrintFileData::make_directory("..", "A:/folder_up.bin", true);

        REQUIRE(data.filename == "..");
        REQUIRE(data.is_dir == true);
        REQUIRE(data.thumbnail_path == "A:/folder_up.bin");
        REQUIRE(data.size_str.empty()); // Parent dirs show empty size
        REQUIRE(data.metadata_fetched == true);
    }

    SECTION("directory has empty metadata fields") {
        PrintFileData data = PrintFileData::make_directory("test", "icon.bin", false);

        REQUIRE(data.file_size_bytes == 0);
        REQUIRE(data.modified_timestamp == 0);
        REQUIRE(data.print_time_minutes == 0);
        REQUIRE(data.filament_grams == 0.0f);
        REQUIRE(data.print_time_str.empty());
        REQUIRE(data.filament_str.empty());
    }
}

TEST_CASE("format_unfetched_print_times fills only files waiting for metadata",
          "[print_file_data]") {
    FileInfo info;
    info.filename = "benchy.gcode";
    std::vector<PrintFileData> files = {PrintFileData::from_moonraker_file(info, ""),
                                        PrintFileData::from_moonraker_file(info, ""),
                                        PrintFileData::make_directory("parts", "", false)};
    files[1].metadata_fetched = true;
    helix::format_unfetched_print_times(files);
    CHECK(files[0].print_time_str == "0 min");
    CHECK(files[1].print_time_str.empty()); // its metadata formats it
    CHECK(files[2].print_time_str.empty());
}

// The file provider builds entries on the WebSocket thread while the language
// and time format change on the main thread: under TSan this reports any
// setting, subject or translation state the factory reads.
TEST_CASE_METHOD(LVGLTestFixture,
                 "from_moonraker_file on a background thread races no settings change",
                 "[print_file_data][threading]") {
    auto& display = helix::DisplaySettingsManager::instance();
    const helix::TimeFormat saved_format = display.get_time_format();
    helix::ui::ensure_translation_loaded("de");
    FileInfo file;
    file.filename = "benchy.gcode";
    file.size = 1024;
    file.modified = 1733063400.0;

    std::atomic<bool> done{false};
    std::atomic<int> complete{0};
    std::thread worker([&]() {
        for (int i = 0; i < 400; ++i) {
            const PrintFileData data = PrintFileData::from_moonraker_file(file, "");
            complete += data.size_str == "1.0 KB" && data.modified_timestamp == 1733063400;
        }
        done = true;
    });
    for (int i = 0; !done; ++i) {
        lv_translation_set_language(i % 2 ? "de" : helix::ui::kIdentityLocale);
        display.set_time_format(i % 2 ? helix::TimeFormat::HOUR_24 : helix::TimeFormat::HOUR_12);
    }
    worker.join();
    lv_translation_set_language(helix::ui::kIdentityLocale);
    display.set_time_format(saved_format);
    REQUIRE(complete == 400);
}
