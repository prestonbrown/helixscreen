// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_section_detail_overlay.cpp
 * @brief Implementation of AmsDeviceSectionDetailOverlay
 */

#include "ui_ams_device_section_detail_overlay.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
#include "ui_text_input.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AmsDeviceSectionDetailOverlay> g_ams_device_section_detail_overlay;

AmsDeviceSectionDetailOverlay& get_ams_device_section_detail_overlay() {
    if (!g_ams_device_section_detail_overlay) {
        g_ams_device_section_detail_overlay = std::make_unique<AmsDeviceSectionDetailOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "AmsDeviceSectionDetailOverlay", []() { g_ams_device_section_detail_overlay.reset(); });
    }
    return *g_ams_device_section_detail_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AmsDeviceSectionDetailOverlay::AmsDeviceSectionDetailOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AmsDeviceSectionDetailOverlay::~AmsDeviceSectionDetailOverlay() {
    // No subjects to deinitialize — title is set imperatively
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AmsDeviceSectionDetailOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // No subjects needed — title is set imperatively in show()
    // and dynamic controls don't use XML bindings.

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void AmsDeviceSectionDetailOverlay::register_callbacks() {
    // No XML-defined callbacks needed — controls are created imperatively
    // (documented exception for dynamic backend-driven controls).
    spdlog::debug("[{}] Callbacks registered (none needed)", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsDeviceSectionDetailOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_device_section_detail", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find the dynamic actions container
    actions_container_ = lv_obj_find_by_name(overlay_, "section_actions_container");
    if (!actions_container_) {
        spdlog::warn("[{}] section_actions_container not found in XML", get_name());
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

void AmsDeviceSectionDetailOverlay::show(lv_obj_t* parent_screen, const std::string& section_id,
                                         const std::string& section_label) {
    spdlog::debug("[{}] show() called for section '{}' ('{}')", get_name(), section_id,
                  section_label);

    parent_screen_ = parent_screen;
    section_id_ = section_id;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Update header title imperatively (overlay_panel title is static at XML creation)
    lv_obj_t* header_title = lv_obj_find_by_name(overlay_, "header_title");
    if (header_title) {
        std::string title = std::string(lv_tr("Multi-Filament System Management")) + ": " +
                            lv_tr(section_label.c_str());
        lv_label_set_text(header_title, title.c_str());
    }

    // Update from backend
    refresh();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_, this);

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_);
}

void AmsDeviceSectionDetailOverlay::refresh() {
    if (!overlay_ || !actions_container_) {
        return;
    }

    spdlog::debug("[{}] Refreshing section '{}' from backend", get_name(), section_id_);

    // Clear existing controls
    helix::ui::safe_clean_children(actions_container_);
    action_ids_.clear();

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[{}] No backend available", get_name());
        return;
    }

    // Get all device actions and filter by section
    cached_actions_ = backend->get_device_actions();

    int count = 0;
    lv_obj_t* button_row = nullptr;
    int button_count_in_row = 0;

    for (const auto& action : cached_actions_) {
        if (action.section != section_id_) {
            continue;
        }

        if (action.type == helix::printer::ActionType::BUTTON) {
            // Start a new row if needed (no current row or current row is full)
            if (!button_row || button_count_in_row >= 2) {
                button_row = lv_obj_create(actions_container_);
                lv_obj_set_width(button_row, LV_PCT(100));
                lv_obj_set_height(button_row, LV_SIZE_CONTENT);
                lv_obj_set_style_pad_all(button_row, 0, 0);
                lv_obj_set_style_pad_column(button_row, theme_manager_get_spacing("space_sm"), 0);
                lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(button_row, 0, 0);
                lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
                lv_obj_remove_flag(button_row, LV_OBJ_FLAG_SCROLLABLE);
                button_count_in_row = 0;
            }

            create_button_in_row(button_row, action);
            button_count_in_row++;
        } else {
            // Non-button action closes any open button row
            button_row = nullptr;
            button_count_in_row = 0;
            create_action_control(actions_container_, action);
        }
        count++;
    }

    spdlog::debug("[{}] Created {} controls for section '{}'", get_name(), count, section_id_);
}

// ============================================================================
// DYNAMIC CONTROL CREATION
// ============================================================================

void AmsDeviceSectionDetailOverlay::create_action_control(
    lv_obj_t* parent, const helix::printer::DeviceAction& action) {
    // NOTE: These controls are created dynamically from backend data, not from XML.
    // Imperative lv_obj_add_event_cb() and lv_obj_set_style_*() are necessary here
    // because the controls don't exist in XML templates. This falls under the same
    // exception as widget pool recycling and chart data in the declarative UI rules.
    spdlog::debug("[{}] Creating action control: {} (type={})", get_name(), action.label,
                  helix::printer::action_type_to_string(action.type));

    // Create row container for action
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_xs"), 0);
    lv_obj_set_style_pad_column(row, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    switch (action.type) {
    case helix::printer::ActionType::BUTTON:
        // Buttons are handled by create_button_in_row() via refresh() loop
        spdlog::warn("[{}] BUTTON action '{}' passed to create_action_control(); use "
                     "create_button_in_row() instead",
                     get_name(), action.id);
        break;

    case helix::printer::ActionType::TOGGLE: {
        // Label on left
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, lv_tr(action.label.c_str()));
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

        // Switch on right
        lv_obj_t* sw = lv_switch_create(row);

        // Try to get current value
        try {
            if (action.current_value.has_value()) {
                bool val = std::any_cast<bool>(action.current_value);
                if (val) {
                    lv_obj_add_state(sw, LV_STATE_CHECKED);
                }
            }
        } catch (const std::bad_any_cast&) {
            spdlog::warn("[{}] Failed to cast toggle value for {}", get_name(), action.id);
        }

        // Store action ID in vector, pass index as user_data
        action_ids_.push_back(action.id);
        lv_obj_set_user_data(sw, reinterpret_cast<void*>(action_ids_.size() - 1));

        // Register value changed callback
        lv_obj_add_event_cb(sw, on_toggle_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        break;
    }

    case helix::printer::ActionType::INFO: {
        // Label on left
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, lv_tr(action.label.c_str()));
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

        // Value on right
        lv_obj_t* value_label = lv_label_create(row);
        lv_obj_set_style_text_color(value_label, theme_manager_get_color("text_muted"), 0);
        try {
            if (action.current_value.has_value()) {
                std::string val = std::any_cast<std::string>(action.current_value);
                if (!action.unit.empty()) {
                    val += " " + action.unit;
                }
                lv_label_set_text(value_label, val.c_str());
            } else {
                lv_label_set_text(value_label, "-");
            }
        } catch (const std::bad_any_cast&) {
            lv_label_set_text(value_label, "-");
        }
        break;
    }

    case helix::printer::ActionType::SLIDER: {
        // Label on left — fixed width so sliders align across rows
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, lv_tr(action.label.c_str()));
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_set_width(label, LV_PCT(30));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        // Slider in the middle with flex-grow
        lv_obj_t* slider = lv_slider_create(row);
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_set_height(slider, theme_manager_get_spacing("space_md"));

        // Set range from action min/max (LVGL slider uses int32_t)
        lv_slider_set_range(slider, static_cast<int32_t>(action.min_value),
                            static_cast<int32_t>(action.max_value));

        // Set current value from action.current_value
        int32_t slider_val = static_cast<int32_t>(action.min_value);
        try {
            if (action.current_value.has_value()) {
                try {
                    slider_val = static_cast<int32_t>(std::any_cast<float>(action.current_value));
                } catch (const std::bad_any_cast&) {
                    slider_val = std::any_cast<int>(action.current_value);
                }
            }
        } catch (const std::bad_any_cast&) {
            spdlog::warn("[{}] Failed to cast slider value for {}", get_name(), action.id);
        }
        lv_slider_set_value(slider, slider_val, LV_ANIM_OFF);

        // Editable numeric text input on right (replaces read-only value_label)
        lv_obj_t* ta = lv_textarea_create(row);
        lv_textarea_set_one_line(ta, true);
        lv_obj_set_width(ta, 80);
        lv_obj_set_height(ta, LV_SIZE_CONTENT);
        lv_textarea_set_accepted_chars(ta, "0123456789.-");
        std::string val_text = std::to_string(slider_val);
        lv_textarea_set_text(ta, val_text.c_str());

        // Store unit as part of placeholder for reference
        if (!action.unit.empty()) {
            lv_textarea_set_placeholder_text(ta, action.unit.c_str());
        }

        // Store action ID in vector, pass index as user_data on slider only.
        // Textarea user_data is reserved for TEXT_INPUT_MAGIC + KeyboardHint;
        // on_value_input_ready finds the index via the sibling slider.
        action_ids_.push_back(action.id);
        size_t action_index = action_ids_.size() - 1;
        lv_obj_set_user_data(slider, reinterpret_cast<void*>(action_index));

        // Set numeric keyboard hint and register with KeyboardManager
        static constexpr uintptr_t TEXT_INPUT_MAGIC = 0xBADC0DE0;
        lv_obj_set_user_data(ta,
                             reinterpret_cast<void*>(
                                 TEXT_INPUT_MAGIC | static_cast<uintptr_t>(KeyboardHint::NUMERIC)));
        KeyboardManager::instance().register_textarea(ta);

        // Slider callbacks: update text input live during drag, execute on release
        lv_obj_add_event_cb(slider, on_slider_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, nullptr);

        // Text input callback: validate and apply on Enter
        lv_obj_add_event_cb(ta, on_value_input_ready, LV_EVENT_READY, nullptr);

        // Handle disabled state
        if (!action.enabled) {
            lv_obj_add_state(slider, LV_STATE_DISABLED);
            lv_obj_add_state(ta, LV_STATE_DISABLED);
            if (!action.disable_reason.empty()) {
                spdlog::debug("[{}] Slider '{}' disabled: {}", get_name(), action.id,
                              action.disable_reason);
            }
        }
        break;
    }

    case helix::printer::ActionType::DROPDOWN: {
        // Label on left
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, lv_tr(action.label.c_str()));
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

        // Dropdown on right
        lv_obj_t* dropdown = lv_dropdown_create(row);

        // Populate options (LVGL expects newline-separated string)
        std::string options_str;
        for (size_t i = 0; i < action.options.size(); i++) {
            if (i > 0) {
                options_str += "\n";
            }
            options_str += action.options[i];
        }
        lv_dropdown_set_options(dropdown, options_str.c_str());

        // Set selected index from current_value (string matching against options)
        try {
            if (action.current_value.has_value()) {
                std::string current = std::any_cast<std::string>(action.current_value);
                for (size_t i = 0; i < action.options.size(); i++) {
                    if (action.options[i] == current) {
                        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(i));
                        break;
                    }
                }
            }
        } catch (const std::bad_any_cast&) {
            spdlog::warn("[{}] Failed to cast dropdown value for {}", get_name(), action.id);
        }

        // Store action ID in vector, pass index as user_data
        action_ids_.push_back(action.id);
        lv_obj_set_user_data(dropdown, reinterpret_cast<void*>(action_ids_.size() - 1));

        // Register value changed callback
        lv_obj_add_event_cb(dropdown, on_dropdown_changed, LV_EVENT_VALUE_CHANGED, nullptr);

        // Handle disabled state
        if (!action.enabled) {
            lv_obj_add_state(dropdown, LV_STATE_DISABLED);
            if (!action.disable_reason.empty()) {
                spdlog::debug("[{}] Dropdown '{}' disabled: {}", get_name(), action.id,
                              action.disable_reason);
            }
        }
        break;
    }
    }
}

