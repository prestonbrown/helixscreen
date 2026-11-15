// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_catalog_picker.h"

#include "lvgl.h"

namespace helix::ui {

// Test-only accessor for FilamentCatalogPickerModal's selection state (now
// delegated to the embedded FilamentCatalogSelector).
struct FilamentPickerTestAccess {
    static void select_first_product(FilamentCatalogPickerModal& m) {
        m.selector_.select_first_product_for_test();
    }

    static void press_select(FilamentCatalogPickerModal& m) {
        m.handle_select_button();
    }

    static void change_vendor(FilamentCatalogPickerModal& m, uint32_t index) {
        m.selector_.change_vendor_for_test(index);
    }

    static std::string highlighted_id(const FilamentCatalogPickerModal& m) {
        const auto* ef = m.selector_.highlighted();
        return ef ? ef->id : std::string();
    }

    static std::string type_options(const FilamentCatalogPickerModal& m) {
        return m.selector_.type_options();
    }

    // Returns true if the "Reset to defaults" affordance is currently hidden. The reset
    // action is the leading (tertiary) button in modal_button_row, so its widget is
    // "btn_tertiary" — on_show() toggles its HIDDEN flag on whether a reset callback was set.
    static bool reset_button_hidden(const FilamentCatalogPickerModal& m) {
        lv_obj_t* btn = lv_obj_find_by_name(m.dialog(), "btn_tertiary");
        if (!btn)
            return true;
        return lv_obj_is_hidden(btn);
    }

    static void press_reset(FilamentCatalogPickerModal& m) {
        if (m.reset_callback_)
            m.reset_callback_();
        m.hide();
    }
};

} // namespace helix::ui
