// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_wizard.h"

#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_search_debounce.h"
#include "ui_subject_registry.h"
#include "ui_swatch.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "color_utils.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "i_moonraker_api.h"
#include "static_panel_registry.h"
#include "text_io.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <unordered_map>

#include "hv/json.hpp"

namespace {

/// Maximum input lengths for sanity checking
constexpr size_t MAX_VENDOR_NAME_LEN = 256;
constexpr size_t MAX_VENDOR_URL_LEN = 2048;

/// Spoolman stores one integer temperature per filament. A catalog range
/// becomes its midpoint; a range with one end set becomes that end.
void set_spoolman_temp(nlohmann::json& data, const char* key, int min_val, int max_val) {
    if (min_val > 0 && max_val > 0) {
        data[key] = (min_val + max_val) / 2;
    } else if (min_val > 0 || max_val > 0) {
        data[key] = std::max(min_val, max_val);
    }
}

} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

// Out of line: ColorPicker is incomplete in the header.
SpoolWizardOverlay::SpoolWizardOverlay() = default;

SpoolWizardOverlay::~SpoolWizardOverlay() {
    cancel_search_timer();
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void SpoolWizardOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Step subject — drives step visibility in XML via bind_flag_if_not_eq
        UI_MANAGED_SUBJECT_INT(step_subject_, static_cast<int32_t>(Step::VENDOR),
                               "spool_wizard_step", subjects_);

        // Can proceed — drives Next/Create button disabled state
        UI_MANAGED_SUBJECT_INT(can_proceed_subject_, 0, "spool_wizard_can_proceed", subjects_);

        // Step label string — "Step 1 of 3"
        std::snprintf(step_label_buf_, sizeof(step_label_buf_), "%s", lv_tr("Step 1 of 3"));
        UI_MANAGED_SUBJECT_STRING(step_label_subject_, step_label_buf_, step_label_buf_,
                                  "spool_wizard_step_label", subjects_);

        // Creating spinner state
        UI_MANAGED_SUBJECT_INT(creating_subject_, 0, "wizard_creating", subjects_);

        // Selected vendor name display (step 1 header)
        UI_MANAGED_SUBJECT_STRING(selected_vendor_name_subject_, selected_vendor_name_buf_, "",
                                  "wizard_selected_vendor_name", subjects_);

        // Summary fields (step 2)
        UI_MANAGED_SUBJECT_STRING(summary_vendor_subject_, summary_vendor_buf_, "",
                                  "wizard_summary_vendor", subjects_);
        UI_MANAGED_SUBJECT_STRING(summary_filament_subject_, summary_filament_buf_, "",
                                  "wizard_summary_filament", subjects_);
        UI_MANAGED_SUBJECT_COLOR(summary_color_subject_, lv_color_hex(0x808080),
                                 "wizard_summary_color", subjects_);
        UI_MANAGED_SUBJECT_INT(summary_edge_subject_, 0, "wizard_summary_edge", subjects_);

        // Create vendor/filament form visibility toggles
        UI_MANAGED_SUBJECT_INT(show_create_vendor_subject_, 0, "spool_wizard_show_create_vendor",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(show_create_filament_subject_, 0,
                               "spool_wizard_show_create_filament", subjects_);

        // List state subjects
        UI_MANAGED_SUBJECT_INT(vendor_count_subject_, -1, "spool_wizard_vendor_count", subjects_);
        UI_MANAGED_SUBJECT_INT(filament_count_subject_, -1, "spool_wizard_filament_count",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(vendors_loading_subject_, 0, "spool_wizard_vendors_loading",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(filaments_loading_subject_, 0, "spool_wizard_filaments_loading",
                               subjects_);

        // Can create vendor (form validation)
        UI_MANAGED_SUBJECT_INT(can_create_vendor_subject_, 0, "spool_wizard_can_create_vendor",
                               subjects_);

        // SpoolmanDB search: whether this server has it, what the section
        // shows, and one row's worth of subjects per result
        UI_MANAGED_SUBJECT_INT(catalog_available_subject_, 0, "spool_db_available", subjects_);
        UI_MANAGED_SUBJECT_INT(catalog_state_subject_, 0, "spool_db_state", subjects_);
        UI_MANAGED_SUBJECT_INT(catalog_count_subject_, 0, "spool_db_result_count", subjects_);
        const size_t rows = static_cast<size_t>(helix::SpoolmanCatalogSearch::kResultLimit);
        catalog_titles_.ensure_size(rows);
        catalog_details_.ensure_size(rows);
        catalog_colors_.ensure_size(rows);
        catalog_edges_.ensure_size(rows);
    });
}

void SpoolWizardOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
    catalog_titles_.reclaim();
    catalog_details_.reclaim();
    catalog_colors_.reclaim();
    catalog_edges_.reclaim();
}

// ============================================================================
// Callback Registration
// ============================================================================

void SpoolWizardOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_wizard_back",
         [](lv_event_t*) {
             spdlog::debug("[SpoolWizard] Back clicked");
             get_global_spool_wizard().navigate_back();
         }},
        {"on_wizard_next",
         [](lv_event_t*) {
             spdlog::debug("[SpoolWizard] Next clicked");
             get_global_spool_wizard().navigate_next();
         }},
        {"on_wizard_create",
         [](lv_event_t*) {
             spdlog::debug("[SpoolWizard] Create clicked");
             get_global_spool_wizard().on_create_requested();
         }},
        {"on_wizard_vendor_selected",
         [](lv_event_t* e) {
             lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
             auto index =
                 static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
             spdlog::debug("[SpoolWizard] Vendor selected, index={}", index);
             get_global_spool_wizard().select_vendor(index);
         }},
        {"on_wizard_show_create_vendor_modal",
         [](lv_event_t*) { get_global_spool_wizard().show_create_vendor_modal(); }},
        {"on_wizard_cancel_create_vendor",
         [](lv_event_t*) {
             spdlog::debug("[SpoolWizard] Cancel create vendor");
             auto& wiz = get_global_spool_wizard();
             if (wiz.create_vendor_dialog_) {
                 Modal::hide(wiz.create_vendor_dialog_);
                 wiz.create_vendor_dialog_ = nullptr;
             }
         }},
        {"on_wizard_vendor_search_changed",
         [](lv_event_t*) { get_global_spool_wizard().on_search_key(); }},
        {"on_wizard_catalog_result_selected",
         [](lv_event_t* e) {
             const char* index = static_cast<const char*>(lv_event_get_user_data(e));
             get_global_spool_wizard().select_catalog_result(index ? std::atoi(index) : -1);
         }},
        {"on_wizard_new_vendor_name_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             spdlog::debug("[SpoolWizard] New vendor name: '{}'", text ? text : "");
             auto& wiz = get_global_spool_wizard();
             wiz.set_new_vendor(text ? text : "", wiz.new_vendor_url_);
         }},
        {"on_wizard_new_vendor_url_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             spdlog::debug("[SpoolWizard] New vendor URL: '{}'", text ? text : "");
             auto& wiz = get_global_spool_wizard();
             wiz.set_new_vendor(wiz.new_vendor_name_, text ? text : "");
         }},
        {"on_wizard_confirm_create_vendor",
         [](lv_event_t*) { get_global_spool_wizard().confirm_create_vendor(); }},
        {"on_wizard_filament_selected",
         [](lv_event_t* e) {
             lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
             auto index =
                 static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
             spdlog::debug("[SpoolWizard] Filament selected, index={}", index);
             get_global_spool_wizard().select_filament(index);
         }},
        {"on_wizard_show_create_filament_modal",
         [](lv_event_t*) { get_global_spool_wizard().show_create_filament_modal(); }},
        {"on_wizard_cancel_create_filament",
         [](lv_event_t*) {
             spdlog::debug("[SpoolWizard] Cancel create filament");
             auto& wiz = get_global_spool_wizard();
             wiz.creating_new_filament_ = false;
             if (wiz.create_filament_dialog_) {
                 Modal::hide(wiz.create_filament_dialog_);
                 wiz.create_filament_dialog_ = nullptr;
             }
         }},
        {"on_wizard_material_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
             char buf[64] = {};
             lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
             spdlog::debug("[SpoolWizard] Material changed: '{}'", buf);
             get_global_spool_wizard().set_new_filament_material(buf);
         }},
        {"on_wizard_new_filament_name_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             spdlog::debug("[SpoolWizard] New filament name: '{}'", text ? text : "");
             auto& wiz = get_global_spool_wizard();
             wiz.new_filament_name_ = text ? text : "";
         }},
        {"on_wizard_pick_filament_color",
         [](lv_event_t*) { get_global_spool_wizard().pick_filament_color(); }},
        {"on_wizard_nozzle_temp_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             int val = text ? std::atoi(text) : 0;
             auto& wiz = get_global_spool_wizard();

             // Determine if this is min or max based on widget name
             const char* name = lv_obj_get_name(ta);
             if (name && std::string_view(name) == "nozzle_temp_min") {
                 wiz.new_filament_nozzle_min_ = val;
             } else {
                 wiz.new_filament_nozzle_max_ = val;
             }

             spdlog::debug("[SpoolWizard] Nozzle temp changed: {}-{}", wiz.new_filament_nozzle_min_,
                           wiz.new_filament_nozzle_max_);
         }},
        {"on_wizard_bed_temp_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             int val = text ? std::atoi(text) : 0;
             auto& wiz = get_global_spool_wizard();

             const char* name = lv_obj_get_name(ta);
             if (name && std::string_view(name) == "bed_temp_min") {
                 wiz.new_filament_bed_min_ = val;
             } else {
                 wiz.new_filament_bed_max_ = val;
             }

             spdlog::debug("[SpoolWizard] Bed temp changed: {}-{}", wiz.new_filament_bed_min_,
                           wiz.new_filament_bed_max_);
         }},
        {"on_wizard_filament_weight_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.new_filament_weight_ = text ? std::atof(text) : 0;
             spdlog::debug("[SpoolWizard] Filament weight: {:.0f}g", wiz.new_filament_weight_);
         }},
        {"on_wizard_spool_weight_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.new_filament_spool_weight_ = text ? std::atof(text) : 0;
             spdlog::debug("[SpoolWizard] Spool weight: {:.0f}g", wiz.new_filament_spool_weight_);
         }},
        {"on_wizard_confirm_create_filament",
         [](lv_event_t*) { get_global_spool_wizard().confirm_create_filament(); }},
        {"on_wizard_remaining_weight_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.spool_remaining_weight_ = text ? std::atof(text) : 0;
             wiz.set_can_proceed(wiz.spool_remaining_weight_ > 0);
             spdlog::debug("[SpoolWizard] Remaining weight: {:.0f}g", wiz.spool_remaining_weight_);
         }},
        {"on_wizard_price_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.spool_price_ = text ? std::atof(text) : 0;
             spdlog::debug("[SpoolWizard] Price: {:.2f}", wiz.spool_price_);
         }},
        {"on_wizard_lot_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.spool_lot_nr_ = text ? text : "";
             spdlog::debug("[SpoolWizard] Lot: '{}'", wiz.spool_lot_nr_);
         }},
        {"on_wizard_notes_changed",
         [](lv_event_t* e) {
             lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
             const char* text = lv_textarea_get_text(ta);
             auto& wiz = get_global_spool_wizard();
             wiz.spool_notes_ = text ? text : "";
             spdlog::debug("[SpoolWizard] Notes: '{}'", wiz.spool_notes_);
         }},
    });
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void SpoolWizardOverlay::on_activate() {
    OverlayBase::on_activate();
    spdlog::debug("[{}] on_activate()", get_name());

    // Reset ALL wizard state for a fresh session
    reset_state();

    // Reset wizard to step 0
    navigate_to_step(Step::VENDOR);

    // Load vendors for step 0
    load_vendors();
    probe_catalog_search();
}