void AmsDeviceSectionDetailOverlay::create_button_in_row(
    lv_obj_t* row, const helix::printer::DeviceAction& action) {
    // NOTE: Imperative lv_obj_add_event_cb() is necessary here — dynamic controls
    // from backend data (documented exception for declarative UI rules).
    spdlog::debug("[{}] Creating button in row: {}", get_name(), action.label);

    lv_obj_t* btn = lv_button_create(row);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, theme_manager_get_spacing("button_height_sm"));
    lv_obj_set_style_radius(btn, theme_manager_get_spacing("border_radius"), 0);

    // Button label
    lv_obj_t* btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, lv_tr(action.label.c_str()));
    lv_obj_center(btn_label);

    // Store action ID in vector, pass index as user_data
    action_ids_.push_back(action.id);
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(action_ids_.size() - 1));

    // Register click callback
    lv_obj_add_event_cb(btn, on_action_clicked, LV_EVENT_CLICKED, nullptr);

    // Handle disabled state
    if (!action.enabled) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        if (!action.disable_reason.empty()) {
            spdlog::debug("[{}] Action '{}' disabled: {}", get_name(), action.id,
                          action.disable_reason);
        }
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AmsDeviceSectionDetailOverlay::on_action_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_action_clicked");

    auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!btn || !lv_obj_is_valid(btn)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_action_clicked: invalid target");
    } else {
        // Get action ID from vector using index stored in user_data
        auto& overlay = get_ams_device_section_detail_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(btn));
        if (index >= overlay.action_ids_.size()) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid action index: {}", index);
        } else {
            const std::string& action_id = overlay.action_ids_[index];
            spdlog::debug("[AmsDeviceSectionDetailOverlay] Action clicked: {}", action_id);

            // Execute via backend
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                spdlog::warn("[AmsDeviceSectionDetailOverlay] No backend available for action");
            } else {
                // Find label for the toast
                std::string label = action_id;
                for (const auto& act : overlay.cached_actions_) {
                    if (act.id == action_id) {
                        label = act.label;
                        break;
                    }
                }

                AmsError result = backend->execute_device_action(action_id);
                if (result.success()) {
                    // Whole sentence, not "{} {}" over two separately translated
                    // words: German sends the participle to the end of the
                    // clause and Japanese renders this as 〜を開始しました, and
                    // neither can be reached by concatenating "label" + "started".
                    NOTIFY_INFO(lv_tr("{} started"), lv_tr(label.c_str()));
                } else {
                    helix::ui::notify_ams_error(result);
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_toggle_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_toggle_changed");

    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!sw || !lv_obj_is_valid(sw)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_toggle_changed: invalid target");
    } else {
        // Get action ID from vector using index stored in user_data
        auto& overlay = get_ams_device_section_detail_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(sw));
        if (index >= overlay.action_ids_.size()) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid toggle action index: {}", index);
        } else {
            const std::string& action_id = overlay.action_ids_[index];
            bool new_value = lv_obj_has_state(sw, LV_STATE_CHECKED);
            spdlog::debug("[AmsDeviceSectionDetailOverlay] Toggle changed: {} = {}", action_id,
                          new_value);

            // Execute via backend with the new value
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                spdlog::warn("[AmsDeviceSectionDetailOverlay] No backend available for toggle");
            } else {
                // Find label for notification
                std::string label = action_id;
                for (const auto& act : overlay.cached_actions_) {
                    if (act.id == action_id) {
                        label = act.label;
                        break;
                    }
                }

                // For toggles, pass the new value as a parameter
                AmsError result = backend->execute_device_action(action_id, std::any(new_value));
                if (result.success()) {
                    NOTIFY_INFO(new_value ? lv_tr("{} enabled") : lv_tr("{} disabled"),
                                lv_tr(label.c_str()));
                } else {
                    helix::ui::notify_ams_error(result);
                    // Revert the toggle state on failure
                    if (new_value) {
                        lv_obj_remove_state(sw, LV_STATE_CHECKED);
                    } else {
                        lv_obj_add_state(sw, LV_STATE_CHECKED);
                    }
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_slider_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_slider_changed");

    // Update the value label live during drag — no backend execution here
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!slider || !lv_obj_is_valid(slider)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_slider_changed: invalid target");
    } else {
        auto& overlay = get_ams_device_section_detail_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(slider));
        if (index < overlay.action_ids_.size()) {
            const std::string& action_id = overlay.action_ids_[index];
            int32_t int_val = lv_slider_get_value(slider);

            // Update the text input (last child of the row: label, slider, textarea)
            // Guard: only update if value actually changed to prevent feedback loops
            lv_obj_t* row = lv_obj_get_parent(slider);
            if (row) {
                uint32_t child_count = lv_obj_get_child_count(row);
                if (child_count >= 3) {
                    lv_obj_t* ta = lv_obj_get_child(row, child_count - 1);
                    if (ta) {
                        std::string val_text = std::to_string(int_val);
                        const char* current = lv_textarea_get_text(ta);
                        if (!current || val_text != current) {
                            lv_textarea_set_text(ta, val_text.c_str());
                        }
                    }
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_slider_released(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_slider_released");

    // Execute the action on release only — avoids spamming G-codes during drag
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!slider || !lv_obj_is_valid(slider)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_slider_released: invalid target");
    } else {
        auto& overlay = get_ams_device_section_detail_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(slider));
        if (index >= overlay.action_ids_.size()) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid slider action index: {}", index);
        } else {
            const std::string& action_id = overlay.action_ids_[index];
            int32_t int_val = lv_slider_get_value(slider);
            float float_val = static_cast<float>(int_val);

            spdlog::debug("[AmsDeviceSectionDetailOverlay] Slider released: {} = {}", action_id,
                          float_val);

            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                spdlog::warn("[AmsDeviceSectionDetailOverlay] No backend available for slider");
            } else {
                // Find label and unit for notification
                std::string label = action_id;
                std::string unit;
                for (const auto& act : overlay.cached_actions_) {
                    if (act.id == action_id) {
                        label = act.label;
                        unit = act.unit;
                        break;
                    }
                }

                AmsError result = backend->execute_device_action(action_id, std::any(float_val));
                if (result.success()) {
                    if (!unit.empty()) {
                        NOTIFY_INFO(lv_tr("{} set to {} {}"), lv_tr(label.c_str()), int_val, unit);
                    } else {
                        NOTIFY_INFO(lv_tr("{} set to {}"), lv_tr(label.c_str()), int_val);
                    }
                } else {
                    helix::ui::notify_ams_error(result);
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_value_input_ready(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_value_input_ready");

    auto* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!ta || !lv_obj_is_valid(ta)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_value_input_ready: invalid target");
    } else {
        auto& overlay = get_ams_device_section_detail_overlay();
        // Get action index from sibling slider (child index 1 of row).
        // Textarea user_data is reserved for TEXT_INPUT_MAGIC + KeyboardHint.
        lv_obj_t* row = lv_obj_get_parent(ta);
        lv_obj_t* slider = row ? lv_obj_get_child(row, 1) : nullptr;
        auto index = slider ? reinterpret_cast<size_t>(lv_obj_get_user_data(slider)) : SIZE_MAX;
        if (index >= overlay.action_ids_.size()) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid text input action index: {}",
                         index);
        } else {
            const std::string& action_id = overlay.action_ids_[index];
            const char* text = lv_textarea_get_text(ta);
            bool valid_input = (text && text[0] != '\0');

            float val = 0.0f;
            if (valid_input) {
                try {
                    val = std::stof(text);
                } catch (...) {
                    spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid numeric input: {}", text);
                    valid_input = false;
                }
            }

            if (valid_input) {
                // Find action to validate range
                float min_val = 0.0f, max_val = 0.0f;
                std::string label, unit;
                for (const auto& act : overlay.cached_actions_) {
                    if (act.id == action_id) {
                        min_val = act.min_value;
                        max_val = act.max_value;
                        label = act.label;
                        unit = act.unit;
                        break;
                    }
                }

                // Clamp to valid range
                if (val < min_val)
                    val = min_val;
                if (val > max_val)
                    val = max_val;

                // Update the textarea with clamped value
                std::string clamped_text = std::to_string(static_cast<int32_t>(val));
                lv_textarea_set_text(ta, clamped_text.c_str());

                // Update the sibling slider (second child of row: label, slider, textarea)
                lv_obj_t* row = lv_obj_get_parent(ta);
                if (row) {
                    lv_obj_t* slider = lv_obj_get_child(row, 1);
                    if (slider) {
                        lv_slider_set_value(slider, static_cast<int32_t>(val), LV_ANIM_ON);
                    }
                }

                // Execute backend action
                AmsBackend* backend = AmsState::instance().get_backend();
                if (backend) {
                    AmsError result = backend->execute_device_action(action_id, std::any(val));
                    if (result.success()) {
                        if (!unit.empty()) {
                            NOTIFY_INFO(lv_tr("{} set to {} {}"), lv_tr(label.c_str()),
                                        static_cast<int32_t>(val), unit);
                        } else {
                            NOTIFY_INFO(lv_tr("{} set to {}"), lv_tr(label.c_str()),
                                        static_cast<int32_t>(val));
                        }
                    } else {
                        helix::ui::notify_ams_error(result);
                    }
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_dropdown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_dropdown_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown || !lv_obj_is_valid(dropdown)) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] on_dropdown_changed: invalid target");
    } else {
        // Get action ID from vector using index stored in user_data
        auto& overlay = get_ams_device_section_detail_overlay();
        auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(dropdown));
        if (index >= overlay.action_ids_.size()) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Invalid dropdown action index: {}",
                         index);
        } else {
            const std::string& action_id = overlay.action_ids_[index];
            uint32_t selected = lv_dropdown_get_selected(dropdown);

            // Get the selected option string
            char buf[128] = {};
            lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
            std::string selected_str(buf);

            spdlog::debug("[AmsDeviceSectionDetailOverlay] Dropdown changed: {} = '{}' (index {})",
                          action_id, selected_str, selected);

            // Find label for notification
            std::string label = action_id;
            for (const auto& act : overlay.cached_actions_) {
                if (act.id == action_id) {
                    label = act.label;
                    break;
                }
            }

            // Execute via backend with the selected option string
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                spdlog::warn("[AmsDeviceSectionDetailOverlay] No backend available for dropdown");
            } else {
                AmsError result = backend->execute_device_action(action_id, std::any(selected_str));
                if (result.success()) {
                    NOTIFY_INFO(lv_tr("{} set to {}"), lv_tr(label.c_str()), selected_str);
                } else {
                    helix::ui::notify_ams_error(result);
                }
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
