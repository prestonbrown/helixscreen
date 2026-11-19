// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_material_z_offset.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_panel_common.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "material_z_offset.h"
#include "material_z_offset_state.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>

namespace helix::settings {

namespace {

/// The Z offset a keypad accepts, in mm either side of zero.
constexpr float kMaxOffsetMm = 5.0f;

/// "material_zoffset_<kind>_<material>", static storage: the XML keeps the pointer.
const char* subject_name(const char* kind, size_t row) {
    static std::array<std::array<std::string, MaterialZOffsetOverlay::kMaterials.size()>, 3> names;
    const size_t k = std::string(kind) == "value" ? 0 : std::string(kind) == "temp" ? 1 : 2;
    std::string& name = names[k][row];
    if (name.empty()) {
        std::string material = MaterialZOffsetOverlay::kMaterials[row];
        for (char& c : material) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        name = std::string("material_zoffset_") + kind + "_" + material;
    }
    return name.c_str();
}

/// The material the state writes: "" is DEFAULT.
std::string stored_material(const std::string& row_material) {
    return row_material == "DEFAULT" ? std::string() : row_material;
}

} // namespace

void MaterialZOffsetOverlay::init_subjects() {
    for (size_t i = 0; i < kMaterials.size(); ++i) {
        UI_MANAGED_SUBJECT_STRING(value_subjects_[i], value_bufs_[i], "", subject_name("value", i),
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(temp_subjects_[i], temp_bufs_[i], "", subject_name("temp", i),
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(mesh_subjects_[i], 0, subject_name("mesh", i), subjects_);
    }
    UI_MANAGED_SUBJECT_STRING(macro_line_subject_, macro_line_buf_,
                              "HELIX_MATERIAL_OFFSET MATERIAL={params.MATERIAL|default(\"\")}",
                              "material_zoffset_macro_line", subjects_);
    UI_MANAGED_SUBJECT_STRING(slicer_line_subject_, slicer_line_buf_,
                              "MATERIAL={filament_type[initial_extruder]}",
                              "material_zoffset_slicer_line", subjects_);
}

void MaterialZOffsetOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_material_zoffset_field_clicked",
         [](lv_event_t* e) {
             const auto* material = static_cast<const char*>(lv_event_get_user_data(e));
             get_material_z_offset_overlay().edit_offset(material ? material : "");
         }},
        {"on_material_zoffset_enabled_changed",
         [](lv_event_t* e) {
             material_zoffset::MaterialZOffsetState::instance().set_enabled(
                 get_moonraker_api(), helix::ui::event_checked(e));
         }},
    });
}

lv_obj_t* MaterialZOffsetOverlay::create(lv_obj_t* parent) {
    overlay_root_ = helix::ui::create_xml_hidden(parent, "material_z_offset_overlay");
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    return overlay_root_;
}

void MaterialZOffsetOverlay::before_show() {
    auto& state = material_zoffset::MaterialZOffsetState::instance();
    state.reconcile(get_moonraker_api());
    // The table fills from status while the overlay is open.
    rev_observer_ = helix::ui::observe<int>(
        state.rev_subject(), this,
        [](MaterialZOffsetOverlay* self, int) { self->refresh_values(); }, state.rev_lifetime());
    refresh_values();
}

void MaterialZOffsetOverlay::on_ui_destroyed() {
    rev_observer_.reset();
}

void MaterialZOffsetOverlay::refresh_values() {
    const auto& state = material_zoffset::MaterialZOffsetState::instance();
    const auto& hw = get_printer_state().get_discovery();
    const auto& table = state.table();
    for (size_t i = 0; i < kMaterials.size(); ++i) {
        const std::string material = stored_material(kMaterials[i]);
        const auto key = material_zoffset::material_key(hw, material);
        const auto it = key ? table.offsets.find(*key) : table.offsets.end();
        if (it != table.offsets.end()) {
            std::snprintf(value_bufs_[i], sizeof(value_bufs_[i]), "%+.3f mm", it->second / 1000.0);
        } else {
            std::snprintf(value_bufs_[i], sizeof(value_bufs_[i]), "%s", lv_tr("Not set"));
        }
        lv_subject_set_string(&value_subjects_[i], value_bufs_[i]);

        const int bed = material.empty() ? -1 : material_zoffset::usual_bed_temp(material);
        if (bed > 0) {
            std::snprintf(temp_bufs_[i], sizeof(temp_bufs_[i]), "%s",
                          fmt::format(fmt::runtime(lv_tr("Bed {} °C")), bed).c_str());
        } else {
            std::snprintf(temp_bufs_[i], sizeof(temp_bufs_[i]), "%s",
                          material.empty() ? lv_tr("Used when a material has no entry") : "");
        }
        lv_subject_set_string(&temp_subjects_[i], temp_bufs_[i]);

        lv_subject_set_int(&mesh_subjects_[i], key && table.meshes.count(*key) ? 1 : 0);
    }
}

void MaterialZOffsetOverlay::edit_offset(const std::string& row_material) {
    const std::string material = stored_material(row_material);
    const auto& state = material_zoffset::MaterialZOffsetState::instance();
    const auto stored = material_zoffset::applied_offset(get_printer_state().get_discovery(),
                                                         state.table(), material);
    ui_keypad_config_t config{};
    config.initial_value = static_cast<float>(stored.value_or(0)) / 1000.0f;
    config.min_value = -kMaxOffsetMm;
    config.max_value = kMaxOffsetMm;
    config.allow_decimal = true;
    config.allow_negative = true;
    config.max_fraction_digits = 3;
    // The keypad keeps the title pointer while it is open.
    editing_title_ = material.empty() ? std::string(lv_tr("Any other material")) : row_material;
    config.title_label = editing_title_.c_str();
    config.unit_label = "mm";
    config.on_confirm = [material](float v) {
        const int microns = static_cast<int>(std::lround(static_cast<double>(v) * 1000.0));
        material_zoffset::MaterialZOffsetState::instance().save_offset(
            material, microns, [](bool ok, const std::string& message) {
                ToastManager::instance().show(ok ? ToastSeverity::SUCCESS : ToastSeverity::ERROR,
                                              message.c_str(), ok ? 2500 : 5000);
            });
    };
    ui_keypad_show(&config);
}

} // namespace helix::settings
