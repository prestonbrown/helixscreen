// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/ui_led_control_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_event_safety.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_color_utils.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

using namespace helix;
using namespace helix::led;

namespace helix {
lv_obj_t* open_led_control_overlay(lv_obj_t* parent_screen, const std::string& device_id) {
    auto& overlay = get_led_control_overlay();
    overlay.request_focus(device_id);
    if (!overlay.show(parent_screen)) {
        overlay.request_focus("");
        return nullptr;
    }
    return overlay.get_root();
}
} // namespace helix

namespace {

lv_color_t light_color() {
    return theme_manager_get_color("light_icon_on");
}

} // namespace

// ============================================================================
// OVERLAYBASE IMPLEMENTATION
// ============================================================================

void LedControlOverlay::init_subjects() {
    {
        UI_MANAGED_SUBJECT_INT(tab_count_, 0, "led_tab_count", subjects_);
        UI_MANAGED_SUBJECT_INT(focused_tab_, 0, "led_focused_tab", subjects_);
        UI_MANAGED_SUBJECT_INT(tabs_fade_, 0, "led_tabs_fade", subjects_);

        UI_MANAGED_SUBJECT_INT(page_lamp_, static_cast<int>(LampControl::None), "led_page_lamp",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(page_white_, 0, "led_page_white", subjects_);
        UI_MANAGED_SUBJECT_INT(page_color_vis_, 0, "led_page_color_vis", subjects_);
        UI_MANAGED_SUBJECT_INT(page_list_, 0, "led_page_list", subjects_);

        UI_MANAGED_SUBJECT_INT(page_on_, static_cast<int>(PowerState::Unknown), "led_page_on",
                               subjects_);
        UI_MANAGED_SUBJECT_COLOR(page_color_, light_color(), "led_page_color", subjects_);
        UI_MANAGED_SUBJECT_COLOR(page_fill_text_, theme_manager_get_contrast_color(light_color()),
                                 "led_page_fill_text", subjects_);
        UI_MANAGED_SUBJECT_INT(page_brightness_, 100, "led_page_brightness", subjects_);
        UI_MANAGED_SUBJECT_STRING(page_brightness_text_, page_brightness_text_buf_, "100%",
                                  "led_page_brightness_text", subjects_);
        UI_MANAGED_SUBJECT_INT(page_white_sel_, -1, "led_page_white_sel", subjects_);
        UI_MANAGED_SUBJECT_INT(swatch_count_, 0, "led_swatch_count", subjects_);
        UI_MANAGED_SUBJECT_INT(selected_swatch_, -1, "led_selected_swatch", subjects_);
        UI_MANAGED_SUBJECT_STRING(page_list_title_, page_list_title_buf_, "", "led_page_list_title",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(chip_count_, 0, "led_chip_count", subjects_);
        UI_MANAGED_SUBJECT_INT(active_chip_, -2, "led_active_chip", subjects_);
        UI_MANAGED_SUBJECT_INT(page_level_, 0, "led_page_level", subjects_);
        UI_MANAGED_SUBJECT_STRING(page_note_, page_note_buf_, "", "led_page_note", subjects_);
    }
}

lv_obj_t* LedControlOverlay::create(lv_obj_t* parent) {
    // Every <repeat> starts empty: on_ui_destroyed() released the pools a stale
    // count would bind new rows to. on_activate() publishes the real counts.
    lv_subject_set_int(&tab_count_, 0);
    lv_subject_set_int(&swatch_count_, 0);
    lv_subject_set_int(&chip_count_, 0);
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    lv_obj_add_event_cb(overlay_root_, on_root_deleted, LV_EVENT_DELETE, nullptr);
    return overlay_root_;
}

void LedControlOverlay::register_callbacks() {
    register_xml_callbacks({
        {"led_tab_clicked_cb",
         [](lv_event_t* e) {
             get_led_control_overlay().handle_tab_clicked(
                 helix::ui::event_user_int(e).value_or(-1));
         }},
        {"led_tabs_scrolled_cb",
         [](lv_event_t* e) {
             auto* row = lv_event_get_current_target_obj(e);
             auto& overlay = get_led_control_overlay();
             if (row != nullptr && overlay.are_subjects_initialized()) {
                 lv_subject_set_int(&overlay.tabs_fade_, lv_obj_get_scroll_right(row) > 0 ? 1 : 0);
             }
         }},
        {"led_power_cb", [](lv_event_t*) { get_led_control_overlay().handle_power(); }},
        {"led_brightness_changed_cb",
         [](lv_event_t* e) {
             get_led_control_overlay().handle_brightness(
                 lv_slider_get_value(lv_event_get_target_obj(e)));
         }},
        {"led_level_cb",
         [](lv_event_t* e) {
             if (const int pct = helix::ui::event_user_int(e).value_or(-1); pct >= 0) {
                 get_led_control_overlay().handle_brightness(pct);
             }
         }},
        {"led_white_cb",
         [](lv_event_t* e) {
             get_led_control_overlay().handle_white(helix::ui::event_user_int(e).value_or(-1));
         }},
        {"led_swatch_cb",
         [](lv_event_t* e) {
             get_led_control_overlay().handle_swatch(helix::ui::event_user_int(e).value_or(-1));
         }},
        {"led_custom_color_cb",
         [](lv_event_t*) { get_led_control_overlay().handle_custom_color(); }},
        {"led_list_chip_cb",
         [](lv_event_t* e) {
             get_led_control_overlay().handle_list_chip(helix::ui::event_user_int(e).value_or(-1));
         }},
        {"led_effects_none_cb",
         [](lv_event_t*) { get_led_control_overlay().handle_effects_none(); }},
        {"led_macro_on_cb", [](lv_event_t*) { get_led_control_overlay().handle_macro_on(); }},
        {"led_macro_off_cb", [](lv_event_t*) { get_led_control_overlay().handle_macro_off(); }},
        {"led_macro_toggle_cb",
         [](lv_event_t*) { get_led_control_overlay().handle_macro_toggle(); }},
    });
}

void LedControlOverlay::request_focus(const std::string& device_id) {
    requested_focus_ = device_id;
}

void LedControlOverlay::on_activate() {
    OverlayBase::on_activate();

    auto& ctrl = LedController::instance();
    rebuild_tabs();

    std::vector<std::string> ids;
    ids.reserve(devices_.size());
    for (const auto& d : devices_) {
        ids.push_back(d.id);
    }
    focus_device(pick_overlay_focus(requested_focus_, last_focused_, ctrl.chamber_light(), ids));
    requested_focus_.clear();

    state_observer_ = helix::ui::observe<int>(
        ctrl.get_led_state_version_subject(), this,
        [](LedControlOverlay* self, int) { self->on_led_state_changed(); },
        ctrl.get_subjects_lifetime());
    // theme_changed is a file-static theme global, deinited only after LVGL is gone.
    theme_observer_ = helix::ui::observe<int>(
        theme_manager_get_changed_subject(), this,
        [](LedControlOverlay* self, int) { self->publish_swatch_edges(); }, subject_never_freed());

    refresh_wled_page();

    spdlog::debug("[{}] Activated on '{}' ({} devices)", get_name(), focused_strip_,
                  devices_.size());
}

void LedControlOverlay::on_deactivating(DeactivateReason) {
    state_observer_.reset();
    theme_observer_.reset();

    auto& ctrl = LedController::instance();
    const auto* info = focused_info();
    if (ctrl.is_initialized() && info && info->backend == LedBackendType::NATIVE) {
        ctrl.set_last_brightness(current_brightness_);
        ctrl.set_last_color(current_color_);
        ctrl.set_last_white(current_white_);
        ctrl.save_config();
    }

    spdlog::debug("[{}] Deactivated", get_name());
}

void LedControlOverlay::cleanup() {
    spdlog::debug("[{}] Cleanup", get_name());
    state_observer_.reset();
    theme_observer_.reset();
    deinit_subjects_base(subjects_);
    on_ui_destroyed();
    OverlayBase::cleanup();
}

void LedControlOverlay::on_ui_destroyed() {
    // The observers and the effect timeout went with on_deactivating() and
    // lifetime_; what is left bound to the freed tree is the row pools.
    tab_name_pool_.reclaim();
    tab_dot_pool_.reclaim();
    tab_dot_color_pool_.reclaim();
    swatch_color_pool_.reclaim();
    swatch_edge_pool_.reclaim();
    chip_label_pool_.reclaim();
}

void LedControlOverlay::on_root_deleted(lv_event_t* e) {
    // Resolved through the global, not user_data: a printer switch destroys the
    // overlay before freeing its tree, and the re-created overlay can be opened
    // on a new root before the old one is freed.
    auto* overlay = helix::lazy_global_if_exists<LedControlOverlay>();
    if (!overlay || overlay->overlay_root_ != lv_event_get_target_obj(e)) {
        return;
    }
    overlay->overlay_root_ = nullptr;
}

// ============================================================================
// TABS AND FOCUS
// ============================================================================

void LedControlOverlay::rebuild_tabs() {
    auto& ctrl = LedController::instance();
    devices_ = ctrl.is_initialized() ? ctrl.all_devices() : std::vector<LedStripInfo>{};

    const size_t n = devices_.size();
    tab_name_pool_.ensure_size(n);
    tab_dot_pool_.ensure_size(n);
    tab_dot_color_pool_.ensure_size(n);
    for (size_t i = 0; i < n; ++i) {
        tab_name_pool_.set_string(i, device_display_name(devices_[i]));
    }
    publish_tab_dots();
    lv_subject_set_int(&tab_count_, static_cast<int>(n));
}

void LedControlOverlay::publish_tab_dots() {
    auto& ctrl = LedController::instance();
    const uint32_t light = lv_color_to_u32(light_color()) & 0xFFFFFF;
    for (size_t i = 0; i < devices_.size(); ++i) {
        const DeviceState st = ctrl.device_state(devices_[i].id);
        tab_dot_pool_.set_int(i, static_cast<int>(st.power));
        tab_dot_color_pool_.set_color(i, st.has_rgb ? st.rgb : light);
    }
}

void LedControlOverlay::focus_device(const std::string& id) {
    focused_strip_ = id;
    pending_effect_chip_.reset();
    if (!id.empty()) {
        last_focused_ = id;
    }

    int index = 0;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id == id) {
            index = static_cast<int>(i);
            break;
        }
    }
    lv_subject_set_int(&focused_tab_, index);
    scroll_tab_into_view(index);

    load_page_state();
    publish_page();
}

