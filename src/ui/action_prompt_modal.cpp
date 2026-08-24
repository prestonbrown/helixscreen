// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "action_prompt_modal.h"

#include "ui_error_reporting.h"

#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix::ui {

namespace {

/**
 * @brief Width the button label needs to render in full, in pixels.
 */
int32_t label_width_px(const std::string& label, const lv_font_t* font) {
    lv_point_t size{};
    lv_text_get_size(&size, label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/**
 * @brief Can every regular button show its label on one equal-width row?
 *
 * The equal-width row divides the container evenly, so it only works while the
 * widest label still fits its share. AFC's four short "Lane N" buttons do fit,
 * which is what #1043 needed; a preheat macro that offers seven "PLA 220/60"
 * material presets does not, and forcing those onto one row shrinks each cell
 * to a few dozen pixels and clips every label.
 *
 * Returns false when the container width cannot be measured yet — wrapping is
 * the safe answer, because it never clips.
 */
bool equal_width_row_fits(lv_obj_t* container, const std::vector<PromptButton>& buttons,
                          int regular_count) {
    if (regular_count <= 0) {
        return false;
    }

    const lv_font_t* font = theme_manager_get_font("font_body");
    if (font == nullptr) {
        return false;
    }

    // Resolve geometry: the container is width=100% of a card whose own width is
    // a percentage of the screen, so the content width is only real after a
    // layout pass.
    lv_obj_update_layout(container);
    const int32_t available = lv_obj_get_content_width(container);
    if (available <= 0) {
        return false;
    }

    const int32_t gap = lv_obj_get_style_pad_column(container, LV_PART_MAIN);
    const int32_t cell_pad = theme_manager_get_spacing("space_sm"); // per side, see create_button()
    const int32_t cell_width = (available - gap * (regular_count - 1)) / regular_count;

    for (const auto& btn : buttons) {
        if (btn.is_footer) {
            continue;
        }
        if (label_width_px(btn.label, font) + 2 * cell_pad > cell_width) {
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

ActionPromptModal::ActionPromptModal() {
    spdlog::debug("[ActionPromptModal] Constructed");
}

ActionPromptModal::~ActionPromptModal() {
    // Modal destructor will call hide() if visible, which invalidates lifetime_
    // Note: No spdlog here - logger may be destroyed before us during shutdown [L010]
}

ActionPromptModal::ActionPromptModal(ActionPromptModal&& other) noexcept
    : Modal(std::move(other)), prompt_data_(std::move(other.prompt_data_)),
      gcode_callback_(std::move(other.gcode_callback_)),
      created_buttons_(std::move(other.created_buttons_)),
      created_text_labels_(std::move(other.created_text_labels_)),
      button_callback_data_(std::move(other.button_callback_data_)) {
    // Update callback data to point to this instance (moved-to)
    for (auto& cbd : button_callback_data_) {
        cbd->modal = this;
    }
}

ActionPromptModal& ActionPromptModal::operator=(ActionPromptModal&& other) noexcept {
    if (this != &other) {
        Modal::operator=(std::move(other));
        prompt_data_ = std::move(other.prompt_data_);
        gcode_callback_ = std::move(other.gcode_callback_);
        created_buttons_ = std::move(other.created_buttons_);
        created_text_labels_ = std::move(other.created_text_labels_);
        button_callback_data_ = std::move(other.button_callback_data_);
        // Update callback data to point to this instance (moved-to)
        for (auto& cbd : button_callback_data_) {
            cbd->modal = this;
        }
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void ActionPromptModal::set_gcode_callback(GcodeCallback callback) {
    gcode_callback_ = std::move(callback);
}

bool ActionPromptModal::show_prompt(lv_obj_t* parent, const PromptData& data) {
    // Store prompt data
    prompt_data_ = data;

    // Show the modal via Modal base class
    if (!Modal::show(parent)) {
        return false;
    }

    spdlog::info("[ActionPromptModal] Shown with title: {}", prompt_data_.title);
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void ActionPromptModal::on_show() {
    populate_content();
}

void ActionPromptModal::on_hide() {
    clear_dynamic_content();
    spdlog::debug("[ActionPromptModal] on_hide()");
}

// ============================================================================
// Content Population
// ============================================================================

void ActionPromptModal::populate_content() {
    if (!dialog_) {
        return;
    }

    // Set title
    lv_obj_t* title_label = find_widget("title");
    if (title_label) {
        lv_label_set_text(title_label, prompt_data_.title.c_str());
    }

    // Error-severity affordance: show the red error icon only for severity "error".
    // The modal instance is reused across shows, so reset both ways to clear any
    // prior error state on a subsequent neutral (action:prompt) show.
    if (lv_obj_t* err_icon = find_widget("icon_error")) {
        if (prompt_data_.severity == "error") {
            lv_obj_remove_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create text lines
    create_text_lines();

    // Create buttons
    create_buttons();
}

void ActionPromptModal::create_text_lines() {
    lv_obj_t* content_container = find_widget("content_container");
    if (!content_container) {
        spdlog::warn("[ActionPromptModal] content_container not found");
        return;
    }

    // Create a label for each text line
    for (const auto& line : prompt_data_.text_lines) {
        lv_obj_t* label = lv_label_create(content_container);
        lv_label_set_text(label, line.c_str());
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        // Apply body text styling
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);

        created_text_labels_.push_back(label);
    }

    // Hide content container if no text lines
    if (prompt_data_.text_lines.empty()) {
        lv_obj_add_flag(content_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void ActionPromptModal::create_buttons() {
    lv_obj_t* button_container = find_widget("button_container");
    lv_obj_t* footer_container = find_widget("footer_container");
    lv_obj_t* footer_divider = find_widget("footer_divider");

    if (!button_container) {
        spdlog::warn("[ActionPromptModal] button_container not found");
        return;
    }

    bool has_footer_buttons = false;
    bool has_regular_buttons = false;
    int footer_button_count = 0;

    // Count regular (non-footer) buttons up front. With >= 4 of them the legacy
    // content-sized row_wrap overflows the fixed-width (320px) dialog and the 4th
    // button wraps to a second line (R2 / #1043). In that case switch the shared
    // button_container to a non-wrapping row of equal-width cells so they all fit
    // on ONE line. With <= 3 regular buttons keep the existing row_wrap behaviour
    // byte-for-byte (this container is shared with L1's recovery modal).
    //
    // The count alone is not sufficient: an equal-width row divides the container
    // evenly, so it is only usable while the labels still fit their share. A
    // macro that offers many long labels (seven "PLA 220/60" material presets)
    // gets a few dozen pixels per cell and every label clips, so those fall back
    // to row_wrap and take the extra lines they need.
    int regular_count = 0;
    for (const auto& btn : prompt_data_.buttons) {
        if (!btn.is_footer) {
            ++regular_count;
        }
    }
    const bool equal_width_row =
        regular_count >= 4 &&
        equal_width_row_fits(button_container, prompt_data_.buttons, regular_count);
    // Set the flow explicitly for BOTH cases so a reused modal instance never
    // inherits the wrong flow from a previous prompt: row_wrap restores the
    // legacy <= 3 look (matching the XML default); plain row drives the >= 4
    // equal-width layout.
    lv_obj_set_flex_flow(button_container,
                         equal_width_row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_ROW_WRAP);

    // Create buttons based on PromptData
    for (const auto& btn : prompt_data_.buttons) {
        if (btn.is_footer) {
            if (footer_container) {
                // Add vertical divider between footer buttons
                if (footer_button_count > 0) {
                    lv_obj_t* divider = lv_obj_create(footer_container);
                    lv_obj_set_size(divider, 1, lv_pct(100));
                    lv_obj_set_style_bg_color(divider, theme_manager_get_color("border"),
                                              LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
                    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
                    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
                }
                create_button(btn, footer_container);
                has_footer_buttons = true;
                footer_button_count++;
            }
        } else {
            create_button(btn, button_container, equal_width_row);
            has_regular_buttons = true;
        }
    }

    // Show/hide footer based on whether there are footer buttons
    if (footer_container) {
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (footer_divider) {
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide button container if no regular buttons
    if (!has_regular_buttons) {
        lv_obj_add_flag(button_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void ActionPromptModal::create_button(const PromptButton& btn, lv_obj_t* container,
                                      bool equal_width) {
    lv_obj_t* button = lv_button_create(container);

    if (btn.is_footer) {
        // Footer buttons: full-width modal style with dividers
        lv_obj_set_height(button, lv_pct(100));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    } else if (equal_width) {
        // Regular buttons, >= 4 of them and all short enough to share a row:
        // equal-width cells on a non-wrapping row (R2 / #1043). grow=1 with
        // width 0 lets short labels ("Lane 1".."Lane 4") share the fixed-width
        // row instead of overflowing and wrapping. Trim the horizontal padding
        // to space_sm so the equal cells stay compact; the label stays centered.
        lv_obj_set_width(button, 0);
        lv_obj_set_height(button, theme_manager_get_spacing("button_height"));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    } else {
        // Everything else: content-sized with padding, which row_wrap spreads
        // over as many lines as the labels need (<= 3 buttons, or more than
        // three that are too wide to share one row).
        lv_obj_set_size(button, LV_SIZE_CONTENT, theme_manager_get_spacing("button_height"));
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    }
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    // Apply button color: hex_color takes priority over named color hint
    lv_color_t bg_color = get_button_color(btn.color);
    if (!btn.hex_color.empty()) {
        uint32_t hex_val = std::strtoul(btn.hex_color.c_str(), nullptr, 16);
        bg_color = lv_color_hex(hex_val);
    }
    lv_obj_set_style_bg_color(button, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);

    // Create label inside button with contrast-aware text color
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, btn.label.c_str());
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, theme_manager_get_contrast_color(bg_color), LV_PART_MAIN);

    // Create callback data with owned copy of gcode string and lifetime token.
    //
    // An empty gcode means DO NOTHING — the button closes the modal and sends
    // no command. This used to fall back to sending the *label*, so a button
    // marked "OK" or "Dismiss" transmitted `OK` to Klipper (#1172). Nothing
    // relies on that fallback: Klipper's own `action_prompt_button` protocol
    // already applies the label-as-gcode convention explicitly in
    // ActionPromptManager::parse_button_spec(), so prompts arriving over the
    // wire are unaffected. Only programmatically built PromptData reaches here
    // with a blank gcode, and there it always meant "no command".
    auto cbd = std::make_unique<ButtonCallbackData>();
    cbd->modal = this;
    cbd->token = lifetime_.token();
    cbd->gcode = btn.gcode;

    // Add click callback with ButtonCallbackData as user_data
    lv_obj_add_event_cb(button, on_button_cb, LV_EVENT_CLICKED, cbd.get());

    // Transfer ownership to the vector (pointer remains stable)
    button_callback_data_.push_back(std::move(cbd));

    created_buttons_.push_back(button);

    spdlog::debug("[ActionPromptModal] Created button: {} (gcode: {}, color: {})", btn.label,
                  btn.gcode.empty() ? "<none>" : btn.gcode,
                  btn.color.empty() ? "primary" : btn.color);
}

lv_color_t ActionPromptModal::get_button_color(const std::string& color_name) {
    // Map Klipper color hints to design tokens
    if (color_name == "primary" || color_name.empty()) {
        return theme_manager_get_color("primary");
    } else if (color_name == "secondary") {
        return theme_manager_get_color("success");
    } else if (color_name == "info") {
        return theme_manager_get_color("info");
    } else if (color_name == "warning") {
        return theme_manager_get_color("warning");
    } else if (color_name == "error") {
        return theme_manager_get_color("danger");
    }

    // Unknown color - default to primary
    spdlog::debug("[ActionPromptModal] Unknown color '{}', using primary", color_name);
    return theme_manager_get_color("primary");
}

void ActionPromptModal::clear_dynamic_content() {
    // Remove click callbacks from each button before freeing their user_data.
    // Without this, LVGL's async teardown can still hold references to the
    // ButtonCallbackData pointers that button_callback_data_.clear() frees,
    // risking use-after-free if any queued event dispatches against a button
    // after on_hide() returns.
    for (lv_obj_t* button : created_buttons_) {
        if (button && lv_obj_is_valid(button)) {
            lv_obj_remove_event_cb(button, on_button_cb);
        }
    }
    created_buttons_.clear();
    created_text_labels_.clear();
    button_callback_data_.clear();
}

// ============================================================================
// Event Handler
// ============================================================================

void ActionPromptModal::handle_button_click(const std::string& gcode) {
    // An empty gcode is a dismiss affordance: close, send nothing. Callers no
    // longer have to smuggle a Klipper comment ("; error-dismiss") through to
    // get a button that does nothing (#1172).
    if (gcode.empty()) {
        spdlog::info("[ActionPromptModal] Dismiss button clicked (no gcode)");
        hide();
        return;
    }

    spdlog::info("[ActionPromptModal] Button clicked, gcode: {}", gcode);

    // Call the gcode callback if set
    if (gcode_callback_) {
        gcode_callback_(gcode);
    }

    // Close the modal
    hide();
}

// ============================================================================
// Static Callbacks
// ============================================================================

void ActionPromptModal::on_button_cb(lv_event_t* e) {
    auto* cbd = static_cast<ButtonCallbackData*>(lv_event_get_user_data(e));
    if (!cbd) {
        spdlog::warn("[ActionPromptModal] Button callback data is null");
        return;
    }

    // Check if the modal is still alive (guards against use-after-free).
    // LifetimeToken is safe to query even after the guard is destroyed (#437).
    if (!cbd->token || cbd->token->expired()) {
        spdlog::debug("[ActionPromptModal] Modal destroyed before button callback fired");
        return;
    }

    SoundManager::instance().play("button_tap");
    cbd->modal->handle_button_click(cbd->gcode);
}

// ============================================================================
// Failure reporting
// ============================================================================

void report_action_prompt_gcode_failure(const std::string& error_message) {
    // Same presentation as every other failed macro in the UI (ui_panel_controls,
    // ui_panel_filament). NOTIFY_ERROR marshals to the main thread itself, which
    // matters here: the RPC error callback fires on the WebSocket thread.
    //
    // Klipper's own wording is the useful part ("Extruder not hot enough"); the
    // generic string only stands in when the transport gave us nothing.
    const std::string detail =
        error_message.empty() ? std::string(lv_tr("Unknown error")) : error_message;
    NOTIFY_ERROR(lv_tr("Macro failed: {}"), detail);
}

} // namespace helix::ui
