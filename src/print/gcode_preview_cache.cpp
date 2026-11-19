// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_preview_cache.h"

#include "config.h"
#include "utils/network_validation.h"

#include <functional>

namespace helix::gcode {

std::string preview_cache_file_name(const char* prefix, const std::string& file_key) {
    auto* cfg = Config::get_instance();
    const std::string printer =
        cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost") + ":" +
        std::to_string(cfg->get<int>(cfg->df() + "moonraker_port", helix::kDefaultMoonrakerPort));
    return std::string(prefix) +
           std::to_string(std::hash<std::string>{}(printer + '\n' + file_key)) + ".gcode";
}

} // namespace helix::gcode
