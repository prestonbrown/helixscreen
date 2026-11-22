// SPDX-License-Identifier: GPL-3.0-or-later
#include "timelapse_frame_check.h"

#include "core/text_io.h"
#include "gcode_ops_detector.h"
#include "macro_param_cache.h"

#include <cctype>
#include <cstdlib>

namespace helix::timelapse {

namespace {

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(s[i])) !=
            std::toupper(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

/// A slicer's layer-change comment: PrusaSlicer, SuperSlicer and OrcaSlicer
/// write ";LAYER_CHANGE", Orca and Bambu "; CHANGE_LAYER", Cura and ideaMaker
/// ";LAYER:<n>", Simplify3D "; layer <n>, Z = ...".
bool is_layer_marker(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || line[i] != ';') {
        return false;
    }
    ++i;
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    const std::string_view rest = line.substr(i);
    if (starts_with_ci(rest, "LAYER_CHANGE") || starts_with_ci(rest, "CHANGE_LAYER")) {
        return true;
    }
    if (starts_with_ci(rest, "LAYER:")) {
        return rest.size() > 6 && std::isdigit(static_cast<unsigned char>(rest[6]));
    }
    if (starts_with_ci(rest, "layer ")) {
        return rest.size() > 6 && std::isdigit(static_cast<unsigned char>(rest[6]));
    }
    return false;
}

/// A move that extrudes: G1 (or G0/G2/G3) with an E word above zero.
bool extrudes(std::string_view line) {
    const std::string_view word = gcode::GCodeOpsDetector::command_word(line);
    if (word.size() != 2 || std::toupper(static_cast<unsigned char>(word[0])) != 'G' ||
        word[1] < '0' || word[1] > '3') {
        return false;
    }
    const size_t comment = line.find(';');
    const std::string_view code = line.substr(0, comment);
    for (size_t i = word.size(); i + 1 < code.size(); ++i) {
        if ((code[i] == 'E' || code[i] == 'e') && (code[i - 1] == ' ' || code[i - 1] == '\t')) {
            const std::string number(code.substr(i + 1, 16));
            return std::strtod(number.c_str(), nullptr) > 0.0;
        }
    }
    return false;
}

} // namespace

FrameCalls frame_calls_in(std::string_view head, bool truncated,
                          const std::set<std::string>& frame_commands) {
    if (frame_commands.empty()) {
        return FrameCalls::Unknown;
    }
    if (truncated) {
        const size_t last_newline = head.rfind('\n');
        head = last_newline == std::string_view::npos ? std::string_view{}
                                                      : head.substr(0, last_newline);
    }

    int markers = 0;
    bool covers_a_layer_change = false;
    size_t line_start = 0;
    while (line_start < head.size()) {
        size_t line_end = head.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = head.size();
        }
        const std::string_view line = head.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        const std::string_view word = gcode::GCodeOpsDetector::command_word(line);
        if (!word.empty() && frame_commands.count(text_io::to_upper(std::string(word))) > 0) {
            return FrameCalls::Present;
        }
        if (is_layer_marker(line)) {
            ++markers;
        } else if (markers >= 2 && extrudes(line)) {
            covers_a_layer_change = true;
        }
    }
    return covers_a_layer_change ? FrameCalls::Missing : FrameCalls::Unknown;
}

std::set<std::string> frame_taking_commands() {
    auto& macros = MacroParamCache::instance();
    if (!macros.is_populated()) {
        return {};
    }
    std::set<std::string> commands = macros.macros_running(kTakeFrameCommand);
    commands.insert(kTakeFrameCommand);
    return commands;
}

} // namespace helix::timelapse
