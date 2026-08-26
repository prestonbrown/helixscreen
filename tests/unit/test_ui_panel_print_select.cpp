// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"
#include "ui_format_utils.h"
#include "ui_panel_print_select.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using helix::ui::format_filament_weight;
using helix::ui::format_file_size;
using helix::ui::format_modified_date;
using helix::ui::format_print_time;

// ============================================================================
// PrintFileData - now defined in ui_panel_print_select.h
// ============================================================================

// ============================================================================
// Sorting comparator (replicates logic from ui_panel_print_select.cpp)
// ============================================================================
// Note: PrintSelectSortColumn and PrintSelectSortDirection are now in ui_panel_print_select.h

bool compare_files(const PrintFileData& a, const PrintFileData& b, PrintSelectSortColumn column,
                   PrintSelectSortDirection direction) {
    // Directories always sort to top (users expect folders first when browsing)
    if (a.is_dir != b.is_dir) {
        return a.is_dir; // dirs always first
    }

    bool result = false;

    switch (column) {
    case PrintSelectSortColumn::FILENAME:
        result = a.filename < b.filename;
        break;
    case PrintSelectSortColumn::SIZE:
        result = a.file_size_bytes < b.file_size_bytes;
        break;
    case PrintSelectSortColumn::MODIFIED:
        result = a.modified_timestamp < b.modified_timestamp; // Older first by default (ascending)
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
PrintFileData create_test_file(const std::string& name, size_t size_bytes, int days_ago,
                               int print_mins, float filament_g) {
    PrintFileData file;
    file.filename = name;
    file.thumbnail_path = "";
    file.file_size_bytes = size_bytes;
    file.modified_timestamp = time(nullptr) - (days_ago * 86400);
    file.print_time_minutes = print_mins;
    file.filament_grams = filament_g;
    file.is_dir = false;

    file.size_str = format_file_size(size_bytes);
    file.modified_str = format_modified_date(file.modified_timestamp);
    file.print_time_str = format_print_time(print_mins);
    file.filament_str = format_filament_weight(filament_g);

    return file;
}

PrintFileData create_test_directory(const std::string& name, int days_ago = 1) {
    PrintFileData dir;
    dir.filename = name;
    dir.thumbnail_path = "";
    dir.file_size_bytes = 0;
    dir.modified_timestamp = time(nullptr) - (days_ago * 86400);
    dir.print_time_minutes = 0;
    dir.filament_grams = 0.0f;
    dir.is_dir = true;

    dir.size_str = "";
    dir.modified_str = format_modified_date(dir.modified_timestamp);
    dir.print_time_str = "";
    dir.filament_str = "";

    return dir;
}

// ============================================================================
// File Sorting Tests
// ============================================================================

TEST_CASE("Print Select: Sort by filename - ascending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by filename - descending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by file size - ascending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by file size - descending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by modified date - ascending (oldest first)", "[ui][sort]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("recent.gcode", 1024, 1, 100, 50.0f));  // 1 day ago
    files.push_back(create_test_file("oldest.gcode", 1024, 30, 100, 50.0f)); // 30 days ago
    files.push_back(create_test_file("middle.gcode", 1024, 15, 100, 50.0f)); // 15 days ago

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::MODIFIED,
                             PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files[0].filename == "oldest.gcode");
    REQUIRE(files[1].filename == "middle.gcode");
    REQUIRE(files[2].filename == "recent.gcode");
}

TEST_CASE("Print Select: Sort by modified date - descending (newest first)", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by print time - ascending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by print time - descending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by filament weight - ascending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort by filament weight - descending", "[ui][sort]") {
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

TEST_CASE("Print Select: Sort - empty file list", "[ui][sort][edge]") {
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

TEST_CASE("Print Select: Sort - single file", "[ui][sort][edge]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("only.gcode", 1024, 1, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                             PrintSelectSortDirection::ASCENDING);
    });

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].filename == "only.gcode");
}

