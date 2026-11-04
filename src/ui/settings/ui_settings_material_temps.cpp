// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_material_temps.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_row_text.h"
#include "ui_temperature_utils.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "material_settings_manager.h"
#include "printer_state.h"
#include "sort_small.h"
#include "temperature_controller.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace helix::settings {

/// The bounds the edit inputs accept: properties of the fields themselves.
/// The printer's live ceiling is a send-time bound (TemperatureController),
/// never a database bound.
constexpr int kNozzleInputAbsMaxC = 500;
constexpr int kBedInputAbsMaxC = 200;
constexpr int kChamberInputAbsMaxC = 120;

// ============================================================================
// INITIALIZATION
// ============================================================================

void MaterialTempsOverlay::init_subjects() {
    // View toggle subject: 0=list, 1=editing
    UI_MANAGED_SUBJECT_INT(editing_subject_, 0, "material_editing", subjects_);

    // Edit view text subjects
    UI_MANAGED_SUBJECT_STRING(edit_name_subject_, edit_name_buf_, "", "material_edit_name",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(edit_defaults_subject_, edit_defaults_buf_, "",
                              "material_edit_defaults", subjects_);

    UI_MANAGED_SUBJECT_INT(has_macro_subject_, 0, "material_has_macro", subjects_);

    UI_MANAGED_SUBJECT_INT(shipped_subject_, 1, "material_edit_is_shipped", subjects_);
}

void MaterialTempsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_material_save", [](lv_event_t*) { get_material_temps_overlay().handle_save(); }},
        {"on_material_reset_defaults",
         [](lv_event_t*) { get_material_temps_overlay().handle_reset_defaults(); }},
        {"on_macro_dropdown_changed",
         [](lv_event_t*) { get_material_temps_overlay().handle_macro_dropdown_changed(); }},
        // The header's back button, so a tap in the edit view returns to the list.
        {"on_material_back_clicked", on_back_clicked},
    });
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* MaterialTempsOverlay::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ = helix::ui::create_xml_hidden(parent, "material_temps_overlay");
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Cache view refs
    list_view_ = helix::ui::find_required(overlay_root_, "material_list_view", get_name());
    edit_view_ = helix::ui::find_required(overlay_root_, "material_edit_view", get_name());
    macro_dropdown_ = helix::ui::find_required(edit_view_, "macro_dropdown", get_name());
    macro_heating_switch_ =
        helix::ui::find_required(edit_view_, "macro_heating_switch", get_name());

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void MaterialTempsOverlay::before_show() {
    show_list_view();
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void MaterialTempsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_material_list();
}

// ============================================================================
// LIST VIEW
// ============================================================================

void MaterialTempsOverlay::populate_material_list() {
    if (!list_view_) {
        return;
    }

    // Clear existing children. Using safe_clean_children rather than
    // lv_obj_clean because populate_material_list() runs from inside
    // LV_EVENT_CLICKED dispatch (handle_save / handle_back / handle_reset);
    // a sync clean in that context corrupts LVGL's event linked list. [L081]
    helix::ui::safe_clean_children(list_view_);

    // Sort materials alphabetically by name
    const auto table = filament::materials();
    std::vector<const filament::MaterialInfo*> sorted;
    sorted.reserve(table->size());
    for (const auto& mat : *table) {
        sorted.push_back(&mat);
    }
    helix::sort_small(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        return strcasecmp(a->name, b->name) < 0;
    });

    auto& mgr = MaterialSettingsManager::instance();

    for (const auto* entry : sorted) {
        const auto& mat = *entry;

        // The table rows already carry the user's overrides
        int nozzle_min = mat.nozzle_min;
        int nozzle_max = mat.nozzle_max;
        int bed_temp = mat.bed_temp;
        bool has_override = mgr.has_override(mat.name) && !mat.user_defined;

        char nozzle_buf[24];
        helix::ui::temperature::format_temperature_range(nozzle_min, nozzle_max, nozzle_buf,
                                                         sizeof(nozzle_buf), false);
        char temp_buf[48];
        snprintf(temp_buf, sizeof(temp_buf), "%s / %d°C", nozzle_buf, bed_temp);

        const char* attrs[] = {
            "temps_text", temp_buf, "hide_override", has_override ? "false" : "true", nullptr,
        };
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(list_view_, "material_temps_row", attrs));
        if (!row) {
            continue;
        }
        lv_obj_set_name(row, mat.name);
        helix::ui::set_row_label_text(row, "material_name", mat.name);
        lv_obj_add_event_cb(row, on_material_row_clicked, LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[{}] Populated {} materials", get_name(), sorted.size());
}

