// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_executor.h"

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace helix {

namespace {

const std::unordered_set<std::string> DANGEROUS_MACROS = {
    "SAVE_CONFIG", "FIRMWARE_RESTART", "RESTART", "SHUTDOWN", "M112", "EMERGENCY_STOP",
};

std::string upper_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

/// Does this error message name a Klippy disconnect?
///
/// Moonraker's wording is "Klippy Disconnected"; matching the two words
/// independently, uppercased, keeps a "klippy has disconnected" phrasing from
/// slipping past while still refusing Klipper's own G-code complaints.
bool contains_klippy_disconnect(const std::string& message) {
    const std::string upper = upper_copy(message);
    return upper.find("KLIPPY") != std::string::npos &&
           upper.find("DISCONNECT") != std::string::npos;
}

/// Commands a macro body issues, uppercased, in command position only.
///
/// Command position is what keeps this honest: ZMOD's own SAVE_CONFIG override
/// carries `RESPOND PREFIX="info" MSG="SAVE_CONFIG..."`, and a body scan that
/// matched anywhere would read that line as a config save. So: skip comments,
/// step over any leading Jinja tag, and take the first token of what is left.
/// Plain string walking rather than std::regex, which overflows the stack on
/// the MIPS targets (same constraint MacroFanAnalyzer works under).
std::vector<std::string> command_tokens(const std::string& gcode) {
    std::vector<std::string> tokens;
    size_t line_start = 0;
    while (line_start <= gcode.size()) {
        size_t line_end = gcode.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = gcode.size();
        }
        size_t pos = line_start;
        // Step over leading whitespace and any number of Jinja tags, so
        // `{% if x %} SAVE_CONFIG {% endif %}` still reports SAVE_CONFIG.
        while (pos < line_end) {
            while (pos < line_end && std::isspace(static_cast<unsigned char>(gcode[pos]))) {
                pos++;
            }
            if (pos + 1 < line_end && gcode[pos] == '{' &&
                (gcode[pos + 1] == '%' || gcode[pos + 1] == '#')) {
                size_t close = gcode.find('}', pos);
                if (close == std::string::npos || close >= line_end) {
                    pos = line_end; // Tag spans lines; nothing callable here.
                    break;
                }
                pos = close + 1;
                continue;
            }
            break;
        }
        if (pos < line_end && gcode[pos] != '#' && gcode[pos] != ';' && gcode[pos] != '{') {
            size_t tok_end = pos;
            while (tok_end < line_end &&
                   !std::isspace(static_cast<unsigned char>(gcode[tok_end]))) {
                tok_end++;
            }
            tokens.push_back(upper_copy(gcode.substr(pos, tok_end - pos)));
        }
        line_start = line_end + 1;
    }
    return tokens;
}

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
                         const MacroParamResult& result, const char* caller_tag,
                         const PrinterDiscovery& hw) {
    if (!api) {
        spdlog::warn("{} No API available — cannot execute macro", caller_tag);
        return;
    }

    std::string gcode = build_macro_gcode(macro_name, result);
    spdlog::info("{} Executing: {}", caller_tag, gcode);

    // Resolved before the send, not inside the callback: the callback runs on
    // the WebSocket thread, and PrinterDiscovery is not ours to read from there.
    const bool restarts_host = is_dangerous_macro(macro_name, hw);

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
        [tag_copy, macro_copy, display_name, restarts_host](const MoonrakerError& err) {
            if (classify_macro_rpc_failure(restarts_host, err) ==
                MacroFailureReport::ExpectedRestart) {
                spdlog::info("{} {} restarted the host, so its rpc was dropped ({}) - "
                             "reporting the restart rather than a failure",
                             tag_copy, macro_copy, err.message);
                // Armed HERE rather than before the send, because this is when
                // the restart actually happens. The suppression window is 15s
                // and a wrapping macro can run for minutes (a full bed level),
                // so arming it at send time would have expired long before
                // Klipper went down and suppressed nothing. Also completes the
                // contract: the klippy-READY observer says "Printer ready" when
                // it comes back, and the recovery dialog still appears if it
                // does not. The literal is the one the PID and input-shaper
                // saves already use for this event, so it costs no new
                // translation key.
                helix::ui::begin_expected_klippy_restart("Firmware restarting...");
                return;
            }
            spdlog::error("{} {} failed: {}", tag_copy, macro_copy, err.message);
            std::string msg = display_name + " failed";
            helix::ui::queue_update([msg]() {
                ToastManager::instance().show(ToastSeverity::ERROR, msg.c_str(), 4000);
            });
        },
        IMoonrakerAPI::MACRO_TIMEOUT_MS);
}

