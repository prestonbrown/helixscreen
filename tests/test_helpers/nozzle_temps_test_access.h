// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/nozzle_temps_widget.h"

namespace helix {

// Friend access to NozzleTempsWidget's cached row/bed widget pointers. The
// teardown-uaf contract is exactly WHICH raw pointers survive WHICH deletion
// path (detach vs a raw lv_obj_delete of the page tree), and that is only
// observable on the privates. Read-only: nothing here mutates widget state.
//
// The header forward-declares this class inside namespace helix, so the
// definition must live in the same namespace — and in ONE place, or two test
// translation units defining their own copy would be an ODR violation.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class NozzleTempsTestAccess {
  public:
    static size_t row_count(const NozzleTempsWidget& widget) {
        return widget.extruder_rows_.size();
    }

    static lv_obj_t* row_temp_label(const NozzleTempsWidget& widget, size_t index) {
        return index < widget.extruder_rows_.size() ? widget.extruder_rows_[index].temp_label
                                                    : nullptr;
    }

    static lv_obj_t* row_target_label(const NozzleTempsWidget& widget, size_t index) {
        return index < widget.extruder_rows_.size() ? widget.extruder_rows_[index].target_label
                                                    : nullptr;
    }

    static lv_obj_t* bed_temp_label(const NozzleTempsWidget& widget) {
        return widget.bed_temp_label_;
    }

    static lv_obj_t* bed_icon(const NozzleTempsWidget& widget) {
        return widget.bed_icon_;
    }
};

} // namespace helix
