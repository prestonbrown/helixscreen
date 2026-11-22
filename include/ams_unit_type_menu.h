// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_unit_type_menu.h
 * @brief The Unit Type picker's options for one unit: Automatic, then every drawing with art
 *        for the unit's bays.
 */

#include "ams_unit_shell.h"

#include <string>
#include <vector>

namespace helix {
struct AmsUnit;
}

namespace helix::ui {

struct UnitTypeMenu {
    /// The drawings after Automatic, in option order: option i + 1 is choices[i].
    std::vector<tray::ShellChoice> choices;
    /// The dropdown's options, newline-separated, Automatic first.
    std::string options;
    /// The option the saved pick names; 0 (Automatic) when it names no listed drawing.
    int selected = 0;
};

/// The menu for @p unit, with @p chosen_id (a shell id, or empty) as the saved pick and
/// @p automatic_label as the first option's text.
UnitTypeMenu unit_type_menu(const AmsUnit& unit, const std::string& chosen_id,
                            const std::string& automatic_label);

/// The shell id option @p option saves: empty for Automatic or an option past the list.
std::string unit_type_menu_choice(const UnitTypeMenu& menu, int option);

} // namespace helix::ui
