// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/error_reporting.h"
#include "moonraker_api.h"
#include "moonraker_api_internal.h"

void helix::report_validation_error(
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error, const char* method,
    const std::string& detail, const std::string& user_message) {
    spdlog::warn("[Moonraker API] {}: {}", method, detail);
    if (on_error) {
        on_error(MoonrakerError::validation_error(method, user_message));
        return;
    }
    NOTIFY_ERROR("{}", user_message);
}

bool helix::IMoonrakerAPI::is_safe_gcode_param(const std::string& str) {
    return moonraker_internal::is_safe_identifier(str);
}

bool helix::IMoonrakerAPI::is_safe_material_param(const std::string& str) {
    return moonraker_internal::is_safe_material_param(str);
}

std::string helix::IMoonrakerAPI::gcode_param_value(const std::string& value) {
    return moonraker_internal::gcode_param_value(value);
}