TEST_CASE("Print Select: Sort - identical filenames", "[ui][sort][edge]") {
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

TEST_CASE("Print Select: Sort - identical file sizes", "[ui][sort][edge]") {
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

TEST_CASE("Print Select: Sort - zero values", "[ui][sort][edge]") {
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

TEST_CASE("Print Select: Sort - very large values", "[ui][sort][edge]") {
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

TEST_CASE("Print Select: Sort - case sensitivity in filenames", "[ui][sort]") {
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

TEST_CASE("Print Select: Filename - very long filename", "[ui][filename][edge]") {
    std::string long_name(300, 'a');
    long_name += ".gcode";

    PrintFileData file = create_test_file(long_name, 1024, 1, 100, 50.0f);

    REQUIRE(file.filename.length() > 250);
    REQUIRE(file.filename.find(".gcode") != std::string::npos);
}

TEST_CASE("Print Select: Filename - special characters", "[ui][filename]") {
    std::vector<std::string> special_names = {
        "file with spaces.gcode",   "file-with-dashes.gcode", "file_with_underscores.gcode",
        "file.multiple.dots.gcode", "file(with)parens.gcode", "file[with]brackets.gcode"};

    for (const auto& name : special_names) {
        PrintFileData file = create_test_file(name, 1024, 1, 100, 50.0f);
        REQUIRE(file.filename == name);
    }
}

TEST_CASE("Print Select: Filename - different extensions", "[ui][filename]") {
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

TEST_CASE("Print Select: Filename - no extension", "[ui][filename][edge]") {
    PrintFileData file = create_test_file("noextension", 1024, 1, 100, 50.0f);
    REQUIRE(file.filename == "noextension");
}

TEST_CASE("Print Select: Filename - empty filename", "[ui][filename][edge]") {
    PrintFileData file = create_test_file("", 1024, 1, 100, 50.0f);
    REQUIRE(file.filename.empty());
}

// ============================================================================
// File Metadata Tests
// ============================================================================

TEST_CASE("Print Select: Metadata - print time formatting", "[ui][metadata]") {
    SECTION("Zero minutes") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 0, 50.0f);
        REQUIRE(file.print_time_str == "0 min");
    }

    SECTION("Minutes only") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 45, 50.0f);
        REQUIRE(file.print_time_str == "45 min");
    }

    SECTION("Hours and minutes") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 125, 50.0f);
        REQUIRE(file.print_time_str == "2h 5m");
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

TEST_CASE("Print Select: Metadata - filament weight formatting", "[ui][metadata]") {
    SECTION("Zero grams") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 0.0f);
        REQUIRE(file.filament_str == "0.0 g");
    }

    SECTION("Small amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 2.5f);
        REQUIRE(file.filament_str == "2.5 g");
    }

    SECTION("Medium amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 85.0f);
        REQUIRE(file.filament_str == "85 g");
    }

    SECTION("Large amount") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 250.5f);
        REQUIRE(file.filament_str == "250 g");
    }
}

TEST_CASE("Print Select: Metadata - file size formatting", "[ui][metadata]") {
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
        PrintFileData file =
            create_test_file("test.gcode", 1024ULL * 1024 * 1024 * 3, 1, 100, 50.0f);
        REQUIRE(file.size_str == "3.00 GB");
    }
}

TEST_CASE("Print Select: Metadata - modified date formatting", "[ui][metadata]") {
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

TEST_CASE("Print Select: Large file list - 100 files", "[ui][performance]") {
    std::vector<PrintFileData> files;

    for (int i = 0; i < 100; i++) {
        std::string name = "file_" + std::to_string(i) + ".gcode";
        files.push_back(
            create_test_file(name, 1024 * (i + 1), i % 30, 60 + (i * 5), 10.0f + (i * 2)));
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

TEST_CASE("Print Select: Large file list - 500 files", "[ui][performance]") {
    std::vector<PrintFileData> files;

    for (int i = 0; i < 500; i++) {
        std::string name = "print_" + std::to_string(i) + ".gcode";
        files.push_back(create_test_file(name, 1024 * 512, i % 90, 120, 50.0f));
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

TEST_CASE("Print Select: Realistic file list - mixed content", "[ui][integration]") {
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

TEST_CASE("Print Select: Sort stability - equal elements maintain order", "[ui][stability]") {
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

// ============================================================================
// Directory Sorting Tests (directories always at top)
// ============================================================================

TEST_CASE("Print Select: Directories sort to top - ascending", "[ui][sort][directory]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("zebra.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_directory("folder_a"));
    files.push_back(create_test_file("apple.gcode", 1024, 2, 100, 50.0f));
    files.push_back(create_test_directory("folder_b"));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                             PrintSelectSortDirection::ASCENDING);
    });

    // Directories should be first, then files
    REQUIRE(files[0].is_dir == true);
    REQUIRE(files[1].is_dir == true);
    REQUIRE(files[2].is_dir == false);
    REQUIRE(files[3].is_dir == false);

    // Directories sorted among themselves
    REQUIRE(files[0].filename == "folder_a");
    REQUIRE(files[1].filename == "folder_b");

    // Files sorted among themselves
    REQUIRE(files[2].filename == "apple.gcode");
    REQUIRE(files[3].filename == "zebra.gcode");
}

TEST_CASE("Print Select: Directories sort to top - descending", "[ui][sort][directory]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("apple.gcode", 1024, 30, 100, 50.0f)); // oldest
    files.push_back(create_test_directory("old_folder", 20));
    files.push_back(create_test_file("zebra.gcode", 1024, 1, 100, 50.0f)); // newest
    files.push_back(create_test_directory("new_folder", 5));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::MODIFIED,
                             PrintSelectSortDirection::DESCENDING);
    });

    // Directories should STILL be first even with descending sort
    REQUIRE(files[0].is_dir == true);
    REQUIRE(files[1].is_dir == true);
    REQUIRE(files[2].is_dir == false);
    REQUIRE(files[3].is_dir == false);

    // Directories sorted by modified (newest first within directories)
    REQUIRE(files[0].filename == "new_folder");
    REQUIRE(files[1].filename == "old_folder");

    // Files sorted by modified (newest first within files)
    REQUIRE(files[2].filename == "zebra.gcode");
    REQUIRE(files[3].filename == "apple.gcode");
}