void SpoolWizardOverlay::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] on_deactivating()", get_name());

    // A search answering after the wizard closed has nothing to show.
    cancel_search_timer();
    catalog_.invalidate();
    catalog_in_flight_ = false;
    catalog_has_pending_ = false;

    // Close create vendor modal if open
    if (create_vendor_dialog_) {
        Modal::hide(create_vendor_dialog_);
        create_vendor_dialog_ = nullptr;
    }

    // Close create filament modal if open
    if (create_filament_dialog_) {
        Modal::hide(create_filament_dialog_);
        create_filament_dialog_ = nullptr;
    }
}

void SpoolWizardOverlay::reset_state() {
    // Vendor state
    all_vendors_.clear();
    filtered_vendors_.clear();
    selected_vendor_ = {};
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    vendor_search_query_.clear();

    // Filament state
    all_filaments_.clear();
    selected_filament_ = {};
    creating_new_filament_ = false;
    new_filament_name_.clear();
    new_filament_material_.clear();
    new_filament_color_hex_.clear();
    new_filament_color_name_.clear();
    new_filament_nozzle_min_ = 0;
    new_filament_nozzle_max_ = 0;
    new_filament_bed_min_ = 0;
    new_filament_bed_max_ = 0;
    new_filament_density_ = 0;
    new_filament_weight_ = 0;
    new_filament_spool_weight_ = 0;

    // Spool details state
    spool_remaining_weight_ = 0;
    spool_price_ = 0;
    spool_lot_nr_.clear();
    spool_notes_.clear();

    // SpoolmanDB search state
    catalog_.invalidate();
    catalog_in_flight_ = false;
    catalog_has_pending_ = false;
    catalog_pending_query_.clear();
    catalog_results_.clear();
    catalog_state_ = CatalogState::Idle;

    // Creation flow tracking
    created_vendor_id_ = -1;
    created_filament_id_ = -1;

    // Navigation
    can_proceed_ = false;

    // Reset subjects
    if (subjects_initialized_) {
        lv_subject_set_int(&can_proceed_subject_, 0);
        lv_subject_set_int(&creating_subject_, 0);
        lv_subject_set_int(&show_create_vendor_subject_, 0);
        lv_subject_set_int(&show_create_filament_subject_, 0);
        lv_subject_set_int(&vendor_count_subject_, -1);
        lv_subject_set_int(&filament_count_subject_, -1);
        lv_subject_set_int(&can_create_vendor_subject_, 0);
        lv_subject_set_int(&catalog_state_subject_, 0);
        lv_subject_set_int(&catalog_count_subject_, 0);
    }

    spdlog::debug("[{}] State reset for new wizard session", get_name());
}

// ============================================================================
// Step Navigation (pure logic — testable without LVGL)
// ============================================================================

void SpoolWizardOverlay::navigate_next() {
    if (!can_proceed_) {
        spdlog::debug("[{}] navigate_next blocked: can_proceed=false", get_name());
        return;
    }

    int next = static_cast<int>(current_step_) + 1;
    if (next >= STEP_COUNT) {
        spdlog::debug("[{}] Already at final step", get_name());
        return;
    }

    auto next_step = static_cast<Step>(next);
    navigate_to_step(next_step);

    // Load data for the new step
    if (next_step == Step::FILAMENT) {
        load_filaments();
    } else if (next_step == Step::SPOOL_DETAILS) {
        enter_spool_details();
    }
}

void SpoolWizardOverlay::enter_spool_details() {
    // Pre-fill remaining weight from selected filament's net weight
    spool_remaining_weight_ = selected_filament_.weight;

    // Update UI fields if overlay is active
    if (overlay_root_) {
        lv_obj_t* weight_input =
            helix::ui::find_required(overlay_root_, "remaining_weight", get_name());
        if (weight_input && spool_remaining_weight_ > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", spool_remaining_weight_);
            lv_textarea_set_text(weight_input, buf);
        }
    }

    // The summary swatch, edged where its colour would vanish into the card
    uint32_t color_val = 0x808080;
    helix::parse_hex_color(selected_filament_.color_hex.c_str(), color_val);
    if (subjects_initialized_) {
        lv_subject_set_color(&summary_color_subject_, lv_color_hex(color_val));
        lv_subject_set_int(&summary_edge_subject_,
                           helix::ui::swatch_needs_edge_here(color_val) ? 1 : 0);
    }

    // Enable proceed if weight is pre-filled
    if (spool_remaining_weight_ > 0) {
        set_can_proceed(true);
    }
}

void SpoolWizardOverlay::navigate_back() {
    int prev = static_cast<int>(current_step_) - 1;
    if (prev < 0) {
        // At first step — close the overlay
        spdlog::debug("[{}] navigate_back at step 0 — closing overlay", get_name());
        if (close_callback_) {
            close_callback_();
        }
        return;
    }

    navigate_to_step(static_cast<Step>(prev));
}

void SpoolWizardOverlay::set_can_proceed(bool val) {
    can_proceed_ = val;
    sync_subjects();
}

std::string SpoolWizardOverlay::step_label() const {
    int step_num = static_cast<int>(current_step_) + 1;
    return fmt::format(lv_tr("New Spool: Step {} of {}"), step_num, STEP_COUNT);
}

void SpoolWizardOverlay::on_create_requested() {
    spdlog::info("[{}] Create spool requested", get_name());

    // Reset tracking for rollback
    created_vendor_id_ = -1;
    created_filament_id_ = -1;

    set_creating(true);

    if (selected_vendor_.server_id < 0) {
        // Vendor is new — create it first, then filament, then spool
        create_vendor_then_filament_then_spool();
    } else if (selected_filament_.server_id < 0) {
        // Vendor exists, filament is new — create filament, then spool
        create_filament_then_spool(selected_vendor_.server_id);
    } else {
        // Both exist — create spool directly
        create_spool(selected_filament_.server_id);
    }
}

// ============================================================================
// Navigation Helpers
// ============================================================================

void SpoolWizardOverlay::navigate_to_step(Step step) {
    current_step_ = step;
    can_proceed_ = false;
    update_step_label();
    sync_subjects();

    spdlog::debug("[{}] Navigated to step {}", get_name(), static_cast<int>(step));
}

