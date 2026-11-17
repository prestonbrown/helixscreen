// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/error_reporting.h"
#include "moonraker_api.h"
#include "moonraker_api_internal.h"

#include <algorithm>

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

// Vendor and product names carry punctuation no material name does: `Overture's`,
// `Matte PLA, Black`, `R&D`. Inside double quotes Klipper's `shlex` pass treats all of
// that as text, so every printable ASCII character passes except the ones that still act
// there: `"` and `\` (they end or escape the quoting), and `#`, `;` and `*`, which
// Klipper's extended-command pattern cuts the line at before `shlex` ever sees it, quotes
// or not. Control characters, CR/LF and non-ASCII bytes are refused as in
// is_safe_material_param().
bool helix::IMoonrakerAPI::is_safe_text_param(const std::string& str) {
    if (str.empty()) {
        return false;
    }
    return std::all_of(str.begin(), str.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E) {
            return false;
        }
        switch (c) {
        case '"':
        case '\\':
        case '#':
        case ';':
        case '*':
            return false;
        default:
            return true;
        }
    });
}

// Every value is quoted, unlike gcode_param_value(): an apostrophe in an unquoted value
// opens a single-quoted string for `shlex`, which then fails with "No closing quotation".
std::string helix::IMoonrakerAPI::quoted_gcode_param_value(const std::string& value) {
    return "\"" + value + "\"";
}
