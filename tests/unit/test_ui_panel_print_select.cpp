/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "../catch_amalgamated.hpp"
#include "ui_panel_print_select.h"
#include "ui_utils.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

using Catch::Approx;

// ============================================================================
// PrintFileData test structure (mirrors internal struct)
// ============================================================================
struct PrintFileData {
    std::string filename;
    std::string thumbnail_path;
    size_t file_size_bytes;
    time_t modified_timestamp;
    int print_time_minutes;
    float filament_grams;

    std::string size_str;
    std::string modified_str;
    std::string print_time_str;
    std::string filament_str;
};

// ============================================================================
// Sorting comparator (replicates logic from ui_panel_print_select.cpp)
// ============================================================================
// Note: PrintSelectSortColumn and PrintSelectSortDirection are now in ui_panel_print_select.h

bool compare_files(const PrintFileData& a, const PrintFileData& b,
                   PrintSelectSortColumn column,
                   PrintSelectSortDirection direction) {
    bool result = false;

    switch (column) {
    case PrintSelectSortColumn::FILENAME:
        result = a.filename < b.filename;
        break;
    case PrintSelectSortColumn::SIZE:
        result = a.file_size_bytes < b.file_size_bytes;
        break;
    case PrintSelectSortColumn::MODIFIED:
        result = a.modified_timestamp > b.modified_timestamp; // Newer first by default
        break;
    case PrintSelectSortColumn::PRINT_TIME:
        result = a.print_time_minutes < b.print_time_minutes;
        break;
    case PrintSelectSortColumn::FILAMENT:
        result = a.filament_grams < b.filament_grams;
        break;
    }

    if (direction == PrintSelectSortDirection::DESCENDING) {
        result = !result;
    }

    return result;
}

// ============================================================================
// Test file creation helpers
// ============================================================================
PrintFileData create_test_file(const std::string& name, size_t size_bytes,
                               int days_ago, int print_mins, float filament_g) {
    PrintFileData file;
    file.filename = name;
    file.thumbnail_path = "A:assets/images/thumbnail-placeholder.png";
    file.file_size_bytes = size_bytes;
    file.modified_timestamp = time(nullptr) - (days_ago * 86400);
    file.print_time_minutes = print_mins;
    file.filament_grams = filament_g;

    file.size_str = format_file_size(size_bytes);
    file.modified_str = format_modified_date(file.modified_timestamp);
    file.print_time_str = format_print_time(print_mins);
    file.filament_str = format_filament_weight(filament_g);

    return file;
}

// ============================================================================
// File Sorting Tests
// ============================================================================

TEST_CASE("Print Select: Sort by filename - ascending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("zebra.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("apple.gcode", 1024, 2, 100, 50.0f));
    files.push_back(create_test_file("banana.gcode", 1024, 3, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "apple.gcode");
    REQUIRE(files[1].filename == "banana.gcode");
    REQUIRE(files[2].filename == "zebra.gcode");
}

TEST_CASE("Print Select: Sort by filename - descending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("apple.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("zebra.gcode", 1024, 2, 100, 50.0f));
    files.push_back(create_test_file("banana.gcode", 1024, 3, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files[0].filename == "zebra.gcode");
    REQUIRE(files[1].filename == "banana.gcode");
    REQUIRE(files[2].filename == "apple.gcode");
}

