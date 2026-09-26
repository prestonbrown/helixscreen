// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_file_sorter.h"

#include "print_file_data.h"

#include "../catch_amalgamated.hpp"

using helix::ui::PrintSelectFileSorter;
using helix::ui::SortColumn;
using helix::ui::SortDirection;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Create a file entry for testing
 */
static PrintFileData make_file(const std::string& name, size_t size, time_t modified,
                               int print_time, float filament) {
    PrintFileData file;
    file.filename = name;
    file.file_size_bytes = size;
    file.modified_timestamp = modified;
    file.print_time_minutes = print_time;
    file.filament_grams = filament;
    file.is_dir = false;
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
    dir.modified_timestamp = 0;
    dir.print_time_minutes = 0;
    dir.filament_grams = 0.0f;
    return dir;
}

// ============================================================================
// Sorting by Filename Tests
// ============================================================================

TEST_CASE("[FileSorter] Sorting by filename ascending", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("zebra.gcode", 1000, 100, 60, 10.0f),
        make_file("apple.gcode", 2000, 200, 120, 20.0f),
        make_file("mango.gcode", 3000, 300, 90, 15.0f),
    };

    sorter.sort_by(SortColumn::FILENAME);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    REQUIRE(files[0].filename == "apple.gcode");
    REQUIRE(files[1].filename == "mango.gcode");
    REQUIRE(files[2].filename == "zebra.gcode");
}

TEST_CASE("[FileSorter] Sorting by filename descending", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("zebra.gcode", 1000, 100, 60, 10.0f),
        make_file("apple.gcode", 2000, 200, 120, 20.0f),
        make_file("mango.gcode", 3000, 300, 90, 15.0f),
    };

    // First click: ascending
    sorter.sort_by(SortColumn::FILENAME);
    // Second click: toggle to descending
    sorter.sort_by(SortColumn::FILENAME);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);
    REQUIRE(files[0].filename == "zebra.gcode");
    REQUIRE(files[1].filename == "mango.gcode");
    REQUIRE(files[2].filename == "apple.gcode");
}

// ============================================================================
// Sorting by Size Tests
// ============================================================================

TEST_CASE("[FileSorter] Sorting by size", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("medium.gcode", 5000, 100, 60, 10.0f),
        make_file("small.gcode", 1000, 200, 120, 20.0f),
        make_file("large.gcode", 10000, 300, 90, 15.0f),
    };

    sorter.sort_by(SortColumn::SIZE);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::SIZE);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    // Smallest to largest
    REQUIRE(files[0].filename == "small.gcode");
    REQUIRE(files[1].filename == "medium.gcode");
    REQUIRE(files[2].filename == "large.gcode");
}

// ============================================================================
// Sorting by Modified Date Tests
// ============================================================================

TEST_CASE("[FileSorter] Sorting by modified date", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("recent.gcode", 1000, 3000, 60, 10.0f),
        make_file("oldest.gcode", 2000, 1000, 120, 20.0f),
        make_file("middle.gcode", 3000, 2000, 90, 15.0f),
    };

    sorter.sort_by(SortColumn::MODIFIED);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::MODIFIED);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    // Oldest to newest
    REQUIRE(files[0].filename == "oldest.gcode");
    REQUIRE(files[1].filename == "middle.gcode");
    REQUIRE(files[2].filename == "recent.gcode");
}

// ============================================================================
// Sorting by Print Time Tests
// ============================================================================

TEST_CASE("[FileSorter] Sorting by print time", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("medium_time.gcode", 1000, 100, 120, 10.0f),
        make_file("short_time.gcode", 2000, 200, 30, 20.0f),
        make_file("long_time.gcode", 3000, 300, 240, 15.0f),
    };

    sorter.sort_by(SortColumn::PRINT_TIME);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::PRINT_TIME);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    // Shortest to longest
    REQUIRE(files[0].filename == "short_time.gcode");
    REQUIRE(files[1].filename == "medium_time.gcode");
    REQUIRE(files[2].filename == "long_time.gcode");
}

// ============================================================================
// Sorting by Filament Tests
// ============================================================================

TEST_CASE("[FileSorter] Sorting by filament", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("medium_filament.gcode", 1000, 100, 60, 50.0f),
        make_file("low_filament.gcode", 2000, 200, 120, 10.0f),
        make_file("high_filament.gcode", 3000, 300, 90, 100.0f),
    };

    sorter.sort_by(SortColumn::FILAMENT);
    sorter.apply_sort(files);

    REQUIRE(sorter.current_column() == SortColumn::FILAMENT);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    // Least to most filament
    REQUIRE(files[0].filename == "low_filament.gcode");
    REQUIRE(files[1].filename == "medium_filament.gcode");
    REQUIRE(files[2].filename == "high_filament.gcode");
}

// ============================================================================
// Directories First Tests
// ============================================================================

