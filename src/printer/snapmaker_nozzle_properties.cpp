// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_nozzle_properties.h"

#include "json_utils.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>

namespace helix::snapmaker::nozzle {
namespace {

/// A reading within this of a kDiameters value is that value.
constexpr double kDiameterTolerance = 1e-4;

/// The option text of each flow type, in the order rows list them. A pick
/// comes back as this text, so it is matched here, never a translation of it.
constexpr std::array<std::pair<const char*, const char*>, 2> kFlowOptions = {
    {{kStandard, "Standard"}, {kHighFlow, "High Flow"}}};

std::optional<double> listed_diameter(double mm) {
    for (const double d : kDiameters) {
        if (std::fabs(mm - d) <= kDiameterTolerance) {
            return d;
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<std::string> diameter_options() {
    std::vector<std::string> out;
    for (const double d : kDiameters) {
        out.push_back(diameter_option(d));
    }
    return out;
}

std::vector<std::string> flow_options() {
    std::vector<std::string> out;
    for (const auto& [type, option] : kFlowOptions) {
        out.emplace_back(option);
    }
    return out;
}

std::string diameter_option(double mm) {
    const auto d = listed_diameter(mm);
    return d ? fmt::format("{:.1f} mm", *d) : std::string();
}

std::string flow_option(const std::string& volume_type) {
    for (const auto& [type, option] : kFlowOptions) {
        if (volume_type == type) {
            return option;
        }
    }
    return {};
}

std::optional<double> diameter_from_option(const std::string& option) {
    for (const double d : kDiameters) {
        if (option == diameter_option(d)) {
            return d;
        }
    }
    return std::nullopt;
}

std::optional<std::string> volume_type_from_option(const std::string& option) {
    for (const auto& [type, text] : kFlowOptions) {
        if (option == text) {
            return std::string(type);
        }
    }
    return std::nullopt;
}

std::optional<nlohmann::json> change_params(int head, const Nozzle& reported,
                                            const Nozzle& wanted) {
    nlohmann::json params = {{"extruder", head}};
    bool any = false;
    if (wanted.diameter) {
        const auto want = listed_diameter(*wanted.diameter);
        if (want &&
            (!reported.diameter || std::fabs(*reported.diameter - *want) > kDiameterTolerance)) {
            params["diameter"] = *want;
            any = true;
        }
    }
    if (wanted.volume_type && wanted.volume_type != reported.volume_type) {
        params["volume_type"] = *wanted.volume_type;
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    return params;
}

Reply read_reply(const nlohmann::json& response) {
    const nlohmann::json* body = &response;
    if (const nlohmann::json* result = json_util::find_member(response, "result");
        result != nullptr && result->is_object()) {
        body = result;
    }
    const nlohmann::json* state = json_util::find_member(*body, "state");
    if (state == nullptr || !state->is_string()) {
        return {ReplyKind::Unexpected, {}};
    }
    const std::string& s = state->get_ref<const std::string&>();
    if (s == "success") {
        return {ReplyKind::Accepted, {}};
    }
    if (s == "error") {
        const nlohmann::json* message = json_util::find_member(*body, "message");
        return {ReplyKind::Refused, message != nullptr && message->is_string()
                                        ? message->get<std::string>()
                                        : std::string()};
    }
    return {ReplyKind::Unexpected, {}};
}

} // namespace helix::snapmaker::nozzle
