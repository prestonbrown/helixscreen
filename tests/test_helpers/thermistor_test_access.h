// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_carousel.h"

#include "src/ui/panel_widgets/thermistor_widget.h"

namespace helix {

// Friend access to ThermistorWidget's carousel page labels. The teardown-uaf contract is exactly
// WHICH raw pointers survive WHICH deletion path (detach vs a raw lv_obj_delete of the page tree),
// and that is only observable on the privates. Read-only.
//
// The header forward-declares this class inside namespace helix, so the
// definition must live in the same namespace — and in ONE place, or two test
// translation units defining their own copy would be an ODR violation.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class ThermistorTestAccess {
  public:
    static size_t carousel_page_count(const ThermistorWidget& widget) {
        return widget.carousel_page_count_;
    }

    /// The reading label of carousel page @p index, found through the tile root
    /// the widget still holds: null once that tree is gone.
    static lv_obj_t* carousel_temp_label(const ThermistorWidget& widget, size_t index) {
        return page_child(widget, index, "page_temp");
    }

    static lv_obj_t* carousel_name_label(const ThermistorWidget& widget, size_t index) {
        return page_child(widget, index, "page_name");
    }

    /// Rebuild the carousel pages, as a TemperatureSensorManager revision does.
    static void bind_carousel(ThermistorWidget& widget) {
        widget.bind_carousel_sensors();
    }

    /// A tap on the tile, as the XML click callback delivers it.
    static void click(ThermistorWidget& widget) {
        widget.handle_clicked();
    }

    /// The target keypad's confirm, as ui_component_keypad delivers it.
    static void keypad_confirm(float value) {
        ThermistorWidget::target_keypad_cb(value, nullptr);
    }

  private:
    static lv_obj_t* page_child(const ThermistorWidget& widget, size_t index, const char* name) {
        if (!widget.widget_obj_)
            return nullptr;
        lv_obj_t* carousel = lv_obj_find_by_name(widget.widget_obj_, "thermistor_carousel");
        auto* state = carousel ? ui_carousel_get_state(carousel) : nullptr;
        if (!state || index >= state->real_tiles.size())
            return nullptr;
        return lv_obj_find_by_name(state->real_tiles[index], name);
    }
};

} // namespace helix
