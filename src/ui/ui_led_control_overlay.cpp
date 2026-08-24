// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/ui_led_control_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_event_safety.h"
#include "ui_global_panel_helper.h"
#include "ui_led_chip_factory.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_color_utils.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

using namespace helix;
using namespace helix::led;

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

DEFINE_GLOBAL_OVERLAY_STORAGE(LedControlOverlay, g_led_control_overlay, get_led_control_overlay)

void init_led_control_overlay(PrinterState& printer_state) {
    INIT_GLOBAL_OVERLAY(LedControlOverlay, g_led_control_overlay, printer_state);
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

LedControlOverlay::LedControlOverlay(PrinterState& printer_state) {
    // All printer data reaches this overlay through LedController; the parameter
    // stays for the DEFINE_GLOBAL_OVERLAY_STORAGE construction signature.
    (void)printer_state;
    spdlog::trace("[{}] Constructor", get_name());
}

LedControlOverlay::~LedControlOverlay() {
    if (!lv_is_initialized()) {
        spdlog::trace("[LedControlOverlay] Destroyed (LVGL already deinit)");
        return;
    }
    spdlog::trace("[LedControlOverlay] Destroyed");
}

// ============================================================================
// OVERLAYBASE IMPLEMENTATION
// ============================================================================

void LedControlOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(brightness_subject_, 100, "led_brightness", subjects_);
        UI_MANAGED_SUBJECT_STRING(brightness_text_subject_, brightness_text_buf_, "100%",
                                  "led_brightness_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(strip_name_subject_, strip_name_buf_, "LED",
                                  "led_active_strip_name", subjects_);
        UI_MANAGED_SUBJECT_INT(wled_brightness_subject_, 100, "led_wled_brightness", subjects_);
        UI_MANAGED_SUBJECT_STRING(wled_brightness_text_subject_, wled_brightness_text_buf_, "100%",
                                  "led_wled_brightness_text", subjects_);

        // WLED toggle state (0=off, 1=on)
        UI_MANAGED_SUBJECT_INT(wled_is_on_, 0, "led_wled_is_on", subjects_);

        // Section visibility subjects (0=hidden, 1=visible)
        UI_MANAGED_SUBJECT_INT(native_visible_, 0, "led_native_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(effects_visible_, 0, "led_effects_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(wled_visible_, 0, "led_wled_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(macro_visible_, 0, "led_macro_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(strip_selector_visible_, 0, "led_strip_selector_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(color_visible_, 0, "led_color_visible", subjects_);
    });
}

lv_obj_t* LedControlOverlay::create(lv_obj_t* parent) {
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "led_control_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find widget containers needed for dynamic population (lv_obj_clean + repopulate)
    // Section visibility is handled declaratively via bind_flag_if_eq subjects
    strip_selector_section_ = lv_obj_find_by_name(overlay_root_, "strip_selector_section");
    current_color_swatch_ = lv_obj_find_by_name(overlay_root_, "current_color_swatch");
    color_presets_container_ = lv_obj_find_by_name(overlay_root_, "color_presets_container");
    effects_container_ = lv_obj_find_by_name(overlay_root_, "effects_container");
    wled_presets_container_ = lv_obj_find_by_name(overlay_root_, "wled_presets_container");
    macro_buttons_container_ = lv_obj_find_by_name(overlay_root_, "macro_buttons_container");

    // Populate based on available backends
    populate_sections();

    spdlog::trace("[{}] Created overlay", get_name());
    return overlay_root_;
}

void LedControlOverlay::register_callbacks() {
    register_xml_callbacks({
        {"led_custom_color_cb", on_custom_color_cb},
        {"led_brightness_changed_cb", on_brightness_changed_cb},
        {"led_native_turn_off_cb", on_native_turn_off_cb},
        {"led_wled_toggle_cb", on_wled_toggle_cb},
        {"led_color_preset_cb", on_color_preset_cb},
    });
    spdlog::trace("[{}] Callbacks registered", get_name());
}

void LedControlOverlay::on_activate() {
    OverlayBase::on_activate();

    auto& controller = LedController::instance();
    if (controller.is_initialized()) {
        // Read current color from the selected strip's cached state
        std::string active_strip = controller.first_available_strip();

        // Determine the backend type of the active strip. backend_for_strip() is
        // the single classifier — toggle_all()/set_color_all() dispatch off it, so
        // the overlay must agree with them about what kind of strip this is.
        if (!active_strip.empty()) {
            selected_backend_type_ = controller.backend_for_strip(active_strip);
        }

        if (selected_backend_type_ == LedBackendType::OUTPUT_PIN && !active_strip.empty()) {
            current_brightness_ = controller.output_pin().brightness_pct(active_strip);
        } else if (selected_backend_type_ != LedBackendType::WLED &&
                   selected_backend_type_ != LedBackendType::MACRO && !active_strip.empty() &&
                   controller.native().has_strip_color(active_strip)) {
            auto color = controller.native().get_strip_color(active_strip);
            // If the reported color is all-off (LED currently off), fall back to
            // the controller's saved last_* values so we don't poison them with
            // zeros when this overlay later persists on deactivate.
            if (color.r == 0.0 && color.g == 0.0 && color.b == 0.0 && color.w == 0.0) {
                current_brightness_ = controller.last_brightness();
                current_color_ = controller.last_color();
                current_white_ = controller.last_white();
            } else {
                color.decompose(current_color_, current_brightness_, current_white_);
            }
        } else if (selected_backend_type_ != LedBackendType::WLED &&
                   selected_backend_type_ != LedBackendType::MACRO) {
            current_brightness_ = controller.last_brightness();
            current_color_ = controller.last_color();
            current_white_ = controller.last_white();
        }

        // Update section visibility based on strip type
        update_section_visibility();

        // Poll WLED status on overlay activation for live state
        if (selected_backend_type_ == LedBackendType::WLED) {
            // Sync WLED brightness slider to active strip's brightness
            std::string wled_strip_id = active_strip;
            if (!wled_strip_id.empty()) {
                auto strip_state = controller.wled().get_strip_state(wled_strip_id);
                int pct = (strip_state.brightness * 100) / 255;
                lv_subject_set_int(&wled_brightness_subject_, pct);
                update_wled_brightness_text(pct);
            }
            update_wled_toggle_button();
            refresh_wled_status();
        }
    }

    // Update visual state — brightness slider syncs via bind_value="led_brightness"
    update_brightness_text(current_brightness_);
    update_current_color_swatch();

    // Sync slider position via subject (bind_value handles the visual update)
    lv_subject_set_int(&brightness_subject_, current_brightness_);

    // Subscribe to WLED brightness slider changes
    wled_brightness_observer_ = helix::ui::observe_int_sync<LedControlOverlay>(
        &wled_brightness_subject_, this, [](LedControlOverlay* self, int value) {
            if (self->is_visible()) {
                self->handle_wled_brightness(value);
            }
        });

    // Sync effect highlight to current Moonraker state
    if (effects_container_ && controller.is_initialized()) {
        const auto& all_effects = controller.effects().effects();
        std::string active_effect;
        for (const auto& eff : all_effects) {
            if (eff.enabled) {
                active_effect = eff.name;
                break;
            }
        }
        highlight_active_effect(active_effect);
    }

    // Register for live color updates from Moonraker subscription
    controller.native().set_color_change_callback(
        [this](const std::string& strip_id, const NativeBackend::StripColor& color) {
            if (!is_visible())
                return;

            // Only update for the currently active strip
            auto& ctrl = LedController::instance();
            if (strip_id != ctrl.first_available_strip())
                return;

            // Queue UI update to main thread — this callback runs on background thread
            uint8_t r = to_channel_byte(color.r);
            uint8_t g = to_channel_byte(color.g);
            uint8_t b = to_channel_byte(color.b);
            lv_obj_t* swatch = current_color_swatch_;
            helix::ui::queue_widget_update(swatch, [r, g, b](lv_obj_t* s) {
                lv_obj_set_style_bg_color(s, lv_color_make(r, g, b), 0);
            });
        });

    spdlog::debug("[{}] Activated (brightness={}, color=0x{:06X})", get_name(), current_brightness_,
                  current_color_);
}

void LedControlOverlay::on_deactivate() {
    OverlayBase::on_deactivate();

    // Stop live color updates + persist state
    auto& controller = LedController::instance();
    if (controller.is_initialized()) {
        controller.native().clear_color_change_callback();
    }

    wled_brightness_observer_.reset();

    // Persist state
    if (controller.is_initialized()) {
        controller.set_last_brightness(current_brightness_);
        controller.set_last_color(current_color_);
        controller.set_last_white(current_white_);
        controller.save_config();
    }

    spdlog::debug("[{}] Deactivated", get_name());
}

void LedControlOverlay::cleanup() {
    spdlog::debug("[{}] Cleanup", get_name());
    wled_brightness_observer_.reset();
    deinit_subjects_base(subjects_);

    // Null widget pointers — WLED poll callbacks may still be in-flight
    strip_selector_section_ = nullptr;
    color_presets_container_ = nullptr;
    effects_container_ = nullptr;
    wled_presets_container_ = nullptr;
    macro_buttons_container_ = nullptr;

    OverlayBase::cleanup();
}

// ============================================================================
// SECTION POPULATION
// ============================================================================

void LedControlOverlay::populate_sections() {
    auto& controller = LedController::instance();
    if (!controller.is_initialized()) {
        spdlog::warn("[{}] LedController not initialized - hiding all sections", get_name());
        update_section_visibility();
        return;
    }

    populate_strip_selector();
    populate_color_presets();
    populate_effects();
    populate_wled();
    populate_macros();
    update_section_visibility();
}

void LedControlOverlay::update_section_visibility() {
    // Section visibility driven by subjects — XML bind_flag_if_eq handles the UI
    auto& controller = LedController::instance();
    bool ctrl_init = controller.is_initialized();

    bool has_native = ctrl_init && controller.native().is_available();
    bool has_effects = ctrl_init && controller.effects().is_available();
    bool has_wled = ctrl_init && controller.wled().is_available();

    bool native_vis = false;
    bool effects_vis = false;
    bool wled_vis = false;
    bool macro_vis = false;

    switch (selected_backend_type_) {
    case LedBackendType::WLED:
        wled_vis = has_wled;
        break;
    case LedBackendType::MACRO:
        macro_vis = true;
        break;
    case LedBackendType::OUTPUT_PIN:
        native_vis = true;
        break;
    case LedBackendType::NATIVE:
    case LedBackendType::LED_EFFECT:
    default:
        native_vis = has_native;
        effects_vis = has_effects;
        break;
    }

    lv_subject_set_int(&native_visible_, native_vis ? 1 : 0);
    lv_subject_set_int(&effects_visible_, effects_vis ? 1 : 0);
    lv_subject_set_int(&wled_visible_, wled_vis ? 1 : 0);
    lv_subject_set_int(&macro_visible_, macro_vis ? 1 : 0);

    // Color section visible for native RGB strips but NOT output_pin (brightness-only)
    // or white-only native strips (e.g. [led chamber_light] with white_pin only).
    // Mirror the capability gate in LedSettingsOverlay::populate_auto_state_rows():
    // treat the selection as color-capable if any selected native strip reports
    // supports_color. Picking a color on a white-only strip silently converts
    // RGB->white luminance, which is misleading — so hide the picker entirely.
    bool selected_supports_color = false;
    if (ctrl_init) {
        const auto& native_strips = controller.native().strips();
        auto is_color_capable = [&native_strips](const std::string& id) {
            const auto* s = find_strip(native_strips, id);
            return s != nullptr && s->supports_color;
        };

        const auto& selected = controller.selected_strips();
        if (selected.empty()) {
            // No explicit selection: fall back to the implicit target used by
            // send_color_to_strips().
            selected_supports_color = is_color_capable(controller.first_available_strip());
        } else {
            for (const auto& strip_id : selected) {
                if (is_color_capable(strip_id)) {
                    selected_supports_color = true;
                    break;
                }
            }
        }
    }

    bool color_vis = (native_vis && selected_backend_type_ != LedBackendType::OUTPUT_PIN &&
                      selected_supports_color);
    lv_subject_set_int(&color_visible_, color_vis ? 1 : 0);

    // Strip selector visible when there are 2+ selectable strips. Must count the
    // same list populate_strip_selector() renders chips from, or the row shows
    // with a single chip in it.
    const size_t total_strips = ctrl_init ? controller.all_selectable_strips().size() : 0;
    lv_subject_set_int(&strip_selector_visible_, total_strips > 1 ? 1 : 0);

    spdlog::debug(
        "[{}] Section visibility: native={}, effects={}, wled={}, macros={}, backend_type={}",
        get_name(), native_vis, effects_vis, wled_vis, macro_vis,
        static_cast<int>(selected_backend_type_));
}

void LedControlOverlay::populate_strip_selector() {
    if (!strip_selector_section_)
        return;

    auto& controller = LedController::instance();

    // One source of truth for what is selectable: the same list Settings renders
    // chips from and the same one discover_from_hardware() prunes the saved
    // selection against. Building it inline here let the overlay offer PRESET
    // macros, which the controller treats as unselectable — selecting one was
    // silently dropped on the next discovery pass.
    const std::vector<LedStripInfo> all_strips = controller.all_selectable_strips();

    if (all_strips.empty())
        return;

    const auto& selected = controller.selected_strips();

    // Determine active strip name for the header
    std::string active_name = all_strips[0].name;
    if (!selected.empty()) {
        if (const auto* s = find_strip(all_strips, selected[0])) {
            active_name = s->name;
        }
    }
    snprintf(strip_name_buf_, sizeof(strip_name_buf_), "%s", active_name.c_str());
    lv_subject_copy_string(&strip_name_subject_, strip_name_buf_);

    // Only show selector chips if multiple strips total
    if (all_strips.size() <= 1)
        return;

    for (const auto& strip : all_strips) {
        bool is_selected =
            selected.empty()
                ? (&strip == &all_strips[0])
                : (std::find(selected.begin(), selected.end(), strip.id) != selected.end());

        // Add suffix for non-native strips to visually distinguish them
        std::string display_name = strip.name;
        if (strip.backend == LedBackendType::WLED)
            display_name += " (WLED)";
        else if (strip.backend == LedBackendType::MACRO)
            display_name += " (Macro)";
        else if (strip.backend == LedBackendType::OUTPUT_PIN)
            display_name += " (Pin)";

        helix::ui::create_led_chip(
            strip_selector_section_, strip.id, display_name, is_selected,
            [this](const std::string& strip_id) { handle_strip_selected(strip_id); });
    }

    spdlog::trace("[{}] Populated strip selector with {} selectable strips", get_name(),
                  all_strips.size());
}

void LedControlOverlay::populate_color_presets() {
    if (!color_presets_container_)
        return;

    // Swatches are defined in XML with event_cb; just set user_data with color values
    // Click handling via led_color_preset_cb registered in register_callbacks()
    static const struct {
        const char* name;
        uint32_t color;
    } swatches[] = {
        {"swatch_white", 0xFFFFFF},  {"swatch_warm", 0xFFD700}, {"swatch_orange", 0xFF6B35},
        {"swatch_blue", 0x4FC3F7},   {"swatch_red", 0xFF4444},  {"swatch_green", 0x66BB6A},
        {"swatch_purple", 0x9C27B0}, {"swatch_cyan", 0x00BCD4},
    };

    int count = 0;
    for (const auto& s : swatches) {
        auto* swatch = lv_obj_find_by_name(overlay_root_, s.name);
        if (!swatch)
            continue;

        // Store color as static data — no heap allocation needed
        static uint32_t color_values[8];
        color_values[count] = s.color;
        lv_obj_set_user_data(swatch, &color_values[count]);
        count++;
    }

    spdlog::trace("[{}] Attached color data to {} presets", get_name(), count);
}

void LedControlOverlay::populate_effects() {
    if (!effects_container_)
        return;

    auto& controller = LedController::instance();

    // Filter effects by the currently selected strip
    const auto& selected = controller.selected_strips();
    std::vector<LedEffectInfo> effects;
    if (!selected.empty()) {
        effects = controller.effects().effects_for_strip(selected[0]);
    } else if (!controller.native().strips().empty()) {
        effects = controller.effects().effects_for_strip(controller.native().strips()[0].id);
    } else {
        effects = controller.effects().effects();
    }

    for (const auto& effect : effects) {
        const char* attrs[] = {"label", effect.display_name.c_str(), nullptr};
        auto* chip =
            static_cast<lv_obj_t*>(lv_xml_create(effects_container_, "led_action_chip", attrs));
        if (!chip)
            continue;

        auto* name_data = new std::string(effect.name);
        lv_obj_set_user_data(chip, name_data);

        lv_obj_add_event_cb(
            chip,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] effect_cb");
                auto* data = static_cast<std::string*>(lv_event_get_user_data(e));
                if (data)
                    get_led_control_overlay().handle_effect_activate(*data);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, name_data);

        lv_obj_add_event_cb(
            chip,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, name_data);
    }

    // Highlight whichever effect is currently enabled (from Moonraker subscription)
    std::string active_effect;
    for (const auto& effect : effects) {
        if (effect.enabled) {
            active_effect = effect.name;
            break;
        }
    }
    if (!active_effect.empty()) {
        highlight_active_effect(active_effect);
    }

    spdlog::trace("[{}] Populated {} effects", get_name(), effects.size());
}

