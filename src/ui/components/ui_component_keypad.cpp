// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_component_keypad.cpp
 * @brief Numeric keypad overlay with reactive Subject-Observer pattern
 *
 * Uses standard overlay navigation (NavigationManager push_overlay/go_back) and reactive
 * bindings for the display. The XML binds to the keypad_display subject,
 * so updating the subject automatically updates the UI.
 */

#include "ui_component_keypad.h"

#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav.h"
#include "ui_pin_utils.h"
#include "ui_slider_scale.h"
#include "ui_utils.h"

#include "core/static_panel_registry.h"
#include "core/temperature_units.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "keypad_input.h"
#include "lvgl/lvgl.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================================
// Reactive State (Subject for XML binding)
// ============================================================================
static lv_subject_t keypad_display_subject;
static char keypad_display_buf[16] = "";
// The keypad is shared by every numeric field in the app, so it has to say which
// one is being edited. Machine Limits alone opens it from five rows.
static lv_subject_t keypad_title_subject;
static char keypad_title_buf[48] = "";
// Unit shown inside the display's right end ("°C", "mm/s").
static lv_subject_t keypad_unit_subject;
static char keypad_unit_buf[16] = "";
// Drives the dot key's disabled state, so an integer-only field never shows a
// live "." that silently does nothing to the buffer.
static lv_subject_t keypad_allow_decimal_subject;
// Shows the +/- key, only on fields whose range reaches below zero.
static lv_subject_t keypad_allow_negative_subject;

/// Dims what stays live beside the pad. NavigationManager only backdrops the FIRST
/// overlay on the stack, and the keypad is almost never that - it opens over a temp
/// graph or a dryer card. While the pad was overlay-width that went unnoticed, because
/// it covered what it failed to dim.
static lv_obj_t* keypad_backdrop = nullptr;

static void destroy_keypad_backdrop() {
    if (keypad_backdrop) {
        helix::ui::safe_delete_deferred(keypad_backdrop);
    }
}
static bool subjects_initialized = false;
static SubjectManager subjects_;

// Widget reference (for showing/hiding via nav system)
static lv_obj_t* keypad_widget = nullptr;
// Parent captured at init; the widget tree itself is built on first show.
static lv_obj_t* keypad_parent = nullptr;

// Decimals offered once a decimal point is entered. Two covers retraction
// distances (0.85mm) and half-step square corner velocity alike.
static constexpr int kKeypadFractionDigits = 2;

// Current config and input state
static ui_keypad_config_t current_config;
// A temperature config's Celsius range, which a typed value converts back into.
static float temperature_min_c = 0.0f;
static float temperature_max_c = 0.0f;
// Set by a confirm: the close that follows is not a cancel, and carries this value.
static bool confirmed = false;
static float confirmed_value = 0.0f;
static std::string confirmed_text;
static helix::ui::KeypadInput input;

// ============================================================================
// Forward declarations
// ============================================================================
static void update_display();
static void handle_confirm();

