// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_metadata.cpp
 * @brief Unit tests for PrintFileData metadata_fetched field integrity
 *
 * Tests that the metadata_fetched field travels correctly with file data
 * during sorting, copying, and vector operations. This prevents the bug
 * where parallel arrays (file_list_ and metadata_fetched_) got out of sync.
 *
 * The fix moved metadata_fetched INTO the PrintFileData struct so it
 * travels with the file during all operations.
 */

#include "ui_format_utils.h"
#include "ui_panel_print_select.h"

#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::format_filament_weight;
using helix::ui::format_file_size;
using helix::ui::format_modified_date;
using helix::ui::format_print_time;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Create a test file with specified metadata_fetched state
 */
static PrintFileData create_test_file_with_metadata(const std::string& name, time_t modified,
                                                    bool metadata_fetched) {
    PrintFileData file;
    file.filename = name;
    file.thumbnail_path = "";
    file.file_size_bytes = 1024;
    file.modified_timestamp = modified;
    file.print_time_minutes = 100;
    file.filament_grams = 50.0f;
    file.metadata_fetched = metadata_fetched;

    file.size_str = format_file_size(file.file_size_bytes);
    file.modified_str = format_modified_date(file.modified_timestamp);
    file.print_time_str = format_print_time(file.print_time_minutes);
    file.filament_str = format_filament_weight(file.filament_grams);

    return file;
}

/**
 * @brief Sorting comparator (replicates logic from ui_panel_print_select.cpp)
 */
static bool compare_files_by_modified(const PrintFileData& a, const PrintFileData& b,
                                      PrintSelectSortDirection direction) {
    bool result = a.modified_timestamp < b.modified_timestamp;
    if (direction == PrintSelectSortDirection::DESCENDING) {
        result = !result;
    }
    return result;
}

// ============================================================================
// PrintFileData Struct Tests
// ============================================================================

TEST_CASE("PrintFileData: metadata_fetched defaults to false", "[ui][metadata]") {
    PrintFileData file;
    REQUIRE(file.metadata_fetched == false);
}

TEST_CASE("PrintFileData: metadata_fetched can be set to true", "[ui][metadata]") {
    PrintFileData file;
    file.metadata_fetched = true;
    REQUIRE(file.metadata_fetched == true);
}

TEST_CASE("PrintFileData: copy constructor preserves metadata_fetched", "[ui][metadata]") {
    PrintFileData original;
    original.filename = "test.gcode";
    original.metadata_fetched = true;

    PrintFileData copy = original;

    REQUIRE(copy.metadata_fetched == true);
    REQUIRE(copy.filename == "test.gcode");
}

TEST_CASE("PrintFileData: copy assignment preserves metadata_fetched", "[ui][metadata]") {
    PrintFileData original;
    original.filename = "test.gcode";
    original.metadata_fetched = true;

    PrintFileData copy;
    copy = original;

    REQUIRE(copy.metadata_fetched == true);
    REQUIRE(copy.filename == "test.gcode");
}

TEST_CASE("PrintFileData: move constructor preserves metadata_fetched", "[ui][metadata]") {
    PrintFileData original;
    original.filename = "test.gcode";
    original.metadata_fetched = true;

    PrintFileData moved = std::move(original);

    REQUIRE(moved.metadata_fetched == true);
    REQUIRE(moved.filename == "test.gcode");
}

TEST_CASE("PrintFileData: move assignment preserves metadata_fetched", "[ui][metadata]") {
    PrintFileData original;
    original.filename = "test.gcode";
    original.metadata_fetched = true;

    PrintFileData moved;
    moved = std::move(original);

    REQUIRE(moved.metadata_fetched == true);
    REQUIRE(moved.filename == "test.gcode");
}

// ============================================================================
// Vector Operations Tests
// ============================================================================

TEST_CASE("PrintFileData: push_back preserves metadata_fetched", "[ui][metadata][vector]") {
    std::vector<PrintFileData> files;

    PrintFileData fetched;
    fetched.filename = "fetched.gcode";
    fetched.metadata_fetched = true;

    PrintFileData not_fetched;
    not_fetched.filename = "not_fetched.gcode";
    not_fetched.metadata_fetched = false;

    files.push_back(fetched);
    files.push_back(not_fetched);

    REQUIRE(files[0].metadata_fetched == true);
    REQUIRE(files[0].filename == "fetched.gcode");
    REQUIRE(files[1].metadata_fetched == false);
    REQUIRE(files[1].filename == "not_fetched.gcode");
}

