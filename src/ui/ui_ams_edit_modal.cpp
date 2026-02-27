// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_modal.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_keyboard_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "color_utils.h"
#include "filament_database.h"
#include "format_utils.h"
#include "moonraker_api.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace helix::ui {

// Static member initialization
bool AmsEditModal::callbacks_registered_ = false;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsEditModal::AmsEditModal() {
    spdlog::debug("[AmsEditModal] Constructed");
}

AmsEditModal::~AmsEditModal() {
    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();

    // Modal destructor will call hide() if visible
    spdlog::trace("[AmsEditModal] Destroyed");
}

AmsEditModal::AmsEditModal(AmsEditModal&& other) noexcept
    : Modal(std::move(other)), slot_index_(other.slot_index_),
      original_info_(std::move(other.original_info_)),
      working_info_(std::move(other.working_info_)), api_(other.api_),
      completion_callback_(std::move(other.completion_callback_)),
      remaining_pre_edit_pct_(other.remaining_pre_edit_pct_),
      color_picker_(std::move(other.color_picker_)),
      subjects_initialized_(other.subjects_initialized_),
      cached_spools_(std::move(other.cached_spools_)) {
    // Copy buffers
    std::memcpy(slot_indicator_buf_, other.slot_indicator_buf_, sizeof(slot_indicator_buf_));
    std::memcpy(color_name_buf_, other.color_name_buf_, sizeof(color_name_buf_));
    std::memcpy(temp_nozzle_buf_, other.temp_nozzle_buf_, sizeof(temp_nozzle_buf_));
    std::memcpy(temp_bed_buf_, other.temp_bed_buf_, sizeof(temp_bed_buf_));
    std::memcpy(remaining_pct_buf_, other.remaining_pct_buf_, sizeof(remaining_pct_buf_));

    // Subjects are not movable - they stay with original
    other.subjects_initialized_ = false;
    other.api_ = nullptr;
    other.slot_index_ = -1;
}