// ============================================================================
// Subject Initialization (call BEFORE XML creation)
// ============================================================================
void ui_keypad_init_subjects() {
    if (subjects_initialized) {
        return;
    }

    // Initialize display subject for reactive binding (starts empty)
    UI_MANAGED_SUBJECT_STRING(keypad_display_subject, keypad_display_buf, "", "keypad_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(keypad_title_subject, keypad_title_buf, "", "keypad_title",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(keypad_unit_subject, keypad_unit_buf, "", "keypad_unit", subjects_);
    UI_MANAGED_SUBJECT_INT(keypad_allow_decimal_subject, 1, "keypad_allow_decimal", subjects_);
    UI_MANAGED_SUBJECT_INT(keypad_allow_negative_subject, 0, "keypad_allow_negative", subjects_);

    subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy("KeypadSubjects", []() {
        // The keypad's tree hangs directly off the screen and nothing else
        // frees it: hiding only pops the nav stack, and teardown deletes the
        // app layout, not the screen. Deletion is forbidden inside the
        // destroy_all() window, so hand the root to its caller the way
        // OverlayBase's destructor does - a soft restart frees it, full
        // shutdown lets lv_deinit() free every widget. No-op when never built.
        StaticPanelRegistry::instance().record_orphaned_widget(keypad_widget);
        keypad_widget = nullptr;
        keypad_parent = nullptr;
        ui_keypad_deinit_subjects();
    });

    spdlog::debug("[Keypad] Subjects initialized");
}

void ui_keypad_deinit_subjects() {
    if (!subjects_initialized) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized = false;
    spdlog::debug("[Keypad] Subjects deinitialized");
}

// ============================================================================
// Widget Initialization (call AFTER XML creation)
// ============================================================================
void ui_keypad_init(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[Keypad] Cannot init keypad: parent is null");
        return;
    }

    if (keypad_widget) {
        spdlog::warn("[Keypad] Already initialized");
        return;
    }

    // Subjects MUST stay eager: XML elsewhere binds "keypad_display" by name at
    // parse time, so the subject has to exist before any component referencing
    // it is built. Only the widget tree is deferred.
    ui_keypad_init_subjects();

    helix::ui::store_until_deleted(&keypad_parent, parent);
    // The in-pad confirm key (phone layout, integer fields) and the header's Set
    // both confirm.
    register_xml_callbacks({
        {"on_keypad_digit",
         [](lv_event_t* e) {
             helix::ui::event_safe_call("keypad_digit", [e]() {
                 const int digit = helix::ui::key_digit(e);
                 if (digit >= 0 && input.append_digit(digit))
                     update_display();
             });
         }},
        {"on_keypad_dot",
         [](lv_event_t*) {
             helix::ui::event_safe_call("keypad_dot", []() {
                 if (input.append_dot())
                     update_display();
             });
         }},
        {"on_keypad_backspace",
         [](lv_event_t*) {
             helix::ui::event_safe_call("keypad_backspace", []() {
                 if (input.backspace())
                     update_display();
             });
         }},
        {"on_keypad_confirm",
         [](lv_event_t*) {
             helix::ui::event_safe_call("keypad_confirm", []() { handle_confirm(); });
         }},
        {"on_keypad_sign",
         [](lv_event_t*) {
             helix::ui::event_safe_call("keypad_sign", []() {
                 if (input.toggle_sign())
                     update_display();
             });
         }},
    });
    spdlog::debug("[Keypad] Numeric keypad registered (tree deferred to first show)");
}

// Build the keypad tree on demand. Plenty of sessions never touch a numeric
// field, and this is a ~15-button subtree whose XML the engine also retains.
static bool ensure_keypad_built() {
    if (keypad_widget) {
        return true;
    }
    if (!keypad_parent) {
        spdlog::error(
            "[Keypad] Cannot build keypad: no parent (never initialized, or it was deleted)");
        return false;
    }

    helix::ui::store_until_deleted(
        &keypad_widget, (lv_obj_t*)lv_xml_create(keypad_parent, "numeric_keypad_panel", nullptr));
    if (!keypad_widget) {
        spdlog::error("[Keypad] Failed to create keypad from XML");
        return false;
    }

    spdlog::debug("[Keypad] Numeric keypad built on first use");
    return true;
}

