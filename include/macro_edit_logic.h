// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <set>
#include <string>
#include <vector>

// Pure decision logic for the Macros panel edit mode, extracted from
// MacrosPanel so it can be unit-tested with realistic inputs without
// instantiating any LVGL widgets (see test_ui_panel_macros_edit_mode.cpp).
//
// All functions are free of global state: they take the macro list, the
// persisted-settings answer, and the per-row facts as arguments and return
// the decision. MacrosPanel calls these from rebuild_rows() / seed logic.
namespace helix::macros {

// The effective set of hidden macro names.
//
// First-run seed: when the per-printer "macros/hidden" key does not yet exist
// (`key_exists == false`), the default is to hide every `_`-prefixed system
// macro. Once the user has saved a choice (`key_exists == true`), the saved
// set is authoritative — even if empty (they un-hid everything).
inline std::set<std::string>
compute_effective_hidden(const std::vector<std::string>& all_macros, bool key_exists,
                         const std::vector<std::string>& saved_hidden) {
    if (!key_exists) {
        std::set<std::string> seeded;
        for (const auto& m : all_macros) {
            if (!m.empty() && m[0] == '_') {
                seeded.insert(m);
            }
        }
        return seeded;
    }
    return std::set<std::string>(saved_hidden.begin(), saved_hidden.end());
}

// Normal-mode (non-edit) visible list: every macro not in the hidden set,
// preserving the input order (callers pass an already-sorted list).
inline std::vector<std::string> filter_visible(const std::vector<std::string>& all_macros,
                                               const std::set<std::string>& hidden) {
    std::vector<std::string> visible;
    visible.reserve(all_macros.size());
    for (const auto& m : all_macros) {
        if (hidden.find(m) == hidden.end()) {
            visible.push_back(m);
        }
    }
    return visible;
}

// The four per-row display-state ints bound by macro_card.xml.
//   visible       -> checkbox state (edit mode) / always-on (normal mode)
//   desc_hidden   -> hide the description sub-label
//   chevron_hidden-> hide the tappable chevron
//   defaults_hidden -> hide the edit-mode "Default Parameters" button
struct RowValues {
    int visible;
    int desc_hidden;
    int chevron_hidden;
    int defaults_hidden;
};

// Compute a row's display state.
//   edit_mode  : panel is in edit mode (checkboxes shown in place of the code
//                icon, no chevron; description stays visible)
//   is_hidden  : this macro is in the pending-hidden set (edit mode only)
//   has_desc   : the macro has a non-empty cached description
//   no_params  : the macro is KNOWN_NO_PARAMS (no chevron in normal mode)
//   has_params : the macro is KNOWN_PARAMS (defaults button in edit mode);
//                there is no field list to save otherwise
inline RowValues compute_row_values(bool edit_mode, bool is_hidden, bool has_desc, bool no_params,
                                    bool has_params = false) {
    RowValues rv;
    // In edit mode the checkbox reflects visibility (un-hidden == checked);
    // in normal mode every displayed row is visible.
    rv.visible = edit_mode ? (is_hidden ? 0 : 1) : 1;
    // Description visibility depends only on whether the macro has one — it
    // stays visible in edit mode too, so the row doesn't jump vertically when
    // toggling modes (the checkbox/icon swap in the leading slot is enough).
    rv.desc_hidden = !has_desc ? 1 : 0;
    // No chevron in edit mode, or when the macro takes no parameters.
    rv.chevron_hidden = (edit_mode || no_params) ? 1 : 0;
    // Saved defaults need a declared parameter list, and editing them is an
    // edit-mode action.
    rv.defaults_hidden = (edit_mode && has_params) ? 0 : 1;
    return rv;
}

} // namespace helix::macros