TEST_CASE("Print Select: Sort by file size - ascending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("large.gcode", 1024 * 1024 * 5, 1, 100, 50.0f));
    files.push_back(create_test_file("small.gcode", 1024 * 10, 2, 100, 50.0f));
    files.push_back(create_test_file("medium.gcode", 1024 * 512, 3, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::SIZE,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "small.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "large.gcode");
}

TEST_CASE("Print Select: Sort by file size - descending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("small.gcode", 1024 * 10, 1, 100, 50.0f));
    files.push_back(create_test_file("large.gcode", 1024 * 1024 * 5, 2, 100, 50.0f));
    files.push_back(create_test_file("medium.gcode", 1024 * 512, 3, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::SIZE,
                           PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files[0].filename == "large.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "small.gcode");
}

TEST_CASE("Print Select: Sort by modified date - ascending (oldest first)", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("recent.gcode", 1024, 1, 100, 50.0f));    // 1 day ago
    files.push_back(create_test_file("oldest.gcode", 1024, 30, 100, 50.0f));   // 30 days ago
    files.push_back(create_test_file("middle.gcode", 1024, 15, 100, 50.0f));   // 15 days ago

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::MODIFIED,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "oldest.gcode");
    REQUIRE(files[1].filename == "middle.gcode");
    REQUIRE(files[2].filename == "recent.gcode");
}

TEST_CASE("Print Select: Sort by modified date - descending (newest first)", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("oldest.gcode", 1024, 30, 100, 50.0f));
    files.push_back(create_test_file("recent.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("middle.gcode", 1024, 15, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::MODIFIED,
                           PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files[0].filename == "recent.gcode");
    REQUIRE(files[1].filename == "middle.gcode");
    REQUIRE(files[2].filename == "oldest.gcode");
}

TEST_CASE("Print Select: Sort by print time - ascending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("long.gcode", 1024, 1, 480, 50.0f));   // 8 hours
    files.push_back(create_test_file("short.gcode", 1024, 2, 30, 50.0f));   // 30 mins
    files.push_back(create_test_file("medium.gcode", 1024, 3, 120, 50.0f)); // 2 hours

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "short.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "long.gcode");
}

TEST_CASE("Print Select: Sort by print time - descending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("short.gcode", 1024, 1, 30, 50.0f));
    files.push_back(create_test_file("long.gcode", 1024, 2, 480, 50.0f));
    files.push_back(create_test_file("medium.gcode", 1024, 3, 120, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                           PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files[0].filename == "long.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "short.gcode");
}

TEST_CASE("Print Select: Sort by filament weight - ascending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("heavy.gcode", 1024, 1, 100, 250.0f));
    files.push_back(create_test_file("light.gcode", 1024, 2, 100, 15.0f));
    files.push_back(create_test_file("medium.gcode", 1024, 3, 100, 85.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILAMENT,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "light.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "heavy.gcode");
}

TEST_CASE("Print Select: Sort by filament weight - descending", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("light.gcode", 1024, 1, 100, 15.0f));
    files.push_back(create_test_file("heavy.gcode", 1024, 2, 100, 250.0f));
    files.push_back(create_test_file("medium.gcode", 1024, 3, 100, 85.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILAMENT,
                           PrintSelectSortDirection::DESCENDING);
    });

    REQUIRE(files[0].filename == "heavy.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "light.gcode");
}

// ============================================================================
// Edge Cases - Sorting
// ============================================================================

TEST_CASE("Print Select: Sort - empty file list", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;

    SECTION("Sort by filename") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files.empty());
    }

    SECTION("Sort by size") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::SIZE,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files.empty());
    }
}

TEST_CASE("Print Select: Sort - single file", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("only.gcode", 1024, 1, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].filename == "only.gcode");
}

TEST_CASE("Print Select: Sort - identical filenames", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("test.gcode", 2048, 1, 100, 50.0f));
    files.push_back(create_test_file("test.gcode", 1024, 2, 200, 75.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::ASCENDING);
    });

    // Order is stable, should maintain original order for equal elements
    REQUIRE(files[0].file_size_bytes == 2048);
    REQUIRE(files[1].file_size_bytes == 1024);
}

TEST_CASE("Print Select: Sort - identical file sizes", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("zebra.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("apple.gcode", 1024, 2, 200, 75.0f));
    files.push_back(create_test_file("banana.gcode", 1024, 3, 300, 100.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::SIZE,
                           PrintSelectSortDirection::ASCENDING);
    });

    // All files same size, order is stable
    REQUIRE(files.size() == 3);
}

TEST_CASE("Print Select: Sort - zero values", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("normal.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("zero_time.gcode", 1024, 2, 0, 50.0f));
    files.push_back(create_test_file("zero_filament.gcode", 1024, 3, 100, 0.0f));

    SECTION("Sort by print time") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "zero_time.gcode");
    }

    SECTION("Sort by filament") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::FILAMENT,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "zero_filament.gcode");
    }
}

TEST_CASE("Print Select: Sort - very large values", "[print_select][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("huge.gcode", SIZE_MAX, 1, 10000, 10000.0f));
    files.push_back(create_test_file("normal.gcode", 1024, 2, 100, 50.0f));

    SECTION("Sort by size") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::SIZE,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "normal.gcode");
        REQUIRE(files[1].filename == "huge.gcode");
    }

    SECTION("Sort by print time") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "normal.gcode");
        REQUIRE(files[1].filename == "huge.gcode");
    }
}