// ============================================================================
// Public API
// ============================================================================
bool ui_keypad_show(const ui_keypad_config_t* config) {
    if (!config) {
        spdlog::error("[Keypad] Cannot show keypad: invalid config");
        return false;
    }
    if (!ensure_keypad_built()) {
        return false;
    }

    // Store config, in the unit the user reads for a temperature
    const helix::units::TempUnit unit = helix::units::display_temp_unit();
    current_config = helix::ui::keypad_config_in_unit(*config, unit);
    temperature_min_c = config->min_value;
    temperature_max_c = config->max_value;
    config = &current_config;

    // Start with empty display (user enters fresh value)
    input.clear();
    input.mode = config->mode;

    // Size the buffer to this field's own range. The 3-digit default is
    // temperature-shaped and cannot express a max accel of 50000; deriving the
    // cap means a new caller cannot forget to widen it. A range reaching further
    // below zero than above it (-100 to 5) needs its minimum's digits.
    input.max_int_digits = helix::ui::keypad_int_digits_for(
        config->allow_negative
            ? std::max(std::fabs(config->min_value), std::fabs(config->max_value))
            : config->max_value);
    if (config->max_fraction_digits > 0) {
        input.max_frac_digits = std::min(config->max_fraction_digits, kKeypadFractionDigits);
        input.max_total_digits = input.max_int_digits + input.max_frac_digits;
    } else {
        input.max_frac_digits = static_cast<int>(helix::ui::KeypadInput::BUF_SIZE);
        input.max_total_digits = input.max_int_digits + kKeypadFractionDigits;
    }
    if (config->mode == helix::ui::KeypadInput::Mode::Ipv4) {
        lv_subject_set_int(&keypad_allow_decimal_subject, 1);
        lv_subject_set_int(&keypad_allow_negative_subject, 0);
        // Typed key by key, so a prefill the keys could not produce starts empty.
        for (char ch : config->initial_text) {
            const bool ok = ch == '.' ? input.append_dot()
                                      : (ch >= '0' && ch <= '9' && input.append_digit(ch - '0'));
            if (!ok) {
                input.clear();
                break;
            }
        }
    } else {
        input.allow_dot = config->allow_decimal;
        lv_subject_set_int(&keypad_allow_decimal_subject, config->allow_decimal ? 1 : 0);
        input.allow_sign = config->allow_negative;
        lv_subject_set_int(&keypad_allow_negative_subject, config->allow_negative ? 1 : 0);
    }

    lv_subject_copy_string(&keypad_title_subject, config->title_label ? config->title_label : "");

    // Update display via subject (reactive binding updates XML automatically)
    update_display();

    lv_subject_copy_string(&keypad_unit_subject, config->unit_label ? config->unit_label : "");

    // Register with nullptr lifecycle — keypad is function-based, not class-based
    // The panel authors its own width (#keypad_width, per breakpoint): a pad of
    // three digit columns, not a screen. Neither navigation width class applies, so opt
    // out of push-time width management or the push stretches it to overlay width.
    helix::nav::set_overlay_width_unmanaged(keypad_widget);
    helix::nav::register_overlay(keypad_widget, nullptr);

    // Same dimming the navigation layer gives a first overlay. Raised before the push so
    // the pad lands above it, and torn down from the close callback rather than
    // ui_keypad_hide(), which is only one of the ways out - the header back button and a
    // tap on the backdrop both pop without going through it.
    if (lv_obj_t* screen = lv_obj_get_screen(keypad_widget)) {
        destroy_keypad_backdrop();
        // Blur when the platform can afford it, a plain dim when it cannot - the same
        // 40/255 the navigation layer uses for a first overlay, so the pad dims its
        // surroundings exactly as much as any other layer does.
        helix::ui::store_until_deleted(&keypad_backdrop,
                                       helix::ui::create_fullscreen_backdrop(screen, 40));
        if (keypad_backdrop) {
            helix::ui::bring_to_front(keypad_backdrop);
        }
    }
    confirmed = false;
    // Runs once the keypad is off screen, so either hook may open the keypad again.
    helix::nav::on_close(keypad_widget, [on_confirm = config->on_confirm,
                                         on_confirm_text = config->on_confirm_text,
                                         on_cancel = config->on_cancel]() {
        destroy_keypad_backdrop();
        const bool was_confirmed = confirmed;
        const float value = confirmed_value;
        const std::string text = confirmed_text;
        if (was_confirmed && on_confirm_text)
            on_confirm_text(text);
        else if (was_confirmed && on_confirm)
            on_confirm(value);
        else if (!was_confirmed && on_cancel)
            on_cancel();
    });

    // Show via overlay navigation, but keep previous panel visible (transparent overlay)
    helix::nav::push_overlay(keypad_widget, false /* hide_previous */);

    spdlog::info("[Keypad] Showing (initial={:.1f}, range={:.0f}-{:.0f})", config->initial_value,
                 config->min_value, config->max_value);
    return true;
}

