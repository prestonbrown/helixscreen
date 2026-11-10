// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_picker.h"

#include "helix_psram_attr.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
// Bound only by the picker's XML and set before each show; one picker is up at a time.
helix::WidgetSubjects s_subjects{"FilamentCatalogPickerSubjects",
                                 StaticSubjectRegistry::Lifetime::Process};
// 1 = "Reset to defaults" is offered: a preset-editing caller set a reset callback.
HELIX_PSRAM_BSS lv_subject_t s_reset_shown_subject;

void init_picker_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_reset_shown_subject, 0, "catalog_picker_reset_shown", s_subjects);
    });
}
} // namespace

bool FilamentCatalogPickerModal::callbacks_registered_ = false;

void FilamentCatalogPickerModal::register_callbacks() {
    if (callbacks_registered_)
        return;
    FilamentCatalogSelector::register_callbacks();
    lv_xml_register_event_cb(nullptr, "catalog_picker_close_cb", [](lv_event_t* e) {
        if (auto* self = Modal::from_event<FilamentCatalogPickerModal>(e))
            self->hide();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_select_cb", [](lv_event_t* e) {
        if (auto* self = Modal::from_event<FilamentCatalogPickerModal>(e))
            self->handle_select_button();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_reset_cb", [](lv_event_t* e) {
        auto* self = Modal::from_event<FilamentCatalogPickerModal>(e);
        if (!self)
            return;
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
    init_picker_subjects();
    lv_subject_set_int(&s_reset_shown_subject, reset_callback_ ? 1 : 0);
    Modal::show(parent); // triggers on_show()
}

void FilamentCatalogPickerModal::on_show() {
    lv_obj_t* fragment = helix::ui::find_required(dialog(), "catalog_selector", get_name());
    selector_.attach(fragment);
    selector_.configure(seed_type_, allowed_types_);
    selector_.set_show_edit_affordances(true); // standalone picker allows catalog editing
    selector_.populate();
}

void FilamentCatalogPickerModal::on_hide() {
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