TEST_CASE("PrintFileData: emplace_back preserves metadata_fetched", "[ui][metadata][vector]") {
    std::vector<PrintFileData> files;

    files.emplace_back();
    files.back().filename = "emplace_test.gcode";
    files.back().metadata_fetched = true;

    REQUIRE(files[0].metadata_fetched == true);
    REQUIRE(files[0].filename == "emplace_test.gcode");
}

TEST_CASE("PrintFileData: std::swap preserves metadata_fetched", "[ui][metadata][vector]") {
    PrintFileData file_a;
    file_a.filename = "file_a.gcode";
    file_a.metadata_fetched = true;

    PrintFileData file_b;
    file_b.filename = "file_b.gcode";
    file_b.metadata_fetched = false;

    std::swap(file_a, file_b);

    REQUIRE(file_a.filename == "file_b.gcode");
    REQUIRE(file_a.metadata_fetched == false);
    REQUIRE(file_b.filename == "file_a.gcode");
    REQUIRE(file_b.metadata_fetched == true);
}

// ============================================================================
// Sorting Scenario Tests - The Exact Bug Scenario
// ============================================================================

TEST_CASE("Sorting: metadata_fetched travels with file during sort", "[ui][metadata][sort]") {
    // This is THE bug scenario:
    // 1. Have existing files with metadata_fetched = true
    // 2. Add a new file with metadata_fetched = false (newest modified date)
    // 3. Sort by modified date descending (newest first)
    // 4. The new file moves to index 0, but must keep metadata_fetched = false

    time_t now = time(nullptr);

    std::vector<PrintFileData> files;

    // Existing files with metadata already fetched (older dates)
    files.push_back(create_test_file_with_metadata("old_file.gcode", now - 86400 * 10, true));
    files.push_back(create_test_file_with_metadata("older_file.gcode", now - 86400 * 20, true));
    files.push_back(create_test_file_with_metadata("oldest_file.gcode", now - 86400 * 30, true));

    // New file just uploaded - metadata NOT fetched yet (newest date)
    files.push_back(create_test_file_with_metadata("new_file.gcode", now, false));

    // Sort by modified date descending (newest first) - this is the default view
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    // Verify: new_file.gcode should be at index 0 (newest first)
    REQUIRE(files[0].filename == "new_file.gcode");
    // CRITICAL: metadata_fetched must still be false - this was the bug!
    REQUIRE(files[0].metadata_fetched == false);

    // Verify: old files are in correct order with correct metadata_fetched state
    REQUIRE(files[1].filename == "old_file.gcode");
    REQUIRE(files[1].metadata_fetched == true);

    REQUIRE(files[2].filename == "older_file.gcode");
    REQUIRE(files[2].metadata_fetched == true);

    REQUIRE(files[3].filename == "oldest_file.gcode");
    REQUIRE(files[3].metadata_fetched == true);
}

TEST_CASE("Sorting: mixed metadata_fetched states remain correct after sort",
          "[ui][metadata][sort]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;

    // Create files with alternating metadata_fetched states and different dates
    files.push_back(create_test_file_with_metadata("file_1.gcode", now - 86400 * 5, true));
    files.push_back(create_test_file_with_metadata("file_2.gcode", now - 86400 * 3, false));
    files.push_back(create_test_file_with_metadata("file_3.gcode", now - 86400 * 7, true));
    files.push_back(create_test_file_with_metadata("file_4.gcode", now - 86400 * 1, false));
    files.push_back(create_test_file_with_metadata("file_5.gcode", now - 86400 * 9, true));

    // Sort by modified date descending (newest first)
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    // Expected order after sort (newest first): file_4, file_2, file_1, file_3, file_5
    // Each file's metadata_fetched state should be preserved

    REQUIRE(files[0].filename == "file_4.gcode"); // 1 day ago
    REQUIRE(files[0].metadata_fetched == false);

    REQUIRE(files[1].filename == "file_2.gcode"); // 3 days ago
    REQUIRE(files[1].metadata_fetched == false);

    REQUIRE(files[2].filename == "file_1.gcode"); // 5 days ago
    REQUIRE(files[2].metadata_fetched == true);

    REQUIRE(files[3].filename == "file_3.gcode"); // 7 days ago
    REQUIRE(files[3].metadata_fetched == true);

    REQUIRE(files[4].filename == "file_5.gcode"); // 9 days ago
    REQUIRE(files[4].metadata_fetched == true);
}

