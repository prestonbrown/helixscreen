// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_path_navigator.h"

#include "core/gcode_filename.h"

namespace helix::ui {

void PrintSelectPathNavigator::navigate_to(const std::string& dirname) {
    if (current_path_.empty()) {
        current_path_ = dirname;
    } else {
        current_path_ = current_path_ + "/" + dirname;
    }
}

void PrintSelectPathNavigator::navigate_up() {
    if (current_path_.empty()) {
        return; // Already at root
    }

    // A path with no slash is one level deep, so its parent is the root.
    current_path_ = gcode_dir_of(current_path_);
}

} // namespace helix::ui
