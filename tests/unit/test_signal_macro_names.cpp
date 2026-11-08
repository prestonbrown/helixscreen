// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_signal_macro_names.cpp
 * @brief The signal macro names and the forwarders that keep a visible name callable.
 */

#include "macro_manager.h"
#include "signal_macro_names.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace sm = helix::signal_macros;

namespace {

/// Names of every [gcode_macro _HELIX_*] section in column 0 of the content.
std::vector<std::string> hidden_sections(const std::string& content) {
    std::vector<std::string> names;
    std::istringstream in(content);
    std::string line;
    const std::string open = "[gcode_macro _HELIX_";
    while (std::getline(in, line)) {
        if (line.rfind(open, 0) == 0 && line.back() == ']') {
            names.push_back(line.substr(13, line.size() - 14));
        }
    }
    return names;
}

} // namespace

TEST_CASE("kVisibleNames lists every signal macro the bundled pack defines", "[signal_macros]") {
    const std::string pack = helix::MacroManager::get_macro_content();
    REQUIRE_FALSE(pack.empty());

    std::vector<std::string> pack_signals;
    for (const auto& name : hidden_sections(pack)) {
        if (name != "_HELIX_STATE") {
            pack_signals.push_back(name);
        }
    }

    std::vector<std::string> listed;
    for (auto visible : sm::kVisibleNames) {
        listed.push_back(sm::hidden_name(visible));
    }

    std::sort(pack_signals.begin(), pack_signals.end());
    std::sort(listed.begin(), listed.end());
    CHECK(pack_signals == listed);
}

TEST_CASE("alias_block forwards one visible name to its hidden macro", "[signal_macros]") {
    CHECK(sm::alias_block({"HELIX_READY"}) == "[gcode_macro HELIX_READY]\n"
                                              "description: HelixScreen alias for _HELIX_READY\n"
                                              "gcode:\n"
                                              "    _HELIX_READY\n");
}

TEST_CASE("alias_block writes pack order and drops unknown names", "[signal_macros]") {
    CHECK(sm::alias_block({"HELIX_PHASE_QGL", "NOT_A_SIGNAL", "HELIX_ENDED"}) ==
          "[gcode_macro HELIX_ENDED]\n"
          "description: HelixScreen alias for _HELIX_ENDED\n"
          "gcode:\n"
          "    _HELIX_ENDED\n"
          "\n"
          "[gcode_macro HELIX_PHASE_QGL]\n"
          "description: HelixScreen alias for _HELIX_PHASE_QGL\n"
          "gcode:\n"
          "    _HELIX_PHASE_QGL\n");
}

TEST_CASE("alias_block of no names is empty", "[signal_macros]") {
    CHECK(sm::alias_block({}).empty());
    CHECK(sm::alias_block({"HELIX_START_PRINT"}).empty());
}
