// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace helix::ui {

/**
 * @brief How a printer numbers its tools where a number stands alone.
 *
 * Most machines are labeled from 1 ("Toolhead 1" on a U1, heads 1-4 on a
 * Creator 5), while a klipper-toolchanger rig is labeled with the G-code names
 * its owner wrote (`[tool T0]`, "T0" on the dock plate). The G-code name itself
 * (ToolLabelForm::Code, tool_label()) is the same in both styles.
 */
enum class ToolNumberStyle : uint8_t {
    Physical, ///< 1-based position: "Tool 1", badge "1". The default.
    Gcode,    ///< The G-code name the machine is labeled with: "T0", badge "T0".
};

/// The persisted per-printer override. Values are the stored ints, so never reorder.
enum class ToolNumberStyleSetting : uint8_t { Auto = 0, Physical = 1, Gcode = 2 };

/**
 * @brief Whether @p name is the generated G-code identity, not a configured one.
 *
 * A Klipper tool carries either a name its owner chose ("Left", "Right") or the
 * "T<n>" form tool_label() produces when none was configured. Only the
 * generated form may be replaced by a display number; a chosen name is what
 * the machine is physically labeled with and is shown verbatim.
 */
inline bool is_generated_tool_name(std::string_view name) {
    if (name.size() < 2 || name[0] != 'T')
        return false;
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

} // namespace helix::ui
