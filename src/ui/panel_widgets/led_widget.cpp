// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_widget.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_tile_rung.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "core/observer_factory.h"
#include "display_settings_manager.h"
#include "grid_layout.h"
#include "helix/xml/indexed_subject_pool.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "light_button_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "text_io.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

void register_led_widget() {
    register_widget_factory("led", [](const std::string& id) {
        auto& ps = get_printer_state();
        auto* api = PanelWidgetManager::instance().shared_resource<IMoonrakerAPI>();
        return std::make_unique<LedWidget>(id, ps, api);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "light_toggle_cb", LedWidget::light_toggle_cb);
    lv_xml_register_event_cb(nullptr, "light_more_cb", LedWidget::light_more_cb);
    lv_xml_register_event_cb(nullptr, "led_picker_row_cb", LedWidget::led_picker_row_cb);
}

bool light_tile_is_wide(int colspan) {
    return colspan >= 2 * GridLayout::TRACKS_PER_CELL;
}

int light_chevron_reserve_px() {
    const ui::TileFace widest = ui::tile_rung_face(ui::TileLadder::Icon, kTileRungs - 2);
    const int glyph =
        widest.font ? widest.px(static_cast<int>(lv_font_get_line_height(widest.font))) : 0;
    return std::max(static_cast<int>(theme_manager_get_spacing("button_height")), glyph);
}

std::vector<led::LedStripInfo> light_picker_devices() {
    return led::LedController::instance().all_selectable_strips();
}

LightIconLook light_icon_look(const std::vector<led::DeviceState>& states) {
    LightIconLook look;
    for (const auto& s : states) {
        if (s.power == led::PowerState::Unknown) {
            continue;
        }
        look.unknown = false;
        if (s.power != led::PowerState::On) {
            continue;
        }
        look.brightness = std::max(look.brightness, std::max(s.brightness, 1));
        if (s.has_rgb && !look.has_rgb) {
            look.has_rgb = true;
            look.rgb = s.rgb;
        }
    }
    return look;
}

namespace {

/// The picker's rows. One picker is open at a time, so these are process-wide.
struct PickerSubjects {
    xml::IndexedSubjectPool names{"led_picker_name", xml::IndexedSubjectPool::Type::String};
    lv_subject_t count{};
    lv_subject_t selected{};
    bool ready = false;
};

PickerSubjects& picker_subjects() {
    static PickerSubjects s;
    if (!s.ready) {
        lv_subject_init_int(&s.count, 0);
        lv_xml_register_subject(nullptr, "led_picker_count", &s.count);
        lv_subject_init_int(&s.selected, -1);
        lv_xml_register_subject(nullptr, "led_picker_selected", &s.selected);
        s.ready = true;
        StaticSubjectRegistry::instance().register_deinit("LedPicker", []() {
            if (s.ready) {
                lv_xml_unregister_subject(nullptr, "led_picker_count");
                lv_xml_unregister_subject(nullptr, "led_picker_selected");
                lv_subject_deinit(&s.count);
                lv_subject_deinit(&s.selected);
                s.names.reclaim();
                s.ready = false;
            }
        });
    }
    return s;
}

} // namespace

// Device state and commands all go through LedController, so the printer state
// and API are accepted and not kept.
LedWidget::LedWidget(const std::string& instance_id, PrinterState& /*printer_state*/,
                     IMoonrakerAPI* /*api*/)
    : TiledPanelWidget(instance_id, TileSizing::Content{"", "", "Light", false}),
      instance_id_(instance_id), name_subject_name_(instance_id + "_led_name"),
      wide_subject_name_(instance_id + "_led_wide") {
    UI_MANAGED_SUBJECT_STRING(name_subject_, name_buf_, "", name_subject_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(wide_subject_, 0, wide_subject_name_.c_str(), subjects_);

    for (const char** a = sizing_.subject_attrs(); *a != nullptr; ++a) {
        attr_storage_.emplace_back(*a);
    }
    attr_storage_.insert(attr_storage_.end(),
                         {"name_subject", name_subject_name_, "wide_subject", wide_subject_name_});
    for (const auto& s : attr_storage_) {
        attrs_.push_back(s.c_str());
    }
    attrs_.push_back(nullptr);
}

LedWidget::~LedWidget() {
    detach();
}

std::unordered_set<LedWidget*>& LedWidget::live_instances() {
    static std::unordered_set<LedWidget*> instances;
    return instances;
}

LedWidget* LedWidget::from_event(lv_event_t* e) {
    for (auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e)); obj != nullptr;
         obj = lv_obj_get_parent(obj)) {
        auto* candidate = static_cast<LedWidget*>(lv_obj_get_user_data(obj));
        if (candidate != nullptr && live_instances().count(candidate) != 0) {
            return candidate;
        }
    }
    return nullptr;
}

void LedWidget::set_config(const nlohmann::json& config) {
    led_key_ = json_util::safe_string(config, "led");
}

