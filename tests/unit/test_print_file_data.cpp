// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_types.h"
#include "print_file_data.h"
#include "usb_backend.h"

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

    SECTION("formats display strings") {
        REQUIRE(data.size_str == "1.0 MB");
        REQUIRE_FALSE(data.modified_str.empty());
        REQUIRE(data.print_time_str == "0 min");
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

    SECTION("formats size and date strings") {
        REQUIRE(data.size_str == "512.0 KB");
        REQUIRE_FALSE(data.modified_str.empty());
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
        REQUIRE(data.modified_str.empty());
        REQUIRE(data.print_time_str.empty());
        REQUIRE(data.filament_str.empty());
    }
}
