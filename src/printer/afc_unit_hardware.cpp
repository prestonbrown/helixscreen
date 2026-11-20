// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_unit_hardware.h"

#include "ace_slot_parse.h"
#include "ams_status_json.h"
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
                                    "ace_dryer_duration", "ace_temp",         "ace_humidity"};

std::optional<int> read_whole_number(const nlohmann::json& data, const char* key) {
    const auto v = ams::read_field<float>(data, key);
    if (!v) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(*v));
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
    return ace;
}

} // namespace

UnitHardwareDelta parse_unit_hardware(const nlohmann::json& data) {
    UnitHardwareDelta d;
    if (data.is_object()) {
        d.ace = parse_ace_unit(data);
    }
    return d;
}

std::vector<std::string> unit_object_fields() {
    std::vector<std::string> fields = {"lanes", "extruders", "hubs", "buffers"};
    fields.insert(fields.end(), std::begin(kAceKeys), std::end(kAceKeys));
    return fields;
}

} // namespace helix::afc