TEST_CASE("[FileSorter] Directories always first", "[FileSorter]") {
    PrintSelectFileSorter sorter;

    SECTION("directories first when sorting by filename ascending") {
        std::vector<PrintFileData> files = {
            make_file("zebra.gcode", 1000, 100, 60, 10.0f),
            make_dir("aaa_folder"),
            make_file("apple.gcode", 2000, 200, 120, 20.0f),
            make_dir("zzz_folder"),
        };

        sorter.sort_by(SortColumn::FILENAME);
        sorter.apply_sort(files);

        // Directories first (sorted among themselves), then files (sorted among themselves)
        REQUIRE(files[0].is_dir == true);
        REQUIRE(files[0].filename == "aaa_folder");
        REQUIRE(files[1].is_dir == true);
        REQUIRE(files[1].filename == "zzz_folder");
        REQUIRE(files[2].is_dir == false);
        REQUIRE(files[2].filename == "apple.gcode");
        REQUIRE(files[3].is_dir == false);
        REQUIRE(files[3].filename == "zebra.gcode");
    }

    SECTION("directories first when sorting by size") {
        std::vector<PrintFileData> files = {
            make_file("small.gcode", 100, 100, 60, 10.0f),
            make_dir("folder_b"),
            make_file("large.gcode", 9999, 200, 120, 20.0f),
            make_dir("folder_a"),
        };

        sorter.sort_by(SortColumn::SIZE);
        sorter.apply_sort(files);

        // Directories first, then files sorted by size
        REQUIRE(files[0].is_dir == true);
        REQUIRE(files[1].is_dir == true);
        REQUIRE(files[2].is_dir == false);
        REQUIRE(files[2].filename == "small.gcode");
        REQUIRE(files[3].is_dir == false);
        REQUIRE(files[3].filename == "large.gcode");
    }

    SECTION("directories first when sorting descending") {
        std::vector<PrintFileData> files = {
            make_file("apple.gcode", 1000, 100, 60, 10.0f),
            make_dir("folder"),
            make_file("zebra.gcode", 2000, 200, 120, 20.0f),
        };

        sorter.sort_by(SortColumn::FILENAME);
        sorter.sort_by(SortColumn::FILENAME); // Toggle to descending
        sorter.apply_sort(files);

        REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);
        // Directory still first even in descending
        REQUIRE(files[0].is_dir == true);
        REQUIRE(files[0].filename == "folder");
        // Files in descending order
        REQUIRE(files[1].filename == "zebra.gcode");
        REQUIRE(files[2].filename == "apple.gcode");
    }
}

// ============================================================================
// Parent Directory ".." Always First Tests
// ============================================================================

TEST_CASE("[FileSorter] Parent directory '..' always sorts first", "[FileSorter]") {
    // NOTE: Default sort is MODIFIED DESCENDING. All directories get
    // modified_timestamp=0 from make_directory(), so the comparator must
    // handle equal values correctly (strict weak ordering) AND pin ".."
    // regardless of sort direction.

    SECTION("'..' first with default sort (modified descending) - the real-world case") {
        // This is what actually happens: fresh sorter, directories with timestamp=0
        PrintSelectFileSorter sorter;
        std::vector<PrintFileData> files = {
            make_dir("jpeg"),       make_dir("cloud"),
            make_dir(".."),         make_dir("video"),
            make_dir("jpeg-video"), make_file("benchy.gcode", 5000, 3000, 60, 10.0f),
        };

        // No sort_by() call - use the default (MODIFIED DESCENDING)
        REQUIRE(sorter.current_column() == SortColumn::MODIFIED);
        REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);
        sorter.apply_sort(files);

        REQUIRE(files[0].filename == "..");
        // All directories before the file
        for (size_t i = 0; i < 5; i++) {
            REQUIRE(files[i].is_dir);
        }
        REQUIRE(files[5].filename == "benchy.gcode");
    }

    SECTION("'..' first when sorting by filename descending") {
        PrintSelectFileSorter sorter;
        std::vector<PrintFileData> files = {
            make_dir("aaa_folder"),
            make_dir(".."),
            make_dir("zzz_folder"),
            make_file("test.gcode", 1000, 100, 60, 10.0f),
        };

        sorter.sort_by(SortColumn::FILENAME);
        sorter.sort_by(SortColumn::FILENAME); // Toggle to descending
        sorter.apply_sort(files);

        REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);
        REQUIRE(files[0].filename == "..");
        REQUIRE(files[1].filename == "zzz_folder");
        REQUIRE(files[2].filename == "aaa_folder");
        REQUIRE(files[3].filename == "test.gcode");
    }

    SECTION("'..' first across all sort columns") {
        // Test every column in both directions
        SortColumn columns[] = {SortColumn::FILENAME, SortColumn::SIZE, SortColumn::MODIFIED,
                                SortColumn::PRINT_TIME, SortColumn::FILAMENT};

        for (auto col : columns) {
            for (int desc = 0; desc < 2; desc++) {
                PrintSelectFileSorter sorter;
                std::vector<PrintFileData> files = {
                    make_dir("folder_b"),
                    make_dir(".."),
                    make_dir("folder_a"),
                    make_file("test.gcode", 1000, 100, 60, 10.0f),
                };

                sorter.sort_by(col);
                if (desc)
                    sorter.sort_by(col); // Toggle to descending
                sorter.apply_sort(files);

                INFO("Column: " << static_cast<int>(col)
                                << " Direction: " << (desc ? "DESC" : "ASC"));
                REQUIRE(files[0].filename == "..");
            }
        }
    }
}