void LedControlOverlay::scroll_tab_into_view(int index) {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "led_tab_row", get_name());
    lv_obj_t* tab =
        row ? lv_obj_find_by_name(row, fmt::format("led_tab_{}", index).c_str()) : nullptr;
    if (tab == nullptr) {
        return;
    }
    // DECLARATIVE_OK: scrolling has no XML form; an overlay opened on a device
    // past the edge of the tab row must bring that tab into sight.
    lv_obj_update_layout(row);
    lv_obj_scroll_to_view(tab, LV_ANIM_ON);
}

const LedStripInfo* LedControlOverlay::focused_info() const {
    return focused_strip_.empty() ? nullptr : find_strip(devices_, focused_strip_);
}

MacroLedType LedControlOverlay::focused_macro_type() const {
    const auto* m = find_macro(LedController::instance().configured_macros(), focused_strip_);
    return m != nullptr ? m->type : MacroLedType::TOGGLE;
}

void LedControlOverlay::load_page_state() {
    const auto* info = focused_info();
    if (info == nullptr) {
        return;
    }
    auto& ctrl = LedController::instance();

    if (info->backend != LedBackendType::NATIVE) {
        // HelixScreen sends these no color, so their page is a neutral white
        // rather than whatever the previously focused strip left behind.
        current_brightness_ = ctrl.device_state(focused_strip_).brightness;
        current_color_ = 0xFFFFFF;
        current_white_ = 0.0;
        return;
    }

    const auto c = ctrl.native().get_strip_color(focused_strip_);
    const bool lit = ctrl.native().has_strip_color(focused_strip_) &&
                     (c.r > 0.0 || c.g > 0.0 || c.b > 0.0 || c.w > 0.0);
    if (lit) {
        c.decompose(current_color_, current_brightness_, current_white_);
        if (c.r == 0.0 && c.g == 0.0 && c.b == 0.0) {
            current_color_ = 0; // pure W: no tint
        }
    } else {
        // Off or never read: the saved look, so turning it up restores it.
        current_brightness_ = ctrl.last_brightness();
        current_color_ = ctrl.last_color();
        current_white_ = ctrl.last_white();
    }
    // The saved look is controller-wide, and status can refine a strip's
    // channels after the tabs were built, so fit against the live strip.
    const auto* live = find_strip(ctrl.native().strips(), focused_strip_);
    const Look look = fit_look(current_color_, current_white_, live ? *live : *info);
    current_color_ = look.rgb;
    current_white_ = look.w;
}