std::vector<std::string> LedWidget::targets() const {
    return led::LedController::instance().light_targets(led_key_);
}

std::string LedWidget::overlay_device() const {
    if (led_key_ == led::kLightButtonAll) {
        return led::LedController::instance().chamber_light();
    }
    const auto t = targets();
    return t.empty() ? std::string() : t.front();
}

void LedWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    (void)rowspan;
    // A wide tile gives its › zone light_chevron_reserve_px(), so the bulb and
    // its name draw in what is left.
    const bool wide = light_tile_is_wide(colspan);
    const int chevron_w = wide ? light_chevron_reserve_px() : 0;
    sizing_.measure_and_publish(width_px - chevron_w, height_px);
    lv_subject_set_int(&wide_subject_, wide ? 1 : 0);
}

void LedWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (!widget_obj_) {
        return;
    }

    live_instances().insert(this);

    light_icon_ = lv_obj_find_by_name(widget_obj_, "light_icon");

    // Rebind when discovery or the macro devices change the set of lights.
    auto token = lifetime_.token();
    auto& led_ctrl = led::LedController::instance();
    led_version_observer_ = helix::ui::observe<int>(
        led_ctrl.get_led_config_version_subject(), this,
        [token](LedWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->bind_led();
        },
        led_ctrl.get_subjects_lifetime());
    // observe<int> delivers through queue_update, so it can fire a tick
    // late; the tile shows its light's name and state from the first frame.
    led_state_observer_ = helix::ui::observe<int>(
        led_ctrl.get_led_state_version_subject(), this,
        [token](LedWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->update_light_icon();
        },
        led_ctrl.get_subjects_lifetime());

    bind_led();

    spdlog::debug("[LedWidget] Attached {} (led: '{}')", instance_id_, led_key_);
}

void LedWidget::detach() {
    lifetime_.invalidate();
    picker_.hide();
    live_instances().erase(this);

    // Nullify widget pointers BEFORE resetting observers
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    light_icon_ = nullptr;

    led_version_observer_.reset();
    led_state_observer_.reset();

    spdlog::debug("[LedWidget] Detached");
}

void LedWidget::bind_led() {
    if (led_key_.empty() && !panel_id().empty()) {
        auto* cfg = Config::get_instance();
        // A sibling tile's bind may already have written this one's value.
        auto& home = PanelWidgetManager::instance().get_widget_config(panel_id());
        adopt_pending_light_button(*cfg, home);
        led_key_ = json_util::safe_string(home.get_widget_config(id()), "led");
    }

    auto& led_ctrl = led::LedController::instance();
    std::string name;
    if (led_key_ == led::kLightButtonAll) {
        name = lv_tr("All lights");
    } else {
        const auto t = targets();
        if (!t.empty()) {
            for (const auto& d : led_ctrl.all_selectable_strips()) {
                if (d.id == t.front()) {
                    name = led::device_display_name(d);
                    break;
                }
            }
        }
    }
    lv_subject_copy_string(&name_subject_, name.c_str());
    sizing_.set_content({"", "", name.empty() ? std::string("Light") : name, false});
    relayout_for_granted_size();

    update_light_icon();
}

void LedWidget::select_light(const std::string& key) {
    led_key_ = key;
    save_widget_config({{"led", key}});
    bind_led();
    spdlog::info("[LedWidget] {} now controls '{}'", instance_id_, key);
}

void LedWidget::handle_light_toggle() {
    if (toggle_lights(targets(), instance_id_.c_str()).unconfirmed) {
        flash_light_icon(light_icon_);
    }
}

LightIconLook LedWidget::icon_look() const {
    auto& led_ctrl = led::LedController::instance();
    std::vector<led::DeviceState> states;
    for (const auto& id : targets()) {
        states.push_back(led_ctrl.device_state(id));
    }
    LightIconLook look = light_icon_look(states);
    if (led_key_ == led::kLightButtonAll) {
        look.has_rgb = false;
    }
    return look;
}

void LedWidget::update_light_icon() {
    if (!light_icon_) {
        return;
    }

    const LightIconLook look = icon_look();

    const char* icon_name = ui_brightness_to_lightbulb_icon(look.brightness);
    helix::ui::icon::set_source(light_icon_, icon_name);

    lv_color_t icon_color;
    if (look.brightness == 0) {
        icon_color = theme_manager_get_color("light_icon_off");
    } else if (!look.has_rgb) {
        icon_color = theme_manager_get_color("light_icon_on");
    } else {
        const uint8_t r = (look.rgb >> 16) & 0xFF;
        const uint8_t g = (look.rgb >> 8) & 0xFF;
        const uint8_t b = look.rgb & 0xFF;
        // A white light reads as the theme's lamp color; a bare white glyph
        // disappears on a light theme.
        icon_color = (r > 200 && g > 200 && b > 200) ? theme_manager_get_color("light_icon_on")
                                                     : lv_color_make(r, g, b);
    }
    helix::ui::icon::set_color(light_icon_, icon_color, LV_OPA_COVER);

    spdlog::trace("[LedWidget] Light icon: {} at {}%{}", icon_name, look.brightness,
                  look.unknown ? " (unknown)" : "");
}

