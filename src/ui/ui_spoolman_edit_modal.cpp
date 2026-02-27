// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spoolman_edit_modal.h"

#include "ui_keyboard_manager.h"
#include "ui_spool_canvas.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/**
 * Parse a hex color string (with or without '#' prefix) into an lv_color_t.
 * Returns fallback_color if the string is empty or unparseable.
 */
lv_color_t parse_spool_color(const std::string& color_hex, lv_color_t fallback_color) {
    if (color_hex.empty()) {
        return fallback_color;
    }

    const char* hex = color_hex.c_str();
    if (hex[0] == '#') {
        hex++;
    }

    unsigned int color_val = 0;
    if (sscanf(hex, "%x", &color_val) == 1) {
        return lv_color_hex(color_val);
    }
    return fallback_color;
}

} // namespace

namespace helix::ui {

// Static member initialization
bool SpoolEditModal::callbacks_registered_ = false;
SpoolEditModal* SpoolEditModal::active_instance_ = nullptr;

// ============================================================================
// Construction / Destruction
// ============================================================================

SpoolEditModal::SpoolEditModal() {
    spdlog::debug("[SpoolEditModal] Constructed");
}

SpoolEditModal::~SpoolEditModal() {
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
    spdlog::trace("[SpoolEditModal] Destroyed");
}

// ============================================================================
// Public API
// ============================================================================

void SpoolEditModal::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

bool SpoolEditModal::show_for_spool(lv_obj_t* parent, const SpoolInfo& spool, MoonrakerAPI* api) {
    register_callbacks();
    init_subjects();

    original_spool_ = spool;
    working_spool_ = spool;
    api_ = api;

    if (!Modal::show(parent)) {
        return false;
    }

    spdlog::info("[SpoolEditModal] Shown for spool {} ({})", spool.id, spool.display_name());
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void SpoolEditModal::on_show() {
    active_instance_ = this;
    callback_guard_ = std::make_shared<bool>(true);

    // Suppress field change events during initial population — setting textarea
    // text fires VALUE_CHANGED which would read formatted values back, and
    // float→string→float round-trips can drift (e.g. 999.7 → "1000" → 1000.0)
    populating_ = true;
    populate_fields();
    populating_ = false;

    // Snap original values to match formatted field values so is_dirty()
    // compares what the user sees, not raw API doubles
    read_fields_into(original_spool_);
    working_spool_ = original_spool_;

    register_textareas();
    update_spool_preview();
    update_save_button_text();
}

void SpoolEditModal::on_hide() {
    active_instance_ = nullptr;
    callback_guard_.reset();
    deinit_subjects();
    spdlog::debug("[SpoolEditModal] on_hide()");
}

// ============================================================================
// Internal Methods
// ============================================================================

void SpoolEditModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    lv_subject_init_string(&save_button_text_subject_, save_button_text_buf_, nullptr,
                           sizeof(save_button_text_buf_), "Close");
    lv_xml_register_subject(nullptr, "spoolman_edit_save_text", &save_button_text_subject_);

    subjects_initialized_ = true;
}

void SpoolEditModal::deinit_subjects() {
    // Subjects persist for the lifetime of SpoolEditModal — the XML widgets
    // that bind to them are destroyed when the modal hides, but the subjects
    // stay alive so they can be rebound on next show().
}

void SpoolEditModal::populate_fields() {
    if (!dialog_) {
        return;
    }

    // Title
    lv_obj_t* title = find_widget("spool_title");
    if (title) {
        std::string title_text = "Edit Spool #" + std::to_string(working_spool_.id);
        lv_label_set_text(title, title_text.c_str());
    }

    // Read-only info labels
    lv_obj_t* material_label = find_widget("material_label");
    if (material_label) {
        const char* material =
            working_spool_.material.empty() ? "Unknown" : working_spool_.material.c_str();
        lv_label_set_text(material_label, material);
    }

    lv_obj_t* color_label = find_widget("color_label");
    if (color_label) {
        std::string color;
        if (!working_spool_.color_name.empty()) {
            color = working_spool_.color_name;
        } else if (!working_spool_.color_hex.empty()) {
            color = working_spool_.color_hex;
        } else {
            color = "No color";
        }
        lv_label_set_text(color_label, color.c_str());
    }

    lv_obj_t* vendor_label = find_widget("vendor_label");
    if (vendor_label) {
        const char* vendor =
            working_spool_.vendor.empty() ? "Unknown" : working_spool_.vendor.c_str();
        lv_label_set_text(vendor_label, vendor);
    }

    // Editable fields
    lv_obj_t* remaining_field = find_widget("field_remaining");
    if (remaining_field) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", working_spool_.remaining_weight_g);
        lv_textarea_set_text(remaining_field, buf);
    }