void SpoolWizardOverlay::update_step_label() {
    std::string label = step_label();
    std::snprintf(step_label_buf_, sizeof(step_label_buf_), "%s", label.c_str());

    // Update subject if initialized
    if (subjects_initialized_) {
        lv_subject_copy_string(&step_label_subject_, step_label_buf_);
    }

    // Update header title directly
    if (overlay_root_) {
        lv_obj_t* title = helix::ui::find_required(overlay_root_, "header_title", get_name());
        if (title) {
            lv_label_set_text(title, step_label_buf_);
        }
    }
}

void SpoolWizardOverlay::sync_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_set_int(&step_subject_, static_cast<int32_t>(current_step_));
    lv_subject_set_int(&can_proceed_subject_, can_proceed_ ? 1 : 0);
}

// ============================================================================
// Creation Flow
// ============================================================================

void SpoolWizardOverlay::set_creating(bool val) {
    if (subjects_initialized_) {
        lv_subject_set_int(&creating_subject_, val ? 1 : 0);
    }
}

nlohmann::json SpoolWizardOverlay::vendor_create_payload(const std::string& name,
                                                         const std::string& url) {
    nlohmann::json data;
    data["name"] = name;
    // Spoolman's vendor has no URL field and drops unknown keys, so the
    // website the user typed goes in the vendor's comment.
    if (!url.empty()) {
        data["comment"] = url;
    }
    return data;
}

void SpoolWizardOverlay::create_vendor_then_filament_then_spool() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"));
        return;
    }

    const nlohmann::json data = vendor_create_payload(selected_vendor_.name, new_vendor_url_);

    api->spoolman().create_spoolman_vendor(
        data,
        lifetime_.bg_cb("SpoolWizard::create_vendor_ok",
                        [this](const VendorInfo& vendor) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Vendor created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            selected_vendor_.server_id = vendor.id;
                            created_vendor_id_ = vendor.id;
                            spdlog::info("[{}] Created vendor id={} name='{}'", get_name(),
                                         vendor.id, vendor.name);

                            if (selected_filament_.server_id < 0) {
                                create_filament_then_spool(vendor.id);
                            } else {
                                create_spool(selected_filament_.server_id);
                            }
                        }),
        lifetime_.bg_cb("SpoolWizard::create_vendor_error", [this](const MoonrakerError& err) {
            on_creation_error(fmt::format("{} {}", lv_tr("Failed to create vendor:"), err.message));
        }));
}

nlohmann::json SpoolWizardOverlay::filament_create_payload(const FilamentEntry& f, int vendor_id) {
    nlohmann::json data;
    data["vendor_id"] = vendor_id;
    data["name"] = f.name.empty() ? f.material + " " + f.color_name : f.name;
    data["material"] = f.material;
    if (!f.multi_color_hexes.empty()) {
        data["multi_color_hexes"] = f.multi_color_hexes;
        if (!f.multi_color_direction.empty()) {
            data["multi_color_direction"] = f.multi_color_direction;
        }
    } else if (!f.color_hex.empty()) {
        data["color_hex"] = f.color_hex;
    }
    // density and diameter are REQUIRED by Spoolman (no defaults in their API)
    data["density"] = f.density > 0 ? f.density : 1.24;
    data["diameter"] = f.diameter > 0 ? f.diameter : 1.75;
    if (f.weight > 0) {
        data["weight"] = f.weight;
    }
    if (f.spool_weight > 0) {
        data["spool_weight"] = f.spool_weight;
    }
    set_spoolman_temp(data, "settings_extruder_temp", f.nozzle_temp_min, f.nozzle_temp_max);
    set_spoolman_temp(data, "settings_bed_temp", f.bed_temp_min, f.bed_temp_max);
    return data;
}

void SpoolWizardOverlay::create_filament_then_spool(int vendor_id) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"), created_vendor_id_);
        return;
    }

    const nlohmann::json data = filament_create_payload(selected_filament_, vendor_id);

    api->spoolman().create_spoolman_filament(
        data,
        lifetime_.bg_cb("SpoolWizard::create_filament_ok",
                        [this](const FilamentInfo& filament) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Filament created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            selected_filament_.server_id = filament.id;
                            created_filament_id_ = filament.id;
                            spdlog::info("[{}] Created filament id={} name='{}'", get_name(),
                                         filament.id, filament.display_name());
                            create_spool(filament.id);
                        }),
        lifetime_.bg_cb("SpoolWizard::create_filament_error", [this](const MoonrakerError& err) {
            on_creation_error(
                fmt::format("{} {}", lv_tr("Failed to create filament:"), err.message),
                created_vendor_id_);
        }));
}

void SpoolWizardOverlay::create_spool(int filament_id) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        on_creation_error(lv_tr("No API connection"), created_vendor_id_, created_filament_id_);
        return;
    }

    nlohmann::json data;
    data["filament_id"] = filament_id;
    if (spool_remaining_weight_ > 0) {
        data["remaining_weight"] = spool_remaining_weight_;
    }
    if (spool_price_ > 0) {
        data["price"] = spool_price_;
    }
    if (!spool_lot_nr_.empty()) {
        data["lot_nr"] = spool_lot_nr_;
    }
    if (!spool_notes_.empty()) {
        data["comment"] = spool_notes_;
    }

    api->spoolman().create_spoolman_spool(
        data,
        lifetime_.bg_cb("SpoolWizard::create_spool_ok",
                        [this](const SpoolInfo& spool) {
                            if (!is_visible()) {
                                spdlog::warn("[{}] Spool created but overlay no longer visible",
                                             get_name());
                                return;
                            }
                            on_creation_success(spool);
                        }),
        lifetime_.bg_cb("SpoolWizard::create_spool_error", [this](const MoonrakerError& err) {
            on_creation_error(fmt::format("{} {}", lv_tr("Failed to create spool:"), err.message),
                              created_vendor_id_, created_filament_id_);
        }));
}

void SpoolWizardOverlay::on_creation_success(const SpoolInfo& spool) {
    spdlog::info("[{}] Spool created successfully (id={})", get_name(), spool.id);
    set_creating(false);

    // Show success toast
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Spool created successfully"));

    // Refresh the spool list in SpoolmanPanel
    if (completion_callback_) {
        completion_callback_();
    }

    // Close the wizard overlay
    helix::nav::go_back();
}

void SpoolWizardOverlay::on_creation_error(const std::string& message, int rollback_vendor_id,
                                           int rollback_filament_id) {
    spdlog::error("[{}] Creation failed: {}", get_name(), message);

    // Show error toast so user knows what happened
    ToastManager::instance().show(ToastSeverity::ERROR, message.c_str());

    // Best-effort rollback — delete filament first (references vendor), then vendor
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        auto delete_vendor = [api, rollback_vendor_id]() {
            if (rollback_vendor_id >= 0) {
                api->spoolman().delete_spoolman_vendor(
                    rollback_vendor_id,
                    [rollback_vendor_id]() {
                        spdlog::info("Rollback: deleted vendor {}", rollback_vendor_id);
                    },
                    [](const MoonrakerError& e) {
                        spdlog::warn("Rollback vendor failed: {}", e.message);
                    });
            }
        };

        if (rollback_filament_id >= 0) {
            // Delete filament first, then vendor (respects FK ordering)
            api->spoolman().delete_spoolman_filament(
                rollback_filament_id,
                [rollback_filament_id, delete_vendor]() {
                    spdlog::info("Rollback: deleted filament {}", rollback_filament_id);
                    delete_vendor();
                },
                [delete_vendor](const MoonrakerError& e) {
                    spdlog::warn("Rollback filament failed: {}", e.message);
                    delete_vendor(); // Still try vendor cleanup
                });
        } else {
            delete_vendor();
        }
    }

    set_creating(false);
}

// ============================================================================
// Vendor Step Logic
// ============================================================================

std::vector<SpoolWizardOverlay::VendorEntry>
SpoolWizardOverlay::sorted_vendors(const std::vector<VendorEntry>& server_vendors) {
    // Spoolman does not keep vendor names unique; the last of a name wins.
    std::unordered_map<std::string, VendorEntry> by_name;
    for (const auto& sv : server_vendors) {
        by_name[helix::text_io::to_lower(sv.name)] = sv;
    }

    // Collect and sort alphabetically by name (case-insensitive)
    std::vector<VendorEntry> result;
    result.reserve(by_name.size());
    for (auto& [_, entry] : by_name) {
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const VendorEntry& a, const VendorEntry& b) {
        return helix::text_io::to_lower(a.name) < helix::text_io::to_lower(b.name);
    });

    return result;
}

std::vector<SpoolWizardOverlay::VendorEntry>
SpoolWizardOverlay::filter_vendor_list(const std::vector<VendorEntry>& vendors,
                                       const std::string& query) {
    if (query.empty()) {
        return vendors;
    }

    std::string lower_query = helix::text_io::to_lower(query);

    std::vector<VendorEntry> result;
    for (const auto& v : vendors) {
        if (helix::text_io::to_lower(v.name).find(lower_query) != std::string::npos) {
            result.push_back(v);
        }
    }
    return result;
}