// ============================================================================
// Toggle Sort Direction Tests
// ============================================================================

TEST_CASE("[FileSorter] Toggle sort direction", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("b.gcode", 2000, 200, 60, 10.0f),
        make_file("a.gcode", 1000, 100, 120, 20.0f),
        make_file("c.gcode", 3000, 300, 90, 15.0f),
    };

    // First click: ascending
    sorter.sort_by(SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    sorter.apply_sort(files);
    REQUIRE(files[0].filename == "a.gcode");
    REQUIRE(files[1].filename == "b.gcode");
    REQUIRE(files[2].filename == "c.gcode");

    // Second click on same column: descending
    sorter.sort_by(SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);
    sorter.apply_sort(files);
    REQUIRE(files[0].filename == "c.gcode");
    REQUIRE(files[1].filename == "b.gcode");
    REQUIRE(files[2].filename == "a.gcode");

    // Third click on same column: back to ascending
    sorter.sort_by(SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);
    sorter.apply_sort(files);
    REQUIRE(files[0].filename == "a.gcode");
    REQUIRE(files[1].filename == "b.gcode");
    REQUIRE(files[2].filename == "c.gcode");
}

// ============================================================================
// New Column Resets Direction Tests
// ============================================================================

TEST_CASE("[FileSorter] New column resets to ascending", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("b.gcode", 3000, 200, 60, 10.0f),
        make_file("a.gcode", 1000, 100, 120, 20.0f),
        make_file("c.gcode", 2000, 300, 90, 15.0f),
    };

    // Sort by filename ascending, then toggle to descending
    sorter.sort_by(SortColumn::FILENAME);
    sorter.sort_by(SortColumn::FILENAME);
    REQUIRE(sorter.current_column() == SortColumn::FILENAME);
    REQUIRE(sorter.current_direction() == SortDirection::DESCENDING);

    // Switch to a different column - should reset to ascending
    sorter.sort_by(SortColumn::SIZE);
    REQUIRE(sorter.current_column() == SortColumn::SIZE);
    REQUIRE(sorter.current_direction() == SortDirection::ASCENDING);

    sorter.apply_sort(files);
    // Files sorted by size ascending (smallest to largest)
    REQUIRE(files[0].filename == "a.gcode"); // 1000 bytes
    REQUIRE(files[1].filename == "c.gcode"); // 2000 bytes
    REQUIRE(files[2].filename == "b.gcode"); // 3000 bytes
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("[FileSorter] Empty list handling", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files;

    sorter.sort_by(SortColumn::FILENAME);
    sorter.apply_sort(files);

    REQUIRE(files.empty());
}

TEST_CASE("[FileSorter] Single file handling", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("only.gcode", 1000, 100, 60, 10.0f),
    };

    sorter.sort_by(SortColumn::FILENAME);
    sorter.apply_sort(files);

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].filename == "only.gcode");
}

TEST_CASE("[FileSorter] Files with same values", "[FileSorter]") {
    PrintSelectFileSorter sorter;
    std::vector<PrintFileData> files = {
        make_file("b.gcode", 1000, 100, 60, 10.0f),
        make_file("a.gcode", 1000, 100, 60, 10.0f),
        make_file("c.gcode", 1000, 100, 60, 10.0f),
    };

    // When sizes are equal, sort should be stable or use secondary sort key
    sorter.sort_by(SortColumn::SIZE);
    sorter.apply_sort(files);

    // All have same size - order may vary, just verify all are present
    REQUIRE(files.size() == 3);
}

TEST_CASE("[FileSorter] Duplicate entries survive every column and direction", "[FileSorter]") {
    // Duplicate sort keys are legal input (a padded mock listing, or a server
    // that reports one file twice). A comparator that answers "a before b"
    // AND "b before a" for equal entries corrupts std::sort - 200 entries is
    // past the threshold where libstdc++ leaves quicksort partitioning for
    // insertion sort and walks off the vector.
    const auto columns = {SortColumn::FILENAME, SortColumn::SIZE, SortColumn::MODIFIED,
                          SortColumn::PRINT_TIME, SortColumn::FILAMENT};
    const auto directions = {SortDirection::ASCENDING, SortDirection::DESCENDING};

    for (auto column : columns) {
        for (auto direction : directions) {
            std::vector<PrintFileData> files;
            for (int i = 0; i < 200; i++) {
                files.push_back(make_file("same.gcode", 1000, 100, 60, 10.0f));
            }

            PrintSelectFileSorter sorter;
            sorter.set_sort(column, direction);
            sorter.apply_sort(files);

            REQUIRE(files.size() == 200);
            for (const auto& f : files) {
                REQUIRE(f.filename == "same.gcode");
            }
        }
    }
}
