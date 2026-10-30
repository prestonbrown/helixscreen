// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_preset_slots.h"

#include "device_display_name.h"
#include "lvgl.h"

#include <spdlog/fmt/fmt.h>

namespace helix {

namespace {

constexpr const char* kPresetsKey = "presets";

struct KindToken {
    ToolheadPresetSlot::Kind kind;
    const char* token;
};

constexpr KindToken kKindTokens[] = {
    {ToolheadPresetSlot::Kind::Empty, "empty"},  {ToolheadPresetSlot::Kind::Position, "position"},
    {ToolheadPresetSlot::Kind::Park, "park"},    {ToolheadPresetSlot::Kind::ZUp, "z_up"},
    {ToolheadPresetSlot::Kind::ZDown, "z_down"}, {ToolheadPresetSlot::Kind::Macro, "macro"},
};

const char* kind_token(ToolheadPresetSlot::Kind kind) {
    for (const auto& k : kKindTokens) {
        if (k.kind == kind)
            return k.token;
    }
    return "empty";
}

/// The string at @p key, or empty when it is missing or not a string.
std::string string_field(const nlohmann::json& entry, const char* key) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

ToolheadPresetSlot slot_from_entry(const nlohmann::json& entry) {
    ToolheadPresetSlot slot;
    if (!entry.is_object())
        return slot;
    const std::string kind = string_field(entry, "kind");
    for (const auto& k : kKindTokens) {
        if (kind == k.token)
            slot.kind = k.kind;
    }
    switch (slot.kind) {
    case ToolheadPresetSlot::Kind::Position: {
        const auto preset = motion_preset_from_token(string_field(entry, "position"));
        if (preset)
            slot.position = *preset;
        else
            slot.kind = ToolheadPresetSlot::Kind::Empty;
        break;
    }
    case ToolheadPresetSlot::Kind::Macro:
        slot.macro = string_field(entry, "macro");
        if (slot.macro.empty())
            slot.kind = ToolheadPresetSlot::Kind::Empty;
        break;
    default:
        break;
    }
    return slot;
}

} // namespace

ToolheadPresetSlots default_toolhead_preset_slots() {
    ToolheadPresetSlots slots;
    slots[0].kind = ToolheadPresetSlot::Kind::Position;
    slots[0].position = MotionPreset::Front;
    slots[1].kind = ToolheadPresetSlot::Kind::Position;
    slots[1].position = MotionPreset::Center;
    slots[2].kind = ToolheadPresetSlot::Kind::Park;
    return slots;
}

ToolheadPresetSlots toolhead_preset_slots_from_config(const nlohmann::json& config) {
    if (!config.is_object())
        return default_toolhead_preset_slots();
    const auto it = config.find(kPresetsKey);
    if (it == config.end() || !it->is_array())
        return default_toolhead_preset_slots();
    ToolheadPresetSlots slots;
    for (std::size_t i = 0; i < slots.size() && i < it->size(); ++i)
        slots[i] = slot_from_entry((*it)[i]);
    return slots;
}

void toolhead_preset_slots_to_config(const ToolheadPresetSlots& slots, nlohmann::json& config) {
    if (!config.is_object())
        config = nlohmann::json::object();
    nlohmann::json array = nlohmann::json::array();
    for (const auto& slot : slots) {
        nlohmann::json entry = {{"kind", kind_token(slot.kind)}};
        if (slot.kind == ToolheadPresetSlot::Kind::Position)
            entry["position"] = motion_preset_token(slot.position);
        else if (slot.kind == ToolheadPresetSlot::Kind::Macro)
            entry["macro"] = slot.macro;
        array.push_back(std::move(entry));
    }
    config[kPresetsKey] = std::move(array);
}

bool toolhead_slot_moves_toolhead(const ToolheadPresetSlot& slot) {
    switch (slot.kind) {
    case ToolheadPresetSlot::Kind::Position:
    case ToolheadPresetSlot::Kind::Park:
    case ToolheadPresetSlot::Kind::ZUp:
    case ToolheadPresetSlot::Kind::ZDown:
        return true;
    case ToolheadPresetSlot::Kind::Empty:
    case ToolheadPresetSlot::Kind::Macro:
        return false;
    }
    return false;
}

std::string toolhead_preset_slot_label(const ToolheadPresetSlot& slot) {
    switch (slot.kind) {
    case ToolheadPresetSlot::Kind::Empty:
        return {};
    case ToolheadPresetSlot::Kind::Park:
        return lv_tr("Park");
    case ToolheadPresetSlot::Kind::ZUp:
        return fmt::format("Z +{:g}", kToolheadZStepMm);
    case ToolheadPresetSlot::Kind::ZDown:
        return fmt::format("Z -{:g}", kToolheadZStepMm);
    case ToolheadPresetSlot::Kind::Macro:
        return get_display_name(slot.macro, DeviceType::MACRO);
    case ToolheadPresetSlot::Kind::Position:
        break;
    }
    // Literal lv_tr calls: the translation extractor only sees string literals.
    switch (slot.position) {
    case MotionPreset::RearLeft:
        return lv_tr("Rear Left");
    case MotionPreset::Rear:
        return lv_tr("Rear");
    case MotionPreset::RearRight:
        return lv_tr("Rear Right");
    case MotionPreset::Left:
        return lv_tr("Left");
    case MotionPreset::Center:
        return lv_tr("Center");
    case MotionPreset::Right:
        return lv_tr("Right");
    case MotionPreset::FrontLeft:
        return lv_tr("Front Left");
    case MotionPreset::Front:
        return lv_tr("Front");
    case MotionPreset::FrontRight:
        return lv_tr("Front Right");
    }
    return {};
}

const char* toolhead_preset_slot_icon(const ToolheadPresetSlot& slot) {
    switch (slot.kind) {
    case ToolheadPresetSlot::Kind::Empty:
        return "";
    case ToolheadPresetSlot::Kind::Park:
        return "home_import";
    case ToolheadPresetSlot::Kind::ZUp:
        return "z_farther";
    case ToolheadPresetSlot::Kind::ZDown:
        return "z_closer";
    case ToolheadPresetSlot::Kind::Macro:
        return "play";
    case ToolheadPresetSlot::Kind::Position:
        break;
    }
    switch (slot.position) {
    case MotionPreset::Rear:
        return "arrow_up";
    case MotionPreset::Front:
        return "arrow_down";
    case MotionPreset::Left:
        return "arrow_left";
    case MotionPreset::Right:
        return "arrow_right";
    case MotionPreset::RearRight:
        return "arrow_up_right";
    case MotionPreset::Center:
        return "crosshairs_gps";
    case MotionPreset::RearLeft:
    case MotionPreset::FrontLeft:
    case MotionPreset::FrontRight:
        return "cursor_move";
    }
    return "";
}

} // namespace helix