void LedControlOverlay::populate_wled() {
    if (!wled_presets_container_)
        return;

    auto& controller = LedController::instance();
    if (!controller.wled().is_available())
        return;

    // Determine active WLED strip
    const auto& selected = controller.selected_strips();
    std::string active_strip_id;
    if (!selected.empty() && selected_backend_type_ == LedBackendType::WLED) {
        active_strip_id = selected[0];
    } else if (!controller.wled().strips().empty()) {
        active_strip_id = controller.wled().strips()[0].id;
    }

    if (active_strip_id.empty())
        return;

    // Get current state for highlighting
    auto state = controller.wled().get_strip_state(active_strip_id);

    // Get presets for this strip (real names from device or mock data)
    const auto& presets = controller.wled().get_strip_presets(active_strip_id);

    // Determine which presets to show
    struct PresetEntry {
        int id;
        std::string name;
    };
    std::vector<PresetEntry> entries;

    if (presets.empty()) {
        // Fallback to numbered presets
        for (int i = 1; i <= 5; ++i) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s %d", lv_tr("Preset"), i);
            entries.push_back({i, buf});
        }
    } else {
        for (const auto& p : presets) {
            entries.push_back({p.id, p.name});
        }
    }

    auto accent = theme_manager_get_color("primary");
    auto on_accent = theme_manager_get_color("screen_bg");

    for (const auto& entry : entries) {
        const char* attrs[] = {"label", entry.name.c_str(), nullptr};
        auto* chip = static_cast<lv_obj_t*>(
            lv_xml_create(wled_presets_container_, "led_action_chip", attrs));
        if (!chip)
            continue;

        auto* id_data = new int(entry.id);
        lv_obj_set_user_data(chip, id_data);

        // Highlight active preset
        if (entry.id == state.active_preset) {
            lv_obj_set_style_bg_color(chip, accent, LV_PART_MAIN);
            auto* label = lv_obj_get_child(chip, 0);
            if (label)
                lv_obj_set_style_text_color(label, on_accent, LV_PART_MAIN);
        }

        lv_obj_add_event_cb(
            chip,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] wled_preset_cb");
                auto* data = static_cast<int*>(lv_event_get_user_data(e));
                if (data)
                    get_led_control_overlay().handle_wled_preset(*data);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, id_data);

        lv_obj_add_event_cb(
            chip, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, id_data);
    }

    spdlog::trace("[{}] Populated {} WLED presets for '{}'", get_name(), entries.size(),
                  active_strip_id);
}

