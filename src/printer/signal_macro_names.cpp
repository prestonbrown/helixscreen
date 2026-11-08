// SPDX-License-Identifier: GPL-3.0-or-later

#include "signal_macro_names.h"

#include <algorithm>

namespace helix::signal_macros {

std::string hidden_name(std::string_view visible) {
    return "_" + std::string(visible);
}

std::string alias_block(const std::vector<std::string>& visible_names) {
    std::string out;
    for (auto visible : kVisibleNames) {
        if (std::find(visible_names.begin(), visible_names.end(), visible) == visible_names.end()) {
            continue;
        }
        const std::string hidden = hidden_name(visible);
        if (!out.empty()) {
            out += '\n';
        }
        out += "[gcode_macro ";
        out += visible;
        out += "]\ndescription: ";
        out += kAliasDescriptionPrefix;
        out += hidden + "\ngcode:\n    " + hidden + "\n";
    }
    return out;
}

} // namespace helix::signal_macros