// ============================================================================
// EDIT VIEW
// ============================================================================

void MaterialTempsOverlay::show_edit_view(const std::string& material_name) {
    editing_material_ = material_name;

    // The shipped row is the default. A user-defined type has none, so its own
    // values stand in and there is nothing to reset to.
    auto shipped = filament::find_shipped_material(material_name);
    auto base = shipped ? shipped : filament::find_material(material_name);
    int default_nozzle_min = base ? base->nozzle_min : 0;
    int default_nozzle_max = base ? base->nozzle_max : 0;
    int default_bed = base ? base->bed_temp : 0;
    int default_chamber = base ? base->chamber_temp_c : 0;
    lv_subject_set_int(&shipped_subject_, shipped ? 1 : 0);

    // Get current effective values (with overrides if any)
    auto& mgr = MaterialSettingsManager::instance();
    const auto* ovr = mgr.get_override(material_name);
    int cur_nozzle_min = (ovr && ovr->nozzle_min) ? *ovr->nozzle_min : default_nozzle_min;
    int cur_nozzle_max = (ovr && ovr->nozzle_max) ? *ovr->nozzle_max : default_nozzle_max;
    int cur_bed = (ovr && ovr->bed_temp) ? *ovr->bed_temp : default_bed;
    int cur_chamber = (ovr && ovr->chamber_temp) ? *ovr->chamber_temp : default_chamber;

    // Update name subject
    snprintf(edit_name_buf_, sizeof(edit_name_buf_), "%s", material_name.c_str());
    lv_subject_copy_string(&edit_name_subject_, edit_name_buf_);

    // Update defaults hint. It mentions the chamber exactly when the chamber column
    // is shown, so it reads the capability that column binds.
    lv_subject_t* chamber_column = lv_xml_get_subject(nullptr, "printer_has_chamber_heater");
    const bool has_chamber = chamber_column && lv_subject_get_int(chamber_column) != 0;
    char nozzle_range[24];
    helix::ui::temperature::format_temperature_range(default_nozzle_min, default_nozzle_max,
                                                     nozzle_range, sizeof(nozzle_range));
    if (has_chamber) {
        snprintf(edit_defaults_buf_, sizeof(edit_defaults_buf_),
                 lv_tr("Default: %s nozzle, %d°C bed, %d°C chamber"), nozzle_range, default_bed,
                 default_chamber);
    } else {
        snprintf(edit_defaults_buf_, sizeof(edit_defaults_buf_),
                 lv_tr("Default: %s nozzle, %d°C bed"), nozzle_range, default_bed);
    }
    lv_subject_copy_string(&edit_defaults_subject_, edit_defaults_buf_);

    // Populate input fields. The chamber input is always filled even when its
    // column is hidden: handle_save() reads it back, which keeps a stored
    // chamber override intact on a printer whose heater is (currently) absent.
    if (edit_view_) {
        lv_obj_t* nozzle_min_input =
            helix::ui::find_required(edit_view_, "edit_nozzle_min", get_name());
        lv_obj_t* nozzle_max_input =
            helix::ui::find_required(edit_view_, "edit_nozzle_max", get_name());
        lv_obj_t* bed_temp_input =
            helix::ui::find_required(edit_view_, "edit_bed_temp", get_name());
        lv_obj_t* chamber_temp_input =
            helix::ui::find_required(edit_view_, "edit_chamber_temp", get_name());

        char buf[8];
        if (nozzle_min_input) {
            snprintf(buf, sizeof(buf), "%d", cur_nozzle_min);
            lv_textarea_set_text(nozzle_min_input, buf);
        }
        if (nozzle_max_input) {
            snprintf(buf, sizeof(buf), "%d", cur_nozzle_max);
            lv_textarea_set_text(nozzle_max_input, buf);
        }
        if (bed_temp_input) {
            snprintf(buf, sizeof(buf), "%d", cur_bed);
            lv_textarea_set_text(bed_temp_input, buf);
        }
        if (chamber_temp_input) {
            snprintf(buf, sizeof(buf), "%d", cur_chamber);
            lv_textarea_set_text(chamber_temp_input, buf);
        }
    }

    // Populate macro dropdown and select current override
    populate_macro_dropdown();
    std::string current_macro;
    bool switch_on = true; // Default: macro handles heating
    if (ovr && ovr->preheat_macro && !ovr->preheat_macro->empty()) {
        current_macro = *ovr->preheat_macro;
        switch_on = ovr->macro_handles_heating.value_or(true);
    }
    // Select the current macro in the dropdown
    if (macro_dropdown_) {
        uint32_t sel = 0; // Default to "None"
        for (size_t i = 0; i < macro_names_.size(); ++i) {
            if (macro_names_[i] == current_macro) {
                sel = static_cast<uint32_t>(i);
                break;
            }
        }
        lv_dropdown_set_selected(macro_dropdown_, sel);
    }
    lv_subject_set_int(&has_macro_subject_, current_macro.empty() ? 0 : 1);

    if (macro_heating_switch_) {
        if (switch_on) {
            lv_obj_add_state(macro_heating_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(macro_heating_switch_, LV_STATE_CHECKED);
        }
    }

    // Switch to edit view
    lv_subject_set_int(&editing_subject_, 1);
    spdlog::debug("[{}] Editing material: {}", get_name(), material_name);
}

void MaterialTempsOverlay::show_list_view() {
    editing_material_.clear();
    lv_subject_set_int(&editing_subject_, 0);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MaterialTempsOverlay::handle_material_row_clicked(const std::string& material_name) {
    spdlog::debug("[{}] Material row clicked: {}", get_name(), material_name);
    show_edit_view(material_name);
}

void MaterialTempsOverlay::handle_save() {
    if (editing_material_.empty() || !edit_view_) {
        return;
    }

    // Read input values
    lv_obj_t* nozzle_min_input =
        helix::ui::find_required(edit_view_, "edit_nozzle_min", get_name());
    lv_obj_t* nozzle_max_input =
        helix::ui::find_required(edit_view_, "edit_nozzle_max", get_name());
    lv_obj_t* bed_temp_input = helix::ui::find_required(edit_view_, "edit_bed_temp", get_name());
    lv_obj_t* chamber_temp_input =
        helix::ui::find_required(edit_view_, "edit_chamber_temp", get_name());

    if (!nozzle_min_input || !nozzle_max_input || !bed_temp_input || !chamber_temp_input) {
        return;
    }

    const char* min_text = lv_textarea_get_text(nozzle_min_input);
    const char* max_text = lv_textarea_get_text(nozzle_max_input);
    const char* bed_text = lv_textarea_get_text(bed_temp_input);
    const char* chamber_text = lv_textarea_get_text(chamber_temp_input);

    if (!min_text || !min_text[0] || !max_text || !max_text[0] || !bed_text || !bed_text[0] ||
        !chamber_text || !chamber_text[0]) {
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("All fields are required"),
                                      3000);
        return;
    }

    int nozzle_min = atoi(min_text);
    int nozzle_max = atoi(max_text);
    int bed_temp = atoi(bed_text);
    int chamber_temp = atoi(chamber_text);

    // The edit view enforces only the fields' own bounds. The printer's live
    // ceiling is applied where a target is sent (TemperatureController), so a
    // material definition can carry values the current printer cannot reach -
    // they clamp at send, not at save.
    if (nozzle_min < 100 || nozzle_max < 100 || nozzle_min > kNozzleInputAbsMaxC ||
        nozzle_max > kNozzleInputAbsMaxC) {
        char msg[kToastBufBytes];
        snprintf(msg, sizeof(msg), lv_tr("Nozzle temp must be 100-%d°C"), kNozzleInputAbsMaxC);
        ToastManager::instance().show(ToastSeverity::WARNING, msg, 3000);
        return;
    }
    if (bed_temp < 0 || bed_temp > kBedInputAbsMaxC) {
        char msg[kToastBufBytes];
        snprintf(msg, sizeof(msg), lv_tr("Bed temp must be 0-%d°C"), kBedInputAbsMaxC);
        ToastManager::instance().show(ToastSeverity::WARNING, msg, 3000);
        return;
    }
    if (chamber_temp < 0 || chamber_temp > kChamberInputAbsMaxC) {
        char msg[kToastBufBytes];
        snprintf(msg, sizeof(msg), lv_tr("Chamber temp must be 0-%d°C"), kChamberInputAbsMaxC);
        ToastManager::instance().show(ToastSeverity::WARNING, msg, 3000);
        return;
    }
    if (nozzle_min > nozzle_max) {
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Nozzle min cannot exceed max"),
                                      3000);
        return;
    }

    // Sparse against the shipped row. A user-defined type has no shipped row,
    // so every value is written: they are its definition.
    filament::MaterialOverride ovr;
    const auto shipped = filament::find_shipped_material(editing_material_);
    if (!shipped || nozzle_min != shipped->nozzle_min)
        ovr.nozzle_min = nozzle_min;
    if (!shipped || nozzle_max != shipped->nozzle_max)
        ovr.nozzle_max = nozzle_max;
    if (!shipped || bed_temp != shipped->bed_temp)
        ovr.bed_temp = bed_temp;
    if (!shipped || chamber_temp != shipped->chamber_temp_c)
        ovr.chamber_temp = chamber_temp;

    // Add macro override from dropdown selection
    if (macro_dropdown_) {
        uint32_t sel = lv_dropdown_get_selected(macro_dropdown_);
        if (sel < macro_names_.size() && !macro_names_[sel].empty()) {
            ovr.preheat_macro = macro_names_[sel];
            bool switch_checked =
                macro_heating_switch_ && lv_obj_has_state(macro_heating_switch_, LV_STATE_CHECKED);
            if (!switch_checked) {
                ovr.macro_handles_heating = false;
            }
        }
    }

    // Only save if there are actual overrides
    if (ovr.nozzle_min || ovr.nozzle_max || ovr.bed_temp || ovr.chamber_temp || ovr.preheat_macro) {
        MaterialSettingsManager::instance().set_override(editing_material_, ovr);
    } else {
        // All values match defaults — clear any existing override
        MaterialSettingsManager::instance().clear_override(editing_material_);
    }

    spdlog::info("[{}] Saved overrides for {}", get_name(), editing_material_);
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Temperatures saved"), 2000);

    // Return to list view and refresh, preserving scroll position
    int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
    show_list_view();
    populate_material_list();
    if (list_view_ && scroll_y > 0) {
        lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
    }
}

