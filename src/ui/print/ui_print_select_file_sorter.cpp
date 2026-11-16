// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_file_sorter.h"

#include <algorithm>

namespace helix::ui {

void PrintSelectFileSorter::sort_by(SortColumn column) {
    if (column == current_column_) {
        current_direction_ = (current_direction_ == SortDirection::ASCENDING)
                                 ? SortDirection::DESCENDING
                                 : SortDirection::ASCENDING;
    } else {
        current_column_ = column;
        current_direction_ = SortDirection::ASCENDING;
    }
}

void PrintSelectFileSorter::set_sort(SortColumn column, SortDirection direction) {
    current_column_ = column;
    current_direction_ = direction;
}

void PrintSelectFileSorter::apply_sort(std::vector<PrintFileData>& files) {
    auto sort_column = current_column_;
    auto sort_direction = current_direction_;

    // Ascending lexicographic order over (column key, filename), used for
    // entries of the same kind (both directories or both files). Descending
    // reuses it with swapped arguments: negating the result instead would
    // report "a < b and b < a" for fully equal entries, which is not a
    // strict weak ordering and corrupts std::sort.
    auto ascending = [sort_column](const PrintFileData& a, const PrintFileData& b) {
        switch (sort_column) {
        case SortColumn::FILENAME:
            return a.filename < b.filename;
        case SortColumn::SIZE:
            return (a.file_size_bytes != b.file_size_bytes)
                       ? (a.file_size_bytes < b.file_size_bytes)
                       : (a.filename < b.filename);
        case SortColumn::MODIFIED:
            return (a.modified_timestamp != b.modified_timestamp)
                       ? (a.modified_timestamp < b.modified_timestamp)
                       : (a.filename < b.filename);
        case SortColumn::PRINT_TIME:
            return (a.print_time_minutes != b.print_time_minutes)
                       ? (a.print_time_minutes < b.print_time_minutes)
                       : (a.filename < b.filename);
        case SortColumn::FILAMENT:
            return (a.filament_grams != b.filament_grams) ? (a.filament_grams < b.filament_grams)
                                                          : (a.filename < b.filename);
        }
        return false;
    };

    std::sort(files.begin(), files.end(),
              [ascending, sort_direction](const PrintFileData& a, const PrintFileData& b) {
                  // Directories always sort to top, regardless of direction
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return sort_direction == SortDirection::DESCENDING ? ascending(b, a)
                                                                     : ascending(a, b);
              });

    // Pin ".." parent directory to position 0 (after sort, bulletproof)
    for (size_t i = 1; i < files.size(); i++) {
        if (files[i].is_dir && files[i].filename == "..") {
            std::rotate(files.begin(), files.begin() + i, files.begin() + i + 1);
            break;
        }
    }
}

} // namespace helix::ui