AmsEditModal& AmsEditModal::operator=(AmsEditModal&& other) noexcept {
    if (this != &other) {
        Modal::operator=(std::move(other));
        slot_index_ = other.slot_index_;
        original_info_ = std::move(other.original_info_);
        working_info_ = std::move(other.working_info_);
        api_ = other.api_;
        completion_callback_ = std::move(other.completion_callback_);
        remaining_pre_edit_pct_ = other.remaining_pre_edit_pct_;
        color_picker_ = std::move(other.color_picker_);
        subjects_initialized_ = other.subjects_initialized_;
        cached_spools_ = std::move(other.cached_spools_);
        std::memcpy(slot_indicator_buf_, other.slot_indicator_buf_, sizeof(slot_indicator_buf_));
        std::memcpy(color_name_buf_, other.color_name_buf_, sizeof(color_name_buf_));
        std::memcpy(temp_nozzle_buf_, other.temp_nozzle_buf_, sizeof(temp_nozzle_buf_));
        std::memcpy(temp_bed_buf_, other.temp_bed_buf_, sizeof(temp_bed_buf_));
        std::memcpy(remaining_pct_buf_, other.remaining_pct_buf_, sizeof(remaining_pct_buf_));
        other.subjects_initialized_ = false;
        other.api_ = nullptr;
        other.slot_index_ = -1;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void AmsEditModal::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

bool AmsEditModal::show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                                 MoonrakerAPI* api) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Initialize subjects if needed
    init_subjects();

    // Store state
    slot_index_ = slot_index;
    original_info_ = initial_info;
    working_info_ = initial_info;
    api_ = api;
    remaining_pre_edit_pct_ = 0;
    cached_spools_.clear();

    // Reset remaining mode subject before showing (0 = view mode)
    lv_subject_set_int(&remaining_mode_subject_, 0);

    // Set static active instance BEFORE Modal::show() so callbacks during
    // on_show() (e.g., async fetch triggers) can resolve the instance
    s_active_instance_ = this;

    // Show the modal via Modal
    if (!Modal::show(parent)) {
        s_active_instance_ = nullptr;
        return false;
    }

    // Determine first view: picker for empty slots with Spoolman, form otherwise
    bool has_spoolman = false;
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (spoolman_subj) {
        has_spoolman = lv_subject_get_int(spoolman_subj) == 1;
    }
    bool slot_empty = initial_info.material.empty() && initial_info.brand.empty();

    if (has_spoolman && slot_empty && initial_info.spoolman_id == 0) {
        switch_to_picker();
    } else {
        switch_to_form();
    }

    spdlog::info("[AmsEditModal] Shown for slot {} (spoolman_id={}, brand={}, material={})",
                 slot_index, initial_info.spoolman_id, initial_info.brand, initial_info.material);
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void AmsEditModal::on_show() {
    // Create callback guard for async operations [L012]
    callback_guard_ = std::make_shared<bool>(true);

    // Fetch vendor list from Spoolman (async, will update dropdown when ready)
    fetch_vendors_from_spoolman();

    // Bind labels to subjects for reactive text updates (save observers for cleanup)
    lv_obj_t* slot_indicator = find_widget("slot_indicator");
    if (slot_indicator) {
        slot_indicator_observer_ =
            lv_label_bind_text(slot_indicator, &slot_indicator_subject_, nullptr);
    }

    lv_obj_t* color_name_label = find_widget("color_name_label");
    if (color_name_label) {
        color_name_observer_ = lv_label_bind_text(color_name_label, &color_name_subject_, nullptr);
    }

    lv_obj_t* temp_nozzle_label = find_widget("temp_nozzle_label");
    if (temp_nozzle_label) {
        temp_nozzle_observer_ =
            lv_label_bind_text(temp_nozzle_label, &temp_nozzle_subject_, nullptr);
    }

    lv_obj_t* temp_bed_label = find_widget("temp_bed_label");
    if (temp_bed_label) {
        temp_bed_observer_ = lv_label_bind_text(temp_bed_label, &temp_bed_subject_, nullptr);
    }

    lv_obj_t* remaining_pct_label = find_widget("remaining_pct_label");
    if (remaining_pct_label) {
        remaining_pct_observer_ =
            lv_label_bind_text(remaining_pct_label, &remaining_pct_subject_, nullptr);
    }

    lv_obj_t* save_btn_label = find_widget("btn_save_label");
    if (save_btn_label) {
        save_btn_text_observer_ =
            lv_label_bind_text(save_btn_label, &save_btn_text_subject_, nullptr);
    }

    // Update the modal UI with current slot data
    update_ui();

    // Set initial sync button state (disabled since nothing is dirty yet)
    update_sync_button_state();

    // Set initial Spoolman button state
    update_spoolman_button_state();

    // Wire software keyboard to picker search input
    lv_obj_t* picker_search = find_widget("picker_search");
    if (picker_search) {
        KeyboardManager::instance().register_textarea(picker_search);
    }
}

void AmsEditModal::on_hide() {
    // Clear static active instance
    s_active_instance_ = nullptr;

    // Invalidate callback guard to prevent async callbacks from using stale 'this' [L012]
    callback_guard_.reset();

    // Check if LVGL is initialized - may be called from destructor during static destruction
    if (!lv_is_initialized()) {
        return;
    }

    // Observer cleanup is handled by SubjectManager::deinit_all() in deinit_subjects()
    // which calls lv_subject_deinit() on each subject. This properly removes all
    // attached observers from the subject side. We avoid manual lv_observer_remove()
    // because the destructor calls deinit_subjects() before the Modal base destructor
    // calls on_hide(), which would leave us with stale observer pointers.

    // Reset edit mode subject
    if (subjects_initialized_) {
        lv_subject_set_int(&remaining_mode_subject_, 0);
        lv_subject_set_int(&view_mode_subject_, 0);
        lv_subject_set_int(&picker_state_subject_, 0);
    }

    // Clear cached picker data
    cached_spools_.clear();

    spdlog::debug("[AmsEditModal] on_hide()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize string subjects with empty/default buffers (local binding only, not XML
    // registered)
    slot_indicator_buf_[0] = '-';
    slot_indicator_buf_[1] = '-';
    slot_indicator_buf_[2] = '\0';
    color_name_buf_[0] = '\0';
    snprintf(temp_nozzle_buf_, sizeof(temp_nozzle_buf_), "200-230°C");
    snprintf(temp_bed_buf_, sizeof(temp_bed_buf_), "60°C");
    snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "100%%");

    lv_subject_init_string(&slot_indicator_subject_, slot_indicator_buf_, nullptr,
                           sizeof(slot_indicator_buf_), "--");
    subjects_.register_subject(&slot_indicator_subject_);

    lv_subject_init_string(&color_name_subject_, color_name_buf_, nullptr, sizeof(color_name_buf_),
                           "");
    subjects_.register_subject(&color_name_subject_);

    lv_subject_init_string(&temp_nozzle_subject_, temp_nozzle_buf_, nullptr,
                           sizeof(temp_nozzle_buf_), "200-230°C");
    subjects_.register_subject(&temp_nozzle_subject_);

    lv_subject_init_string(&temp_bed_subject_, temp_bed_buf_, nullptr, sizeof(temp_bed_buf_),
                           "60°C");
    subjects_.register_subject(&temp_bed_subject_);

    lv_subject_init_string(&remaining_pct_subject_, remaining_pct_buf_, nullptr,
                           sizeof(remaining_pct_buf_), "100%");
    subjects_.register_subject(&remaining_pct_subject_);

    // Initialize save button text subject
    snprintf(save_btn_text_buf_, sizeof(save_btn_text_buf_), "Close");
    lv_subject_init_string(&save_btn_text_subject_, save_btn_text_buf_, nullptr,
                           sizeof(save_btn_text_buf_), "Close");
    subjects_.register_subject(&save_btn_text_subject_);

    // Initialize remaining mode subject (0=view, 1=edit) - registered globally for XML binding
    UI_MANAGED_SUBJECT_INT(remaining_mode_subject_, 0, "edit_remaining_mode", subjects_);

    // Initialize view mode subject (0=form, 1=picker) - registered globally for XML binding
    UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "edit_modal_view", subjects_);

    // Initialize picker state subject (0=loading, 1=empty, 2=content) - registered globally
    UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[AmsEditModal] Subjects initialized");
}