void MaterialTempsOverlay::handle_back_clicked() {
    if (lv_subject_get_int(&editing_subject_) != 0) {
        // In edit view — go back to list, preserving scroll position
        int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
        show_list_view();
        populate_material_list();
        if (list_view_ && scroll_y > 0) {
            lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
        }
    } else {
        // In list view — close overlay
        helix::nav::go_back();
    }
}

void MaterialTempsOverlay::handle_reset_defaults() {
    if (editing_material_.empty()) {
        return;
    }

    MaterialSettingsManager::instance().clear_override(editing_material_);
    spdlog::info("[{}] Reset {} to defaults", get_name(), editing_material_);

    // Return to list view and refresh, preserving scroll position
    int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
    show_list_view();
    populate_material_list();
    if (list_view_ && scroll_y > 0) {
        lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
    }
}

// ============================================================================
// MACRO DROPDOWN
// ============================================================================

void MaterialTempsOverlay::populate_macro_dropdown() {
    if (!macro_dropdown_)
        return;

    macro_names_.clear();
    macro_names_.push_back(""); // Index 0 = "None" (empty string = no macro)

    std::string options = lv_tr("None");

    auto* api = get_moonraker_api();
    if (api) {
        const auto& macros = api->hardware().macros();
        std::vector<std::string> sorted(macros.begin(), macros.end());
        std::sort(sorted.begin(), sorted.end());

        for (const auto& name : sorted) {
            if (!name.empty() && name[0] == '_')
                continue; // Skip system macros
            macro_names_.push_back(name);
            options += "\n";
            options += name;
        }
    }

    lv_dropdown_set_options(macro_dropdown_, options.c_str());
}

void MaterialTempsOverlay::handle_macro_dropdown_changed() {
    if (!macro_dropdown_)
        return;

    uint32_t sel = lv_dropdown_get_selected(macro_dropdown_);
    bool has_macro = sel < macro_names_.size() && !macro_names_[sel].empty();
    lv_subject_set_int(&has_macro_subject_, has_macro ? 1 : 0);

    spdlog::debug("[{}] Macro dropdown changed: index={} name='{}'", get_name(), sel,
                  (sel < macro_names_.size()) ? macro_names_[sel] : "(invalid)");
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

// Attached with lv_obj_add_event_cb: the rows and the back button are built in
// code, so there is no XML slot to register against.
void MaterialTempsOverlay::on_material_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_material_row_clicked");
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* name = lv_obj_get_name(row);
    if (name) {
        get_material_temps_overlay().handle_material_row_clicked(name);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_back_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_back_clicked");
    get_material_temps_overlay().handle_back_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
