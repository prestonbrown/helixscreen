// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace helix::gcode {

/// Value of KEY=value in one G-code command line, read the way Klipper's
/// parameter parser reads it: KEY matches a whole word only (OFFSET= is not
/// read out of TIME_OFFSET=), and a quoted value ('a b' or "a b") comes back
/// without its quotes. nullopt when the key is absent or has no value.
std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key);

} // namespace helix::gcode
