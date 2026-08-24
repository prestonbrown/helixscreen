// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_executor.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "device_display_name.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace helix {

namespace {

const std::unordered_set<std::string> DANGEROUS_MACROS = {
    "SAVE_CONFIG", "FIRMWARE_RESTART", "RESTART", "SHUTDOWN", "M112", "EMERGENCY_STOP",
};

} // namespace

std::string build_macro_gcode(const std::string& macro_name, const MacroParamResult& result) {
    std::string gcode;
    for (const auto& [key, value] : result.variables) {
        std::string var_lower = key;
        std::transform(var_lower.begin(), var_lower.end(), var_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        gcode += "SET_GCODE_VARIABLE MACRO=" + macro_name + " VARIABLE=" + var_lower +
                 " VALUE=" + value + "\n";
    }

    gcode += macro_name;
    for (const auto& [key, value] : result.params) {
        gcode += " " + key + "=" + value;
    }

    return gcode;
}

void execute_macro_gcode(IMoonrakerAPI* api, const std::string& macro_name,
                         const MacroParamResult& result, const char* caller_tag) {
    if (!api) {
        spdlog::warn("{} No API available — cannot execute macro", caller_tag);
        return;
    }

    std::string gcode = build_macro_gcode(macro_name, result);
    spdlog::info("{} Executing: {}", caller_tag, gcode);

    std::string macro_copy = macro_name;
    std::string tag_copy = caller_tag;
    std::string display_name = helix::get_display_name(macro_name, helix::DeviceType::MACRO);
    api->execute_gcode(
        gcode,
        [tag_copy, macro_copy, display_name]() {
            spdlog::info("{} {} executed successfully", tag_copy, macro_copy);
            std::string msg = display_name + " sent";
            helix::ui::queue_update([msg]() {
                ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str(), 2000);
            });
        },
        [tag_copy, macro_copy, display_name](const MoonrakerError& err) {
            spdlog::error("{} {} failed: {}", tag_copy, macro_copy, err.message);
            std::string msg = display_name + " failed";
            helix::ui::queue_update([msg]() {
                ToastManager::instance().show(ToastSeverity::ERROR, msg.c_str(), 4000);
            });
        },
        IMoonrakerAPI::MACRO_TIMEOUT_MS);
}

bool is_dangerous_macro(const std::string& name) {
    std::string upper_name = name;
    std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return DANGEROUS_MACROS.count(upper_name) > 0;
}

} // namespace helix