void SpoolWizardOverlay::load_vendors() {
    spdlog::debug("[{}] Loading vendors", get_name());

    // Reset vendor state
    all_vendors_.clear();
    filtered_vendors_.clear();
    selected_vendor_ = {};
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    vendor_search_query_.clear();

    // Show loading state
    if (subjects_initialized_) {
        lv_subject_set_int(&vendors_loading_subject_, 1);
        lv_subject_set_int(&vendor_count_subject_, -1);
        lv_subject_set_int(&show_create_vendor_subject_, 0);
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, showing empty vendors", get_name());
        if (subjects_initialized_) {
            lv_subject_set_int(&vendors_loading_subject_, 0);
            lv_subject_set_int(&vendor_count_subject_, 0);
        }
        populate_vendor_list();
        return;
    }

    // The server's vendors only: Spoolman serves its external database as one
    // multi-megabyte file with no vendor listing, too heavy to pull here.
    // Captures a lifetime token rather than touching `this->lifetime_`
    // off-thread (#707 TOCTOU); the deferred body is skipped outright if the
    // overlay was deactivated in the meantime.
    auto apply = [this, tok = lifetime_.token()](std::vector<VendorEntry> server_vendors) {
        tok.defer("SpoolWizard::load_vendors_apply",
                  [this, server_vendors = std::move(server_vendors)]() {
                      all_vendors_ = sorted_vendors(server_vendors);
                      filtered_vendors_ = filter_vendor_list(all_vendors_, vendor_search_query_);

                      if (subjects_initialized_) {
                          lv_subject_set_int(&vendors_loading_subject_, 0);
                          lv_subject_set_int(&vendor_count_subject_,
                                             static_cast<int32_t>(filtered_vendors_.size()));
                      }

                      populate_vendor_list();
                      spdlog::info("[SpoolWizard] Loaded {} vendors", all_vendors_.size());
                  });
    };

    api->spoolman().get_spoolman_vendors(
        [apply](const std::vector<VendorInfo>& server_list) {
            std::vector<VendorEntry> server_vendors;
            server_vendors.reserve(server_list.size());
            for (const auto& vi : server_list) {
                VendorEntry entry;
                entry.name = vi.name;
                entry.server_id = vi.id;
                entry.from_server = true;
                server_vendors.push_back(std::move(entry));
            }
            apply(std::move(server_vendors));
        },
        [apply](const MoonrakerError& err) {
            spdlog::warn("[SpoolWizard] Failed to fetch server vendors: {}", err.message);
            apply({});
        });
}

void SpoolWizardOverlay::filter_vendors(const std::string& query) {
    vendor_search_query_ = query;
    filtered_vendors_ = filter_vendor_list(all_vendors_, query);

    if (subjects_initialized_) {
        lv_subject_set_int(&vendor_count_subject_, static_cast<int32_t>(filtered_vendors_.size()));
    }

    populate_vendor_list();
    spdlog::debug("[{}] Filtered vendors: {} match '{}'", get_name(), filtered_vendors_.size(),
                  query);
}

void SpoolWizardOverlay::select_vendor(int index) {
    if (index < 0 || index >= static_cast<int>(filtered_vendors_.size())) {
        spdlog::warn("[{}] Invalid vendor index: {}", get_name(), index);
        return;
    }

    selected_vendor_ = filtered_vendors_[static_cast<size_t>(index)];
    new_vendor_name_.clear();
    new_vendor_url_.clear();

    spdlog::info("[{}] Selected vendor: '{}' (server_id={})", get_name(), selected_vendor_.name,
                 selected_vendor_.server_id);

    // Update checked state on vendor rows
    if (overlay_root_) {
        lv_obj_t* vendor_list = helix::ui::find_required(overlay_root_, "vendor_list", get_name());
        if (vendor_list) {
            uint32_t count = lv_obj_get_child_count(vendor_list);
            for (uint32_t i = 0; i < count; i++) {
                lv_obj_t* row = lv_obj_get_child(vendor_list, static_cast<int32_t>(i));
                lv_obj_set_state(row, LV_STATE_CHECKED, static_cast<int>(i) == index);
            }
        }
    }

    publish_vendor_selection();
    set_can_proceed(true);
}

void SpoolWizardOverlay::publish_vendor_selection() {
    // Shown on the filament step header and the spool step summary
    if (subjects_initialized_) {
        std::snprintf(selected_vendor_name_buf_, sizeof(selected_vendor_name_buf_), "%s",
                      selected_vendor_.name.c_str());
        lv_subject_copy_string(&selected_vendor_name_subject_, selected_vendor_name_buf_);

        std::snprintf(summary_vendor_buf_, sizeof(summary_vendor_buf_), "%s",
                      selected_vendor_.name.c_str());
        lv_subject_copy_string(&summary_vendor_subject_, summary_vendor_buf_);
    }
}

void SpoolWizardOverlay::set_new_vendor(const std::string& name, const std::string& url) {
    new_vendor_name_ = name.substr(0, MAX_VENDOR_NAME_LEN);
    new_vendor_url_ = url.substr(0, MAX_VENDOR_URL_LEN);

    bool valid = !helix::text_io::trim(new_vendor_name_).empty();

    if (subjects_initialized_) {
        lv_subject_set_int(&can_create_vendor_subject_, valid ? 1 : 0);
    }

    spdlog::debug("[{}] New vendor name='{}' url='{}' valid={}", get_name(), name, url, valid);
}

void SpoolWizardOverlay::populate_vendor_list() {
    if (!overlay_root_) {
        spdlog::trace("[{}] populate_vendor_list: no overlay_root_, skipping UI", get_name());
        return;
    }

    lv_obj_t* vendor_list = helix::ui::find_required(overlay_root_, "vendor_list", get_name());
    if (!vendor_list) {
        return;
    }

    // Clear existing rows
    helix::ui::safe_clean_children(vendor_list);

    for (size_t i = 0; i < filtered_vendors_.size(); i++) {
        const auto& vendor = filtered_vendors_[i];

        // Create row from XML component
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(vendor_list, "wizard_vendor_row", nullptr));
        if (!row) {
            spdlog::error("[{}] Failed to create vendor row for '{}'", get_name(), vendor.name);
            continue;
        }

        // Store index in user_data for click handling
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        // Set vendor name
        lv_obj_t* name_label = helix::ui::find_required(row, "vendor_name", get_name());
        if (name_label) {
            lv_label_set_text(name_label, vendor.name.c_str());
        }

        // Set source badge
        lv_obj_t* source_label = helix::ui::find_required(row, "vendor_source", get_name());
        if (source_label) {
            // A vendor not yet on the server is one the user is creating.
            lv_label_set_text(source_label, vendor.from_server
                                                ? "Spoolman"
                                                : ""); // i18n: product name, do not translate
        }
    }

    spdlog::debug("[{}] Populated {} vendor rows", get_name(), filtered_vendors_.size());
}

// ============================================================================
// Vendor Step Handlers
// ============================================================================

void SpoolWizardOverlay::show_create_vendor_modal() {
    spdlog::debug("[SpoolWizard] Show create vendor modal");

    // Clear previous input state
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    if (subjects_initialized_) {
        lv_subject_set_int(&can_create_vendor_subject_, 0);
    }

    // Show the modal
    create_vendor_dialog_ = Modal::show("create_vendor_modal");

    if (create_vendor_dialog_) {
        // Register keyboards for text inputs
        lv_obj_t* name_input = lv_obj_find_by_name(create_vendor_dialog_, "new_vendor_name");
        if (name_input) {
            helix::ui::modal_register_keyboard(create_vendor_dialog_, name_input);
        }
        lv_obj_t* url_input = lv_obj_find_by_name(create_vendor_dialog_, "new_vendor_url");
        if (url_input) {
            helix::ui::modal_register_keyboard(create_vendor_dialog_, url_input);
        }
    }
}