    lv_obj_t* spool_weight_field = find_widget("field_spool_weight");
    if (spool_weight_field) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", working_spool_.spool_weight_g);
        lv_textarea_set_text(spool_weight_field, buf);
    }

    lv_obj_t* price_field = find_widget("field_price");
    if (price_field) {
        if (working_spool_.price > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", working_spool_.price);
            lv_textarea_set_text(price_field, buf);
        } else {
            lv_textarea_set_text(price_field, "");
        }
    }

    lv_obj_t* lot_field = find_widget("field_lot_nr");
    if (lot_field) {
        lv_textarea_set_text(lot_field, working_spool_.lot_nr.c_str());
    }

    lv_obj_t* comment_field = find_widget("field_comment");
    if (comment_field) {
        lv_textarea_set_text(comment_field, working_spool_.comment.c_str());
    }
}

void SpoolEditModal::read_fields_into(SpoolInfo& spool) {
    if (!dialog_) {
        return;
    }

    lv_obj_t* field = find_widget("field_remaining");
    if (field) {
        const char* text = lv_textarea_get_text(field);
        spool.remaining_weight_g = (text && text[0]) ? std::atof(text) : 0;
    }

    field = find_widget("field_spool_weight");
    if (field) {
        const char* text = lv_textarea_get_text(field);
        spool.spool_weight_g = (text && text[0]) ? std::atof(text) : 0;
    }

    field = find_widget("field_price");
    if (field) {
        const char* text = lv_textarea_get_text(field);
        spool.price = (text && text[0]) ? std::atof(text) : 0;
    }

    field = find_widget("field_lot_nr");
    if (field) {
        const char* text = lv_textarea_get_text(field);
        spool.lot_nr = text ? text : "";
    }

    field = find_widget("field_comment");
    if (field) {
        const char* text = lv_textarea_get_text(field);
        spool.comment = text ? text : "";
    }
}

void SpoolEditModal::register_textareas() {
    if (!dialog_) {
        return;
    }

    // Field names in tab order — single-line fields first, then multiline Notes
    static constexpr const char* field_names[] = {
        "field_remaining", "field_spool_weight", "field_price", "field_lot_nr", "field_comment",
    };
    static constexpr int num_fields = sizeof(field_names) / sizeof(field_names[0]);

    // Collect textarea widgets
    lv_obj_t* fields[num_fields] = {};
    for (int i = 0; i < num_fields; i++) {
        fields[i] = find_widget(field_names[i]);
    }

    // Register each with keyboard manager (sets up auto-show/hide + adds to input group)
    for (int i = 0; i < num_fields; i++) {
        if (fields[i]) {
            KeyboardManager::instance().register_textarea(fields[i]);
        }
    }

    // Add Enter-to-next-field for single-line fields (not the multiline Notes)
    // LVGL fires LV_EVENT_READY on the textarea when Enter is pressed on a one-line textarea.
    // For multiline textareas, Enter inserts a newline instead (no READY event).
    // We must explicitly re-show the keyboard because LVGL's default keyboard handler
    // hides it on READY before our handler runs.
    for (int i = 0; i < num_fields - 1; i++) {
        if (fields[i] && fields[i + 1]) {
            lv_obj_add_event_cb(
                fields[i],
                [](lv_event_t* e) {
                    auto* next = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
                    if (next) {
                        lv_group_focus_obj(next);
                        KeyboardManager::instance().show(next);
                    }
                },
                LV_EVENT_READY, fields[i + 1]);
        }
    }

    spdlog::debug("[SpoolEditModal] Registered {} textareas with keyboard", num_fields);
}