void LedControlOverlay::populate_macros() {
    if (!macro_buttons_container_)
        return;

    auto& controller = LedController::instance();

    // If a specific macro device is selected, show controls for just that one.
    // Read from configured_macros() — the persisted list all_selectable_strips()
    // builds the chips from — so the chip and its controls can't disagree.
    const auto& macros = controller.configured_macros();
    const auto& selected = controller.selected_strips();
    if (!selected.empty() && is_macro_strip_id(selected[0])) {
        if (const auto* m = find_macro(macros, selected[0])) {
            populate_macro_controls(*m);
            return;
        }
    }

    // Default: show all macro controls (initial state before selection)
    for (const auto& macro : macros) {
        populate_macro_controls(macro);
    }

    spdlog::trace("[{}] Populated macros section", get_name());
}

void LedControlOverlay::populate_macro_controls(const LedMacroInfo& macro) {
    switch (macro.type) {
    case MacroLedType::ON_OFF:
        add_macro_chip(lv_tr("Turn On"), macro.display_name, &LedControlOverlay::handle_macro_on);
        add_macro_chip(lv_tr("Turn Off"), macro.display_name, &LedControlOverlay::handle_macro_off);
        break;

    case MacroLedType::TOGGLE:
        add_macro_chip(lv_tr("Toggle"), macro.display_name,
                       &LedControlOverlay::handle_macro_toggle);
        break;

    case MacroLedType::PRESET:
        for (const auto& preset_macro : macro.presets) {
            add_macro_chip(pretty_print_macro(preset_macro), preset_macro,
                           &LedControlOverlay::handle_macro_custom);
        }
        break;
    }
}