void SpoolWizardOverlay::confirm_create_vendor() {
    spdlog::debug("[SpoolWizard] Confirm create vendor");

    std::string name(helix::text_io::trim(new_vendor_name_));
    if (name.empty()) {
        spdlog::warn("[SpoolWizard] Cannot create vendor with empty name");
        return;
    }

    // Check for duplicate vendor name (case-insensitive)
    std::string name_lower = helix::text_io::to_lower(name);
    for (const auto& v : all_vendors_) {
        if (helix::text_io::to_lower(v.name) == name_lower) {
            spdlog::warn("[SpoolWizard] Duplicate vendor name: '{}'", name);
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Vendor already exists"));
            return;
        }
    }

    // Close the modal first (before touching the list, to avoid focus/scroll side effects)
    if (create_vendor_dialog_) {
        Modal::hide(create_vendor_dialog_);
        create_vendor_dialog_ = nullptr;
    }

    // Set as selected vendor with server_id = -1 (will be created on final submit)
    VendorEntry new_vendor = {name, -1, false};
    selected_vendor_ = new_vendor;

    // Add to vendor lists and re-sort alphabetically
    all_vendors_.push_back(new_vendor);
    std::sort(all_vendors_.begin(), all_vendors_.end(),
              [](const VendorEntry& a, const VendorEntry& b) {
                  return helix::text_io::to_lower(a.name) < helix::text_io::to_lower(b.name);
              });
    filtered_vendors_ = filter_vendor_list(all_vendors_, vendor_search_query_);

    // Update display subjects
    if (subjects_initialized_) {
        std::snprintf(selected_vendor_name_buf_, sizeof(selected_vendor_name_buf_), "%s",
                      name.c_str());
        lv_subject_copy_string(&selected_vendor_name_subject_, selected_vendor_name_buf_);

        std::snprintf(summary_vendor_buf_, sizeof(summary_vendor_buf_), "%s", name.c_str());
        lv_subject_copy_string(&summary_vendor_subject_, summary_vendor_buf_);

        lv_subject_set_int(&vendor_count_subject_, static_cast<int32_t>(filtered_vendors_.size()));
    }

    // Repopulate the list and select the new vendor
    populate_vendor_list();

    // Find the new vendor's index in filtered list and highlight it
    for (size_t i = 0; i < filtered_vendors_.size(); i++) {
        if (helix::text_io::to_lower(filtered_vendors_[i].name) == helix::text_io::to_lower(name)) {
            // Set checked state on the matching row
            if (overlay_root_) {
                lv_obj_t* vendor_list =
                    helix::ui::find_required(overlay_root_, "vendor_list", get_name());
                if (vendor_list) {
                    uint32_t count = lv_obj_get_child_count(vendor_list);
                    for (uint32_t j = 0; j < count; j++) {
                        lv_obj_t* row = lv_obj_get_child(vendor_list, static_cast<int32_t>(j));
                        lv_obj_set_state(row, LV_STATE_CHECKED, j == i);
                    }
                    // Scroll to show the selected row
                    lv_obj_t* selected_row = lv_obj_get_child(vendor_list, static_cast<int32_t>(i));
                    if (selected_row) {
                        lv_obj_scroll_to_view(selected_row, LV_ANIM_ON);
                    }
                }
            }
            break;
        }
    }

    set_can_proceed(true);
    spdlog::info("[SpoolWizard] New vendor '{}' confirmed (will be created on submit)", name);
}

// ============================================================================
// Filament Step Logic
// ============================================================================

void SpoolWizardOverlay::load_filaments() {
    spdlog::debug("[{}] Loading filaments for vendor '{}' (server_id={})", get_name(),
                  selected_vendor_.name, selected_vendor_.server_id);

    // Reset filament state
    all_filaments_.clear();
    selected_filament_ = {};
    creating_new_filament_ = false;
    new_filament_name_.clear();
    new_filament_material_.clear();
    new_filament_color_hex_.clear();
    new_filament_color_name_.clear();
    new_filament_nozzle_min_ = 0;
    new_filament_nozzle_max_ = 0;
    new_filament_bed_min_ = 0;
    new_filament_bed_max_ = 0;
    new_filament_density_ = 0;
    new_filament_weight_ = 0;
    new_filament_spool_weight_ = 0;

    if (subjects_initialized_) {
        lv_subject_set_int(&filament_count_subject_, -1);
        lv_subject_set_int(&show_create_filament_subject_, 0);
        lv_subject_set_int(&filaments_loading_subject_, 1);
    }

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No API available, showing empty filaments", get_name());
        if (subjects_initialized_) {
            lv_subject_set_int(&filament_count_subject_, 0);
            lv_subject_set_int(&filaments_loading_subject_, 0);
        }
        populate_filament_list();
        return;
    }

    // DB-only vendor (not yet created on server) — no filaments to fetch.
    // User must use "+ New" to create filaments for this vendor.
    if (selected_vendor_.server_id < 0) {
        spdlog::debug("[{}] DB-only vendor '{}', no server filaments to fetch", get_name(),
                      selected_vendor_.name);
        if (subjects_initialized_) {
            lv_subject_set_int(&filaments_loading_subject_, 0);
            lv_subject_set_int(&filament_count_subject_, 0);
        }
        populate_filament_list();
        return;
    }

    // Fetch filaments from Spoolman server, filtered by vendor.id.
    // NOTE: We intentionally do NOT call the external DB endpoint here —
    // /v1/external/filament has no vendor filtering and returns the entire
    // SpoolmanDB (~thousands of entries), which is too heavy for embedded.
    // Users can create filaments via "+ New" if the server list is empty.
    int vendor_id = selected_vendor_.server_id;
    api->spoolman().get_spoolman_filaments(
        vendor_id,
        lifetime_.bg_cb("SpoolWizard::load_filaments_apply",
                        [this, vendor_id](const std::vector<FilamentInfo>& server_list) {
                            // Convert FilamentInfo -> FilamentEntry
                            for (const auto& fi : server_list) {
                                FilamentEntry entry;
                                entry.name = fi.display_name();
                                entry.material = fi.material;
                                entry.color_hex = fi.color_hex;
                                // See to_entry(): a filament name is not a colour name.
                                entry.server_id = fi.id;
                                entry.vendor_id = fi.vendor_id;
                                entry.density = fi.density;
                                entry.diameter = fi.diameter;
                                entry.weight = fi.weight;
                                entry.spool_weight = fi.spool_weight;
                                entry.nozzle_temp_min = fi.nozzle_temp_min;
                                entry.nozzle_temp_max = fi.nozzle_temp_max;
                                entry.bed_temp_min = fi.bed_temp_min;
                                entry.bed_temp_max = fi.bed_temp_max;
                                entry.from_server = true;
                                all_filaments_.push_back(entry);
                            }

                            // Sort by material then name
                            std::sort(all_filaments_.begin(), all_filaments_.end(),
                                      [](const FilamentEntry& a, const FilamentEntry& b) {
                                          std::string a_mat = helix::text_io::to_lower(a.material);
                                          std::string b_mat = helix::text_io::to_lower(b.material);
                                          if (a_mat != b_mat)
                                              return a_mat < b_mat;
                                          return helix::text_io::to_lower(a.name) <
                                                 helix::text_io::to_lower(b.name);
                                      });

                            if (subjects_initialized_) {
                                lv_subject_set_int(&filaments_loading_subject_, 0);
                                lv_subject_set_int(&filament_count_subject_,
                                                   static_cast<int32_t>(all_filaments_.size()));
                            }

                            populate_filament_list();
                            spdlog::info("[SpoolWizard] Loaded {} filaments for vendor_id {}",
                                         all_filaments_.size(), vendor_id);
                        }),
        lifetime_.bg_cb("SpoolWizard::load_filaments_error", [this](const MoonrakerError& err) {
            spdlog::warn("[SpoolWizard] Failed to fetch filaments: {}", err.message);
            if (cleanup_called())
                return;
            if (subjects_initialized_) {
                lv_subject_set_int(&filaments_loading_subject_, 0);
                lv_subject_set_int(&filament_count_subject_, 0);
            }
            populate_filament_list();
        }));
}

void SpoolWizardOverlay::select_filament(int index) {
    if (index < 0 || index >= static_cast<int>(all_filaments_.size())) {
        spdlog::warn("[{}] Invalid filament index: {}", get_name(), index);
        return;
    }

    selected_filament_ = all_filaments_[static_cast<size_t>(index)];
    creating_new_filament_ = false;

    spdlog::info("[{}] Selected filament: '{}' {} (server_id={})", get_name(),
                 selected_filament_.name, selected_filament_.material,
                 selected_filament_.server_id);

    // Update checked state on filament rows
    if (overlay_root_) {
        lv_obj_t* filament_list =
            helix::ui::find_required(overlay_root_, "filament_list", get_name());
        if (filament_list) {
            uint32_t count = lv_obj_get_child_count(filament_list);
            for (uint32_t i = 0; i < count; i++) {
                lv_obj_t* row = lv_obj_get_child(filament_list, static_cast<int32_t>(i));
                lv_obj_set_state(row, LV_STATE_CHECKED, static_cast<int>(i) == index);
            }
        }
    }

    publish_filament_summary();
    set_can_proceed(true);
}

void SpoolWizardOverlay::publish_filament_summary() {
    if (subjects_initialized_) {
        // The shared label rule, so a name that already says the material
        // does not repeat it
        const std::string summary =
            helix::compose_filament_label("", selected_filament_.name, selected_filament_.material);
        std::snprintf(summary_filament_buf_, sizeof(summary_filament_buf_), "%s", summary.c_str());
        lv_subject_copy_string(&summary_filament_subject_, summary_filament_buf_);
    }
}