// ============================================================================
// PAGE SUBJECTS
// ============================================================================

void LedControlOverlay::publish_page() {
    const auto* info = focused_info();
    auto& ctrl = LedController::instance();

    page_ = info == nullptr
                ? DevicePage{}
                : classify_device_page(*info, focused_macro_type(),
                                       !ctrl.effects().effects_for_strip(focused_strip_).empty());
    lv_subject_set_int(&page_lamp_, static_cast<int>(page_.lamp));
    lv_subject_set_int(&page_white_, static_cast<int>(page_.white));
    lv_subject_set_int(&page_color_vis_, page_.color ? 1 : 0);
    lv_subject_set_int(&page_list_, static_cast<int>(page_.list));

    lv_subject_set_int(&page_on_, static_cast<int>(ctrl.device_state(focused_strip_).power));

    const auto& presets = ctrl.color_presets();
    swatch_color_pool_.ensure_size(presets.size());
    for (size_t i = 0; i < presets.size(); ++i) {
        swatch_color_pool_.set_color(i, presets[i]);
    }
    publish_swatch_edges();
    lv_subject_set_int(&swatch_count_, static_cast<int>(presets.size()));

    publish_color_state();
    publish_list();

    // A PRESET device's chips already say what it runs.
    std::string note;
    if (info != nullptr && info->backend == LedBackendType::WLED) {
        note = lv_tr("Presets come from the WLED device. Edit them in the WLED app.");
    } else if (info != nullptr && info->backend == LedBackendType::MACRO) {
        const auto* m = find_macro(ctrl.configured_macros(), focused_strip_);
        if (m != nullptr && m->type != MacroLedType::PRESET) {
            note = macro_device_note(*m);
        }
    }
    lv_subject_copy_string(&page_note_, note.c_str());
}