const std::unordered_set<std::string>& dangerous_command_names() {
    return DANGEROUS_MACROS;
}

std::unordered_set<std::string>
analyze_host_restarting_macros(const nlohmann::json& config_settings) {
    std::unordered_set<std::string> flagged;
    if (!config_settings.is_object()) {
        return flagged;
    }

    static constexpr const char* PREFIX = "gcode_macro ";
    static constexpr size_t PREFIX_LEN = 12; // strlen(PREFIX)
    std::unordered_map<std::string, std::vector<std::string>> calls;

    for (const auto& [section, values] : config_settings.items()) {
        if (section.rfind(PREFIX, 0) != 0 || !values.is_object()) {
            continue;
        }
        auto gcode = values.find("gcode");
        if (gcode == values.end() || !gcode->is_string()) {
            continue;
        }
        std::string name = upper_copy(section.substr(PREFIX_LEN));
        std::vector<std::string> tokens = command_tokens(gcode->get<std::string>());
        for (const auto& token : tokens) {
            if (DANGEROUS_MACROS.count(token) > 0) {
                flagged.insert(name);
                break;
            }
        }
        calls.emplace(std::move(name), std::move(tokens));
    }

    // Propagate: calling a flagged macro flags the caller. Iterate to a
    // fixpoint rather than recursing - mutual recursion between macros is legal
    // config, it only fails when Klipper runs it, and a DFS would not return.
    // Each round can only add, so the chain length bounds the rounds.
    for (size_t round = 0; round <= calls.size(); ++round) {
        bool changed = false;
        for (const auto& [name, tokens] : calls) {
            if (flagged.count(name) > 0) {
                continue;
            }
            for (const auto& token : tokens) {
                if (flagged.count(token) > 0) {
                    flagged.insert(name);
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) {
            break;
        }
    }

    return flagged;
}

bool is_dangerous_macro(const std::string& name) {
    return DANGEROUS_MACROS.count(upper_copy(name)) > 0;
}

bool is_dangerous_macro(const std::string& name, const PrinterDiscovery& hw) {
    return is_dangerous_macro(name) || hw.macro_restarts_host(name);
}

MacroFailureReport classify_macro_rpc_failure(bool macro_restarts_host, const MoonrakerError& err) {
    if (!macro_restarts_host) {
        return MacroFailureReport::Error;
    }

    switch (err.type) {
    case MoonrakerErrorType::CONNECTION_LOST:
    case MoonrakerErrorType::TIMEOUT:
    case MoonrakerErrorType::NOT_READY:
        // The socket, the reply, or Klipper itself went away. Nothing here can
        // be Klipper's opinion of the macro.
        return MacroFailureReport::ExpectedRestart;
    default:
        break;
    }

    // Moonraker fails the pending request with 503 "Klippy Disconnected" when
    // the host it was talking to restarts, and that arrives as a JSON-RPC error
    // like any other. Klipper's OWN rejections come through the same channel
    // carrying its complaint ("Unknown command", "Must home axis first"), so the
    // text is what separates them.
    if (err.code == 503 || contains_klippy_disconnect(err.message)) {
        return MacroFailureReport::ExpectedRestart;
    }
    return MacroFailureReport::Error;
}

} // namespace helix