void SpoolWizardOverlay::set_new_filament_material(const std::string& material) {
    new_filament_material_ = material;

    // Look up material in the static filament database for auto-fill
    auto mat_info = filament::find_material(material);
    if (mat_info.has_value()) {
        new_filament_nozzle_min_ = mat_info->nozzle_min;
        new_filament_nozzle_max_ = mat_info->nozzle_max;
        new_filament_bed_min_ = mat_info->bed_temp;
        new_filament_bed_max_ = mat_info->bed_temp;
        new_filament_density_ = static_cast<double>(mat_info->density_g_cm3);

        spdlog::debug("[{}] Auto-filled temps for {}: nozzle {}-{}, bed {}, density {:.2f}",
                      get_name(), material, new_filament_nozzle_min_, new_filament_nozzle_max_,
                      new_filament_bed_min_, new_filament_density_);

        // Update UI text inputs in the modal dialog
        lv_obj_t* search_root = create_filament_dialog_ ? create_filament_dialog_ : overlay_root_;
        if (search_root) {
            lv_obj_t* nozzle_min = lv_obj_find_by_name(search_root, "nozzle_temp_min");
            lv_obj_t* nozzle_max = lv_obj_find_by_name(search_root, "nozzle_temp_max");
            lv_obj_t* bed_min = lv_obj_find_by_name(search_root, "bed_temp_min");
            lv_obj_t* bed_max = lv_obj_find_by_name(search_root, "bed_temp_max");

            char buf[16];
            if (nozzle_min) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_nozzle_min_);
                lv_textarea_set_text(nozzle_min, buf);
            }
            if (nozzle_max) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_nozzle_max_);
                lv_textarea_set_text(nozzle_max, buf);
            }
            if (bed_min) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_bed_min_);
                lv_textarea_set_text(bed_min, buf);
            }
            if (bed_max) {
                std::snprintf(buf, sizeof(buf), "%d", new_filament_bed_max_);
                lv_textarea_set_text(bed_max, buf);
            }
        }
    } else {
        spdlog::debug("[{}] Material '{}' not found in database, no auto-fill", get_name(),
                      material);
    }

    update_new_filament_can_proceed();
}

void SpoolWizardOverlay::set_new_filament_color(const std::string& hex, const std::string& name) {
    new_filament_color_hex_ = hex;
    new_filament_color_name_ = name;

    spdlog::debug("[{}] New filament color: #{} ({})", get_name(), hex, name);

    // Update the color swatch in the modal dialog
    lv_obj_t* search_root = create_filament_dialog_ ? create_filament_dialog_ : overlay_root_;
    if (search_root && !hex.empty()) {
        lv_obj_t* swatch = lv_obj_find_by_name(search_root, "filament_color_swatch");
        if (swatch) {
            uint32_t color_val = std::strtoul(hex.c_str(), nullptr, 16);
            lv_obj_set_style_bg_color(swatch, lv_color_hex(color_val), 0);
        }
    }

    update_new_filament_can_proceed();
}

void SpoolWizardOverlay::populate_filament_list() {
    if (!overlay_root_) {
        spdlog::trace("[{}] populate_filament_list: no overlay_root_, skipping UI", get_name());
        return;
    }

    lv_obj_t* filament_list = helix::ui::find_required(overlay_root_, "filament_list", get_name());
    if (!filament_list) {
        return;
    }

    // Clear existing rows
    helix::ui::safe_clean_children(filament_list);

    for (size_t i = 0; i < all_filaments_.size(); i++) {
        const auto& fil = all_filaments_[i];

        // Create row from XML component
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(filament_list, "wizard_filament_row", nullptr));
        if (!row) {
            spdlog::error("[{}] Failed to create filament row for '{}'", get_name(), fil.name);
            continue;
        }

        // Store index in user_data for click handling
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        // Set color swatch
        lv_obj_t* swatch = helix::ui::find_required(row, "color_swatch", get_name());
        if (swatch && !fil.color_hex.empty()) {
            uint32_t color_val = std::strtoul(fil.color_hex.c_str(), nullptr, 16);
            helix::ui::apply_swatch_color(swatch, color_val, "");
        }

        // Set combined material - name label
        lv_obj_t* material_label = helix::ui::find_required(row, "filament_material", get_name());
        if (material_label) {
            std::string display = fil.material;
            if (!fil.name.empty()) {
                display += " - " + fil.name;
            }
            lv_label_set_text(material_label, display.c_str());
        }

        // Set temps label
        lv_obj_t* temps_label = helix::ui::find_required(row, "filament_temps", get_name());
        if (temps_label) {
            char temp_buf[32] = {};
            if (fil.nozzle_temp_max > 0) {
                helix::ui::temperature::format_temperature_range(
                    fil.nozzle_temp_min, fil.nozzle_temp_max, temp_buf, sizeof(temp_buf));
            }
            lv_label_set_text(temps_label, temp_buf);
        }
    }

    spdlog::debug("[{}] Populated {} filament rows", get_name(), all_filaments_.size());
}

void SpoolWizardOverlay::update_new_filament_can_proceed() {
    // Material + color are required for a new filament
    bool valid = !new_filament_material_.empty() && !new_filament_color_hex_.empty();

    if (valid && creating_new_filament_) {
        set_can_proceed(true);
    }

    spdlog::debug("[{}] New filament can_proceed: material='{}' color='{}' valid={}", get_name(),
                  new_filament_material_, new_filament_color_hex_, valid);
}

// ============================================================================
// Filament Step Handlers
// ============================================================================

void SpoolWizardOverlay::show_create_filament_modal() {
    spdlog::debug("[SpoolWizard] Show create filament modal");

    // Clear previous filament input state, default material to first in database
    new_filament_name_.clear();
    const auto table = filament::materials();
    new_filament_material_ = table->empty() ? "PLA" : table->front().name;
    new_filament_color_hex_.clear();
    new_filament_color_name_.clear();
    new_filament_nozzle_min_ = 0;
    new_filament_nozzle_max_ = 0;
    new_filament_bed_min_ = 0;
    new_filament_bed_max_ = 0;
    new_filament_density_ = 0;
    new_filament_weight_ = 0;
    new_filament_spool_weight_ = 0;
    creating_new_filament_ = true;

    // Clear previous selection so can_proceed is false until form is confirmed
    selected_filament_ = {};
    set_can_proceed(false);

    // Show the modal
    create_filament_dialog_ = Modal::show("create_filament_modal");

    if (create_filament_dialog_) {
        // Register keyboards for text inputs
        lv_obj_t* name_input = lv_obj_find_by_name(create_filament_dialog_, "new_filament_name");
        if (name_input) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, name_input);
        }
        lv_obj_t* nozzle_min = lv_obj_find_by_name(create_filament_dialog_, "nozzle_temp_min");
        if (nozzle_min) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, nozzle_min);
        }
        lv_obj_t* nozzle_max = lv_obj_find_by_name(create_filament_dialog_, "nozzle_temp_max");
        if (nozzle_max) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, nozzle_max);
        }
        lv_obj_t* bed_min = lv_obj_find_by_name(create_filament_dialog_, "bed_temp_min");
        if (bed_min) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, bed_min);
        }
        lv_obj_t* bed_max = lv_obj_find_by_name(create_filament_dialog_, "bed_temp_max");
        if (bed_max) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, bed_max);
        }
        lv_obj_t* weight = lv_obj_find_by_name(create_filament_dialog_, "filament_weight");
        if (weight) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, weight);
        }
        lv_obj_t* spool_weight =
            lv_obj_find_by_name(create_filament_dialog_, "filament_spool_weight");
        if (spool_weight) {
            helix::ui::modal_register_keyboard(create_filament_dialog_, spool_weight);
        }

        // Populate material dropdown from filament database
        lv_obj_t* dropdown = lv_obj_find_by_name(create_filament_dialog_, "material_dropdown");
        if (dropdown) {
            auto names = filament::get_all_material_names();
            std::string options;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0)
                    options += '\n';
                options += names[i];
            }
            lv_dropdown_set_options(dropdown, options.c_str());

            // Default to first material (PLA) and trigger auto-fill
            lv_dropdown_set_selected(dropdown, 0);
            set_new_filament_material(names[0]);
        }
    }
}

void SpoolWizardOverlay::pick_filament_color() {
    spdlog::debug("[SpoolWizard] Pick filament color");

    // Create picker on first use (lazy initialization)
    if (!color_picker_) {
        color_picker_ = std::make_unique<helix::ui::ColorPicker>();
    }

    // Set callback for when color is selected (access global, don't capture reference)
    color_picker_->set_color_callback([](uint32_t color_rgb, const std::string& color_name) {
        char hex_buf[8];
        std::snprintf(hex_buf, sizeof(hex_buf), "%06X", color_rgb);
        get_global_spool_wizard().set_new_filament_color(hex_buf, color_name);
    });

    // Parse current color for initial value
    uint32_t initial_color = 0x808080;
    if (!new_filament_color_hex_.empty()) {
        initial_color = std::strtoul(new_filament_color_hex_.c_str(), nullptr, 16);
    }

    // Show color picker on the screen (it creates its own modal)
    lv_obj_t* parent = create_filament_dialog_
                           ? lv_obj_get_parent(create_filament_dialog_)
                           : (overlay_root_ ? lv_obj_get_parent(overlay_root_) : nullptr);
    if (parent) {
        color_picker_->show_with_color(parent, initial_color);
    }
}