void SpoolEditModal::update_spool_preview() {
    if (!dialog_) {
        return;
    }

    lv_obj_t* canvas = find_widget("spool_preview");
    if (!canvas) {
        return;
    }

    // Set color from spool's hex color
    lv_color_t color =
        parse_spool_color(working_spool_.color_hex, theme_manager_get_color("text_muted"));
    ui_spool_canvas_set_color(canvas, color);

    // Set fill level from remaining weight
    float fill_level = 0.5f;
    if (working_spool_.initial_weight_g > 0) {
        fill_level = static_cast<float>(working_spool_.remaining_weight_g) /
                     static_cast<float>(working_spool_.initial_weight_g);
        fill_level = std::max(0.0f, std::min(1.0f, fill_level));
    }
    ui_spool_canvas_set_fill_level(canvas, fill_level);
    ui_spool_canvas_redraw(canvas);
}

bool SpoolEditModal::is_dirty() const {
    return std::abs(working_spool_.remaining_weight_g - original_spool_.remaining_weight_g) > 0.1 ||
           std::abs(working_spool_.spool_weight_g - original_spool_.spool_weight_g) > 0.1 ||
           std::abs(working_spool_.price - original_spool_.price) > 0.001 ||
           working_spool_.lot_nr != original_spool_.lot_nr ||
           working_spool_.comment != original_spool_.comment;
}

bool SpoolEditModal::validate_fields() {
    if (!dialog_) {
        return true;
    }

    // Numeric fields that must be >= 0
    static constexpr const char* numeric_fields[] = {
        "field_remaining",
        "field_spool_weight",
        "field_price",
    };

    lv_color_t color_valid = theme_manager_get_color("text");
    lv_color_t color_invalid = theme_manager_get_color("danger");
    bool all_valid = true;

    for (const char* field_name : numeric_fields) {
        lv_obj_t* field = find_widget(field_name);
        if (!field) {
            continue;
        }

        // Check if value is negative
        const char* text = lv_textarea_get_text(field);
        bool valid = true;
        if (text && text[0] != '\0') {
            double val = std::atof(text);
            if (val < 0) {
                valid = false;
            }
        }

        // Set the label color — label is first child of the field's parent container
        lv_obj_t* container = lv_obj_get_parent(field);
        if (container) {
            lv_obj_t* label = lv_obj_get_child(container, 0);
            if (label) {
                lv_obj_set_style_text_color(label, valid ? color_valid : color_invalid, 0);
            }
        }

        if (!valid) {
            all_valid = false;
        }
    }

    return all_valid;
}

void SpoolEditModal::update_save_button_text() {
    lv_subject_copy_string(&save_button_text_subject_, is_dirty() ? lv_tr("Save") : lv_tr("Close"));
}

// ============================================================================
// Event Handlers
// ============================================================================

void SpoolEditModal::handle_close() {
    spdlog::debug("[SpoolEditModal] Close requested");

    if (completion_callback_) {
        completion_callback_(false);
    }
    hide();
}

void SpoolEditModal::handle_field_changed() {
    if (!dialog_ || populating_) {
        return;
    }

    read_fields_into(working_spool_);
    validate_fields();
    update_spool_preview();
    update_save_button_text();
}

void SpoolEditModal::handle_reset() {
    spdlog::debug("[SpoolEditModal] Resetting to original values");

    working_spool_ = original_spool_;

    populating_ = true;
    populate_fields();
    populating_ = false;

    validate_fields();
    update_spool_preview();
    update_save_button_text();

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Reset to original values"), 2000);
}

