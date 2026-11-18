// SPDX-License-Identifier: GPL-3.0-or-later

#include "fault_path_position.h"

#include "afc_fault_position.h"

#include <cctype>

namespace helix::faultpath {

namespace {

struct Provider {
    std::optional<PathSegment> (*position)(std::string_view message);
    std::string (*strip)(std::string_view message);
    LaneSensor (*lane_sensor)(std::string_view message);
};

const Provider kProviders[] = {
    {&afc::afc_fault_position, &afc::afc_strip_position_diagram, &afc::afc_fault_lane_sensor},
};

// Whether @p line names @p lane as a word of its own.
bool names_lane(std::string_view line, std::string_view lane) {
    for (size_t at = line.find(lane); at != std::string_view::npos; at = line.find(lane, at + 1)) {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
        const size_t after = at + lane.size();
        if ((at == 0 || !word(line[at - 1])) && (after >= line.size() || !word(line[after])))
            return true;
    }
    return false;
}

const Provider* recognising_provider(std::string_view message) {
    for (const Provider& p : kProviders) {
        if (p.position(message)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::optional<PathSegment> fault_position(std::string_view message) {
    const Provider* p = recognising_provider(message);
    return p ? p->position(message) : std::nullopt;
}

LaneSensor lane_sensor_at(std::string_view message, std::string_view lane_name, bool current,
                          const std::vector<std::string>& lanes) {
    // A lane's fault is in the lines naming it: a message about another lane says nothing of
    // this one's sensors.
    for (size_t pos = 0; !lane_name.empty() && pos <= message.size();) {
        const size_t nl = message.find('\n', pos);
        const std::string_view line =
            message.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (names_lane(line, lane_name)) {
            for (const Provider& p : kProviders) {
                if (const LaneSensor s = p.lane_sensor(line); s != LaneSensor::NONE)
                    return s;
            }
        }
        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    if (!current || names_lane(message, lane_name))
        return LaneSensor::NONE;
    for (const std::string& other : lanes)
        if (!other.empty() && names_lane(message, other))
            return LaneSensor::NONE;
    for (const Provider& p : kProviders) {
        if (const LaneSensor s = p.lane_sensor(message); s != LaneSensor::NONE)
            return s;
    }
    return LaneSensor::NONE;
}

std::string strip_position_diagram(std::string_view message) {
    const Provider* p = recognising_provider(message);
    return p ? p->strip(message) : std::string(message);
}

} // namespace helix::faultpath
