// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker.h"

#include "ui_callback_helpers.h"
#include "ui_hsv_picker.h"
#include "ui_modal.h"

#include "color_utils.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix {

// Special preset names that don't follow standard color naming
static const struct {
    uint32_t hex;
    const char* name;
} SPECIAL_COLOR_NAMES[] = {
    {0xD4AF37, "Gold"},  {0xCD7F32, "Bronze"}, {0x8B4513, "Wood"},
    {0xE8E8FF, "Clear"}, {0xC0C0C0, "Silver"}, {0xE0D5C7, "Marble"},
    {0xFF7043, "Coral"}, {0x1A237E, "Navy"},   {0xBCAAA4, "Taupe"},
};

std::string get_color_name_from_hex(uint32_t rgb) {
    // Check for special preset names first
    for (const auto& entry : SPECIAL_COLOR_NAMES) {
        if (entry.hex == rgb) {
            return entry.name;
        }
    }
    // Use algorithmic color description
    return helix::describe_color(rgb);
}

} // namespace helix

namespace helix::ui {

// Static member initialization
bool ColorPicker::callbacks_registered_ = false;
ColorPicker* ColorPicker::active_instance_ = nullptr;

namespace {

// Drives the <if cond="color_picker_palette eq 1"> in color_picker.xml, which
// builds either the general or the theme preset grid — never both. Process-wide
// rather than per-instance: only one picker is visible at a time, and the XML
// name must outlive any individual ColorPicker (the LED overlay holds a static
// one, the theme editor a unique_ptr).
lv_subject_t s_palette_subject;
bool s_palette_subject_ready = false;

void ensure_palette_subject() {
    if (s_palette_subject_ready) {
        return;
    }
    lv_subject_init_int(&s_palette_subject, static_cast<int>(ColorPicker::Palette::General));
    lv_xml_register_subject(nullptr, "color_picker_palette", &s_palette_subject);
    s_palette_subject_ready = true;

    // Self-register cleanup so the subject is torn down before lv_deinit().
    StaticSubjectRegistry::instance().register_deinit("ColorPickerPalette", []() {
        if (s_palette_subject_ready) {
            lv_subject_deinit(&s_palette_subject);
            s_palette_subject_ready = false;
        }
    });
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

ColorPicker::ColorPicker() {
    spdlog::debug("[ColorPicker] Constructed");
}

ColorPicker::~ColorPicker() {
    // Modal destructor will call hide() if visible
    deinit_subjects();
    spdlog::trace("[ColorPicker] Destroyed");
}

ColorPicker::ColorPicker(ColorPicker&& other) noexcept
    : Modal(std::move(other)), selected_color_(other.selected_color_), palette_(other.palette_),
      color_callback_(std::move(other.color_callback_)),
      dismiss_callback_(std::move(other.dismiss_callback_)),
      subjects_initialized_(other.subjects_initialized_) {
    // Copy buffers
    std::memcpy(hex_buf_, other.hex_buf_, sizeof(hex_buf_));
    std::memcpy(name_buf_, other.name_buf_, sizeof(name_buf_));

    // Subjects are not movable - they stay with original
    other.subjects_initialized_ = false;

    is_tiny_mode_ = other.is_tiny_mode_;
    other.is_tiny_mode_ = false;
}

ColorPicker& ColorPicker::operator=(ColorPicker&& other) noexcept {
    if (this != &other) {
        Modal::operator=(std::move(other));
        selected_color_ = other.selected_color_;
        palette_ = other.palette_;
        color_callback_ = std::move(other.color_callback_);
        dismiss_callback_ = std::move(other.dismiss_callback_);
        subjects_initialized_ = other.subjects_initialized_;
        std::memcpy(hex_buf_, other.hex_buf_, sizeof(hex_buf_));
        std::memcpy(name_buf_, other.name_buf_, sizeof(name_buf_));
        other.subjects_initialized_ = false;

        is_tiny_mode_ = other.is_tiny_mode_;
        other.is_tiny_mode_ = false;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void ColorPicker::set_color_callback(ColorCallback callback) {
    color_callback_ = std::move(callback);
}

void ColorPicker::set_dismiss_callback(std::function<void()> callback) {
    dismiss_callback_ = std::move(callback);
}

void ColorPicker::set_palette(Palette palette) {
    palette_ = palette;
}

bool ColorPicker::show_with_color(lv_obj_t* parent, uint32_t initial_color) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Initialize subjects if needed
    init_subjects();

    // Set initial color before showing
    selected_color_ = initial_color;

    // Select the preset grid BEFORE Modal::show() builds the tree — the <if>
    // in color_picker.xml reads this subject as it constructs the view.
    ensure_palette_subject();
    lv_subject_set_int(&s_palette_subject, static_cast<int>(palette_));

    // Show the modal via Modal
    if (!Modal::show(parent)) {
        return false;
    }

    // Track active instance for static callbacks
    active_instance_ = this;

    spdlog::info("[ColorPicker] Shown with initial color #{:06X}", initial_color);
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void ColorPicker::on_show() {
    // Cache all widget pointers up front
    preview_ = find_widget("selected_color_preview");
    preview_tiny_ = find_widget("selected_color_preview_tiny");
    hex_input_ = find_widget("hex_input");
    hex_input_tiny_ = find_widget("hex_input_tiny");
    hsv_picker_ = find_widget("hsv_picker");
    hsv_picker_tiny_ = find_widget("hsv_picker_tiny");
    name_label_ = find_widget("selected_name_label");
    name_label_tiny_ = find_widget("selected_name_label_tiny");

    // Register keyboard for hex input so software keyboard appears on touch
    if (hex_input_ && dialog_) {
        helix::ui::modal_register_keyboard(dialog_, hex_input_);
    }

    // Bind name label to subject
    if (name_label_) {
        lv_label_bind_text(name_label_, &name_subject_, nullptr);
    }

    // Initialize preview with current color
    update_preview(selected_color_);

    // Initialize HSV picker with current color and set callback
    if (hsv_picker_) {
        ui_hsv_picker_set_color_rgb(hsv_picker_, selected_color_);
        ui_hsv_picker_set_callback(
            hsv_picker_,
            [](uint32_t rgb, void* user_data) {
                auto* self = static_cast<ColorPicker*>(user_data);
                self->update_preview(rgb, true); // from HSV picker
            },
            this);
        spdlog::debug("[ColorPicker] HSV picker initialized with color #{:06X}", selected_color_);
    }

    // Detect MICRO/TINY breakpoint for compact layout
    lv_subject_t* bp_subject = theme_manager_get_breakpoint_subject();
    UiBreakpoint bp =
        bp_subject ? as_breakpoint(lv_subject_get_int(bp_subject)) : UiBreakpoint::Tiny;
    is_tiny_mode_ = (bp == UiBreakpoint::Micro || bp == UiBreakpoint::Tiny);

    if (is_tiny_mode_ && dialog_) {
        // Full-screen on TINY
        lv_obj_set_size(dialog_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_radius(dialog_, 0, 0);

        // Find TINY-specific containers
        presets_content_ = find_widget("presets_content");
        custom_content_ = find_widget("custom_content");
        btn_tab_presets_ = find_widget("btn_tab_presets");
        btn_tab_custom_ = find_widget("btn_tab_custom");

        // Wire up TINY-mode HSV picker
        if (hsv_picker_tiny_) {
            ui_hsv_picker_set_color_rgb(hsv_picker_tiny_, selected_color_);
            ui_hsv_picker_set_callback(
                hsv_picker_tiny_,
                [](uint32_t rgb, void* user_data) {
                    auto* self = static_cast<ColorPicker*>(user_data);
                    self->update_preview(rgb, true);
                },
                this);
        }

        // Wire up TINY-mode hex input
        if (hex_input_tiny_ && dialog_) {
            helix::ui::modal_register_keyboard(dialog_, hex_input_tiny_);
        }

        // Bind TINY name label to subject
        if (name_label_tiny_) {
            lv_label_bind_text(name_label_tiny_, &name_subject_, nullptr);
        }

        // Override hex_input_ to point to TINY version so existing handlers work
        hex_input_ = hex_input_tiny_;

        // Start on presets tab
        switch_tab(false);

        spdlog::debug("[ColorPicker] TINY mode: full-screen with tabbed layout");
    }

    // Highlight the preset that matches the initial color, if any. Done after
    // layout selection so we search the correct (visible) swatch grid.
    if (lv_obj_t* match = find_swatch_for_color(selected_color_)) {
        highlight_swatch(match);
    }
}

void ColorPicker::on_hide() {
    // Clear active instance
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }

    spdlog::debug("[ColorPicker] on_hide()");

    // Clear all cached widget pointers
    preview_ = nullptr;
    preview_tiny_ = nullptr;
    hex_input_ = nullptr;
    hex_input_tiny_ = nullptr;
    hsv_picker_ = nullptr;
    hsv_picker_tiny_ = nullptr;
    name_label_ = nullptr;
    name_label_tiny_ = nullptr;
    presets_content_ = nullptr;
    custom_content_ = nullptr;
    btn_tab_presets_ = nullptr;
    btn_tab_custom_ = nullptr;
    selected_swatch_ = nullptr;
    is_tiny_mode_ = false;

    // Call dismiss callback if set (fires on any close - select, cancel, or backdrop)
    if (dismiss_callback_) {
        dismiss_callback_();
    }
}

void ColorPicker::on_cancel() {
    spdlog::debug("[ColorPicker] Cancelled");
    Modal::on_cancel(); // Calls hide()
}

// ============================================================================
// Subject Management
// ============================================================================

void ColorPicker::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize string subjects with empty buffers (local binding only, not XML registered)
    name_buf_[0] = '\0';

    lv_subject_init_string(&name_subject_, name_buf_, nullptr, sizeof(name_buf_), "");
    subjects_.register_subject(&name_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[ColorPicker] Subjects initialized");
}

void ColorPicker::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[ColorPicker] Subjects deinitialized");
}

// ============================================================================
// Internal Methods
// ============================================================================

void ColorPicker::update_preview(uint32_t color_rgb, bool from_hsv_picker, bool from_hex_input) {
    if (!dialog_) {
        return;
    }

    selected_color_ = color_rgb;

    // Update preview swatches
    if (preview_)
        lv_obj_set_style_bg_color(preview_, lv_color_hex(color_rgb), 0);
    if (preview_tiny_)
        lv_obj_set_style_bg_color(preview_tiny_, lv_color_hex(color_rgb), 0);

    // Update hex input (unless change came from hex input itself)
    if (!from_hex_input) {
        snprintf(hex_buf_, sizeof(hex_buf_), "#%06X", color_rgb);
        auto text_color = theme_manager_get_color("text");
        if (hex_input_) {
            hex_input_updating_ = true;
            lv_textarea_set_text(hex_input_, hex_buf_);
            lv_obj_set_style_text_color(hex_input_, text_color, LV_PART_MAIN);
            hex_input_updating_ = false;
        }
        if (hex_input_tiny_ && hex_input_tiny_ != hex_input_) {
            lv_textarea_set_text(hex_input_tiny_, hex_buf_);
            lv_obj_set_style_text_color(hex_input_tiny_, text_color, LV_PART_MAIN);
        }
    }

    // Update color name via subject (both labels bound to same subject)
    std::string name = helix::get_color_name_from_hex(color_rgb);
    snprintf(name_buf_, sizeof(name_buf_), "%s", name.c_str());
    lv_subject_copy_string(&name_subject_, name_buf_);

    // Sync HSV pickers (unless change came from HSV picker)
    if (!from_hsv_picker) {
        if (hsv_picker_)
            ui_hsv_picker_set_color_rgb(hsv_picker_, color_rgb);
        if (hsv_picker_tiny_)
            ui_hsv_picker_set_color_rgb(hsv_picker_tiny_, color_rgb);
    }

    // Any color change from HSV/hex means the user has diverged from the
    // preset grid — clear the preset selection outline.
    if (from_hsv_picker || from_hex_input) {
        highlight_swatch(nullptr);
    }
}

void ColorPicker::handle_swatch_clicked(lv_obj_t* swatch) {
    if (!swatch || !dialog_) {
        return;
    }

    // Get the background color from the clicked swatch
    lv_color_t color = lv_obj_get_style_bg_color(swatch, LV_PART_MAIN);
    uint32_t rgb = lv_color_to_u32(color) & 0xFFFFFF;

    highlight_swatch(swatch);
    update_preview(rgb);
}

void ColorPicker::highlight_swatch(lv_obj_t* swatch) {
    if (selected_swatch_ == swatch) {
        return;
    }

    // Clear outline on previously selected swatch (if still alive)
    if (selected_swatch_ && lv_obj_is_valid(selected_swatch_)) {
        lv_obj_set_style_outline_width(selected_swatch_, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_opa(selected_swatch_, 0, LV_PART_MAIN);
    }

    selected_swatch_ = swatch;

    if (swatch) {
        // Outline sits outside the swatch and does not affect layout, so it
        // works regardless of whether the swatch already has an XML border.
        lv_obj_set_style_outline_color(swatch, theme_manager_get_color("primary"), LV_PART_MAIN);
        lv_obj_set_style_outline_width(swatch, 3, LV_PART_MAIN);
        lv_obj_set_style_outline_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_outline_pad(swatch, 1, LV_PART_MAIN);
    }
}

static void find_swatch_recursive(lv_obj_t* root, uint32_t color_rgb, lv_obj_t*& out) {
    if (!root || out) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        if (!child) {
            continue;
        }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_color_t bg = lv_obj_get_style_bg_color(child, LV_PART_MAIN);
            if ((lv_color_to_u32(bg) & 0xFFFFFF) == color_rgb) {
                // Skip HSV picker children — identify swatches by their small
                // radius token match would be nicer, but bg-color equality plus
                // clickable is sufficient given HSV children don't use solid bg.
                out = child;
                return;
            }
        }
        find_swatch_recursive(child, color_rgb, out);
        if (out) {
            return;
        }
    }
}

lv_obj_t* ColorPicker::find_swatch_for_color(uint32_t color_rgb) {
    if (!dialog_) {
        return nullptr;
    }
    // Both the standard and tiny preset grids live in the tree — scope the
    // search to whichever is visible for the current breakpoint so we don't
    // highlight a swatch the user can't see.
    lv_obj_t* root = is_tiny_mode_ ? presets_content_ : find_widget("standard_content");
    if (!root) {
        return nullptr;
    }
    lv_obj_t* found = nullptr;
    find_swatch_recursive(root, color_rgb, found);
    return found;
}

void ColorPicker::handle_select() {
    std::string color_name = helix::get_color_name_from_hex(selected_color_);
    spdlog::info("[ColorPicker] Color selected: #{:06X} ({})", selected_color_, color_name);

    // Invoke callback before hiding
    if (color_callback_) {
        color_callback_(selected_color_, color_name);
    }

    // Hide the picker
    hide();
}

void ColorPicker::handle_hex_input_changed() {
    if (hex_input_updating_ || !hex_input_) {
        return;
    }

    const char* text = lv_textarea_get_text(hex_input_);
    uint32_t parsed_color;

    if (helix::parse_hex_color(text, parsed_color)) {
        // Valid - normal text color, update preview
        lv_obj_set_style_text_color(hex_input_, theme_manager_get_color("text"), LV_PART_MAIN);
        update_preview(parsed_color, false, true); // from_hex_input=true
    } else {
        // Invalid - show error color
        lv_obj_set_style_text_color(hex_input_, theme_manager_get_color("danger"), LV_PART_MAIN);
    }
}

void ColorPicker::handle_hex_input_defocused() {
    if (!hex_input_) {
        return;
    }

    const char* text = lv_textarea_get_text(hex_input_);
    uint32_t parsed_color;

    if (!helix::parse_hex_color(text, parsed_color)) {
        // Invalid on defocus - revert to current selected color
        hex_input_updating_ = true;
        snprintf(hex_buf_, sizeof(hex_buf_), "#%06X", selected_color_);
        lv_textarea_set_text(hex_input_, hex_buf_);
        lv_obj_set_style_text_color(hex_input_, theme_manager_get_color("text"), LV_PART_MAIN);
        hex_input_updating_ = false;
    }
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void ColorPicker::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"color_picker_close_cb", on_close_cb},
        {"color_swatch_clicked_cb", on_swatch_cb},
        {"color_picker_cancel_cb", on_cancel_cb},
        {"color_picker_select_cb", on_select_cb},
        {"hex_input_changed_cb", on_hex_input_changed_cb},
        {"hex_input_defocused_cb", on_hex_input_defocused_cb},
        {"color_picker_tab_presets_cb", on_tab_presets_cb},
        {"color_picker_tab_custom_cb", on_tab_custom_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[ColorPicker] Callbacks registered");
}

// ============================================================================
// TINY Mode Tab Switching
// ============================================================================

void ColorPicker::switch_tab(bool show_custom) {
    if (!is_tiny_mode_)
        return;

    if (presets_content_) {
        if (show_custom)
            lv_obj_add_flag(presets_content_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(presets_content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (custom_content_) {
        if (show_custom)
            lv_obj_remove_flag(custom_content_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(custom_content_, LV_OBJ_FLAG_HIDDEN);
    }

    // Style active tab as a filled segmented-control pill; matches the
    // extrude_length_btn_selected pattern (primary fill / text-on-primary).
    const auto primary = theme_manager_get_color("primary");
    const auto text = theme_manager_get_color("text");
    const auto text_muted = theme_manager_get_color("text_muted");

    auto apply_tab_style = [&](lv_obj_t* btn, bool active) {
        if (!btn) {
            return;
        }
        if (active) {
            lv_obj_set_style_bg_color(btn, primary, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, text, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_text_color(btn, text_muted, LV_PART_MAIN);
        }
    };

    apply_tab_style(btn_tab_presets_, !show_custom);
    apply_tab_style(btn_tab_custom_, show_custom);
}

// ============================================================================
// Static Callbacks (Instance Lookup via User Data)
// ============================================================================

ColorPicker* ColorPicker::get_instance_from_event(lv_event_t* e) {
    (void)e; // Not needed - we use static instance tracking
    return active_instance_;
}

void ColorPicker::on_close_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->hide();
    }
}

void ColorPicker::on_swatch_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* swatch = static_cast<lv_obj_t*>(lv_event_get_target(e));
        self->handle_swatch_clicked(swatch);
    }
}

void ColorPicker::on_cancel_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->on_cancel();
    }
}

void ColorPicker::on_select_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_select();
    }
}

void ColorPicker::on_hex_input_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_hex_input_changed();
    }
}

void ColorPicker::on_hex_input_defocused_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_hex_input_defocused();
    }
}

void ColorPicker::on_tab_presets_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self)
        self->switch_tab(false);
}

void ColorPicker::on_tab_custom_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self)
        self->switch_tab(true);
}

} // namespace helix::ui