void SpoolWizardOverlay::confirm_create_filament() {
    spdlog::debug("[SpoolWizard] Confirm create filament");

    // Helper to set/clear error highlighting on a named label within the modal
    lv_obj_t* dialog = create_filament_dialog_;
    auto& theme = ThemeManager::instance();
    auto set_field_error = [dialog, &theme](const char* label_name, bool error) {
        if (!dialog)
            return;
        lv_obj_t* label = lv_obj_find_by_name(dialog, label_name);
        if (!label)
            return;
        lv_color_t color = error ? theme.get_color("danger") : theme.get_color("text_muted");
        lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    };

    // Validate required fields
    bool has_error = false;

    bool material_missing = new_filament_material_.empty();
    set_field_error("material_label", material_missing);
    if (material_missing) {
        has_error = true;
    }

    bool color_missing = new_filament_color_hex_.empty();
    set_field_error("color_label", color_missing);
    if (color_missing) {
        has_error = true;
    }

    if (has_error) {
        spdlog::warn("[SpoolWizard] Cannot create filament — missing required fields");
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Please fill in the highlighted fields"));
        return;
    }

    // Check for duplicate (case-insensitive material + name match)
    std::string mat_lower = helix::text_io::to_lower(new_filament_material_);
    std::string name_lower =
        helix::text_io::to_lower(std::string(helix::text_io::trim(new_filament_name_)));
    for (const auto& f : all_filaments_) {
        if (helix::text_io::to_lower(f.material) == mat_lower &&
            helix::text_io::to_lower(f.name) == name_lower) {
            spdlog::warn("[SpoolWizard] Duplicate filament: {} '{}'", new_filament_material_,
                         new_filament_name_);
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Filament already exists"));
            return;
        }
    }

    // Close the modal first
    if (create_filament_dialog_) {
        Modal::hide(create_filament_dialog_);
        create_filament_dialog_ = nullptr;
    }

    // Build a display summary for the filament
    std::string summary = new_filament_material_;
    if (!new_filament_name_.empty()) {
        summary += " - " + new_filament_name_;
    } else if (!new_filament_color_name_.empty()) {
        summary += " " + new_filament_color_name_;
    }

    // Build the new filament entry
    FilamentEntry new_fil;
    new_fil.name = new_filament_name_;
    new_fil.material = new_filament_material_;
    new_fil.color_hex = new_filament_color_hex_;
    new_fil.color_name = new_filament_color_name_;
    new_fil.server_id = -1;
    new_fil.vendor_id = selected_vendor_.server_id;
    new_fil.density = new_filament_density_;
    new_fil.weight = new_filament_weight_;
    new_fil.spool_weight = new_filament_spool_weight_;
    new_fil.nozzle_temp_min = new_filament_nozzle_min_;
    new_fil.nozzle_temp_max = new_filament_nozzle_max_;
    new_fil.bed_temp_min = new_filament_bed_min_;
    new_fil.bed_temp_max = new_filament_bed_max_;

    // Set as selected filament
    selected_filament_ = new_fil;

    // Add to filament list and re-sort by material then name
    all_filaments_.push_back(new_fil);
    std::sort(all_filaments_.begin(), all_filaments_.end(),
              [](const FilamentEntry& a, const FilamentEntry& b) {
                  std::string a_mat = helix::text_io::to_lower(a.material);
                  std::string b_mat = helix::text_io::to_lower(b.material);
                  if (a_mat != b_mat)
                      return a_mat < b_mat;
                  return a.name < b.name;
              });

    // Update filament count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&filament_count_subject_, static_cast<int32_t>(all_filaments_.size()));

        // Update summary display
        std::snprintf(summary_filament_buf_, sizeof(summary_filament_buf_), "%s", summary.c_str());
        lv_subject_copy_string(&summary_filament_subject_, summary_filament_buf_);
    }

    // Repopulate the list and highlight the new entry
    populate_filament_list();

    // Find the new filament's index and set checked state
    for (size_t i = 0; i < all_filaments_.size(); i++) {
        if (helix::text_io::to_lower(all_filaments_[i].material) == mat_lower &&
            helix::text_io::to_lower(all_filaments_[i].name) == name_lower) {
            if (overlay_root_) {
                lv_obj_t* filament_list =
                    helix::ui::find_required(overlay_root_, "filament_list", get_name());
                if (filament_list) {
                    uint32_t count = lv_obj_get_child_count(filament_list);
                    for (uint32_t j = 0; j < count; j++) {
                        lv_obj_t* row = lv_obj_get_child(filament_list, static_cast<int32_t>(j));
                        lv_obj_set_state(row, LV_STATE_CHECKED, j == i);
                    }
                    // Scroll to show the selected row
                    lv_obj_t* selected_row =
                        lv_obj_get_child(filament_list, static_cast<int32_t>(i));
                    if (selected_row) {
                        lv_obj_scroll_to_view(selected_row, LV_ANIM_ON);
                    }
                }
            }
            break;
        }
    }

    creating_new_filament_ = false;
    set_can_proceed(true);
    spdlog::info("[SpoolWizard] New filament '{}' confirmed (will be created on submit)", summary);
}

// ============================================================================
// SpoolmanDB Search
// ============================================================================

SpoolWizardOverlay::FilamentEntry
SpoolWizardOverlay::entry_from_catalog(const helix::ExternalFilament& f) {
    FilamentEntry e;
    e.name = f.name;
    e.material = f.material;
    if (!f.color_hexes.empty()) {
        e.color_hex = f.color_hexes.front();
        for (const auto& hex : f.color_hexes) {
            e.multi_color_hexes += (e.multi_color_hexes.empty() ? "" : ",") + hex;
        }
        e.multi_color_direction = f.multi_color_direction;
    } else {
        e.color_hex = f.color_hex;
    }
    e.density = f.density;
    e.diameter = f.diameter > 0 ? f.diameter : 1.75;
    e.weight = f.weight;
    e.spool_weight = f.spool_weight;
    e.nozzle_temp_min = e.nozzle_temp_max = f.extruder_temp;
    e.bed_temp_min = e.bed_temp_max = f.bed_temp;
    return e;
}

