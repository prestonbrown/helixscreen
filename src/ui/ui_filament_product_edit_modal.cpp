// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_product_edit_modal.h"

#include "ui_toast_manager.h"

#include "filament_catalog.h"
#include "filament_database.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <vector>

namespace helix::ui {

using helix::printer::EffectiveFilament;
using helix::printer::FilamentCatalog;

namespace {

// Built-in catalog search paths, mirroring filament_catalog.cpp's private
// BUILTIN_PATHS. Used only to answer "is this id shipped?" for the secondary
// button label (Delete vs Restore Defaults); the resolved values themselves
// come from FilamentCatalog::load_full().
const char* BUILTIN_PATHS[] = {"assets/filaments.json", "../assets/filaments.json",
                               "/opt/helixscreen/assets/filaments.json"};

std::string first_existing_builtin() {
    for (const char* p : BUILTIN_PATHS) {
        std::ifstream f(p);
        if (f.is_open())
            return p;
    }
    return "";
}

/// True if @p id is present in the shipped (built-in-only) catalog.
bool id_in_builtin(const std::string& id) {
    const std::string path = first_existing_builtin();
    if (path.empty())
        return false;
    FilamentCatalog builtin = FilamentCatalog::load_from_file(path, /*codes_only=*/false, "");
    return builtin.resolve_id(id) != nullptr;
}

/// True if an authored overlay entry already exists for @p id.
bool id_in_overlay(const std::string& id) {
    for (const auto& p : FilamentCatalog::load_user_products()) {
        if (p.is_object() && p.value("id", "") == id)
            return true;
    }
    return false;
}

void set_input_text(lv_obj_t* dialog, const char* name, const std::string& text) {
    lv_obj_t* w = lv_obj_find_by_name(dialog, name);
    if (w)
        lv_textarea_set_text(w, text.c_str());
}

std::string get_input_text(lv_obj_t* dialog, const char* name) {
    lv_obj_t* w = lv_obj_find_by_name(dialog, name);
    if (!w)
        return {};
    const char* t = lv_textarea_get_text(w);
    return t ? t : "";
}

// Numeric field -> its display string; blank when <= 0 so an unset/zero value
// shows as an empty (omittable) field rather than a literal "0".
std::string int_to_field(int v) {
    return v > 0 ? std::to_string(v) : std::string{};
}

std::string density_to_field(float v) {
    if (v <= 0.0f)
        return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

} // namespace

// Static members
bool FilamentProductEditModal::callbacks_registered_ = false;
FilamentProductEditModal* FilamentProductEditModal::active_instance_ = nullptr;

FilamentProductEditModal::FilamentProductEditModal() = default;

FilamentProductEditModal::~FilamentProductEditModal() {
    if (active_instance_ == this)
        active_instance_ = nullptr;
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&secondary_text_subject_);
        subjects_initialized_ = false;
    }
}

void FilamentProductEditModal::set_on_saved(SavedCallback cb) {
    on_saved_ = std::move(cb);
}

// ============================================================================
// Show entry points
// ============================================================================

bool FilamentProductEditModal::show_for_add(lv_obj_t* parent) {
    register_callbacks();
    init_subjects();
    mode_ = Mode::Add;
    edit_id_.clear();
    is_builtin_ = false;
    has_overlay_ = false;
    return Modal::show(parent);
}

bool FilamentProductEditModal::show_for_edit(lv_obj_t* parent, const std::string& product_id) {
    register_callbacks();
    init_subjects();
    mode_ = Mode::Edit;
    edit_id_ = product_id;
    is_builtin_ = id_in_builtin(product_id);
    has_overlay_ = id_in_overlay(product_id);
    return Modal::show(parent);
}

// ============================================================================
// Modal hooks
// ============================================================================

void FilamentProductEditModal::on_show() {
    active_instance_ = this;

    // Title.
    lv_obj_t* title = find_widget("header_title");
    if (title) {
        lv_label_set_text(title,
                          mode_ == Mode::Add ? lv_tr("Add Filament") : lv_tr("Edit Filament"));
    }

    populate_fields();
    register_keyboards();
    configure_secondary_button();
}