TEST_CASE("Sorting: ascending sort preserves metadata_fetched", "[ui][metadata][sort]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;

    files.push_back(create_test_file_with_metadata("new.gcode", now, false));
    files.push_back(create_test_file_with_metadata("old.gcode", now - 86400 * 30, true));

    // Sort by modified date ascending (oldest first)
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "old.gcode");
    REQUIRE(files[0].metadata_fetched == true);

    REQUIRE(files[1].filename == "new.gcode");
    REQUIRE(files[1].metadata_fetched == false);
}

TEST_CASE("Sorting: multiple sorts preserve metadata_fetched", "[ui][metadata][sort]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;

    files.push_back(create_test_file_with_metadata("file_a.gcode", now - 86400 * 1, false));
    files.push_back(create_test_file_with_metadata("file_b.gcode", now - 86400 * 2, true));
    files.push_back(create_test_file_with_metadata("file_c.gcode", now - 86400 * 3, false));

    // Sort descending
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    // Verify after first sort
    REQUIRE(files[0].filename == "file_a.gcode");
    REQUIRE(files[0].metadata_fetched == false);

    // Sort ascending
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::ASCENDING);
    });

    // Verify after second sort
    REQUIRE(files[0].filename == "file_c.gcode");
    REQUIRE(files[0].metadata_fetched == false);
    REQUIRE(files[1].filename == "file_b.gcode");
    REQUIRE(files[1].metadata_fetched == true);
    REQUIRE(files[2].filename == "file_a.gcode");
    REQUIRE(files[2].metadata_fetched == false);

    // Sort descending again
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    // Verify after third sort - back to original order
    REQUIRE(files[0].filename == "file_a.gcode");
    REQUIRE(files[0].metadata_fetched == false);
    REQUIRE(files[1].filename == "file_b.gcode");
    REQUIRE(files[1].metadata_fetched == true);
    REQUIRE(files[2].filename == "file_c.gcode");
    REQUIRE(files[2].metadata_fetched == false);
}

// ============================================================================
// File Provider Callback Scenario Tests
// ============================================================================

TEST_CASE("File provider: new files have metadata_fetched = false", "[ui][metadata][provider]") {
    // Simulates what PrintSelectFileProvider does for new files
    PrintFileData new_file;
    new_file.filename = "new_upload.gcode";
    new_file.is_dir = false;
    new_file.file_size_bytes = 1024 * 512;
    new_file.modified_timestamp = time(nullptr);
    new_file.metadata_fetched = false; // New files need metadata fetch

    REQUIRE(new_file.metadata_fetched == false);
}

TEST_CASE("File provider: preserved files keep metadata_fetched = true",
          "[ui][metadata][provider]") {
    // Simulates preserving existing file data when file is unchanged
    PrintFileData existing_file;
    existing_file.filename = "existing.gcode";
    existing_file.modified_timestamp = time(nullptr) - 86400;
    existing_file.metadata_fetched = true; // Already fetched

    // Simulate preservation (copy to new list)
    PrintFileData preserved = existing_file;

    REQUIRE(preserved.metadata_fetched == true);
}

TEST_CASE("File provider: modified files reset metadata_fetched to false",
          "[ui][metadata][provider]") {
    // When a file is modified (re-uploaded with same name), metadata must be re-fetched
    PrintFileData existing_file;
    existing_file.filename = "modified.gcode";
    existing_file.modified_timestamp = time(nullptr) - 86400;
    existing_file.metadata_fetched = true;

    // File was modified - create new entry with reset metadata
    PrintFileData modified_file;
    modified_file.filename = "modified.gcode";
    modified_file.modified_timestamp = time(nullptr); // Newer timestamp
    modified_file.metadata_fetched = false;           // Must re-fetch

    REQUIRE(modified_file.metadata_fetched == false);
}