void LedControlOverlay::publish_swatch_edges() {
    const auto& presets = LedController::instance().color_presets();
    swatch_edge_pool_.ensure_size(presets.size());
    for (size_t i = 0; i < presets.size(); ++i) {
        swatch_edge_pool_.set_int(i, helix::ui::swatch_needs_edge_here(presets[i]) ? 1 : 0);
    }
}

void LedControlOverlay::publish_color_state() {
    const auto* info = focused_info();
    lv_color_t color = light_color();
    if (info != nullptr && info->supports_color) {
        double r = 0.0, g = 0.0, b = 0.0;
        unpack_rgb(current_color_, r, g, b);
        color = lv_color_hex(output_rgb(r, g, b, current_white_));
    }
    lv_subject_set_color(&page_color_, color);
    lv_subject_set_color(&page_fill_text_, theme_manager_get_contrast_color(color));

    lv_subject_set_int(&page_brightness_, current_brightness_);
    const std::string text = fmt::format("{}%", current_brightness_);
    lv_subject_copy_string(&page_brightness_text_, text.c_str());

    const LookRing ring = page_.color ? ring_for_look(current_color_, current_white_, page_.white,
                                                      LedController::instance().color_presets())
                                      : LookRing{};
    lv_subject_set_int(&page_white_sel_, ring.white);
    lv_subject_set_int(&selected_swatch_, ring.swatch);

    const bool is_level = std::find(std::begin(LEVEL_CHIPS), std::end(LEVEL_CHIPS),
                                    current_brightness_) != std::end(LEVEL_CHIPS);
    lv_subject_set_int(&page_level_, is_level ? current_brightness_ : 0);
}