namespace {

void flash_opa_cb(void* obj, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
}

/// The icon's text color before a flash, packed for lv_anim user data: bit 24
/// set means a local color to put back, clear means the icon's styles own it.
constexpr uintptr_t kFlashHadLocalColor = uintptr_t{1} << 24;

} // namespace

void flash_light_icon(lv_obj_t* icon) {
    if (!icon)
        return;

    // A flash already running keeps the color it will restore; reading the icon
    // now would capture the flash's own lamp color.
    uintptr_t resting = 0;
    if (lv_anim_t* running = lv_anim_get(icon, flash_opa_cb)) {
        resting = reinterpret_cast<uintptr_t>(lv_anim_get_user_data(running));
        lv_anim_delete(icon, flash_opa_cb);
    } else {
        lv_style_value_t v;
        if (lv_obj_get_local_style_prop(icon, LV_STYLE_TEXT_COLOR, &v, LV_PART_MAIN) ==
            LV_STYLE_RES_FOUND) {
            resting = kFlashHadLocalColor | (lv_color_to_u32(v.color) & 0xFFFFFF);
        }
    }

    helix::ui::icon::set_color(icon, theme_manager_get_color("light_icon_on"), LV_OPA_COVER);

    // With animations off the lamp color holds for the same time, unfaded.
    const bool fade = DisplaySettingsManager::instance().get_animations_enabled();
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, icon);
    lv_anim_set_values(&anim, LV_OPA_COVER, fade ? LV_OPA_TRANSP : LV_OPA_COVER);
    lv_anim_set_duration(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, flash_opa_cb);
    lv_anim_set_user_data(&anim, reinterpret_cast<void*>(resting));
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* obj = static_cast<lv_obj_t*>(a->var);
        const auto packed = reinterpret_cast<uintptr_t>(lv_anim_get_user_data(a));
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
        if (packed & kFlashHadLocalColor) {
            lv_obj_set_style_text_color(obj, lv_color_hex(packed & 0xFFFFFF), LV_PART_MAIN);
        } else {
            lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
        }
    });
    lv_anim_start(&anim);

    spdlog::debug("[LightButtons] Flash light icon (state unknown)");
}

bool LedWidget::on_edit_configure() {
    show_led_picker();
    return false; // no rebuild — the tile rebinds in place
}

void LedWidget::show_led_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    auto& subs = picker_subjects();
    const auto devices = light_picker_devices();
    subs.names.ensure_size(devices.size());
    int selected = -1;
    const auto t = targets();
    picker_.row_ids.clear();
    for (size_t i = 0; i < devices.size(); ++i) {
        picker_.row_ids.push_back(devices[i].id);
        subs.names.set_string(i, led::device_display_name(devices[i]));
        if (led_key_ != led::kLightButtonAll && !t.empty() && devices[i].id == t.front()) {
            selected = static_cast<int>(i);
        }
    }
    lv_subject_set_int(&subs.selected, selected);
    lv_subject_set_int(&subs.count, static_cast<int>(devices.size()));

    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Center);
}

void LedWidget::LedPicker::on_created(lv_obj_t* menu_obj) {
    // DECLARATIVE_OK: measured cap. A share of the screen, so a printer with a
    // dozen lights scrolls the list instead of growing the card past the panel.
    if (lv_obj_t* list = lv_obj_find_by_name(menu_obj, "led_picker_list")) {
        lv_obj_set_style_max_height(list, screen_height_pct(66), 0);
    }
}

void LedWidget::light_toggle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedWidget] light_toggle_cb");
    if (auto* self = from_event(e)) {
        self->record_interaction();
        self->handle_light_toggle();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LedWidget::light_more_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedWidget] light_more_cb");
    if (auto* self = from_event(e)) {
        self->record_interaction();
        open_led_control_overlay(self->parent_screen_, self->overlay_device());
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LedWidget::led_picker_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedWidget] led_picker_row_cb");
    auto* picker = helix::ui::ContextMenu::active_as<LedPicker>();
    const char* ud = static_cast<const char*>(lv_event_get_user_data(e));
    if (picker == nullptr || ud == nullptr) {
        return;
    }
    const int index = helix::text_io::parse_leading<int>(ud).value_or(-1);
    std::string key = led::kLightButtonAll;
    if (index >= 0 && static_cast<size_t>(index) < picker->row_ids.size()) {
        key = picker->row_ids[static_cast<size_t>(index)];
    }
    LedWidget& owner = picker->owner();
    picker->hide();
    owner.select_light(key);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