void SpoolEditModal::handle_save() {
    if (!is_dirty()) {
        // Nothing changed — just close
        handle_close();
        return;
    }

    if (!validate_fields()) {
        spdlog::debug("[SpoolEditModal] Save blocked — validation errors");
        return;
    }

    if (!api_) {
        spdlog::warn("[SpoolEditModal] No API, cannot save");
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("API not available"), 3000);
        return;
    }

    spdlog::info("[SpoolEditModal] Saving spool {} edits", working_spool_.id);

    // Split changes into spool-level and filament-level PATCHes
    nlohmann::json spool_patch;
    nlohmann::json filament_patch;

    // Spool-level fields (per-spool in Spoolman API)
    if (std::abs(working_spool_.remaining_weight_g - original_spool_.remaining_weight_g) > 0.1) {
        spool_patch["remaining_weight"] = working_spool_.remaining_weight_g;
    }
    if (std::abs(working_spool_.price - original_spool_.price) > 0.001) {
        spool_patch["price"] = working_spool_.price;
    }
    if (working_spool_.lot_nr != original_spool_.lot_nr) {
        spool_patch["lot_nr"] = working_spool_.lot_nr;
    }
    if (working_spool_.comment != original_spool_.comment) {
        spool_patch["comment"] = working_spool_.comment;
    }

    // Filament-level fields (affect all spools using this filament definition)
    if (std::abs(working_spool_.spool_weight_g - original_spool_.spool_weight_g) > 0.1) {
        filament_patch["spool_weight"] = working_spool_.spool_weight_g;
    }

    int spool_id = working_spool_.id;
    int filament_id = working_spool_.filament_id;
    std::weak_ptr<bool> guard = callback_guard_;

    // Completion handler — called after all PATCHes succeed
    auto on_all_saved = [this, guard, spool_id]() {
        if (guard.expired()) {
            return;
        }
        spdlog::info("[SpoolEditModal] All changes saved for spool {}", spool_id);
        helix::ui::async_call(
            [](void* ud) {
                auto* self = static_cast<SpoolEditModal*>(ud);
                if (!self->callback_guard_) {
                    return;
                }
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Spool saved"), 2000);
                if (self->completion_callback_) {
                    self->completion_callback_(true);
                }
                self->hide();
            },
            this);
    };

    auto on_error = [spool_id](const MoonrakerError& err) {
        spdlog::error("[SpoolEditModal] Failed to save spool {}: {}", spool_id, err.message);
        helix::ui::async_call(
            [](void*) {
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to save spool"),
                                              3000);
            },
            nullptr);
    };

    // Send spool PATCH first, then filament PATCH if needed
    if (!spool_patch.empty()) {
        api_->spoolman().update_spoolman_spool(
            spool_id, spool_patch,
            [this, guard, filament_id, filament_patch, on_all_saved, on_error]() {
                if (guard.expired()) {
                    return;
                }
                if (!filament_patch.empty() && filament_id > 0) {
                    api_->spoolman().update_spoolman_filament(filament_id, filament_patch,
                                                              on_all_saved, on_error);
                } else {
                    on_all_saved();
                }
            },
            on_error);
    } else if (!filament_patch.empty() && filament_id > 0) {
        api_->spoolman().update_spoolman_filament(filament_id, filament_patch, on_all_saved,
                                                  on_error);
    } else {
        // Nothing to save (shouldn't happen since is_dirty() was true)
        handle_close();
    }
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void SpoolEditModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    lv_xml_register_event_cb(nullptr, "spoolman_edit_close_cb", on_close_cb);
    lv_xml_register_event_cb(nullptr, "spoolman_edit_field_changed_cb", on_field_changed_cb);
    lv_xml_register_event_cb(nullptr, "spoolman_edit_reset_cb", on_reset_cb);
    lv_xml_register_event_cb(nullptr, "spoolman_edit_save_cb", on_save_cb);

    callbacks_registered_ = true;
    spdlog::debug("[SpoolEditModal] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via User Data)
// ============================================================================

SpoolEditModal* SpoolEditModal::get_instance_from_event(lv_event_t* e) {
    (void)e;
    // Use static instance — parent chain traversal is unsafe because text_input
    // widgets store keyboard hint magic values in user_data, which would be
    // misinterpreted as a SpoolEditModal pointer.
    if (!active_instance_) {
        spdlog::warn("[SpoolEditModal] No active instance for event");
    }
    return active_instance_;
}

void SpoolEditModal::on_close_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_close();
    }
}

void SpoolEditModal::on_field_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_field_changed();
    }
}

void SpoolEditModal::on_reset_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_reset();
    }
}

void SpoolEditModal::on_save_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_save();
    }
}

} // namespace helix::ui
