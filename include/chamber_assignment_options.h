// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file chamber_assignment_options.h
 * @brief Option-list builder shared by the chamber heater and chamber sensor dropdowns
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace helix::settings {

/**
 * @brief Translated labels the option list is assembled from.
 *
 * The overlay resolves these through lv_tr(); taking them as data keeps the
 * builder free of LVGL and testable on its own.
 */
struct ChamberAssignmentLabels {
    std::string auto_label;    ///< "Auto"
    std::string none_detected; ///< "(none detected)"
    std::string not_detected;  ///< "not detected"
    std::string none_disable;  ///< "None (disable)"
};

/**
 * @brief A built dropdown option list and the Klipper names its options map to.
 */
struct ChamberAssignmentOptions {
    std::string options;            ///< Newline-separated, for lv_dropdown_set_options()
    std::vector<std::string> names; ///< Klipper object behind option index i + 1
    uint32_t selected = 0;          ///< Option index the saved assignment maps to
};

/**
 * @brief Build the option list for a chamber assignment dropdown.
 *
 * Option 0 hands the role to discovery, options 1 through names.size() assign a
 * specific object, and option names.size() + 1 disables the role. A saved
 * assignment naming an object discovery did not return gets an option of its own,
 * marked with @p labels.not_detected, so it stays visible and clearable instead of
 * hiding behind a dropdown that reads "Auto". That option lands inside the names
 * range, which is what keeps the disable option at names.size() + 1.
 *
 * @param discovered  Assignable Klipper objects, already filtered for the role
 * @param detected    Object discovery would pick on its own, empty when none
 * @param saved       Persisted assignment: "auto", "none", or a Klipper object
 * @param strip_prefix Klipper class prefix to drop for display (e.g. "heater_generic ")
 * @param labels      Translated labels
 */
ChamberAssignmentOptions
build_chamber_assignment_options(const std::vector<std::string>& discovered,
                                 const std::string& detected, const std::string& saved,
                                 const std::string& strip_prefix,
                                 const ChamberAssignmentLabels& labels);

} // namespace helix::settings