TEST_CASE("Print Select: Parent directory (..) sorts to top", "[ui][sort][directory]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("benchy.gcode", 1024, 1, 100, 50.0f));
    files.push_back(create_test_directory("subdir"));
    files.push_back(create_test_directory(".."));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::FILENAME,
                             PrintSelectSortDirection::ASCENDING);
    });

    // All directories first, ".." sorts before other dirs alphabetically
    REQUIRE(files[0].is_dir == true);
    REQUIRE(files[0].filename == "..");
    REQUIRE(files[1].is_dir == true);
    REQUIRE(files[1].filename == "subdir");
    REQUIRE(files[2].is_dir == false);
}

TEST_CASE("Print Select: Mixed files and directories by size", "[ui][sort][directory]") {
    std::vector<PrintFileData> files;
    files.push_back(create_test_file("large.gcode", 1024 * 1024, 1, 100, 50.0f));
    files.push_back(create_test_directory("folder"));
    files.push_back(create_test_file("small.gcode", 1024, 2, 100, 50.0f));

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return compare_files(a, b, PrintSelectSortColumn::SIZE,
                             PrintSelectSortDirection::DESCENDING);
    });

    // Directory first regardless of size sort
    REQUIRE(files[0].is_dir == true);
    REQUIRE(files[0].filename == "folder");
    // Files sorted by size descending
    REQUIRE(files[1].filename == "large.gcode");
    REQUIRE(files[2].filename == "small.gcode");
}

// ============================================================================
// Folder Type Determination Tests
// ============================================================================

TEST_CASE("Print Select: Folder type determination", "[ui][folder_type]") {
    // folder_type: 0=file, 1=directory, 2=parent directory (..)

    SECTION("Regular file has folder_type 0") {
        PrintFileData file = create_test_file("test.gcode", 1024, 1, 100, 50.0f);
        int folder_type = file.is_dir ? (file.filename == ".." ? 2 : 1) : 0;
        REQUIRE(folder_type == 0);
    }

    SECTION("Regular directory has folder_type 1") {
        PrintFileData dir = create_test_directory("subdir");
        int folder_type = dir.is_dir ? (dir.filename == ".." ? 2 : 1) : 0;
        REQUIRE(folder_type == 1);
    }

    SECTION("Parent directory has folder_type 2") {
        PrintFileData parent = create_test_directory("..");
        int folder_type = parent.is_dir ? (parent.filename == ".." ? 2 : 1) : 0;
        REQUIRE(folder_type == 2);
    }
}

// ============================================================================
// Metadata Path Construction Tests
// ============================================================================

TEST_CASE("Print Select: Metadata path construction", "[ui][metadata]") {
    // Calls the production join directly. fetch_metadata_range() and the
    // print-start path beside it build the Moonraker file path with
    // helix::gcode::join_gcode_path(current_path_, filename); these cases used to
    // restate that ternary inline, asserting their own expression instead, which
    // could not go red however the panel changed.
    using helix::gcode::join_gcode_path;

    SECTION("Root directory - no path prefix") {
        // Root is the empty string, and must NOT produce a leading slash —
        // Moonraker's virtual-SD paths are root-relative.
        REQUIRE(join_gcode_path("", "benchy.gcode") == "benchy.gcode");
    }

    SECTION("Subdirectory - path prefix added") {
        REQUIRE(join_gcode_path("usb", "flowrate_0.gcode") == "usb/flowrate_0.gcode");
    }

    SECTION("Nested subdirectory - full path constructed") {
        REQUIRE(join_gcode_path("projects/voron", "toolhead.gcode") ==
                "projects/voron/toolhead.gcode");
    }
}

