// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_unit_hardware.h"

#include "ace_slot_parse.h"
#include "ams_status_json.h"
#include "lane_translation.h"
#include "text_io.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "hv/json.hpp"

namespace helix::afc {

bool ace_dryer_running(const std::string& status) {
    const std::string word = text_io::to_lower(status);
    static constexpr const char* kIdle[] = {"",     "stop", "stopped", "stopping",
                                            "idle", "off",  "none"};
    return std::none_of(std::begin(kIdle), std::end(kIdle),
                        [&word](const char* idle) { return word == idle; });
}

namespace {

constexpr const char* kAceKeys[] = {"ace_dryer",          "ace_dryer_target", "ace_dryer_remain",
                                    "ace_dryer_duration", "ace_temp",         "ace_humidity",
                                    "ace_slots"};

std::optional<int> read_whole_number(const nlohmann::json& data, const char* key) {
    const auto v = ams::read_field<float>(data, key);
    if (!v) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(*v));
}

// An ACE slot's tag: the material and color of a slot whose RFID read
// identified its spool, read as the ACE backend reads the same hub's slots.
std::optional<ams::TagReading> ace_slot_tag(const nlohmann::json& slot) {
    if (!slot.is_object() || helix::ace::read_tag_state(slot) != helix::ace::TagRead::Identified) {
        return std::nullopt;
    }
    ams::TagReading tag;
    tag.material = helix::ace::read_slot_material(slot);
    if (const auto color = slot.find("color"); color != slot.end()) {
        tag.color_rgb = helix::ace::parse_slot_color(*color);
    }
    if (tag.empty()) {
        return std::nullopt;
    }
    return tag;
}

// A CFS tag as AFC_CFS.py decode_rfid_record() publishes it: `material` (null
// for a code the unit has no name for), bare-hex `color`, `weight` in grams and
// a Spoolman `spool_id`.
std::optional<ams::TagReading> cfs_lane_tag(const nlohmann::json& tag_json) {
    if (!tag_json.is_object()) {
        return std::nullopt;
    }
    ams::TagReading tag;
    if (auto material = ams::read_field<std::string>(tag_json, "material");
        material && !material->empty()) {
        tag.material = std::move(*material);
    }
    if (auto color = ams::read_field<std::string>(tag_json, "color")) {
        const auto reading = ams::read_lane_color(*color);
        if (reading.kind == ams::ColorReadingKind::Observed) {
            tag.color_rgb = reading.rgb;
        }
    }
    if (auto weight = ams::read_field<float>(tag_json, "weight"); weight && *weight > 0.0f) {
        tag.total_weight_g = *weight;
    }
    if (auto id = ams::read_integer_field(tag_json, "spool_id"); id && *id > 0) {
        tag.spoolman_id = *id;
    }
    if (tag.empty()) {
        return std::nullopt;
    }
    return tag;
}

std::optional<std::map<std::string, std::optional<ams::TagReading>>>
parse_cfs_lane_tags(const nlohmann::json& data) {
    const auto cfs = data.find("cfs");
    if (cfs == data.end() || !cfs->is_object()) {
        return std::nullopt;
    }
    const auto lanes = cfs->find("lanes");
    if (lanes == cfs->end() || !lanes->is_object()) {
        return std::nullopt;
    }
    std::map<std::string, std::optional<ams::TagReading>> out;
    for (auto it = lanes->begin(); it != lanes->end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("tag")) {
            continue;
        }
        out[it.key()] = cfs_lane_tag(it.value()["tag"]);
    }
    return out;
}

std::optional<AceUnitDelta> parse_ace_unit(const nlohmann::json& data) {
    if (std::none_of(std::begin(kAceKeys), std::end(kAceKeys),
                     [&data](const char* key) { return data.contains(key); })) {
        return std::nullopt;
    }
    AceUnitDelta ace;
    ace.dryer_status = ams::read_field<std::string>(data, "ace_dryer");
    ace.dryer_target_c = ams::read_field<float>(data, "ace_dryer_target");
    // AFC passes the firmware's dryer times through unconverted.
    if (const auto v = read_whole_number(data, "ace_dryer_remain")) {
        ace.dryer_remaining_min = helix::ace::dryer_remaining_minutes(*v);
    }
    if (const auto v = read_whole_number(data, "ace_dryer_duration")) {
        ace.dryer_duration_min = helix::ace::dryer_duration_minutes(*v);
    }
    ace.temperature_c = ams::read_field<float>(data, "ace_temp");
    ace.humidity_pct = ams::read_field<float>(data, "ace_humidity");
    if (const auto slots = data.find("ace_slots"); slots != data.end() && slots->is_array()) {
        // Each entry names its slot; the array order is AFC's, not a promise.
        constexpr int kMaxAceSlots = 16;
        std::vector<std::optional<ams::TagReading>> tags;
        for (const auto& slot : *slots) {
            const int index =
                (slot.is_object() ? ams::read_integer_field(slot, "index") : std::nullopt)
                    .value_or(static_cast<int>(tags.size()));
            if (index < 0 || index >= kMaxAceSlots) {
                continue;
            }
            if (tags.size() <= static_cast<size_t>(index)) {
                tags.resize(static_cast<size_t>(index) + 1);
            }
            tags[static_cast<size_t>(index)] = ace_slot_tag(slot);
        }
        ace.slot_tags = std::move(tags);
    }
    return ace;
}

} // namespace

UnitHardwareDelta parse_unit_hardware(const nlohmann::json& data) {
    UnitHardwareDelta d;
    if (data.is_object()) {
        d.ace = parse_ace_unit(data);
        d.cfs_lane_tags = parse_cfs_lane_tags(data);
    }
    return d;
}

std::vector<std::string> unit_object_fields() {
    std::vector<std::string> fields = {"lanes", "extruders", "hubs", "buffers", "cfs"};
    fields.insert(fields.end(), std::begin(kAceKeys), std::end(kAceKeys));
    return fields;
}

} // namespace helix::afc