// ============================================================================
// ACTION HANDLERS
// ============================================================================

void LedControlOverlay::handle_color_preset(uint32_t color) {
    current_color_ = color;

    // For RGBW strips, white swatch (0xFFFFFF) uses the dedicated white LED
    auto& controller = LedController::instance();
    const auto* active =
        find_strip(controller.native().strips(), controller.first_available_strip());
    const bool strip_has_white = active != nullptr && active->supports_white;

    if (color == 0xFFFFFF && strip_has_white) {
        current_white_ = 1.0;
    } else {
        current_white_ = 0.0;
    }

    // Presets are defined at full brightness — reset brightness to 100%
    current_brightness_ = 100;
    update_brightness_text(current_brightness_);
    lv_subject_set_int(&brightness_subject_, current_brightness_);

    apply_current_color();
    spdlog::info("[{}] Color preset applied: 0x{:06X} W={:.1f}", get_name(), color, current_white_);
}

void LedControlOverlay::handle_brightness_change(int brightness) {
    if (brightness == current_brightness_)
        return;

    current_brightness_ = brightness;
    update_brightness_text(brightness);

    // Route brightness to output_pin backend directly (no color to apply).
    // Only the output_pin members of the selection: a mixed selection would
    // otherwise turn "neopixel a" into SET_PIN PIN=a.
    if (selected_backend_type_ == LedBackendType::OUTPUT_PIN) {
        auto& controller = LedController::instance();
        for (const auto& strip_id : target_strips_for(LedBackendType::OUTPUT_PIN)) {
            controller.output_pin().set_brightness(strip_id, brightness);
        }
    } else {
        // Re-apply current color at new brightness
        apply_current_color();
    }

    spdlog::debug("[{}] Brightness changed to {}%", get_name(), brightness);
}