void FilamentProductEditModal::on_hide() {
    active_instance_ = nullptr;
}

// ============================================================================
// Internal
// ============================================================================

void FilamentProductEditModal::init_subjects() {
    if (subjects_initialized_)
        return;
    lv_subject_init_string(&secondary_text_subject_, secondary_text_buf_, nullptr,
                           sizeof(secondary_text_buf_), lv_tr("Delete"));
    lv_xml_register_subject(nullptr, "filament_product_secondary_text", &secondary_text_subject_);
    subjects_initialized_ = true;
}

void FilamentProductEditModal::populate_type_dropdown(const std::string& selected_type) {
    lv_obj_t* dd = find_widget("type_dropdown");
    if (!dd)
        return;
    auto names = filament::get_all_material_names(); // std::vector<const char*>
    std::string options;
    uint32_t sel = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0)
            options += '\n';
        options += names[i];
        if (!selected_type.empty() && selected_type == names[i])
            sel = static_cast<uint32_t>(i);
    }
    lv_dropdown_set_options(dd, options.c_str());
    lv_dropdown_set_selected(dd, sel);
}

void FilamentProductEditModal::populate_fields() {
    if (!dialog_)
        return;

    if (mode_ == Mode::Add) {
        set_input_text(dialog_, "field_id", "");
        set_input_text(dialog_, "field_brand", "");
        set_input_text(dialog_, "field_name", "");
        set_input_text(dialog_, "field_nozzle_min", "");
        set_input_text(dialog_, "field_nozzle_max", "");
        set_input_text(dialog_, "field_nozzle", "");
        set_input_text(dialog_, "field_bed", "");
        set_input_text(dialog_, "field_density", "");
        populate_type_dropdown(""); // defaults to first material
        return;
    }

    // Edit: pre-fill from the resolved (inheritance-applied) catalog entry so
    // unchanged fields display sensible values.
    FilamentCatalog full = FilamentCatalog::load_full();
    const EffectiveFilament* ef = full.resolve_id(edit_id_);
    if (!ef) {
        spdlog::warn("[FilamentProductEditModal] edit id '{}' did not resolve", edit_id_);
        set_input_text(dialog_, "field_id", edit_id_);
        populate_type_dropdown("");
        return;
    }

    set_input_text(dialog_, "field_id", ef->id);
    set_input_text(dialog_, "field_brand", ef->brand);
    set_input_text(dialog_, "field_name", ef->name);
    set_input_text(dialog_, "field_nozzle_min", int_to_field(ef->nozzle_min));
    set_input_text(dialog_, "field_nozzle_max", int_to_field(ef->nozzle_max));
    set_input_text(dialog_, "field_nozzle", int_to_field(ef->nozzle_recommended));
    set_input_text(dialog_, "field_bed", int_to_field(ef->bed_temp));
    set_input_text(dialog_, "field_density", density_to_field(ef->density_g_cm3));
    populate_type_dropdown(ef->type);
}

void FilamentProductEditModal::register_keyboards() {
    if (!dialog_)
        return;
    static constexpr const char* fields[] = {
        "field_id",     "field_brand",      "field_name", "field_nozzle_min",
        "field_nozzle", "field_nozzle_max", "field_bed",  "field_density",
    };
    for (const char* name : fields) {
        lv_obj_t* w = lv_obj_find_by_name(dialog_, name);
        if (w)
            modal_register_keyboard(dialog_, w);
    }
}

FilamentFormValues FilamentProductEditModal::read_form() const {
    FilamentFormValues v;
    if (!dialog_)
        return v;
    v.id = get_input_text(dialog_, "field_id");
    v.brand = get_input_text(dialog_, "field_brand");
    v.name = get_input_text(dialog_, "field_name");

    if (lv_obj_t* dd = lv_obj_find_by_name(dialog_, "type_dropdown")) {
        char buf[64] = {};
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
        v.type = buf;
    }

    v.nozzle_min = get_input_text(dialog_, "field_nozzle_min");
    v.nozzle_max = get_input_text(dialog_, "field_nozzle_max");
    v.nozzle = get_input_text(dialog_, "field_nozzle");
    v.bed = get_input_text(dialog_, "field_bed");
    v.density = get_input_text(dialog_, "field_density");
    return v;
}