TEST_CASE("Print Select: Sort - case sensitivity in filenames", "[print_select][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("ZEBRA.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("apple.gcode", 1024, 2, 100, 50.0f));
    files.push_back(create_test_file("Banana.gcode", 1024, 3, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::ASCENDING);
    });

    // Lexicographic ordering: uppercase letters come before lowercase in ASCII
    REQUIRE(files[0].filename == "Banana.gcode");
    REQUIRE(files[1].filename == "ZEBRA.gcode");
    REQUIRE(files[2].filename == "apple.gcode");
}

// ============================================================================
// Filename Handling Tests
// ============================================================================

TEST_CASE("Print Select: Filename - very long filename", "[print_select][filename][edge]") {
    std::string long_name(300, 'a');
    long_name += ".gcode";

    PrintFileData file = create_test_file(long_name, 1024, 1, 100, 50.0f);

    REQUIRE(file.filename.length() > 250);
    REQUIRE(file.filename.find(".gcode") != std::string::npos);
}

TEST_CASE("Print Select: Filename - special characters", "[print_select][filename]") {
    std::vector<std::string> special_names = {
        "file with spaces.gcode",
        "file-with-dashes.gcode",
        "file_with_underscores.gcode",
        "file.multiple.dots.gcode",
        "file(with)parens.gcode",
        "file[with]brackets.gcode"
    };

    for (const auto& name : special_names) {
        PrintFileData file = create_test_file(name, 1024, 1, 100, 50.0f);
        REQUIRE(file.filename == name);
    }
}

TEST_CASE("Print Select: Filename - different extensions", "[print_select][filename]") {
    SECTION(".gcode extension") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 50.0f);
        REQUIRE(file.filename.find(".gcode") != std::string::npos);
    }

    SECTION(".g extension") {
        PrintFileData file = create_test_file("test.g", 1024, 1, 100, 50.0f);
        REQUIRE(file.filename.find(".g") != std::string::npos);
    }

    SECTION(".ufp extension (UltiMaker format)") {
        PrintFileData file = create_test_file("test.ufp", 1024, 1, 100, 50.0f);
        REQUIRE(file.filename.find(".ufp") != std::string::npos);
    }

    SECTION(".3mf extension") {
        PrintFileData file = create_test_file("test.3mf", 1024, 1, 100, 50.0f);
        REQUIRE(file.filename.find(".3mf") != std::string::npos);
    }
}

TEST_CASE("Print Select: Filename - no extension", "[print_select][filename][edge]") {
    PrintFileData file = create_test_file("noextension", 1024, 1, 100, 50.0f);
    REQUIRE(file.filename == "noextension");
}

TEST_CASE("Print Select: Filename - empty filename", "[print_select][filename][edge]") {
    PrintFileData file = create_test_file("", 1024, 1, 100, 50.0f);
    REQUIRE(file.filename.empty());
}

// ============================================================================
// File Metadata Tests
// ============================================================================

TEST_CASE("Print Select: Metadata - print time formatting", "[print_select][metadata]") {
    SECTION("Zero minutes") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 0, 50.0f);
        REQUIRE(file.print_time_str == "0m");
    }

    SECTION("Minutes only") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 45, 50.0f);
        REQUIRE(file.print_time_str == "45m");
    }

    SECTION("Hours and minutes") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 125, 50.0f);
        REQUIRE(file.print_time_str == "2h5m");
    }

    SECTION("Exact hours") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 180, 50.0f);
        REQUIRE(file.print_time_str == "3h");
    }

    SECTION("Very long print") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 1440, 50.0f);
        REQUIRE(file.print_time_str == "24h");
    }
}

TEST_CASE("Print Select: Metadata - filament weight formatting", "[print_select][metadata]") {
    SECTION("Zero grams") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 0.0f);
        REQUIRE(file.filament_str == "0.0g");
    }

    SECTION("Small amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 2.5f);
        REQUIRE(file.filament_str == "2.5g");
    }

    SECTION("Medium amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 85.0f);
        REQUIRE(file.filament_str == "85g");
    }

    SECTION("Large amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 250.5f);
        REQUIRE(file.filament_str == "250g");
    }
}

