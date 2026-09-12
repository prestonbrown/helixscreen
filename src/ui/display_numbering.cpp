// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_numbering.h"

#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"

namespace helix::ui {

namespace {
/// U+00B7 MIDDLE DOT, the separator between a unit name and its position.
constexpr const char* kUnitSeparator = "\xc2\xb7";
} // namespace

std::string tool_label(int gcode_tool) {
    if (gcode_tool < 0)
        return {};
    return "T" + std::to_string(gcode_tool);
}

int lane_number(int index) {
    if (index < 0)
        return -1;
    return index + 1;
}

std::string lane_number_text(int index) {
    const int n = lane_number(index);
    if (n < 0)
        return {};
    return std::to_string(n);
}

std::string noun_text(LaneNoun noun) {
    switch (noun) {
    case LaneNoun::Lane:
        return lv_tr("Lane");
    case LaneNoun::Gate:
        return lv_tr("Gate");
    case LaneNoun::Tool:
        return lv_tr("Tool");
    case LaneNoun::Feeder:
        return lv_tr("Feeder");
    case LaneNoun::Toolhead:
        return lv_tr("Toolhead");
    case LaneNoun::Slot:
        break;
    }
    return lv_tr("Slot");
}

std::string lane_label(LaneNoun noun, int index) {
    const int n = lane_number(index);
    if (n < 0)
        return {};
    return noun_text(noun) + " " + std::to_string(n);
}

std::string lane_label(LaneNoun noun, std::string_view unit_display_name, int index) {
    if (unit_display_name.empty())
        return lane_label(noun, index);
    const std::string body = lane_label(noun, index);
    if (body.empty())
        return {};
    return std::string(unit_display_name) + " " + kUnitSeparator + " " + body;
}

LaneNoun active_lane_noun() {
    auto& ams = AmsState::instance();
    const auto* backend = ams.get_backend(ams.active_backend_index());
    return backend ? backend->lane_noun() : LaneNoun::Slot;
}

LaneNoun active_tool_noun() {
    auto& ams = AmsState::instance();
    const auto* backend = ams.get_backend(ams.active_backend_index());
    return backend ? backend->tool_noun() : LaneNoun::Tool;
}

} // namespace helix::ui