void LedControlOverlay::handle_custom_color() {
    spdlog::info("[{}] Opening custom color picker", get_name());

    // Use the ColorPicker modal
    static helix::ui::ColorPicker color_picker;
    color_picker.set_color_callback([this](uint32_t rgb, const std::string& name) {
        spdlog::info("[{}] Custom color selected: 0x{:06X} ({})", get_name(), rgb, name);

        // Split the picked color into brightness (V) + full-brightness base
        // color using the same decomposition the strip cache goes through.
        NativeBackend::StripColor picked;
        unpack_rgb(rgb, picked.r, picked.g, picked.b);

        uint32_t full_color = 0;
        int brightness = 0;
        double picked_white = 0.0;
        picked.decompose(full_color, brightness, picked_white);
        if (brightness < 1)
            brightness = 1; // Avoid zero brightness from very dark picks

        // Apply the full-brightness base color first, then sync brightness
        spdlog::debug("[{}] Custom color decomposed: base=0x{:06X} brightness={}%", get_name(),
                      full_color, brightness);

        // Set brightness BEFORE handle_color_preset so it uses the new value
        current_brightness_ = brightness;
        update_brightness_text(brightness);
        handle_color_preset(full_color);

        // Sync slider via subject (bind_value handles the visual update)
        lv_subject_set_int(&brightness_subject_, brightness);
    });

    if (overlay_root_) {
        color_picker.show_with_color(lv_obj_get_parent(overlay_root_), current_color_);
    }
}

void LedControlOverlay::handle_effect_activate(const std::string& effect_name) {
    spdlog::info("[{}] Activating effect: {}", get_name(), effect_name);
    auto& controller = LedController::instance();
    controller.effects().activate_effect(
        effect_name, []() { spdlog::debug("[LedControlOverlay] Effect activated successfully"); },
        // Log-only handler: the user is told nothing here, so the report stays
        // with GcodeErrorRouter's `!!` broadcast (include/rpc_error_policy.h).
        [](const std::string& err) {
            spdlog::error("[LedControlOverlay] Effect activation failed: {}", err);
        },
        /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);

    // Highlight active chip, unhighlight others
    highlight_active_effect(effect_name);
}