TEST_CASE("Print Select: Metadata - file size formatting", "[print_select][metadata]") {
    SECTION("Bytes") {
        PrintFileData file = create_test_file("test.gcode", 512, 1, 100, 50.0f);
        REQUIRE(file.size_str == "512 B");
    }

    SECTION("Kilobytes") {
        PrintFileData file = create_test_file("test.gcode", 1024 * 128, 1, 100, 50.0f);
        REQUIRE(file.size_str == "128.0 KB");
    }

    SECTION("Megabytes") {
        PrintFileData file = create_test_file("test.gcode", 1024 * 1024 * 2, 1, 100, 50.0f);
        REQUIRE(file.size_str == "2.0 MB");
    }

    SECTION("Gigabytes") {
        PrintFileData file = create_test_file("test.gcode", 1024ULL * 1024 * 1024 * 3, 1, 100, 50.0f);
        REQUIRE(file.size_str == "3.0 GB");
    }
}

TEST_CASE("Print Select: Metadata - modified date formatting", "[print_select][metadata]") {
    time_t now = time(nullptr);

    SECTION("Recent file") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 50.0f);
        REQUIRE(!file.modified_str.empty());
    }

    SECTION("Old file") {
        PrintFileData file = create_test_file("test.gcode", 1024, 365, 100, 50.0f);
        REQUIRE(!file.modified_str.empty());
    }
}

// ============================================================================
// Large File List Tests
// ============================================================================

TEST_CASE("Print Select: Large file list - 100 files", "[print_select][performance]") {
    std::vector<PrintFileData> files;

    for (int i = 0; i < 100; i++) {
        std::string name = "file_" + std::to_string(i) + ".gcode";
        files.push_back(create_test_file(name, 1024 * (i + 1), i % 30,
                                        60 + (i * 5), 10.0f + (i * 2)));
    }

    REQUIRE(files.size() == 100);

    SECTION("Sort by filename") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "file_0.gcode");
        REQUIRE(files[99].filename == "file_99.gcode");
    }

    SECTION("Sort by size") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::SIZE,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].file_size_bytes == 1024);
        REQUIRE(files[99].file_size_bytes == 1024 * 100);
    }
}

TEST_CASE("Print Select: Large file list - 500 files", "[print_select][performance]") {
    std::vector<PrintFileData> files;

    for (int i = 0; i < 500; i++) {
        std::string name = "print_" + std::to_string(i) + ".gcode";
        files.push_back(create_test_file(name, 1024 * 512, i % 90,
                                        120, 50.0f));
    }

    REQUIRE(files.size() == 500);

    // Performance test: sorting large list should complete quickly
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                           PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files.size() == 500);
}

// ============================================================================
// Multi-criteria Sorting Tests (Realistic Scenarios)
// ============================================================================

TEST_CASE("Print Select: Realistic file list - mixed content", "[print_select][integration]") {
    std::vector<PrintFileData> files;

    // Realistic file list with various sizes and properties
    files.push_back(create_test_file("Benchy.gcode", 1024 * 512, 1, 150, 45.0f));
    files.push_back(create_test_file("Calibration_Cube.gcode", 1024 * 128, 2, 45, 12.0f));
    files.push_back(create_test_file("Large_Vase.gcode", 1024 * 1024 * 2, 3, 300, 85.0f));
    files.push_back(create_test_file("Keychain.gcode", 1024 * 64, 10, 30, 8.0f));

    SECTION("Sort by print time finds quickest print") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "Keychain.gcode");
        REQUIRE(files[0].print_time_minutes == 30);
    }

    SECTION("Sort by filament finds most efficient") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::FILAMENT,
                               PrintSelectSortDirection::ASCENDING);
        });
        REQUIRE(files[0].filename == "Keychain.gcode");
        REQUIRE(files[0].filament_grams == Approx(8.0f));
    }

    SECTION("Sort by modified date finds newest") {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return compare_files(a, b, PrintSelectSortColumn::MODIFIED,
                               PrintSelectSortDirection::DESCENDING);
        });
        REQUIRE(files[0].filename == "Benchy.gcode");
    }
}

// ============================================================================
// Stability Tests
// ============================================================================

TEST_CASE("Print Select: Sort stability - equal elements maintain order", "[print_select][stability]") {
    std::vector<PrintFileData> files;

    // Create files with same sort key but different filenames
    files.push_back(create_test_file("first.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_file("second.gcode", 1024, 2, 100, 50.0f));
    files.push_back(create_test_file("third.gcode", 1024, 3, 100, 50.0f));

    // Sort by print time (all equal)
    std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::PRINT_TIME,
                           PrintSelectSortDirection::ASCENDING);
    });

    // Stable sort should maintain original order
    REQUIRE(files[0].filename == "first.gcode");
    REQUIRE(files[1].filename == "second.gcode");
    REQUIRE(files[2].filename == "third.gcode");
}