void AmsEditModal::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[AmsEditModal] Subjects deinitialized");
}

void AmsEditModal::fetch_vendors_from_spoolman() {
    if (!api_ || vendors_loaded_) {
        return;
    }

    // Skip Spoolman API call if not configured (avoids "method not found" toast)
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (spoolman_subj && lv_subject_get_int(spoolman_subj) != 1) {
        return;
    }

    // Capture callback guard for async safety [L012]
    std::weak_ptr<bool> guard = callback_guard_;

    api_->spoolman().get_spoolman_spools(
        [this, guard](const std::vector<SpoolInfo>& spools) {
            // Extract vendor list on this thread (WebSocket), then marshal to main
            std::set<std::string> unique_vendors;
            unique_vendors.insert("Generic"); // Always have Generic as first option
            for (const auto& spool : spools) {
                if (!spool.vendor.empty()) {
                    unique_vendors.insert(spool.vendor);
                }
            }

            // Build vendor list and options string (local copies, no member access)
            std::vector<std::string> vendors;
            std::string options;
            for (const auto& vendor : unique_vendors) {
                if (!options.empty()) {
                    options += '\n';
                }
                options += vendor;
                vendors.push_back(vendor);
            }

            // Marshal member writes to main thread
            helix::ui::queue_update([this, guard, vendors = std::move(vendors),
                                     options = std::move(options)]() mutable {
                if (guard.expired()) {
                    return;
                }
                vendor_list_ = std::move(vendors);
                vendor_options_ = std::move(options);
                vendors_loaded_ = true;
                spdlog::debug("[AmsEditModal] Loaded {} vendors from Spoolman",
                              vendor_list_.size());
                update_vendor_dropdown();
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AmsEditModal] Failed to fetch Spoolman spools for vendor list: {}",
                         err.message);
            // Keep using fallback vendors
        });
}

void AmsEditModal::update_vendor_dropdown() {
    if (!dialog_ || vendor_options_.empty()) {
        return;
    }

    lv_obj_t* vendor_dropdown = find_widget("vendor_dropdown");
    if (!vendor_dropdown) {
        return;
    }

    lv_dropdown_set_options(vendor_dropdown, vendor_options_.c_str());

    // Set selection based on working_info_.brand
    int vendor_idx = 0; // Default to first (Generic)
    for (size_t i = 0; i < vendor_list_.size(); i++) {
        if (working_info_.brand == vendor_list_[i]) {
            vendor_idx = static_cast<int>(i);
            break;
        }
    }
    lv_dropdown_set_selected(vendor_dropdown, vendor_idx);
}

// ============================================================================
// View Switching
// ============================================================================

void AmsEditModal::switch_to_picker() {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&view_mode_subject_, 1);
    populate_picker();
    spdlog::debug("[AmsEditModal] Switched to picker view");
}

void AmsEditModal::switch_to_form() {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&view_mode_subject_, 0);
    spdlog::debug("[AmsEditModal] Switched to form view");
}

void AmsEditModal::populate_picker() {
    if (!dialog_ || !api_) {
        lv_subject_set_int(&picker_state_subject_, 1);
        return;
    }

    // Show loading state
    lv_subject_set_int(&picker_state_subject_, 0);

    // Clear search input
    lv_obj_t* search = find_widget("picker_search");
    if (search) {
        lv_textarea_set_text(search, "");
    }

    // Use weak_ptr pattern for async callback safety [L012]
    std::weak_ptr<bool> weak_guard = callback_guard_;

    api_->spoolman().get_spoolman_spools(
        [this, weak_guard](const std::vector<SpoolInfo>& spools) {
            helix::ui::queue_update([this, weak_guard, spools]() {
                if (weak_guard.expired() || !dialog_ || !subjects_initialized_) {
                    return;
                }

                if (spools.empty()) {
                    lv_subject_set_int(&picker_state_subject_, 1);
                    return;
                }

                cached_spools_ = spools;
                render_spool_list("");
            });
        },
        [this, weak_guard](const MoonrakerError& err) {
            helix::ui::queue_update([this, weak_guard, msg = err.message]() {
                if (weak_guard.expired() || !dialog_ || !subjects_initialized_) {
                    return;
                }
                spdlog::warn("[AmsEditModal] Failed to fetch spools: {}", msg);
                lv_subject_set_int(&picker_state_subject_, 1);
            });
        });
}