void LedControlOverlay::handle_native_turn_off() {
    spdlog::info("[{}] Turn off: stopping effects + turning off LED", get_name());
    auto& controller = LedController::instance();

    // Handle output_pin: just set value to 0
    if (selected_backend_type_ == LedBackendType::OUTPUT_PIN) {
        const auto& selected = controller.selected_strips();
        if (!selected.empty()) {
            controller.output_pin().turn_off(selected[0]);
        }
        current_brightness_ = 0;
        update_brightness_text(0);
        lv_subject_set_int(&brightness_subject_, 0);
        return;
    }

    // Stop led_effects if any are available
    if (controller.effects().is_available()) {
        controller.effects().stop_all_effects(
            []() { spdlog::debug("[LedControlOverlay] All effects stopped"); },
            // Log-only handler — see handle_effect_activate().
            [](const std::string& err) {
                spdlog::error("[LedControlOverlay] Stop effects failed: {}", err);
            },
            /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
        highlight_active_effect("");
    }

    // Turn off all selected native strips (set color to black)
    for (const auto& strip_id : native_target_strips()) {
        controller.native().turn_off(strip_id);
    }
}

std::vector<std::string> LedControlOverlay::target_strips_for(LedBackendType type) {
    auto& controller = LedController::instance();

    // Keep only the strips this backend actually owns. backend_for_strip() is the
    // same lookup the controller dispatches on, so the filter cannot drift from
    // where the command would really be sent.
    std::vector<std::string> targets;
    for (const auto& strip_id : controller.selected_strips()) {
        if (controller.backend_for_strip(strip_id) == type) {
            targets.push_back(strip_id);
        }
    }
    if (!targets.empty()) {
        return targets;
    }

    // Nothing of this backend is selected: fall back to its first strip, the
    // implicit target the color/turn-off paths have always used.
    const std::vector<LedStripInfo>* pool = nullptr;
    switch (type) {
    case LedBackendType::NATIVE:
        pool = &controller.native().strips();
        break;
    case LedBackendType::OUTPUT_PIN:
        pool = &controller.output_pin().pins();
        break;
    case LedBackendType::WLED:
        pool = &controller.wled().strips();
        break;
    case LedBackendType::MACRO:
    case LedBackendType::LED_EFFECT:
        // Macro devices and effects are addressed by name from their own lists;
        // there is no meaningful "first strip" to fall back to.
        break;
    }
    if (pool == nullptr || pool->empty()) {
        return {};
    }
    return {(*pool)[0].id};
}

std::vector<std::string> LedControlOverlay::native_target_strips() {
    return target_strips_for(LedBackendType::NATIVE);
}

void LedControlOverlay::handle_wled_toggle() {
    auto& controller = LedController::instance();
    const auto& selected = controller.selected_strips();
    if (!selected.empty() && selected_backend_type_ == LedBackendType::WLED) {
        spdlog::info("[{}] WLED toggle: {}", get_name(), selected[0]);
        // toggle() completes on an HttpExecutor worker thread, and both helpers
        // touch subjects/widgets — bg_cb marshals the whole body to the main
        // thread behind the overlay's generation guard.
        controller.wled().toggle(selected[0],
                                 lifetime_.bg_cb("LedControlOverlay::wled_toggle",
                                                 [this]() {
                                                     update_wled_toggle_button();
                                                     refresh_wled_status();
                                                 }),
                                 nullptr);
    }
}

void LedControlOverlay::update_wled_toggle_button() {
    // Button text, colors, and styling driven declaratively via led_wled_is_on subject
    auto& controller = LedController::instance();
    const auto& selected = controller.selected_strips();
    std::string strip_id;
    if (!selected.empty() && selected_backend_type_ == LedBackendType::WLED) {
        strip_id = selected[0];
    } else if (!controller.wled().strips().empty()) {
        strip_id = controller.wled().strips()[0].id;
    }

    if (strip_id.empty())
        return;

    auto state = controller.wled().get_strip_state(strip_id);
    lv_subject_set_int(&wled_is_on_, state.is_on ? 1 : 0);
}

void LedControlOverlay::highlight_active_effect(const std::string& active_name) {
    if (!effects_container_)
        return;

    auto accent = theme_manager_get_color("primary");
    auto card_bg = theme_manager_get_color("card_bg");
    auto text_color = theme_manager_get_color("text");
    auto on_accent = theme_manager_get_color("screen_bg");

    uint32_t count = lv_obj_get_child_count(effects_container_);
    for (uint32_t i = 0; i < count; i++) {
        auto* child = lv_obj_get_child(effects_container_, i);
        auto* data = static_cast<std::string*>(lv_obj_get_user_data(child));
        if (!data)
            continue; // skip stop button (has no user data)

        bool is_active = (*data == active_name);
        lv_obj_set_style_bg_color(child, is_active ? accent : card_bg, LV_PART_MAIN);
        auto* label = lv_obj_get_child(child, 0);
        if (label)
            lv_obj_set_style_text_color(label, is_active ? on_accent : text_color, LV_PART_MAIN);
    }
}

void LedControlOverlay::handle_wled_preset(int preset_id) {
    spdlog::info("[{}] Activating WLED preset {}", get_name(), preset_id);
    auto& controller = LedController::instance();
    const auto& selected = controller.selected_strips();
    if (!selected.empty() && selected_backend_type_ == LedBackendType::WLED) {
        controller.wled().set_preset(
            selected[0], preset_id, []() { get_led_control_overlay().refresh_wled_status(); },
            nullptr);
    }
}

void LedControlOverlay::handle_wled_brightness(int brightness) {
    update_wled_brightness_text(brightness);

    // The wled_brightness subject observer fires immediately on registration
    // with its default value, so logging unconditionally here reported "WLED
    // brightness: 100%" on every activation even with no WLED device present.
    // Log only where the write actually happens.
    auto& controller = LedController::instance();
    const auto& selected = controller.selected_strips();
    if (!selected.empty() && selected_backend_type_ == LedBackendType::WLED) {
        spdlog::debug("[{}] WLED brightness: {}%", get_name(), brightness);
        controller.wled().set_brightness(selected[0], brightness);
    }
}

void LedControlOverlay::handle_macro_on(const std::string& macro_name) {
    spdlog::info("[{}] Executing macro ON: {}", get_name(), macro_name);
    auto& controller = LedController::instance();
    controller.macro().execute_on(macro_name);
}

void LedControlOverlay::handle_macro_off(const std::string& macro_name) {
    spdlog::info("[{}] Executing macro OFF: {}", get_name(), macro_name);
    auto& controller = LedController::instance();
    controller.macro().execute_off(macro_name);
}

void LedControlOverlay::handle_macro_toggle(const std::string& macro_name) {
    spdlog::info("[{}] Executing macro TOGGLE: {}", get_name(), macro_name);
    auto& controller = LedController::instance();
    controller.macro().execute_toggle(macro_name);
}

void LedControlOverlay::handle_macro_custom(const std::string& gcode) {
    spdlog::info("[{}] Executing custom macro: {}", get_name(), gcode);
    auto& controller = LedController::instance();
    controller.macro().execute_custom_action(gcode);
}

void LedControlOverlay::handle_strip_selected(const std::string& strip_id) {
    spdlog::info("[{}] Strip selected: {}", get_name(), strip_id);

    auto& controller = LedController::instance();

    // The chip row is multi-select — populate_strip_selector() marks every strip
    // in selected_strips() as checked, and every consumer of that vector acts on
    // all of it (toggle_all, set_color_all, set_brightness_all,
    // light_state_trackable, send_color_to_strips). So a tap on an unselected
    // chip ADDS to the selection; replacing it silently discarded a multi-strip
    // choice made in Settings, which on_deactivate() then persisted.
    //
    // The strip the overlay focuses on lands at the front: selected_strips()[0]
    // drives the header name, the effects/WLED sections, first_available_strip()
    // and query_tracked_led_state(), so the front must be what the user tapped.
    auto selected = controller.selected_strips();
    auto it = std::find(selected.begin(), selected.end(), strip_id);
    std::string focus_id = strip_id;

    if (it != selected.end()) {
        // Already selected — deselect it, unless it is the last one standing.
        if (selected.size() > 1) {
            selected.erase(it);
            focus_id = selected.front();
        }
    } else {
        selected.insert(selected.begin(), strip_id);
    }

    controller.set_selected_strips(selected);

    // Classify via the controller so the overlay's sections agree with the
    // backend toggle_all()/set_color_all() will actually dispatch to.
    selected_backend_type_ = controller.backend_for_strip(focus_id);

    std::string display_name = focus_id;
    switch (selected_backend_type_) {
    case LedBackendType::MACRO:
        display_name = strip_macro_name(focus_id);
        break;
    case LedBackendType::WLED:
        if (const auto* s = find_strip(controller.wled().strips(), focus_id))
            display_name = s->name;
        break;
    case LedBackendType::OUTPUT_PIN:
        if (const auto* p = find_strip(controller.output_pin().pins(), focus_id))
            display_name = p->name;
        break;
    default:
        if (const auto* s = find_strip(controller.native().strips(), focus_id))
            display_name = s->name;
        break;
    }

    // Update strip name display
    snprintf(strip_name_buf_, sizeof(strip_name_buf_), "%s", display_name.c_str());
    lv_subject_copy_string(&strip_name_subject_, strip_name_buf_);

    if (selected_backend_type_ == LedBackendType::WLED) {
        // WLED strip selected: rebuild WLED section, update visibility
        if (wled_presets_container_) {
            helix::ui::safe_clean_children(wled_presets_container_);
            populate_wled();
        }

        // Sync WLED brightness slider to the focused strip's brightness
        auto& ctrl_ref = LedController::instance();
        auto strip_state = ctrl_ref.wled().get_strip_state(focus_id);
        int pct = (strip_state.brightness * 100) / 255;
        lv_subject_set_int(&wled_brightness_subject_, pct);
        update_wled_brightness_text(pct);
        update_wled_toggle_button();
    } else if (selected_backend_type_ == LedBackendType::OUTPUT_PIN) {
        // Output pin focused: sync brightness from pin value
        int pct = controller.output_pin().brightness_pct(focus_id);
        current_brightness_ = pct;
        update_brightness_text(pct);
        lv_subject_set_int(&brightness_subject_, pct);
    } else if (selected_backend_type_ == LedBackendType::MACRO) {
        // Macro strip focused: rebuild macro controls for this specific macro
        if (macro_buttons_container_) {
            helix::ui::safe_clean_children(macro_buttons_container_);
            if (const auto* m = find_macro(controller.configured_macros(), focus_id)) {
                populate_macro_controls(*m);
            }
        }
    } else {
        // Native strip focused: update color/brightness from cache
        auto strip_color = controller.native().get_strip_color(focus_id);
        strip_color.decompose(current_color_, current_brightness_, current_white_);
        update_brightness_text(current_brightness_);
        update_current_color_swatch();
        lv_subject_set_int(&brightness_subject_, current_brightness_);

        // Rebuild effects for the newly selected strip
        if (effects_container_) {
            helix::ui::safe_clean_children(effects_container_);
            populate_effects();
        }
    }

    // Defer rebuild (#80) AND use safe_clean_children (#776): the clicked chip is
    // a child of strip_selector_section_; deleting it mid-callback is the #80 crash.
    // lifetime_.defer moves the rebuild off the click stack; safe_clean_children
    // escapes UpdateQueue::process_pending() so the sync clean can't corrupt LVGL's
    // event linked list.
    if (!strips_rebuild_pending_) {
        strips_rebuild_pending_ = true;
        lifetime_.defer("LedControlOverlay::rebuild_strips", [this]() {
            strips_rebuild_pending_ = false;
            if (strip_selector_section_) {
                lv_obj_update_layout(strip_selector_section_);
                helix::ui::safe_clean_children(strip_selector_section_);
                populate_strip_selector();
            }
            update_section_visibility();
        });
    }
}

// ============================================================================
// HELPERS
// ============================================================================

void LedControlOverlay::apply_current_color() {
    // Stop any running LED effects before applying a manual color
    auto& controller = LedController::instance();
    if (controller.effects().is_available()) {
        controller.effects().stop_all_effects();
        highlight_active_effect("");
    }

    double bf = static_cast<double>(current_brightness_) / 100.0;

    if (current_white_ > 0.0) {
        // RGBW white mode: use dedicated white LED, not RGB
        send_color_to_strips(0.0, 0.0, 0.0, current_white_ * bf);
    } else {
        double r = 0.0, g = 0.0, b = 0.0;
        unpack_rgb(current_color_, r, g, b);
        send_color_to_strips(r * bf, g * bf, b * bf, 0.0);
    }
    update_current_color_swatch();
}

void LedControlOverlay::send_color_to_strips(double r, double g, double b, double w) {
    auto& controller = LedController::instance();
    if (!controller.native().is_available())
        return;

    for (const auto& strip_id : native_target_strips()) {
        controller.native().set_color(strip_id, r, g, b, w);
    }
}

void LedControlOverlay::update_brightness_text(int brightness) {
    snprintf(brightness_text_buf_, sizeof(brightness_text_buf_), "%d%%", brightness);
    lv_subject_copy_string(&brightness_text_subject_, brightness_text_buf_);
}

void LedControlOverlay::update_current_color_swatch() {
    if (!current_color_swatch_)
        return;

    // Show the actual output color (base color × brightness)
    double bf = static_cast<double>(current_brightness_) / 100.0;
    double r = 0.0, g = 0.0, b = 0.0;
    unpack_rgb(current_color_, r, g, b);
    lv_obj_set_style_bg_color(
        current_color_swatch_,
        lv_color_make(to_channel_byte(r * bf), to_channel_byte(g * bf), to_channel_byte(b * bf)),
        0);
}

void LedControlOverlay::update_wled_brightness_text(int brightness) {
    snprintf(wled_brightness_text_buf_, sizeof(wled_brightness_text_buf_), "%d%%", brightness);
    lv_subject_copy_string(&wled_brightness_text_subject_, wled_brightness_text_buf_);
}

void LedControlOverlay::add_macro_chip(const std::string& label, const std::string& data,
                                       MacroClickHandler handler) {
    const char* attrs[] = {"label", label.c_str(), nullptr};
    auto* chip =
        static_cast<lv_obj_t*>(lv_xml_create(macro_buttons_container_, "led_action_chip", attrs));
    if (!chip)
        return;

    // Pack handler + data together for the callback
    struct ChipCallbackData {
        std::string value;
        MacroClickHandler handler;
    };
    auto* cb_data = new ChipCallbackData{data, handler};
    lv_obj_set_user_data(chip, cb_data);

    lv_obj_add_event_cb(
        chip,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] macro_cb");
            auto* d = static_cast<ChipCallbackData*>(lv_event_get_user_data(e));
            if (d)
                (get_led_control_overlay().*(d->handler))(d->value);
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, cb_data);

    lv_obj_add_event_cb(
        chip,
        [](lv_event_t* e) { delete static_cast<ChipCallbackData*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, cb_data);
}

void LedControlOverlay::refresh_wled_status() {
    auto& controller = LedController::instance();
    if (!controller.is_initialized() || selected_backend_type_ != LedBackendType::WLED)
        return;

    auto tok = lifetime_.token();
    controller.wled().poll_status([this, tok]() {
        if (tok.expired())
            return;
        // poll_status fires on the BG thread — tok.defer marshals to the main
        // thread (#80) and safe_clean_children schedules child deletion via
        // lv_obj_delete_async, which runs on LVGL's own async list OUTSIDE
        // UpdateQueue::process_pending() — preventing lv_event_mark_deleted
        // corruption (#776).
        tok.defer([this]() {
            if (cleanup_called())
                return;
            if (wled_presets_container_) {
                lv_obj_update_layout(wled_presets_container_);
                helix::ui::safe_clean_children(wled_presets_container_);
                populate_wled();
            }
            update_wled_toggle_button();
        });
    });
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void LedControlOverlay::on_custom_color_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] custom_color_cb");
    (void)e;
    get_led_control_overlay().handle_custom_color();
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlOverlay::on_native_turn_off_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] native_turn_off_cb");
    (void)e;
    get_led_control_overlay().handle_native_turn_off();
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlOverlay::on_wled_toggle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] wled_toggle_cb");
    (void)e;
    get_led_control_overlay().handle_wled_toggle();
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlOverlay::on_color_preset_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] color_preset_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* data = static_cast<uint32_t*>(lv_obj_get_user_data(target));
    if (data)
        get_led_control_overlay().handle_color_preset(*data);
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlOverlay::on_brightness_changed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlOverlay] brightness_changed_cb");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = lv_slider_get_value(slider);
    auto& overlay = get_led_control_overlay();
    overlay.handle_brightness_change(value);
    LVGL_SAFE_EVENT_CB_END();
}
