// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe_touch_z.h"

#include "json_utils.h"
#include "printer_discovery.h"

#include <array>

#include "hv/json.hpp"

namespace helix::sensors::touch_z {

namespace {

struct Row {
    ProbeSensorType type;
    /// Status field on the probe's own object, or null when the status says nothing.
    const char* status_field;
    /// Child of that field whose value names the loaded touch model ("none" = none).
    const char* status_model_key;
    /// Config key on the probe's section, and the value that means touch homing.
    const char* config_key;
    const char* config_value;
};

// Cartographer (cartographer3d-plugin): a touch model is loaded once
// CARTOGRAPHER_TOUCH_CALIBRATE has run, and its touch home then sets Z.
// Beacon: `home_method: contact` homes Z with the nozzle on the bed.
constexpr std::array<Row, 2> kRows{{
    {ProbeSensorType::CARTOGRAPHER, "touch", "current_model", nullptr, nullptr},
    {ProbeSensorType::BEACON, nullptr, nullptr, "home_method", "contact"},
}};

const Row* row_for(ProbeSensorType type) {
    for (const auto& row : kRows) {
        if (row.type == type) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

std::optional<StatusField> status_field(ProbeSensorType type, const std::string& klipper_name) {
    const Row* row = row_for(type);
    if (row == nullptr || row->status_field == nullptr) {
        return std::nullopt;
    }
    return StatusField{klipper_name, row->status_field};
}

std::optional<bool> from_status(ProbeSensorType type, const std::string& klipper_name,
                                const nlohmann::json& status) {
    const Row* row = row_for(type);
    if (row == nullptr || row->status_field == nullptr || !status.is_object()) {
        return std::nullopt;
    }
    auto* object = helix::json_util::find_object(status, klipper_name);
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto field = object->find(row->status_field);
    if (field == object->end() || !field->is_object()) {
        return std::nullopt;
    }
    const auto model = helix::json_util::read_field<std::string>(*field, row->status_model_key);
    if (!model) {
        return std::nullopt;
    }
    const auto& name = *model;
    return !name.empty() && name != "none";
}

bool from_config(ProbeSensorType type, const std::string& klipper_name,
                 const PrinterDiscovery& hw) {
    const Row* row = row_for(type);
    if (row == nullptr || row->config_key == nullptr) {
        return false;
    }
    const nlohmann::json* section = hw.section(klipper_name);
    if (section == nullptr || !section->is_object()) {
        return false;
    }
    const auto value = section->find(row->config_key);
    return value != section->end() && value->is_string() &&
           value->get_ref<const std::string&>() == row->config_value;
}

} // namespace helix::sensors::touch_z
