// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "hv/json.hpp"

/// The versioned status contract klipper_openams publishes on `oams_manager`
/// (docs/UI_API.md in klipper_openams). Discovery and the backend share this
/// one definition of "supported", so a printer is claimed exactly when the
/// backend can read it.
namespace helix::openams {

inline constexpr const char* kManagerObject = "oams_manager";
inline constexpr int kApiVersion = 1;
inline constexpr const char* kSchema = "openams.manager";

/// Whether @p manager_status (the `oams_manager` status object, or any part of
/// it carrying `api_version` and `schema`) speaks a version this build reads.
/// A manager that predates the contract publishes neither field.
[[nodiscard]] inline bool api_supported(const nlohmann::json& manager_status) {
    if (!manager_status.is_object()) {
        return false;
    }
    auto version = manager_status.find("api_version");
    auto schema = manager_status.find("schema");
    return version != manager_status.end() && version->is_number_integer() &&
           version->get<int>() == kApiVersion && schema != manager_status.end() &&
           schema->is_string() && schema->get<std::string>() == kSchema;
}

} // namespace helix::openams