void FilamentProductEditModal::configure_secondary_button() {
    lv_obj_t* btn = find_widget("btn_secondary");
    if (!btn)
        return;

    // Add mode has no destructive action; edit of a pristine built-in (no
    // override yet) likewise has nothing to remove. Hide the button in both.
    const bool show_secondary = (mode_ == Mode::Edit) && (has_overlay_ || !is_builtin_);
    if (!show_secondary) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    // Built-in with an override -> restore; purely user entry -> delete.
    lv_subject_copy_string(&secondary_text_subject_,
                           is_builtin_ ? lv_tr("Restore Defaults") : lv_tr("Delete"));
}

// ============================================================================
// Actions
// ============================================================================

void FilamentProductEditModal::handle_save() {
    FilamentFormValues v = read_form();

    std::string error;
    if (!validate_product_form(v, error)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr(error.c_str()), 3000);
        return; // keep modal open
    }

    const nlohmann::json sparse = build_product_json(v);
    const std::string id = sparse.value("id", "");

    auto products = FilamentCatalog::load_user_products();

    // Preserve any previously authored fields (codes, etc.) for this id by
    // merge-patching the sparse form values over the existing overlay entry.
    nlohmann::json to_write = sparse;
    for (const auto& p : products) {
        if (p.is_object() && p.value("id", "") == id) {
            to_write = p;                 // start from the existing authored entry
            to_write.merge_patch(sparse); // overlay the form's fields
            break;
        }
    }
    FilamentCatalog::upsert_product(products, to_write); // replace-by-id or append

    if (!FilamentCatalog::save_user_products(products)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Could not save filament"), 3000);
        return;
    }

    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Filament saved"), 2000);
    spdlog::info("[FilamentProductEditModal] saved product '{}'", id);
    if (on_saved_)
        on_saved_(id);
    hide();
}

void FilamentProductEditModal::handle_secondary() {
    if (mode_ != Mode::Edit || edit_id_.empty()) {
        hide();
        return;
    }

    auto products = FilamentCatalog::load_user_products();
    FilamentCatalog::remove_product(products, edit_id_);
    if (!FilamentCatalog::save_user_products(products)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Could not update filaments"),
                                      3000);
        return;
    }

    ToastManager::instance().show(
        ToastSeverity::SUCCESS,
        is_builtin_ ? lv_tr("Defaults restored") : lv_tr("Filament deleted"), 2000);
    spdlog::info("[FilamentProductEditModal] removed overlay entry '{}'", edit_id_);
    if (on_saved_)
        on_saved_(std::string{}); // removed: nothing to focus
    hide();
}

void FilamentProductEditModal::handle_close() {
    hide();
}

// ============================================================================
// Callback registration + dispatch
// ============================================================================

void FilamentProductEditModal::register_callbacks() {
    if (callbacks_registered_)
        return;
    lv_xml_register_event_cb(nullptr, "filament_product_save_cb", on_save_cb);
    lv_xml_register_event_cb(nullptr, "filament_product_secondary_cb", on_secondary_cb);
    lv_xml_register_event_cb(nullptr, "filament_product_close_cb", on_close_cb);
    callbacks_registered_ = true;
}

void FilamentProductEditModal::on_save_cb(lv_event_t* /*e*/) {
    if (active_instance_)
        active_instance_->handle_save();
}

void FilamentProductEditModal::on_secondary_cb(lv_event_t* /*e*/) {
    if (active_instance_)
        active_instance_->handle_secondary();
}

void FilamentProductEditModal::on_close_cb(lv_event_t* /*e*/) {
    if (active_instance_)
        active_instance_->handle_close();
}

} // namespace helix::ui