ui_keypad_config_t helix::ui::keypad_config_in_unit(const ui_keypad_config_t& config,
                                                    units::TempUnit unit) {
    ui_keypad_config_t shown = config;
    if (!config.temperature) {
        return shown;
    }
    // Whole degrees inside the Celsius range, so every accepted value converts back
    // to a target the range allows.
    shown.min_value = static_cast<float>(std::ceil(units::celsius_to_unit(config.min_value, unit)));
    shown.max_value =
        static_cast<float>(std::floor(units::celsius_to_unit(config.max_value, unit)));
    shown.initial_value =
        static_cast<float>(units::celsius_to_unit_whole(config.initial_value, unit));
    shown.unit_label = units::temp_unit_symbol(unit);
    return shown;
}

float helix::ui::keypad_value_to_celsius(float typed, float min_c, float max_c,
                                         units::TempUnit unit) {
    const int celsius = units::unit_to_celsius_whole(typed, unit);
    return std::clamp(static_cast<float>(celsius), min_c, max_c);
}

void helix::ui::show_keypad_for_slider(lv_obj_t* slider, const SliderFieldSpec& spec,
                                       ui_keypad_callback_t callback, void* user_data,
                                       const char* log_name) {
    const SliderScale scale{spec.divisor};
    ui_keypad_config_t config = {
        .initial_value = static_cast<float>(scale.to_value(lv_slider_get_value(slider))),
        .min_value = static_cast<float>(scale.to_value(lv_slider_get_min_value(slider))),
        .max_value = static_cast<float>(scale.to_value(lv_slider_get_max_value(slider))),
        .title_label = lv_tr(spec.title),
        .unit_label = spec.unit,
        .allow_decimal = spec.allow_decimal,
        .allow_negative = false,
        .callback = callback,
        .user_data = user_data};

    spdlog::debug("[{}] Keypad for {} ({}-{})", log_name, spec.title, config.min_value,
                  config.max_value);
    ui_keypad_show(&config);
}

void ui_keypad_hide() {
    if (keypad_widget && ui_keypad_is_visible()) {
        helix::nav::go_back();
    }
}

bool ui_keypad_is_visible() {
    if (!keypad_widget)
        return false;
    return !lv_obj_has_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);
}

lv_subject_t* ui_keypad_get_display_subject() {
    return &keypad_display_subject;
}

// ============================================================================
// Input Logic
// ============================================================================
static void update_display() {
    lv_subject_copy_string(&keypad_display_subject, input.buf);
}

static void handle_confirm() {
    if (input.mode == helix::ui::KeypadInput::Mode::Ipv4) {
        // The form the text goes back to validates the whole address.
        confirmed = true;
        confirmed_text = input.text();
        spdlog::info("[Keypad] Confirmed text={}", confirmed_text);
        ui_keypad_hide();
        return;
    }
    float value = input.value();

    // Validate range - show error if out of bounds
    if (value < current_config.min_value || value > current_config.max_value) {
        NOTIFY_ERROR(lv_tr("Value must be between {} and {}"), current_config.min_value,
                     current_config.max_value);
        return; // Don't close keypad, let user correct the value
    }
    if (current_config.temperature) {
        value = helix::ui::keypad_value_to_celsius(value, temperature_min_c, temperature_max_c,
                                                   helix::units::display_temp_unit());
    }
    if (current_config.validate) {
        const std::string why = current_config.validate(value);
        if (!why.empty()) {
            NOTIFY_ERROR("{}", why);
            return;
        }
    }
    confirmed = true;
    confirmed_value = value;

    // Invoke callback before hiding — hiding pops the overlay stack which
    // invalidates lifetime tokens on the parent overlay, so the callback
    // must run while the parent is still active.
    if (current_config.callback) {
        current_config.callback(value, current_config.user_data);
        spdlog::info("[Keypad] Confirmed value={:.1f}", value);
    }

    ui_keypad_hide();
}