void LedControlOverlay::publish_list() {
    auto& ctrl = LedController::instance();
    std::vector<std::string> labels;
    list_values_.clear();
    int active = -2;
    const char* title = "";

    if (page_.list == ListKind::Effects) {
        title = lv_tr("Effects");
        active = active_effect_index();
        for (const auto& eff : ctrl.effects().effects_for_strip(focused_strip_)) {
            labels.push_back(eff.display_name);
            list_values_.push_back(eff.name);
        }
    } else if (page_.list == ListKind::Presets) {
        title = lv_tr("Presets");
        if (const auto* m = find_macro(ctrl.configured_macros(), focused_strip_)) {
            for (const auto& preset : m->presets) {
                labels.push_back(pretty_print_macro(preset));
                list_values_.push_back(preset);
            }
        }
    }

    chip_label_pool_.ensure_size(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        chip_label_pool_.set_string(i, labels[i]);
    }
    lv_subject_set_int(&chip_count_, static_cast<int>(labels.size()));
    lv_subject_set_int(&active_chip_, active);
    lv_subject_copy_string(&page_list_title_, title);
}

int LedControlOverlay::active_effect_index() const {
    const auto effects = LedController::instance().effects().effects_for_strip(focused_strip_);
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].enabled) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<bool> LedControlOverlay::focused_effects_enabled() const {
    std::vector<bool> enabled;
    for (const auto& eff : LedController::instance().effects().effects_for_strip(focused_strip_)) {
        enabled.push_back(eff.enabled);
    }
    return enabled;
}

void LedControlOverlay::set_pending_effect_chip(int chip) {
    pending_effect_chip_ = chip;
    pending_effects_snapshot_ = focused_effects_enabled();
    const unsigned gen = ++pending_effect_gen_;
    lv_subject_set_int(&active_chip_, chip);

    // A command Klipper drops never produces a frame, so the tapped chip gives
    // way to the real state after a bounded wait.
    struct Timeout {
        LifetimeToken tok;
        LedControlOverlay* self;
        unsigned gen;
    };
    auto* t = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* data = static_cast<Timeout*>(lv_timer_get_user_data(timer));
            data->tok.defer(
                "LedControlOverlay::effect_timeout",
                [self = data->self, gen = data->gen]() { self->end_pending_effect(gen); });
            delete data;
        },
        PENDING_EFFECT_TIMEOUT_MS,
        new Timeout{lifetime_.token(), this, gen}); // TIMER_DTOR_OK: LifetimeToken-guarded one-shot
    lv_timer_set_repeat_count(t, 1);
}

void LedControlOverlay::end_pending_effect(unsigned gen) {
    if (!pending_effect_chip_ || pending_effect_gen_ != gen) {
        return;
    }
    pending_effect_chip_.reset();
    if (page_.list == ListKind::Effects) {
        lv_subject_set_int(&active_chip_, active_effect_index());
    }
}

void LedControlOverlay::resolve_effect_chip() {
    if (!pending_effect_chip_) {
        lv_subject_set_int(&active_chip_, active_effect_index());
        return;
    }
    const auto enabled = focused_effects_enabled();
    if (enabled == pending_effects_snapshot_) {
        return; // this frame carried nothing about the focused strip's effects
    }
    const int pending = *pending_effect_chip_;
    pending_effect_chip_.reset();
    const bool confirmed = pending >= 0 && static_cast<size_t>(pending) < enabled.size() &&
                           enabled[static_cast<size_t>(pending)];
    lv_subject_set_int(&active_chip_, confirmed ? pending : active_effect_index());
}

