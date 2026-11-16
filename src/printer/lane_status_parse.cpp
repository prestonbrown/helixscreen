// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_status_parse.h"

#include "core/text_io.h"
#include "filament_slot_override_store.h"
#include "json_utils.h"

#include <algorithm>
#include <unordered_set>

#include "hv/json.hpp"

namespace helix::lane_status {

namespace {

using json = nlohmann::json;

std::vector<std::string> string_list(const json& object, const char* key) {
    std::vector<std::string> out;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) {
        return out;
    }
    for (const auto& v : *it) {
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

/// The major of a "major.minor" version, or the whole string without a dot.
std::string_view major_of(std::string_view version) {
    return version.substr(0, version.find('.'));
}

Kind kind_from(const std::string& kind) {
    if (kind == "status_only") {
        return Kind::StatusOnly;
    }
    if (kind == "controller") {
        return Kind::Controller;
    }
    return Kind::Unknown;
}

/// "T3" as 3; -1 for null, absent or anything else.
int tool_from(const json& record) {
    const std::string tool = json_util::safe_string(record, "tool");
    if (tool.size() < 2 || (tool[0] != 'T' && tool[0] != 't')) {
        return -1;
    }
    const auto n = text_io::parse_leading<int>(tool.substr(1));
    return n && *n >= 0 ? *n : -1;
}

} // namespace

bool Status::has_capability(std::string_view token) const {
    return std::find(capabilities.begin(), capabilities.end(), token) != capabilities.end();
}

bool Status::offers(std::string_view token) const {
    return readable && kind == Kind::Controller && has_capability(token);
}

int Status::lane_index(std::string_view lane) const {
    const auto it = std::find(lanes.begin(), lanes.end(), lane);
    return it == lanes.end() ? -1 : static_cast<int>(it - lanes.begin());
}

bool is_lane_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

Status parse_status(const json& object) {
    Status s;
    if (!object.is_object()) {
        return s;
    }
    s.spec_version = json_util::safe_string(object, "spec_version");
    s.readable = major_of(s.spec_version) == kSpecMajor;
    if (const auto p = object.find("producer"); p != object.end() && p->is_object()) {
        s.producer_name = json_util::safe_string(*p, "name");
        s.producer_version = json_util::safe_string(*p, "version");
        s.producer_url = json_util::safe_string(*p, "url");
    }
    s.kind = kind_from(json_util::safe_string(object, "kind"));
    s.wraps = json_util::safe_string(object, "wraps");
    s.capabilities = string_list(object, "capabilities");
    s.automap = json_util::safe_string(object, "automap");
    s.compat_profiles = string_list(object, "compat_profiles");
    s.units = string_list(object, "units");
    std::unordered_set<std::string> seen;
    for (auto& lane : string_list(object, "lanes")) {
        if (is_lane_name(lane) && seen.insert(lane).second) {
            s.lanes.push_back(std::move(lane));
        }
    }
    s.current_lane = json_util::safe_string(object, "current_lane");
    if (s.lane_index(s.current_lane) < 0) {
        s.current_lane.clear();
    }
    s.state = json_util::safe_string(object, "state");
    if (const auto e = object.find("error"); e != object.end() && e->is_object()) {
        s.error_active = json_util::safe_bool(*e, "active", false);
        s.error_message = json_util::safe_string(*e, "message");
        s.error_type = json_util::safe_string(*e, "type");
    }
    return s;
}

AmsAction action_for(const Status& status) {
    if (status.error_active || status.state == "Error") {
        return AmsAction::ERROR;
    }
    if (status.state == "Loading" || status.state == "ToolPickup") {
        return AmsAction::LOADING;
    }
    if (status.state == "Unloading" || status.state == "Ejecting" || status.state == "ToolDock") {
        return AmsAction::UNLOADING;
    }
    if (status.state == "Moving" || status.state == "ToolSwap") {
        return AmsAction::SELECTING;
    }
    return AmsAction::IDLE;
}

std::vector<std::optional<LaneRecord>> join_lane_records(const json& namespace_doc,
                                                         const std::vector<std::string>& lanes,
                                                         const std::string& producer) {
    std::vector<std::optional<LaneRecord>> out(lanes.size());
    std::vector<bool> from_producer(lanes.size(), false);
    if (!namespace_doc.is_object()) {
        return out;
    }
    const auto index_of = [&lanes](const std::string& name) {
        const auto it = std::find(lanes.begin(), lanes.end(), name);
        return it == lanes.end() ? -1 : static_cast<int>(it - lanes.begin());
    };
    for (auto it = namespace_doc.begin(); it != namespace_doc.end(); ++it) {
        const json& wire = it.value();
        if (!wire.is_object()) {
            continue;
        }
        const std::string lss = json_util::safe_string(wire, "lss_version");
        if (!lss.empty() && major_of(lss) != kSpecMajor) {
            continue;
        }
        const std::string writer = json_util::safe_string(wire, "writer");
        const bool legacy = writer.empty();
        if (!legacy && writer != producer) {
            continue;
        }
        const std::string lane_name = json_util::safe_string(wire, "lane_name");
        auto parsed = ams::from_lane_data_record(wire);
        if (!parsed && !lane_name.empty()) {
            // `lane_name` alone joins the record; a missing or unreadable
            // `lane` costs only the index this reader does not use.
            json named = wire;
            named["lane"] = "0";
            parsed = ams::from_lane_data_record(named);
        }
        int idx = !lane_name.empty() ? index_of(lane_name) : -1;
        if (idx < 0 && lane_name.empty() && legacy && parsed) {
            idx = parsed->first < static_cast<int>(lanes.size()) ? parsed->first : -1;
        }
        if (idx < 0) {
            continue;
        }
        const auto slot = static_cast<std::size_t>(idx);
        if (legacy && from_producer[slot]) {
            continue;
        }
        LaneRecord record;
        if (parsed) {
            record.identity = parsed->second;
        }
        record.loaded = record.identity.loaded;
        record.tool = tool_from(wire);
        out[slot] = std::move(record);
        from_producer[slot] = !legacy;
    }
    return out;
}

} // namespace helix::lane_status
