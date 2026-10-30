// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_numbering.h"

#include "ams_state.h"
#include "i18n_plural.h"
#include "lvgl.h"

#include <spdlog/fmt/fmt.h>

#include <cstdio>

namespace helix::ui {

namespace {

/**
 * @brief The translated plural for @p noun, as a range header spells it.
 *
 * One fixed nominative plural per noun per locale, never derived from how many
 * positions the range covers: Russian would need three forms for a count and
 * agrees with neither end of a range, and a header is not a count anyway.
 */
std::string noun_text_plural(LaneNoun noun) {
    switch (noun) {
    case LaneNoun::Lane:
        return lv_tr("Lanes");
    case LaneNoun::Gate:
        return lv_tr("Gates");
    case LaneNoun::Tool:
        return lv_tr("Tools");
    case LaneNoun::Feeder:
        return lv_tr("Feeders");
    case LaneNoun::Toolhead:
        return lv_tr("Toolheads");
    case LaneNoun::Slot:
        break;
    }
    return lv_tr("Slots");
}
} // namespace

std::string tool_label(int gcode_tool) {
    if (gcode_tool < 0)
        return {};
    return "T" + std::to_string(gcode_tool);
}

bool is_generated_tool_name(std::string_view name) {
    if (name.size() < 2 || name[0] != 'T')
        return false;
    for (size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

std::string tool_name_label(LaneNoun noun, std::string_view name, int index) {
    if (index < 0)
        return {};
    if (is_generated_tool_name(name))
        return lane_label(noun, index);
    return std::string(name);
}

std::string tool_label_in_form(LaneNoun noun, std::string_view name, int index,
                               ToolLabelForm form) {
    if (index < 0)
        return {};
    const std::string code = is_generated_tool_name(name) ? std::string(name) : tool_label(index);
    if (form == ToolLabelForm::Code)
        return code;
    return fmt::format(lv_tr("{} ({})"), tool_name_label(noun, name, index), code);
}

ToolLabelForm pick_tool_label_form(const std::function<bool(ToolLabelForm)>& fits) {
    for (ToolLabelForm form : kToolLabelForms) {
        if (fits(form))
            return form;
    }
    return ToolLabelForm::Code;
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

std::string backup_lane_text(LaneNoun noun) {
    switch (noun) {
    case LaneNoun::Lane:
        return lv_tr("Backup Lane");
    case LaneNoun::Gate:
        return lv_tr("Backup Gate");
    case LaneNoun::Tool:
        return lv_tr("Backup Tool");
    case LaneNoun::Feeder:
        return lv_tr("Backup Feeder");
    case LaneNoun::Toolhead:
        return lv_tr("Backup Toolhead");
    case LaneNoun::Slot:
        break;
    }
    return lv_tr("Backup Slot");
}

std::string lane_label(LaneNoun noun, int index) {
    const int n = lane_number(index);
    if (n < 0)
        return {};
    return noun_text(noun) + " " + std::to_string(n);
}

std::string clear_spool_blocked_hint(LaneNoun noun, int index) {
    // lv_tr() returns the format string in the active locale; fmt::format
    // substitutes the label, the same composition NOTIFY_INFO uses for the
    // "{} spool cleared" toast.
    return fmt::format(lv_tr("{} is feeding the current print. Clear it after the print ends."),
                       lane_label(noun, index));
}

std::string unit_scoped_label(std::string_view unit_display_name, std::string_view body) {
    if (unit_display_name.empty() || body.empty())
        return std::string(body);
    return std::string(unit_display_name) + " " + kUnitSeparator + " " + std::string(body);
}

std::string lane_label(LaneNoun noun, std::string_view unit_display_name, int index) {
    if (unit_display_name.empty())
        return lane_label(noun, index);
    const std::string body = lane_label(noun, index);
    if (body.empty())
        return {};
    return unit_scoped_label(unit_display_name, body);
}

std::string lane_range_label(LaneNoun noun, int first_index, int last_index) {
    const int first = lane_number(first_index);
    const int last = lane_number(last_index);
    if (first < 0 || last < 0 || last < first)
        return {};
    // "Slots 3-3" is not how anyone says it, and a plural header over one
    // position is wrong in every locale that inflects.
    if (first == last)
        return lane_label(noun, first_index);
    return noun_text_plural(noun) + " " + std::to_string(first) + "-" + std::to_string(last);
}

std::string lane_count_label(LaneNoun noun, int count) {
    // Wide enough for the longest form: ru "Печатающие головки" is 35 bytes.
    // Each tr_plural() sits inside its snprintf so the format-specifier gate
    // sees the pair - a format held in a variable between the two is invisible
    // to it. No default: a new enumerator must fail to compile here, not pass a
    // null format to snprintf.
    char buf[64];
    const auto n = static_cast<uint64_t>(count);
    switch (noun) {
    case LaneNoun::Lane:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d lanes", n), count);
        return buf;
    case LaneNoun::Gate:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d gates", n), count);
        return buf;
    case LaneNoun::Tool:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d tools", n), count);
        return buf;
    case LaneNoun::Feeder:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d feeders", n), count);
        return buf;
    case LaneNoun::Toolhead:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d toolheads", n), count);
        return buf;
    case LaneNoun::Slot:
        snprintf(buf, sizeof(buf), tr_plural(nullptr, "%d slots", n), count);
        return buf;
    }
    return {};
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

std::string active_tool_label(std::string_view name, int index, ToolLabelForm form) {
    return tool_label_in_form(active_tool_noun(), name, index, form);
}

bool tool_badges_add_information(const std::vector<int>& tool_to_slot, int slot_count) {
    if (tool_to_slot.empty())
        return true;
    for (size_t tool = 0; tool < tool_to_slot.size(); ++tool) {
        const int slot = tool_to_slot[tool];
        if (slot < 0 || slot >= slot_count || slot != static_cast<int>(tool))
            return true;
    }
    return false;
}

} // namespace helix::ui
