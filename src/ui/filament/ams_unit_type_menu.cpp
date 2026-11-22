// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_unit_type_menu.h"

#include "ams_types.h"

namespace helix::ui {

UnitTypeMenu unit_type_menu(const AmsUnit& unit, const std::string& chosen_id,
                            const std::string& automatic_label) {
    UnitTypeMenu menu;
    menu.choices = tray::unit_shell_choices(unit.slot_count);
    menu.options = automatic_label;
    for (size_t i = 0; i < menu.choices.size(); ++i) {
        menu.options += "\n";
        menu.options += menu.choices[i].name;
        if (chosen_id == menu.choices[i].shell_id)
            menu.selected = static_cast<int>(i) + 1;
    }
    return menu;
}

std::string unit_type_menu_choice(const UnitTypeMenu& menu, int option) {
    if (option < 1 || option > static_cast<int>(menu.choices.size()))
        return {};
    return menu.choices[static_cast<size_t>(option) - 1].shell_id;
}

} // namespace helix::ui