void LedControlOverlay::on_led_state_changed() {
    auto& ctrl = LedController::instance();
    publish_tab_dots();

    const auto* info = focused_info();
    if (info == nullptr) {
        return;
    }
    lv_subject_set_int(&page_on_, static_cast<int>(ctrl.device_state(focused_strip_).power));
    if (page_.list == ListKind::Effects) {
        resolve_effect_chip();
    }

    if (info->backend == LedBackendType::NATIVE && info->supports_color &&
        ctrl.native().has_strip_color(focused_strip_)) {
        const auto c = ctrl.native().get_strip_color(focused_strip_);
        if (c.r > 0.0 || c.g > 0.0 || c.b > 0.0 || c.w > 0.0) {
            const lv_color_t color = lv_color_hex(output_rgb(c.r, c.g, c.b, c.w));
            lv_subject_set_color(&page_color_, color);
            lv_subject_set_color(&page_fill_text_, theme_manager_get_contrast_color(color));
        }
    }
}

// ============================================================================
// ACTION HANDLERS
// ============================================================================

void LedControlOverlay::handle_tab_clicked(int index) {
    if (index < 0 || static_cast<size_t>(index) >= devices_.size()) {
        return;
    }
    focus_device(devices_[index].id);
    refresh_wled_page();
}

void LedControlOverlay::refresh_wled_page() {
    const auto* info = focused_info();
    if (info == nullptr || info->backend != LedBackendType::WLED) {
        return;
    }
    // A WLED strip's state arrives by poll, so its page is read again when the
    // poll lands, unless the user has moved focus or touched a control since.
    // on_done runs on the main thread.
    auto tok = lifetime_.token();
    const std::string id = focused_strip_;
    const unsigned gen = page_gen_;
    LedController::instance().refresh_wled_state([this, tok, id, gen]() {
        if (tok.expired() || focused_strip_ != id || page_gen_ != gen) {
            return;
        }
        load_page_state();
        publish_color_state();
        publish_list();
    });
}

void LedControlOverlay::handle_power() {
    if (focused_strip_.empty()) {
        return;
    }
    auto& ctrl = LedController::instance();
    ctrl.set_power({focused_strip_}, ctrl.device_state(focused_strip_).power != PowerState::On);
}

void LedControlOverlay::handle_brightness(int pct) {
    const auto* info = focused_info();
    if (info == nullptr) {
        return;
    }
    ++page_gen_;
    current_brightness_ = std::clamp(pct, 0, 100);
    auto& ctrl = LedController::instance();
    switch (info->backend) {
    case LedBackendType::NATIVE:
        apply_current_color();
        break;
    case LedBackendType::OUTPUT_PIN:
        ctrl.output_pin().set_brightness(focused_strip_, current_brightness_);
        break;
    case LedBackendType::WLED:
        ctrl.wled().set_brightness(focused_strip_, current_brightness_);
        break;
    case LedBackendType::MACRO:
    case LedBackendType::LED_EFFECT:
        return;
    }
    publish_color_state();
}

void LedControlOverlay::handle_white(int tone) {
    const auto* info = focused_info();
    if (info == nullptr || info->backend != LedBackendType::NATIVE || tone < 0 || tone > 2 ||
        page_.white == WhiteMode::None) {
        return;
    }
    const Rgbw c = white_tone(static_cast<WhiteTone>(tone), page_.white);
    if (current_brightness_ <= 0) {
        current_brightness_ = 100;
    }
    apply_look(pack_rgb(c.r, c.g, c.b), c.w);
}

void LedControlOverlay::handle_swatch(int index) {
    const auto& presets = LedController::instance().color_presets();
    if (index < 0 || static_cast<size_t>(index) >= presets.size()) {
        return;
    }
    apply_swatch_color(presets[index]);
}

void LedControlOverlay::apply_swatch_color(uint32_t rgb) {
    const auto* info = focused_info();
    if (info == nullptr || info->backend != LedBackendType::NATIVE) {
        return;
    }
    if (current_brightness_ <= 0 ||
        LedController::instance().device_state(focused_strip_).power != PowerState::On) {
        current_brightness_ = 100;
    }
    apply_look(rgb, 0.0);
}