TEST_CASE("thumbnail_subject_value never publishes an empty buffer",
          "[ui][print_select][thumbnail]") {
    // LVGL's lv_image_src_get_type classifies a buffer whose first byte is < 0x20
    // (e.g. an empty string) as LV_IMAGE_SRC_VARIABLE, then fails to decode it and
    // logs "lv_image_set_src: failed to get image info". The no-thumbnail sentinel
    // must therefore be nullptr, not an empty buffer (prestonbrown/helixscreen#990).

    SECTION("empty buffer -> nullptr") {
        char buf[8] = "";
        REQUIRE(thumbnail_subject_value(buf) == nullptr);
    }

    SECTION("null pointer -> nullptr") {
        REQUIRE(thumbnail_subject_value(nullptr) == nullptr);
    }

    SECTION("real thumbnail path -> same buffer (unchanged)") {
        char buf[32] = "A:/cache/thumb.png";
        REQUIRE(thumbnail_subject_value(buf) == buf);
    }
}

// ============================================================================
// Card column selection (#1208 follow-up)
//
// The rule used to walk column counts downward and take the first that fit,
// which always maximized columns and so always produced the narrowest legal
// card. choose_card_columns() keeps the count landing nearest CARD_WIDTH_TARGET.
// ============================================================================

TEST_CASE("choose_card_columns prefers the designed card width", "[print_select][layout]") {
    SECTION("800x480 gets 4 cards of 167px, not 5 of 132px") {
        // Measured from a live 800x480 run: card_view_container content width 704, gap 11.
        int cols = choose_card_columns(704, 11);
        REQUIRE(cols == 4);
        REQUIRE(card_width_for_columns(704, 11, cols) == 167);

        // 5 columns still *fits* the bounds — the old rule took it precisely because
        // it fit. Pin that we now reject it.
        int five_wide = card_width_for_columns(704, 11, 5);
        REQUIRE(five_wide >= CARD_WIDTH_MIN);
        REQUIRE(five_wide <= CARD_WIDTH_MAX);
        REQUIRE(cols != 5);
    }

    SECTION("480x272 is unchanged at 3 cards of 138px") {
        int cols = choose_card_columns(436, 11);
        REQUIRE(cols == 3);
        REQUIRE(card_width_for_columns(436, 11, cols) == 138);
    }

    SECTION("The chosen width is never beaten by another legal column count") {
        for (int width = 300; width <= 1600; width += 7) {
            int cols = choose_card_columns(width, 11);
            if (cols == 0) {
                continue;
            }

            int chosen = card_width_for_columns(width, 11, cols);
            int chosen_distance = std::abs(chosen - CARD_WIDTH_TARGET);

            for (int other = 1; other <= 10; other++) {
                int other_width = card_width_for_columns(width, 11, other);
                if (other_width < CARD_WIDTH_MIN || other_width > CARD_WIDTH_MAX) {
                    continue;
                }
                INFO("container " << width << ": chose " << cols << " cols (" << chosen
                                  << "px), alternative " << other << " cols (" << other_width
                                  << "px)");
                REQUIRE(chosen_distance <= std::abs(other_width - CARD_WIDTH_TARGET));
            }
        }
    }

    SECTION("Chosen width always lands inside the card bounds") {
        for (int width = 300; width <= 1600; width += 7) {
            int cols = choose_card_columns(width, 11);
            if (cols == 0) {
                continue;
            }
            int card = card_width_for_columns(width, 11, cols);
            INFO("container " << width << " -> " << cols << " cols, " << card << "px");
            REQUIRE(card >= CARD_WIDTH_MIN);
            REQUIRE(card <= CARD_WIDTH_MAX);
        }
    }

    SECTION("Ties keep the wider card") {
        // Contrived container where two counts sit equidistant from the target.
        // Whichever way the tie falls, it must be the lower column count.
        for (int width = 300; width <= 1600; width += 1) {
            int cols = choose_card_columns(width, 0);
            if (cols <= 1) {
                continue;
            }
            int chosen = card_width_for_columns(width, 0, cols);
            int wider = card_width_for_columns(width, 0, cols - 1);
            if (wider >= CARD_WIDTH_MIN && wider <= CARD_WIDTH_MAX) {
                INFO("container " << width << ": " << cols << " cols");
                REQUIRE(std::abs(chosen - CARD_WIDTH_TARGET) < std::abs(wider - CARD_WIDTH_TARGET));
            }
        }
    }

    SECTION("No legal column count reports zero rather than guessing") {
        REQUIRE(choose_card_columns(40, 11) == 0); // narrower than one min-width card
        REQUIRE(choose_card_columns(0, 11) == 0);
        REQUIRE(choose_card_columns(-704, 11) == 0);
        REQUIRE(choose_card_columns(704, -1) == 0);
    }
}
