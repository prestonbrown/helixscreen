// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/thermistor_widget.h"

namespace helix {

// Friend access to ThermistorWidget's cached label pointers (single mode)
// and carousel page labels. The teardown-uaf contract is exactly WHICH raw
// pointers survive WHICH deletion path (detach vs a raw lv_obj_delete of the
// page tree), and that is only observable on the privates. Read-only.
//
// The header forward-declares this class inside namespace helix, so the
// definition must live in the same namespace — and in ONE place, or two test
// translation units defining their own copy would be an ODR violation.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class ThermistorTestAccess {
  public:
    static lv_obj_t* temp_label(const ThermistorWidget& widget) {
        return widget.temp_label_;
    }

    static lv_obj_t* name_label(const ThermistorWidget& widget) {
        return widget.name_label_;
    }

    static size_t carousel_page_count(const ThermistorWidget& widget) {
        return widget.carousel_pages_.size();
    }

    static lv_obj_t* carousel_temp_label(const ThermistorWidget& widget, size_t index) {
        return index < widget.carousel_pages_.size() ? widget.carousel_pages_[index].temp_label
                                                     : nullptr;
    }

    static lv_obj_t* carousel_name_label(const ThermistorWidget& widget, size_t index) {
        return index < widget.carousel_pages_.size() ? widget.carousel_pages_[index].name_label
                                                     : nullptr;
    }
};

} // namespace helix