void LedControlOverlay::apply_look(uint32_t rgb, double w) {
    ++page_gen_;
    current_color_ = rgb;
    current_white_ = w;
    apply_current_color();
    publish_color_state();
}

void LedControlOverlay::handle_custom_color() {
    if (focused_info() == nullptr || !overlay_root_) {
        return;
    }
    static helix::ui::ColorPicker color_picker;
    color_picker.set_color_callback([this](uint32_t rgb, const std::string& name) {
        spdlog::info("[{}] Custom color selected: 0x{:06X} ({})", get_name(), rgb, name);
        // The picked color splits into a full-brightness base and a brightness,
        // the same decomposition the strip cache goes through.
        NativeBackend::StripColor picked;
        unpack_rgb(rgb, picked.r, picked.g, picked.b);
        uint32_t base = 0;
        int brightness = 0;
        double white = 0.0;
        picked.decompose(base, brightness, white);
        const auto* info = focused_info();
        if (info == nullptr || info->backend != LedBackendType::NATIVE) {
            return;
        }
        current_brightness_ = std::max(brightness, 1);
        apply_look(base, 0.0);
    });
    color_picker.show_with_color(lv_obj_get_parent(overlay_root_), current_color_);
}

void LedControlOverlay::handle_list_chip(int index) {
    if (focused_strip_.empty() || index < 0 || static_cast<size_t>(index) >= list_values_.size()) {
        return;
    }
    auto& ctrl = LedController::instance();
    const std::string& value = list_values_[index];
    const auto* info = focused_info();
    ++page_gen_;

    if (page_.list == ListKind::Effects) {
        ctrl.effects().activate_effect(
            value, nullptr,
            // Log-only handler: the user is told nothing here, so the report stays
            // with GcodeErrorRouter's `!!` broadcast (include/rpc_error_policy.h).
            [](const std::string& err) {
                spdlog::error("[LedControlOverlay] Effect activation failed: {}", err);
            },
            /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
    } else if (info != nullptr && info->backend == LedBackendType::MACRO) {
        ctrl.macro().execute_custom_action(value);
    } else {
        return;
    }
    if (page_.list == ListKind::Effects) {
        set_pending_effect_chip(index);
    } else {
        lv_subject_set_int(&active_chip_, index);
    }
}

void LedControlOverlay::stop_focused_effects() {
    auto& effects = LedController::instance().effects();
    for (const auto& eff : effects.effects_for_strip(focused_strip_)) {
        if (eff.enabled) {
            effects.stop_effect(eff.name);
        }
    }
}

void LedControlOverlay::handle_effects_none() {
    if (focused_strip_.empty() || page_.list != ListKind::Effects) {
        return;
    }
    stop_focused_effects();
    set_pending_effect_chip(-1);
}

// Through set_power so a light button toggles from what these buttons last sent.
void LedControlOverlay::handle_macro_on() {
    if (focused_info() != nullptr) {
        LedController::instance().set_power({focused_strip_}, true);
    }
}

void LedControlOverlay::handle_macro_off() {
    if (focused_info() != nullptr) {
        LedController::instance().set_power({focused_strip_}, false);
    }
}

void LedControlOverlay::handle_macro_toggle() {
    if (focused_info() != nullptr) {
        LedController::instance().toggle_power({focused_strip_});
    }
}

void LedControlOverlay::apply_current_color() {
    auto& ctrl = LedController::instance();
    // led_effect keeps writing its own frames over a manual color.
    stop_focused_effects();
    if (page_.list == ListKind::Effects) {
        set_pending_effect_chip(-1);
    }

    const double bf = static_cast<double>(current_brightness_) / 100.0;
    double r = 0.0, g = 0.0, b = 0.0;
    unpack_rgb(current_color_, r, g, b);
    ctrl.native().set_color(focused_strip_, r * bf, g * bf, b * bf, current_white_ * bf);
}