TEST_CASE("File provider: directories have metadata_fetched = true", "[ui][metadata][provider]") {
    // Directories don't need metadata fetch - they're pre-populated
    PrintFileData dir;
    dir.filename = "subdir";
    dir.is_dir = true;
    dir.metadata_fetched = true; // Directories are always "done"

    REQUIRE(dir.metadata_fetched == true);
}

TEST_CASE("File provider: parent directory (..) has metadata_fetched = true",
          "[ui][metadata][provider]") {
    // Parent directory entry never needs metadata
    PrintFileData parent_dir;
    parent_dir.filename = "..";
    parent_dir.is_dir = true;
    parent_dir.metadata_fetched = true;

    REQUIRE(parent_dir.metadata_fetched == true);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("Sorting: empty vector doesn't crash", "[ui][metadata][sort][edge]") {
    std::vector<PrintFileData> files;

    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files.empty());
}

TEST_CASE("Sorting: single file preserves metadata_fetched", "[ui][metadata][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file_with_metadata("only.gcode", time(nullptr), false));

    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].filename == "only.gcode");
    REQUIRE(files[0].metadata_fetched == false);
}

TEST_CASE("Sorting: all files fetched preserves state", "[ui][metadata][sort][edge]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;
    files.push_back(create_test_file_with_metadata("a.gcode", now - 86400 * 1, true));
    files.push_back(create_test_file_with_metadata("b.gcode", now - 86400 * 2, true));
    files.push_back(create_test_file_with_metadata("c.gcode", now - 86400 * 3, true));

    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    for (const auto& file : files) {
        REQUIRE(file.metadata_fetched == true);
    }
}

TEST_CASE("Sorting: all files not fetched preserves state", "[ui][metadata][sort][edge]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;
    files.push_back(create_test_file_with_metadata("a.gcode", now - 86400 * 1, false));
    files.push_back(create_test_file_with_metadata("b.gcode", now - 86400 * 2, false));
    files.push_back(create_test_file_with_metadata("c.gcode", now - 86400 * 3, false));

    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    for (const auto& file : files) {
        REQUIRE(file.metadata_fetched == false);
    }
}

// ============================================================================
// Large List Performance / Integrity Tests
// ============================================================================

TEST_CASE("Sorting: large list preserves metadata_fetched integrity", "[ui][metadata][sort]") {
    time_t now = time(nullptr);

    std::vector<PrintFileData> files;

    // Create 100 files with alternating metadata_fetched states
    for (int i = 0; i < 100; i++) {
        std::string name = "file_" + std::to_string(i) + ".gcode";
        time_t modified = now - (86400 * i); // Each file 1 day older
        bool fetched = (i % 2 == 0);         // Even indices are fetched

        files.push_back(create_test_file_with_metadata(name, modified, fetched));
    }

    // Sort by modified date descending
    std::sort(files.begin(), files.end(), [](const PrintFileData& a, const PrintFileData& b) {
        return compare_files_by_modified(a, b, PrintSelectSortDirection::DESCENDING);
    });

    // Verify: file_0 should be first (newest), and all metadata_fetched states correct
    REQUIRE(files[0].filename == "file_0.gcode");
    REQUIRE(files[0].metadata_fetched == true); // Index 0 was even

    // Verify all files maintain correct metadata_fetched based on original index
    for (int i = 0; i < 100; i++) {
        std::string expected_name = "file_" + std::to_string(i) + ".gcode";
        bool expected_fetched = (i % 2 == 0);

        REQUIRE(files[i].filename == expected_name);
        REQUIRE(files[i].metadata_fetched == expected_fetched);
    }
}

TEST_CASE("Object height carries 'tall' only when there is a height", "[ui][metadata][qidi_3mf]") {
    using helix::ui::format_print_height;
    CHECK(format_print_height(42.0, true) == std::string("42 mm ") + lv_tr("tall"));
    CHECK(format_print_height(0.0, true) == helix::format::UNAVAILABLE);
    CHECK(format_print_height(42.0) == "42 mm");
}

TEST_CASE("Layer height reads as unavailable when unknown", "[ui][metadata][qidi_3mf]") {
    using helix::ui::format_layer_height;
    CHECK(format_layer_height(0.2) == "0.20 mm");
    CHECK(format_layer_height(0.0) == helix::format::UNAVAILABLE);
}
