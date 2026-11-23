// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * @file macro_types.h
 * @brief Data structures for G-code macro management
 *
 * Types for browsing, categorizing, and executing Klipper macros.
 * Used by the macro browser panel and macro execution features.
 */

// Macro Types

/**
 * @brief Information about a G-code macro
 */
struct MacroInfo {
    std::string name;                ///< Macro name (e.g., "CLEAN_NOZZLE", "PRINT_START")
    std::string description;         ///< Description from gcode_macro description field
    std::vector<std::string> params; ///< Detected parameter names

    bool is_helix_macro = false;  ///< True if HELIX_* prefix
    bool is_system_macro = false; ///< True if _* prefix (hidden by default)
    bool is_dangerous = false;    ///< True if potentially destructive (SAVE_CONFIG, etc.)

    /**
     * @brief Check if macro accepts parameters
     */
    [[nodiscard]] bool has_params() const {
        return !params.empty();
    }

    /**
     * @brief Get display name (without HELIX_ prefix if applicable)
     */
    [[nodiscard]] std::string display_name() const {
        if (is_helix_macro && name.length() > 6) {
            return name.substr(6); // Remove "HELIX_" prefix
        }
        return name;
    }
};

/**
 * @brief Category for grouping macros in the browser
 */
enum class MacroCategory {
    COMMON,      ///< Frequently used (G28, G32, CLEAN_NOZZLE, etc.)
    CALIBRATION, ///< Calibration macros (BED_MESH_CALIBRATE, Z_TILT_ADJUST, etc.)
    HELIX,       ///< HelixScreen helper macros (HELIX_*)
    USER,        ///< User-defined macros
    SYSTEM       ///< System/internal macros (_* prefix)
};

// Macro Callback Types

namespace helix {
/// Macro list callback
using MacroListCallback = std::function<void(const std::vector<MacroInfo>&)>;
} // namespace helix
