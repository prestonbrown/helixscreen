// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_picker.h"

#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

bool FilamentCatalogPickerModal::callbacks_registered_ = false;
FilamentCatalogPickerModal* FilamentCatalogPickerModal::active_instance_ = nullptr;

void FilamentCatalogPickerModal::register_callbacks() {
    if (callbacks_registered_)
        return;
    FilamentCatalogSelector::register_callbacks();
    lv_xml_register_event_cb(nullptr, "catalog_picker_close_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->hide();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_select_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->handle_select_button();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_reset_cb", [](lv_event_t*) {
        if (!active_instance_)
            return;
        FilamentCatalogPickerModal* self = active_instance_;
        if (self->reset_callback_)
            self->reset_callback_();
        self->hide();
    });
    callbacks_registered_ = true;
}

void FilamentCatalogPickerModal::set_reset_callback(std::function<void()> cb) {
    reset_callback_ = std::move(cb);
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      SelectCallback on_select) {
    show(parent, std::move(seed_type), std::nullopt, std::move(on_select));
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      std::optional<std::vector<std::string>> allowed_types,
                                      SelectCallback on_select) {
    register_callbacks();
    seed_type_ = std::move(seed_type);
    allowed_types_ = std::move(allowed_types);
    on_select_ = std::move(on_select);
    Modal::show(parent); // triggers on_show()
}

void FilamentCatalogPickerModal::on_show() {
    active_instance_ = this;

    lv_obj_t* fragment = helix::ui::find_required(dialog(), "catalog_selector", get_name());
    selector_.attach(fragment);
    selector_.configure(seed_type_, allowed_types_);
    selector_.set_show_edit_affordances(true); // standalone picker allows catalog editing
    selector_.populate();

    // "Reset to defaults" is the leading (tertiary) action in the button row; gate it and
    // its divider on whether a reset callback was set before show() (preset-editing
    // context) — same gate the retired MaterialPickerMenu used.
    const bool show_reset = (reset_callback_ != nullptr);
    for (const char* n : {"btn_tertiary", "div_tertiary"}) {
        lv_obj_t* o = lv_obj_find_by_name(dialog(), n);
        if (!o)
            continue;
        if (show_reset) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FilamentCatalogPickerModal::on_hide() {
    if (active_instance_ == this)
        active_instance_ = nullptr;
    selector_.detach();
    selector_.clear_catalog(); // free the catalog; reloaded on next show()
}

void FilamentCatalogPickerModal::handle_select_button() {
    const helix::printer::EffectiveFilament* ef = selector_.highlighted();
    if (!ef) {
        spdlog::debug("[FilamentCatalogPicker] Select pressed with no product highlighted");
        return; // no-op; user must pick a row first
    }
    if (on_select_) {
        helix::printer::EffectiveFilament copy =
            *ef; // by value — nothing dangles after catalog reloads
        on_select_(copy);
    }
    hide();
}

} // namespace helix::ui