int SpoolWizardOverlay::find_server_vendor(const std::vector<VendorEntry>& vendors,
                                           const std::string& name) {
    auto folded = [](const std::string& s) {
        return helix::text_io::to_lower(std::string(helix::text_io::trim(s)));
    };
    const std::string needle = folded(name);
    for (size_t i = 0; i < vendors.size(); ++i) {
        if (vendors[i].server_id >= 0 && folded(vendors[i].name) == needle) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SpoolWizardOverlay::set_catalog_state(CatalogState state) {
    catalog_state_ = state;
    if (subjects_initialized_) {
        lv_subject_set_int(&catalog_state_subject_, static_cast<int32_t>(state));
    }
}

void SpoolWizardOverlay::sync_catalog_available() {
    IMoonrakerAPI* api = get_moonraker_api();
    const bool available = api && helix::SpoolmanCatalogSearch::availability(
                                      api->get_client().connection_generation()) ==
                                      helix::SpoolmanCatalogSearch::Availability::Available;
    if (subjects_initialized_) {
        lv_subject_set_int(&catalog_available_subject_, available ? 1 : 0);
    }
}

void SpoolWizardOverlay::probe_catalog_search() {
    sync_catalog_available();
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    const uint64_t conn = api->get_client().connection_generation();
    if (helix::SpoolmanCatalogSearch::availability(conn) !=
        helix::SpoolmanCatalogSearch::Availability::Unknown) {
        return;
    }
    // The cheapest question the route can answer: does it exist.
    api->spoolman().search_spoolman_external_filaments(
        "", 1,
        lifetime_.bg_cb("SpoolWizard::catalog_probe_ok",
                        [this, conn](const std::vector<helix::ExternalFilament>&) {
                            helix::SpoolmanCatalogSearch::record_success(conn);
                            sync_catalog_available();
                        }),
        lifetime_.bg_cb("SpoolWizard::catalog_probe_error",
                        [this, conn](const MoonrakerError& err) {
                            helix::SpoolmanCatalogSearch::record_error(conn, err);
                            sync_catalog_available();
                            spdlog::debug("[SpoolWizard] SpoolmanDB search probe: {} (code {})",
                                          err.message, err.code);
                        }));
}

void SpoolWizardOverlay::on_search_key() {
    // One one-shot timer for the session, made on the first keystroke and kept
    // (not auto-deleted) between bursts, so every later keystroke only re-arms it.
    if (!search_timer_) {
        search_timer_ = lv_timer_create(search_timer_cb, helix::ui::kDefaultSearchDebounceMs, this);
        lv_timer_set_auto_delete(search_timer_, false);
    }
    lv_timer_set_repeat_count(search_timer_, 1);
    lv_timer_reset(search_timer_);
    lv_timer_resume(search_timer_);
    spdlog::debug("[SpoolWizard] search keystroke");
}

void SpoolWizardOverlay::search_timer_cb(lv_timer_t* timer) {
    static_cast<SpoolWizardOverlay*>(lv_timer_get_user_data(timer))->apply_search_text();
}

void SpoolWizardOverlay::cancel_search_timer() {
    if (search_timer_ && lv_is_initialized()) {
        // A kept one-shot: hand it back to LVGL's reaping, which deletes an
        // auto-delete timer whose count is spent but never runs a paused one.
        lv_timer_set_auto_delete(search_timer_, true);
        lv_timer_resume(search_timer_);
        helix::ui::lv_timer_cancel_safe(search_timer_);
    }
    search_timer_ = nullptr;
}

void SpoolWizardOverlay::apply_search_text() {
    lv_obj_t* input = helix::ui::find_required(overlay_root_, "vendor_search", get_name());
    const char* text = input ? lv_textarea_get_text(input) : nullptr;
    const std::string query = text ? text : "";
    spdlog::debug("[SpoolWizard] search settled on '{}'", query);
    filter_vendors(query);
    run_catalog_search(query);
}

void SpoolWizardOverlay::run_catalog_search(const std::string& query) {
    IMoonrakerAPI* api = get_moonraker_api();
    const uint64_t conn = api ? api->get_client().connection_generation() : 0;
    if (!api ||
        helix::SpoolmanCatalogSearch::availability(conn) ==
            helix::SpoolmanCatalogSearch::Availability::Unavailable ||
        !helix::SpoolmanCatalogSearch::is_searchable(query)) {
        catalog_.invalidate();
        catalog_has_pending_ = false;
        set_catalog_state(CatalogState::Idle);
        return;
    }

    if (catalog_in_flight_) {
        // One search in flight at most: this query waits, and the in-flight
        // answer is dropped when it lands.
        catalog_.invalidate();
        catalog_pending_query_ = query;
        catalog_has_pending_ = true;
        set_catalog_state(CatalogState::Loading);
        return;
    }
    send_catalog_search(query);
}

void SpoolWizardOverlay::send_catalog_search(std::string query) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    const uint64_t conn = api->get_client().connection_generation();
    const uint64_t ticket = catalog_.begin();
    catalog_in_flight_ = true;
    set_catalog_state(CatalogState::Loading);
    spdlog::debug("[SpoolWizard] SpoolmanDB search #{} sent for '{}'", ticket, query);

    // Whichever way it ends, the in-flight slot frees and a waiting query goes.
    auto finish = [this]() {
        catalog_in_flight_ = false;
        if (catalog_has_pending_) {
            catalog_has_pending_ = false;
            send_catalog_search(std::move(catalog_pending_query_));
            return true;
        }
        return false;
    };

    api->spoolman().search_spoolman_external_filaments(
        std::string(helix::text_io::trim(query)), helix::SpoolmanCatalogSearch::kResultLimit,
        lifetime_.bg_cb("SpoolWizard::catalog_results",
                        [this, ticket, conn, finish](std::vector<helix::ExternalFilament> results) {
                            helix::SpoolmanCatalogSearch::record_success(conn);
                            sync_catalog_available();
                            const bool current = catalog_.is_current(ticket);
                            if (finish() || !current) {
                                spdlog::debug("[SpoolWizard] SpoolmanDB search #{} superseded; "
                                              "answer dropped",
                                              ticket);
                                return;
                            }
                            apply_catalog_results(std::move(results));
                        }),
        lifetime_.bg_cb(
            "SpoolWizard::catalog_error", [this, ticket, conn, finish](const MoonrakerError& err) {
                helix::SpoolmanCatalogSearch::record_error(conn, err);
                sync_catalog_available();
                const bool current = catalog_.is_current(ticket);
                if (finish() || !current) {
                    return;
                }
                spdlog::warn("[SpoolWizard] SpoolmanDB search failed: {} (code {})", err.message,
                             err.code);
                set_catalog_state(helix::SpoolmanCatalogSearch::availability(conn) ==
                                          helix::SpoolmanCatalogSearch::Availability::Unavailable
                                      ? CatalogState::Idle
                                      : CatalogState::Error);
            }));
}

void SpoolWizardOverlay::apply_catalog_results(std::vector<helix::ExternalFilament> results) {
    const auto started = std::chrono::steady_clock::now();
    const size_t limit = static_cast<size_t>(helix::SpoolmanCatalogSearch::kResultLimit);
    if (results.size() > limit) {
        results.resize(limit);
    }
    catalog_results_ = std::move(results);

    if (subjects_initialized_) {
        // The rows bind to these when the count rebuilds them, so they go first.
        for (size_t i = 0; i < catalog_results_.size(); ++i) {
            const auto& f = catalog_results_[i];
            catalog_titles_.set_string(i, f.name.empty() ? f.id : f.name);
            catalog_details_.set_string(i, f.material.empty()
                                               ? f.manufacturer
                                               : f.manufacturer + " \xC2\xB7 " + f.material);
            const std::string& hex = f.color_hexes.empty() ? f.color_hex : f.color_hexes.front();
            uint32_t rgb = 0x808080;
            helix::parse_hex_color(hex.c_str(), rgb);
            catalog_colors_.set_color(i, rgb);
            catalog_edges_.set_int(i, helix::ui::swatch_needs_edge_here(rgb) ? 1 : 0);
        }
        lv_subject_set_int(&catalog_count_subject_, static_cast<int32_t>(catalog_results_.size()));
    }
    set_catalog_state(catalog_results_.empty() ? CatalogState::NoResults : CatalogState::Results);
    spdlog::debug("[SpoolWizard] SpoolmanDB rows applied: {} in {} us", catalog_results_.size(),
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count());
}

void SpoolWizardOverlay::select_catalog_result(int index) {
    if (index < 0 || index >= static_cast<int>(catalog_results_.size())) {
        spdlog::warn("[{}] Invalid SpoolmanDB result index: {}", get_name(), index);
        return;
    }
    const helix::ExternalFilament picked = catalog_results_[static_cast<size_t>(index)];

    // The pick ends the search: typing that settles now, or an answer still on
    // its way, must not run over it.
    if (search_timer_) {
        lv_timer_set_repeat_count(search_timer_, 0);
        lv_timer_pause(search_timer_);
    }
    catalog_has_pending_ = false;

    // A vendor the server already has is reused; otherwise it is created on save.
    const int vendor = find_server_vendor(all_vendors_, picked.manufacturer);
    selected_vendor_ = vendor >= 0 ? all_vendors_[static_cast<size_t>(vendor)]
                                   : VendorEntry{picked.manufacturer, -1, false};
    new_vendor_name_.clear();
    new_vendor_url_.clear();
    // The filament step behind the spool step lists this vendor's filaments,
    // so Back lands on a loaded list. It resets the selection, so it goes first.
    load_filaments();
    selected_filament_ = entry_from_catalog(picked);
    creating_new_filament_ = false;
    spdlog::info("[{}] Picked SpoolmanDB '{}' ({}; vendor server_id={})", get_name(), picked.name,
                 picked.id, selected_vendor_.server_id);
    publish_vendor_selection();
    publish_filament_summary();

    auto show_details = [this]() {
        navigate_to_step(Step::SPOOL_DETAILS);
        enter_spool_details();
    };
    IMoonrakerAPI* api = get_moonraker_api();
    if (selected_vendor_.server_id < 0 || !api) {
        show_details();
        return;
    }

    // An existing vendor may already have this filament; reuse it rather than
    // create a duplicate.
    const int vendor_id = selected_vendor_.server_id;
    const uint64_t ticket = catalog_.begin();
    api->spoolman().get_spoolman_filaments(
        vendor_id,
        lifetime_.bg_cb(
            "SpoolWizard::catalog_pick_filaments",
            [this, ticket, vendor_id, show_details](const std::vector<FilamentInfo>& filaments) {
                if (!catalog_.is_current(ticket)) {
                    return;
                }
                const std::string& colors = selected_filament_.multi_color_hexes.empty()
                                                ? selected_filament_.color_hex
                                                : selected_filament_.multi_color_hexes;
                if (const FilamentInfo* f = helix::spoolman::find_catalog_filament(
                        filaments, vendor_id, selected_filament_.name, selected_filament_.material,
                        colors)) {
                    selected_filament_.server_id = f->id;
                    selected_filament_.vendor_id = vendor_id;
                    selected_filament_.from_server = true;
                }
                show_details();
            }),
        lifetime_.bg_cb("SpoolWizard::catalog_pick_filaments_error",
                        [this, ticket, show_details](const MoonrakerError& err) {
                            if (!catalog_.is_current(ticket)) {
                                return;
                            }
                            spdlog::warn("[SpoolWizard] Could not list the vendor's filaments "
                                         "({}); a new one will be created",
                                         err.message);
                            show_details();
                        }));
}
