// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_history.h"

#include "print_file_data.h"
#include "print_history_data.h"

#include "../catch_amalgamated.hpp"

using ::PrintHistoryStats;
using helix::ui::PrintSelectHistoryIntegration;

// Helper Functions

/**
 * @brief Create a file entry for testing
 */
static PrintFileData make_file(const std::string& name, size_t size = 1000,
                               const std::string& uuid = "") {
    PrintFileData file;
    file.filename = name;
    file.file_size_bytes = size;
    file.uuid = uuid;
    file.is_dir = false;
    file.history_status = FileHistoryStatus::NEVER_PRINTED;
    file.success_count = 0;
    return file;
}

/**
 * @brief Create a directory entry for testing
 */
static PrintFileData make_dir(const std::string& name) {
    PrintFileData dir;
    dir.filename = name;
    dir.is_dir = true;
    dir.file_size_bytes = 0;
    dir.history_status = FileHistoryStatus::NEVER_PRINTED;
    dir.success_count = 0;
    return dir;
}

/**
 * @brief Create history stats for testing
 */
static PrintHistoryStats make_stats(PrintJobStatus status, int success = 0, int failure = 0,
                                    const std::string& uuid = "", size_t size = 0) {
    PrintHistoryStats stats;
    stats.last_status = status;
    stats.success_count = success;
    stats.failure_count = failure;
    stats.uuid = uuid;
    stats.size_bytes = size;
    return stats;
}

// No History Tests

TEST_CASE("[HistoryIntegration] File with no history gets NEVER_PRINTED", "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_file("test.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    // No entry for "test.gcode" in the map

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::NEVER_PRINTED);
    REQUIRE(files[0].success_count == 0);
}

// Currently Printing Tests

TEST_CASE("[HistoryIntegration] File matching current print gets CURRENTLY_PRINTING",
          "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_file("printing_now.gcode"),
        make_file("other_file.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    // Even if file has history, current print takes precedence
    stats_map["printing_now.gcode"] = make_stats(PrintJobStatus::COMPLETED, 5);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "printing_now.gcode");

    REQUIRE(files[0].history_status == FileHistoryStatus::CURRENTLY_PRINTING);
    REQUIRE(files[1].history_status == FileHistoryStatus::NEVER_PRINTED);
}

// Completed Status Tests

TEST_CASE("[HistoryIntegration] Completed file shows COMPLETED status and success_count",
          "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_file("benchy.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    stats_map["benchy.gcode"] = make_stats(PrintJobStatus::COMPLETED, 3, 1);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::COMPLETED);
    REQUIRE(files[0].success_count == 3);
}

// Failed Status Tests

TEST_CASE("[HistoryIntegration] Failed file shows FAILED status", "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_file("failed_print.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    stats_map["failed_print.gcode"] = make_stats(PrintJobStatus::ERROR, 0, 2);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::FAILED);
}

// Cancelled Status Tests

TEST_CASE("[HistoryIntegration] Cancelled file shows CANCELLED status", "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_file("cancelled_print.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    stats_map["cancelled_print.gcode"] = make_stats(PrintJobStatus::CANCELLED, 1, 0);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::CANCELLED);
}

// UUID Match Tests

TEST_CASE("[HistoryIntegration] UUID match confirms history", "[HistoryIntegration]") {
    const std::string uuid = "abc123-uuid-456";
    std::vector<PrintFileData> files = {
        make_file("renamed_file.gcode", 1000, uuid),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    // Stats have matching UUID
    stats_map["renamed_file.gcode"] = make_stats(PrintJobStatus::COMPLETED, 2, 0, uuid, 0);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::COMPLETED);
    REQUIRE(files[0].success_count == 2);
}

// Size Match Tests

TEST_CASE("[HistoryIntegration] Size match confirms history", "[HistoryIntegration]") {
    const size_t file_size = 12345;
    std::vector<PrintFileData> files = {
        make_file("myprint.gcode", file_size, ""), // No UUID
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    // Stats have no UUID but matching size
    stats_map["myprint.gcode"] = make_stats(PrintJobStatus::COMPLETED, 1, 0, "", file_size);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    REQUIRE(files[0].history_status == FileHistoryStatus::COMPLETED);
    REQUIRE(files[0].success_count == 1);
}

// Directory Handling Tests

TEST_CASE("[HistoryIntegration] Directories are skipped", "[HistoryIntegration]") {
    std::vector<PrintFileData> files = {
        make_dir("my_folder"),
        make_file("test.gcode"),
    };

    std::unordered_map<std::string, PrintHistoryStats> stats_map;
    // Even if there's stats for the directory name, it should be ignored
    stats_map["my_folder"] = make_stats(PrintJobStatus::COMPLETED, 5);
    stats_map["test.gcode"] = make_stats(PrintJobStatus::COMPLETED, 2);

    PrintSelectHistoryIntegration::merge_history_into_files(files, stats_map, "");

    // Directory should remain unchanged
    REQUIRE(files[0].is_dir == true);
    REQUIRE(files[0].history_status == FileHistoryStatus::NEVER_PRINTED);
    REQUIRE(files[0].success_count == 0);

    // File should be updated
    REQUIRE(files[1].history_status == FileHistoryStatus::COMPLETED);
    REQUIRE(files[1].success_count == 2);
}

// Extract Basename Tests

TEST_CASE("[HistoryIntegration] extract_basename strips path", "[HistoryIntegration]") {
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("path/to/file.gcode") == "file.gcode");
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("deep/nested/path/model.gcode") ==
            "model.gcode");
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("single/benchy.gcode") ==
            "benchy.gcode");
}

TEST_CASE("[HistoryIntegration] extract_basename handles no path", "[HistoryIntegration]") {
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("file.gcode") == "file.gcode");
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("benchy.gcode") == "benchy.gcode");
    REQUIRE(PrintSelectHistoryIntegration::extract_basename("model_v2.gcode") == "model_v2.gcode");
}