void AmsEditModal::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = find_widget("picker_spool_list");
    if (!spool_list) {
        return;
    }

    lv_obj_clean(spool_list);

    // Reuse shared filter_spools() from spoolman_types
    auto filtered = filter_spools(cached_spools_, filter);

    // Get spool IDs assigned to other tools (exclude current slot's tool)
    auto in_use = ToolState::instance().assigned_spool_ids(slot_index_);

    for (const auto& spool : filtered) {
        lv_obj_t* item =
            static_cast<lv_obj_t*>(lv_xml_create(spool_list, "spoolman_spool_item", nullptr));
        if (!item) {
            continue;
        }

        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(spool.id)));

        lv_obj_t* name_label = lv_obj_find_by_name(item, "spool_name");
        if (name_label) {
            std::string name = "#" + std::to_string(spool.id) + " ";
            name += spool.vendor.empty() ? spool.material : (spool.vendor + " " + spool.material);
            lv_label_set_text(name_label, name.c_str());
        }

        lv_obj_t* color_label = lv_obj_find_by_name(item, "spool_color");
        if (color_label && !spool.color_name.empty()) {
            lv_label_set_text(color_label, spool.color_name.c_str());
        }

        lv_obj_t* weight_label = lv_obj_find_by_name(item, "spool_weight");
        if (weight_label && spool.remaining_weight_g > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fg", spool.remaining_weight_g);
            lv_label_set_text(weight_label, buf);
        }

        lv_obj_t* swatch = lv_obj_find_by_name(item, "spool_swatch");
        if (swatch && !spool.color_hex.empty()) {
            const char* hex = spool.color_hex.c_str();
            if (hex[0] == '#') {
                hex++;
            }
            uint32_t hex_val = static_cast<uint32_t>(strtoul(hex, nullptr, 16));
            lv_color_t color = lv_color_hex(hex_val);
            lv_obj_set_style_bg_color(swatch, color, 0);
            lv_obj_set_style_border_color(swatch, color, 0);
        }

        // Mark current spool as checked (matching spoolman list view pattern)
        bool is_selected = (spool.id == working_info_.spoolman_id);
        lv_obj_set_state(item, LV_STATE_CHECKED, is_selected);
        if (is_selected) {
            lv_obj_t* check_icon = lv_obj_find_by_name(item, "selected_icon");
            if (check_icon) {
                lv_obj_remove_flag(check_icon, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Disable spools already assigned to other tools
        if (in_use.count(spool.id)) {
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_subject_set_int(&picker_state_subject_, filtered.empty() ? 1 : 2);
    spdlog::debug("[AmsEditModal] Rendered {} spool items (filter='{}')", filtered.size(), filter);
}

void AmsEditModal::handle_spool_selected(int spool_id) {
    spdlog::info("[AmsEditModal] Spool {} selected for slot {}", spool_id, slot_index_);

    // Look up SpoolInfo from cached spools
    for (const auto& spool : cached_spools_) {
        if (spool.id == spool_id) {
            // Auto-fill working_info_ from the selected spool
            working_info_.spoolman_id = spool.id;
            working_info_.color_name = spool.color_name;
            working_info_.material = spool.material;
            working_info_.brand = spool.vendor;
            working_info_.spool_name = spool.vendor + " " + spool.material;
            working_info_.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
            working_info_.total_weight_g = static_cast<float>(spool.initial_weight_g);
            working_info_.nozzle_temp_min = spool.nozzle_temp_min;
            working_info_.nozzle_temp_max = spool.nozzle_temp_max;
            working_info_.bed_temp = spool.bed_temp_recommended;

            // Parse color hex to RGB
            if (!spool.color_hex.empty()) {
                uint32_t rgb = 0;
                if (helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
                    working_info_.color_rgb = rgb;
                } else {
                    spdlog::warn("[AmsEditModal] Failed to parse color hex: {}", spool.color_hex);
                }
            }

            break;
        }
    }

    // Switch to form view and refresh UI
    switch_to_form();
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditModal::handle_manual_entry() {
    spdlog::debug("[AmsEditModal] Manual entry requested - switching to form");
    switch_to_form();
}

void AmsEditModal::handle_change_spool() {
    spdlog::debug("[AmsEditModal] Change spool requested - switching to picker");
    switch_to_picker();
}

void AmsEditModal::handle_picker_search(const char* text) {
    if (cached_spools_.empty()) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditModal::handle_unlink() {
    spdlog::info("[AmsEditModal] Unlink requested for slot {}", slot_index_);
    working_info_.spoolman_id = 0;
    working_info_.spool_name.clear();
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditModal::update_spoolman_button_state() {
    if (!dialog_) {
        return;
    }

    lv_obj_t* btn_change = find_widget("btn_change_spool");
    lv_obj_t* btn_unlink = find_widget("btn_unlink_spool");

    if (working_info_.spoolman_id > 0) {
        // Linked to Spoolman: show "Change Spool" and "Unlink"
        if (btn_change) {
            ui_button_set_text(btn_change, "Change Spool");
        }
        if (btn_unlink) {
            lv_obj_remove_flag(btn_unlink, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Not linked: show "Link to Spoolman", hide "Unlink"
        if (btn_change) {
            ui_button_set_text(btn_change, "Link to Spoolman");
        }
        if (btn_unlink) {
            lv_obj_add_flag(btn_unlink, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void AmsEditModal::update_ui() {
    if (!dialog_) {
        return;
    }

    // Update slot indicator via subject (used in header)
    if (slot_index_ < 0) {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), "External Filament");
    } else {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), "Slot %d Filament",
                 slot_index_ + 1);
    }
    lv_subject_copy_string(&slot_indicator_subject_, slot_indicator_buf_);

    // Update Spoolman ID label in header
    lv_obj_t* spoolman_label = find_widget("spoolman_id_label");
    if (spoolman_label) {
        if (working_info_.spoolman_id > 0) {
            char spoolman_text[32];
            snprintf(spoolman_text, sizeof(spoolman_text), "(Spoolman #%d)",
                     working_info_.spoolman_id);
            lv_label_set_text(spoolman_label, spoolman_text);
            lv_obj_remove_flag(spoolman_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(spoolman_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Build material options from filament database (if not already built)
    if (material_list_.empty()) {
        auto all_materials = filament::get_all_material_names();
        material_list_.reserve(all_materials.size());
        for (const char* mat : all_materials) {
            if (!material_options_.empty()) {
                material_options_ += '\n';
            }
            material_options_ += mat;
            material_list_.push_back(mat);
        }
        spdlog::debug("[AmsEditModal] Built material list with {} materials from database",
                      material_list_.size());
    }

    // Set up vendor dropdown (use cached vendors from Spoolman, or fallback)
    lv_obj_t* vendor_dropdown = find_widget("vendor_dropdown");
    if (vendor_dropdown) {
        if (!vendor_options_.empty()) {
            // Use vendors from Spoolman
            lv_dropdown_set_options(vendor_dropdown, vendor_options_.c_str());
        } else {
            // Fallback: static vendor list while Spoolman query is pending
            static const char* fallback_vendors =
                "Generic\nPolymaker\nBambu\neSUN\nOverture\nPrusa\nHatchbox";
            lv_dropdown_set_options(vendor_dropdown, fallback_vendors);

            // Build fallback vendor_list_ for index lookup
            if (vendor_list_.empty()) {
                vendor_list_ = {"Generic",  "Polymaker", "Bambu",   "eSUN",
                                "Overture", "Prusa",     "Hatchbox"};
            }
        }

        // Set initial selection based on working_info_.brand
        int vendor_idx = 0; // Default to first
        for (size_t i = 0; i < vendor_list_.size(); i++) {
            if (working_info_.brand == vendor_list_[i]) {
                vendor_idx = static_cast<int>(i);
                break;
            }
        }
        lv_dropdown_set_selected(vendor_dropdown, vendor_idx);
    }

    // Set up material dropdown from filament database
    lv_obj_t* material_dropdown = find_widget("material_dropdown");
    if (material_dropdown) {
        lv_dropdown_set_options(material_dropdown, material_options_.c_str());

        // Set initial selection based on working_info_.material
        int material_idx = 0; // Default to first (PLA)
        for (size_t i = 0; i < material_list_.size(); i++) {
            if (working_info_.material == material_list_[i]) {
                material_idx = static_cast<int>(i);
                break;
            }
        }
        lv_dropdown_set_selected(material_dropdown, material_idx);
    }

    // Update color swatch
    lv_obj_t* color_swatch = find_widget("color_swatch");
    if (color_swatch) {
        lv_obj_set_style_bg_color(color_swatch, lv_color_hex(working_info_.color_rgb), 0);
    }

    // Update color name label via subject
    if (!working_info_.color_name.empty()) {
        snprintf(color_name_buf_, sizeof(color_name_buf_), "%s", working_info_.color_name.c_str());
    } else {
        color_name_buf_[0] = '\0';
    }
    lv_subject_copy_string(&color_name_subject_, color_name_buf_);

    // Update remaining slider and label
    // Use synthetic 1000g total if no weight data (manual spool without Spoolman)
    if (working_info_.total_weight_g <= 0) {
        working_info_.total_weight_g = 1000.0f;
        working_info_.remaining_weight_g =
            (working_info_.remaining_weight_g > 0) ? working_info_.remaining_weight_g : 1000.0f;
    }
    int remaining_pct =
        static_cast<int>(100.0f * working_info_.remaining_weight_g / working_info_.total_weight_g);
    remaining_pct = std::max(0, std::min(100, remaining_pct));

    lv_obj_t* remaining_slider = find_widget("remaining_slider");
    if (remaining_slider) {
        lv_slider_set_value(remaining_slider, remaining_pct, LV_ANIM_OFF);
    }

    // Update remaining percentage label via subject
    helix::format::format_percent(remaining_pct, remaining_pct_buf_, sizeof(remaining_pct_buf_));
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    // Update progress bar fill width (shown in view mode)
    // Use percentage width to avoid layout timing issues
    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (progress_fill) {
        lv_obj_set_width(progress_fill, lv_pct(remaining_pct));
    }

    // Update temperature display based on material
    update_temp_display();
}

void AmsEditModal::update_temp_display() {
    if (!dialog_) {
        return;
    }

    // Get temperature range from slot info (populated from Spoolman or material defaults)
    int nozzle_min = working_info_.nozzle_temp_min;
    int nozzle_max = working_info_.nozzle_temp_max;
    int bed_temp = working_info_.bed_temp;

    // Fall back to material-based defaults from filament database if not set
    if (nozzle_min == 0 && nozzle_max == 0 && !working_info_.material.empty()) {
        auto mat_info = filament::find_material(working_info_.material);
        if (mat_info) {
            nozzle_min = mat_info->nozzle_min;
            nozzle_max = mat_info->nozzle_max;
            bed_temp = mat_info->bed_temp;
            spdlog::debug("[AmsEditModal] Using filament database temps for {}: {}-{}°C nozzle, "
                          "{}°C bed",
                          working_info_.material, nozzle_min, nozzle_max, bed_temp);
        } else {
            // Fallback to PLA defaults for unknown materials
            auto pla_info = filament::find_material("PLA");
            if (pla_info) {
                nozzle_min = pla_info->nozzle_min;
                nozzle_max = pla_info->nozzle_max;
                bed_temp = pla_info->bed_temp;
            } else {
                // Ultimate fallback (should never happen - PLA is in database)
                nozzle_min = 200;
                nozzle_max = 230;
                bed_temp = 60;
            }
            spdlog::debug("[AmsEditModal] Material '{}' not in database, using PLA defaults",
                          working_info_.material);
        }
    }

    // Update nozzle temp label via subject
    snprintf(temp_nozzle_buf_, sizeof(temp_nozzle_buf_), "%d-%d°C", nozzle_min, nozzle_max);
    lv_subject_copy_string(&temp_nozzle_subject_, temp_nozzle_buf_);

    // Update bed temp label via subject
    snprintf(temp_bed_buf_, sizeof(temp_bed_buf_), "%d°C", bed_temp);
    lv_subject_copy_string(&temp_bed_subject_, temp_bed_buf_);
}

bool AmsEditModal::is_dirty() const {
    // Compare relevant fields that can be edited
    return working_info_.color_rgb != original_info_.color_rgb ||
           working_info_.material != original_info_.material ||
           working_info_.brand != original_info_.brand ||
           working_info_.spoolman_id != original_info_.spoolman_id ||
           std::abs(working_info_.remaining_weight_g - original_info_.remaining_weight_g) > 0.1f;
}

void AmsEditModal::update_sync_button_state() {
    if (!dialog_) {
        return;
    }

    bool dirty = is_dirty();

    // Update save button text based on dirty state
    const char* btn_text = dirty ? "Save" : "Close";
    snprintf(save_btn_text_buf_, sizeof(save_btn_text_buf_), "%s", btn_text);
    lv_subject_copy_string(&save_btn_text_subject_, save_btn_text_buf_);
}

void AmsEditModal::show_color_picker() {
    if (!parent_) {
        spdlog::warn("[AmsEditModal] No parent for color picker");
        return;
    }

    // Create picker on first use (lazy initialization)
    if (!color_picker_) {
        color_picker_ = std::make_unique<ColorPicker>();
    }

    // Set callback to update edit modal when color is selected
    color_picker_->set_color_callback([this](uint32_t color_rgb, const std::string& color_name) {
        // Update the working slot info with selected color
        working_info_.color_rgb = color_rgb;
        working_info_.color_name = color_name;

        // Update the edit modal's color swatch to show new selection
        if (dialog_) {
            lv_obj_t* swatch = find_widget("color_swatch");
            if (swatch) {
                lv_obj_set_style_bg_color(swatch, lv_color_hex(color_rgb), 0);
            }

            // Update color name label via subject
            snprintf(color_name_buf_, sizeof(color_name_buf_), "%s", color_name.c_str());
            lv_subject_copy_string(&color_name_subject_, color_name_buf_);

            update_sync_button_state();
        }
    });

    // Show with current edit color
    color_picker_->show_with_color(parent_, working_info_.color_rgb);
}

// ============================================================================
// Save Orchestration
// ============================================================================

void AmsEditModal::fire_completion(bool saved) {
    if (completion_callback_) {
        EditResult result;
        result.saved = saved;
        result.slot_index = slot_index_;
        result.slot_info = working_info_;
        completion_callback_(result);
    }
    hide();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsEditModal::handle_close() {
    spdlog::debug("[AmsEditModal] Close requested");
    fire_completion(false);
}

void AmsEditModal::handle_vendor_changed(int index) {
    if (index >= 0 && index < static_cast<int>(vendor_list_.size())) {
        working_info_.brand = vendor_list_[index];
        spdlog::debug("[AmsEditModal] Vendor changed to: {}", working_info_.brand);
        update_sync_button_state();
    }
}

void AmsEditModal::handle_material_changed(int index) {
    if (index >= 0 && index < static_cast<int>(material_list_.size())) {
        working_info_.material = material_list_[index];
        spdlog::debug("[AmsEditModal] Material changed to: {}", working_info_.material);

        // Clear existing temp values so update_temp_display uses material-based defaults
        working_info_.nozzle_temp_min = 0;
        working_info_.nozzle_temp_max = 0;
        working_info_.bed_temp = 0;

        // Update temperature display based on new material
        update_temp_display();
        update_sync_button_state();
    }
}

void AmsEditModal::handle_color_clicked() {
    spdlog::info("[AmsEditModal] Opening color picker");
    show_color_picker();
}

void AmsEditModal::handle_remaining_changed(int percent) {
    if (!dialog_) {
        return;
    }

    // Update the percentage label via subject
    helix::format::format_percent(percent, remaining_pct_buf_, sizeof(remaining_pct_buf_));
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    // Update slot info remaining weight based on percentage
    // Use synthetic 1000g total if no weight data (manual spool without Spoolman)
    if (working_info_.total_weight_g <= 0) {
        working_info_.total_weight_g = 1000.0f;
    }
    working_info_.remaining_weight_g =
        working_info_.total_weight_g * static_cast<float>(percent) / 100.0f;

    update_sync_button_state();
    spdlog::trace("[AmsEditModal] Remaining changed to {}%", percent);
}

void AmsEditModal::handle_remaining_edit() {
    if (!dialog_) {
        return;
    }

    // Store current remaining percentage before entering edit mode
    lv_obj_t* slider = find_widget("remaining_slider");
    if (slider) {
        remaining_pre_edit_pct_ = lv_slider_get_value(slider);
    }

    // Enter edit mode - subject binding will show slider/accept/cancel, hide progress/edit button
    lv_subject_set_int(&remaining_mode_subject_, 1);
    spdlog::debug("[AmsEditModal] Entered remaining edit mode (was {}%)", remaining_pre_edit_pct_);
}

void AmsEditModal::handle_remaining_accept() {
    if (!dialog_) {
        return;
    }

    // Get the current slider value
    lv_obj_t* slider = find_widget("remaining_slider");
    int new_pct = slider ? lv_slider_get_value(slider) : remaining_pre_edit_pct_;

    // Update the progress bar fill to match
    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (progress_fill) {
        lv_obj_set_width(progress_fill, lv_pct(new_pct));
    }

    // Exit edit mode - subject binding will show progress/edit button, hide slider/accept/cancel
    lv_subject_set_int(&remaining_mode_subject_, 0);
    spdlog::debug("[AmsEditModal] Accepted remaining edit: {}%", new_pct);
}

void AmsEditModal::handle_remaining_cancel() {
    if (!dialog_) {
        return;
    }

    // Revert slider to pre-edit value
    lv_obj_t* slider = find_widget("remaining_slider");
    if (slider) {
        lv_slider_set_value(slider, remaining_pre_edit_pct_, LV_ANIM_OFF);
    }

    // Revert the percentage label via subject
    helix::format::format_percent(remaining_pre_edit_pct_, remaining_pct_buf_,
                                  sizeof(remaining_pct_buf_));
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    // Revert the remaining weight in working_info_
    if (working_info_.total_weight_g > 0) {
        working_info_.remaining_weight_g =
            working_info_.total_weight_g * static_cast<float>(remaining_pre_edit_pct_) / 100.0f;
    }

    // Exit edit mode
    lv_subject_set_int(&remaining_mode_subject_, 0);
    update_sync_button_state();
    spdlog::debug("[AmsEditModal] Cancelled remaining edit (reverted to {}%)",
                  remaining_pre_edit_pct_);
}

void AmsEditModal::handle_reset() {
    spdlog::debug("[AmsEditModal] Cancelling - discarding changes");

    // Discard changes and close
    working_info_ = original_info_;
    fire_completion(false);
}

void AmsEditModal::handle_save() {
    spdlog::info("[AmsEditModal] Saving edits for slot {}", slot_index_);

    // If slot is linked to Spoolman and there are changes, use SpoolmanSlotSaver
    if (working_info_.spoolman_id > 0 && api_) {
        auto changes = helix::SpoolmanSlotSaver::detect_changes(original_info_, working_info_);
        if (changes.any()) {
            std::weak_ptr<bool> guard = callback_guard_;
            auto saver = std::make_shared<helix::SpoolmanSlotSaver>(api_);
            saver->save(original_info_, working_info_, [this, guard, saver](bool success) {
                if (guard.expired()) {
                    return;
                }
                // Spoolman callback arrives on a background thread — defer
                // to the UI thread before touching LVGL subjects/widgets.
                helix::ui::queue_update([this, guard, success]() {
                    if (guard.expired()) {
                        return;
                    }
                    if (!success) {
                        spdlog::error("[AmsEditModal] Spoolman save failed, saving locally");
                    }
                    fire_completion(true);
                });
            });
            return; // Async path - fire_completion called from callback
        }
    }

    // No Spoolman changes (or no Spoolman) - save locally immediately
    fire_completion(true);
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsEditModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"ams_edit_modal_close_cb", on_close_cb},
        {"ams_edit_vendor_changed_cb", on_vendor_changed_cb},
        {"ams_edit_material_changed_cb", on_material_changed_cb},
        {"ams_edit_color_clicked_cb", on_color_clicked_cb},
        {"ams_edit_remaining_changed_cb", on_remaining_changed_cb},
        {"ams_edit_remaining_edit_cb", on_remaining_edit_cb},
        {"ams_edit_remaining_accept_cb", on_remaining_accept_cb},
        {"ams_edit_remaining_cancel_cb", on_remaining_cancel_cb},
        {"ams_edit_reset_cb", on_reset_cb},
        {"ams_edit_save_cb", on_save_cb},
        {"ams_edit_manual_entry_cb", on_manual_entry_cb},
        {"ams_edit_change_spool_cb", on_change_spool_cb},
        {"ams_edit_unlink_cb", on_unlink_cb},
        {"ams_edit_picker_search_cb", on_picker_search_cb},
        // Register handler for spool_item clicks (shared component uses this callback name)
        {"spoolman_spool_item_clicked_cb", on_spool_item_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsEditModal] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via User Data)
// ============================================================================

AmsEditModal* AmsEditModal::s_active_instance_ = nullptr;

AmsEditModal* AmsEditModal::get_instance_from_event(lv_event_t* /*e*/) {
    // Use static active instance — only one edit modal can be open at a time.
    // The old parent-walk approach was unsafe: any ancestor with user_data
    // (e.g., panels, screens) would be miscast as AmsEditModal*.
    if (!s_active_instance_) {
        spdlog::warn("[AmsEditModal] Callback fired with no active instance");
    }
    return s_active_instance_;
}

void AmsEditModal::on_close_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_close();
    }
}

void AmsEditModal::on_vendor_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_vendor_changed(index);
    }
}

void AmsEditModal::on_material_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_material_changed(index);
    }
}

void AmsEditModal::on_color_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_color_clicked();
    }
}

void AmsEditModal::on_remaining_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int value = lv_slider_get_value(slider);
        self->handle_remaining_changed(value);
    }
}

void AmsEditModal::on_remaining_edit_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_edit();
    }
}

void AmsEditModal::on_remaining_accept_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_accept();
    }
}

void AmsEditModal::on_remaining_cancel_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_cancel();
    }
}

void AmsEditModal::on_reset_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_reset();
    }
}

void AmsEditModal::on_save_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_save();
    }
}

void AmsEditModal::on_manual_entry_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_manual_entry();
    }
}

void AmsEditModal::on_change_spool_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_change_spool();
    }
}

void AmsEditModal::on_unlink_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_unlink();
    }
}

void AmsEditModal::on_picker_search_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
        const char* text = lv_textarea_get_text(ta);
        self->handle_picker_search(text);
    }
}

void AmsEditModal::on_spool_item_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (!self) {
        return;
    }

    // Use current_target (the button with the handler), not target (the clicked child)
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item) {
        return;
    }
    auto spool_id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
    if (spool_id <= 0) {
        spdlog::warn("[AmsEditModal] Spool item clicked with invalid spool_id={}", spool_id);
        return;
    }
    self->handle_spool_selected(spool_id);
}

} // namespace helix::ui
