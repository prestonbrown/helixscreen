// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api.h"
#include "moonraker_api_internal.h"

bool IMoonrakerAPI::is_safe_gcode_param(const std::string& str) {
    return moonraker_internal::is_safe_identifier(str);
}

bool IMoonrakerAPI::is_safe_material_param(const std::string& str) {
    return moonraker_internal::is_safe_material_param(str);
}

std::string IMoonrakerAPI::gcode_param_value(const std::string& value) {
    return moonraker_internal::gcode_param_value(value);
}
