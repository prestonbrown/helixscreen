// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermistor_widget.h"

#include "ui_carousel.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <set>

namespace helix {
void register_thermistor_widget() {
    register_widget_factory(
        "thermistor", [](const std::string& id) { return std::make_unique<ThermistorWidget>(id); });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "thermistor_clicked_cb",
                             ThermistorWidget::thermistor_clicked_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::deci_to_degrees_f;
using helix::ui::temperature::format_temperature_f;

/// Strip redundant " Temperature" suffix — the widget context already implies it
static void strip_temperature_suffix(std::string& name) {
    const std::string suffix = " Temperature";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
    }
}

namespace {

// Thermistor-related icons for the picker grid
static const char* const THERMISTOR_ICONS[] = {
    // clang-format off
    "thermometer", "thermometer_lines", "thermometer_plus", "thermometer_minus",
    "thermometer_probe", "thermometer_off",
    "coolant_temperature", "home_thermometer",
    "heat_wave", "heater", "radiator", "cooldown",
    // clang-format on
};
static constexpr size_t THERMISTOR_ICON_COUNT = std::size(THERMISTOR_ICONS);
static constexpr int ICON_CELL_SIZE = 36;
static constexpr const char* DEFAULT_ICON = "thermometer";

// Icons with distinct on/off glyphs. Config stores the ON variant;
// resolve_icon_for_state() derives the OFF variant from this table.
struct IconPair {
    const char* on_icon;
    const char* off_icon;
};
static const IconPair ICON_PAIRS[] = {
    {"thermometer", "thermometer_off"},
};

/// Map an off-variant icon name to its on-variant (e.g., "thermometer_off" -> "thermometer").
/// Returns the input unchanged if it's not an off-variant.
static const char* to_on_variant(const char* icon) {
    for (const auto& pair : ICON_PAIRS) {
        if (std::strcmp(icon, pair.off_icon) == 0)
            return pair.on_icon;
    }
    return icon;
}

/// Apply highlight styling to an icon grid cell.
void apply_icon_cell_highlight(lv_obj_t* cell, bool selected) {
    if (selected) {
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(cell, 20, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("primary"), 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, 0, 0);
    }
}

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// Create a sensor row in a picker list. Returns the row object.
/// @param is_multi  If true, adds a checkbox; if false, adds highlight styling.
lv_obj_t* create_sensor_row(lv_obj_t* list, const std::string& display_name,
                            const std::string& klipper_name, bool is_selected, bool is_multi,
                            int space_sm, int space_xs) {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();

    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, space_sm, 0);
    lv_obj_set_style_pad_gap(row, space_xs, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (is_multi) {
        // Checkbox for multi-select
        lv_obj_t* cb = lv_checkbox_create(row);
        lv_checkbox_set_text(cb, "");
        lv_obj_set_style_pad_all(cb, 0, 0);
        if (is_selected) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // Highlight for single-select
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);
    }

    // Sensor display name
    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, display_name.c_str());
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, lv_font_get_default(), 0);

    // Current temperature
    auto state = tsm.get_sensor_state(klipper_name);
    char temp_buf[16];
    if (state && state->available) {
        helix::ui::temperature::format_temperature(static_cast<int>(state->temperature), temp_buf,
                                                   sizeof(temp_buf));
    } else {
        std::strcpy(temp_buf, "--\xC2\xB0"
                              "C");
    }
    lv_obj_t* temp = lv_label_create(row);
    lv_label_set_text(temp, temp_buf);
    lv_obj_set_style_text_font(temp, lv_font_get_default(), 0);
    lv_obj_set_style_text_opa(temp, 180, 0);

    return row;
}

} // anonymous namespace

ThermistorWidget::ThermistorWidget(const std::string& instance_id) : instance_id_(instance_id) {
    std::strcpy(temp_buffer_, "--\xC2\xB0"
                              "C"); // "--°C"
}

ThermistorWidget::~ThermistorWidget() {
    detach();
}

void ThermistorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);
    }

    if (is_carousel_mode()) {
        attach_carousel();
    } else {
        attach_single();
    }
}

void ThermistorWidget::attach_single() {
    // Cache label pointers
    temp_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_temp");
    name_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_name");

    // Apply custom icon
    lv_obj_t* icon_obj = lv_obj_find_by_name(widget_obj_, "thermistor_icon");
    if (icon_obj) {
        const char* icon = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        ui_icon_set_source(icon_obj, icon);
    }

    // If no sensor saved (set_config provided none), auto-select first available
    if (selected_sensor_.empty()) {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        auto sensors = tsm.get_sensors_sorted();
        if (!sensors.empty()) {
            select_sensor(sensors.front().klipper_name);
        }
    } else {
        // Re-bind observer to saved sensor
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        temp_lifetime_.reset();
        temp_observer_.reset();
        lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_, temp_lifetime_);
        if (subject) {
            auto token = lifetime_.token();
            temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
                subject, this,
                [token](ThermistorWidget* self, int temp) {
                    if (token.expired())
                        return;
                    self->on_temp_changed(temp);
                },
                temp_lifetime_);
        }
        update_display();
    }

    spdlog::debug("[ThermistorWidget] Attached single (sensor: {})",
                  selected_sensor_.empty() ? "none" : selected_sensor_);
}

void ThermistorWidget::attach_carousel() {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "thermistor_carousel");
    if (!carousel) {
        spdlog::error("[ThermistorWidget] Could not find thermistor_carousel in XML");
        return;
    }

    // Observe sensor count to rebind when sensors are discovered
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto token = lifetime_.token();
    version_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
        tsm.get_sensor_count_subject(), this, [token](ThermistorWidget* self, int /*count*/) {
            if (token.expired())
                return;
            self->bind_carousel_sensors();
        });

    // Bind immediately (deferred observer fire may be dropped during populate_widgets freeze)
    bind_carousel_sensors();

    spdlog::debug("[ThermistorWidget] Attached carousel ({} sensors)", sensors_.size());
}

void ThermistorWidget::bind_carousel_sensors() {
    if (!widget_obj_)
        return;

    // Reentrancy guard: drain() can process a queued version_observer callback
    // that recursively calls bind_carousel_sensors(). Without this guard, the
    // recursive call creates pages that the outer call then destroys via
    // lv_obj_clean(), leaving stale temp_label pointers in carousel_pages_.
    if (binding_in_progress_)
        return;
    binding_in_progress_ = true;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "thermistor_carousel");
    if (!carousel) {
        binding_in_progress_ = false;
        return;
    }

    // Freeze update queue while tearing down observers and widgets.
    // Lifetimes must clear BEFORE observers so weak_ptr-tracked subject-death
    // detection in ObserverGuard::reset() correctly skips lv_observer_remove()
    // on a recreated subject (#705 ordering rule).
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        carousel_lifetimes_.clear();
        carousel_observers_.clear();
        carousel_pages_.clear();

        auto* state_ptr = ui_carousel_get_state(carousel);
        if (state_ptr && state_ptr->scroll_container) {
            helix::ui::UpdateQueue::instance().drain();
            helix::ui::safe_clean_children(state_ptr->scroll_container);
            state_ptr->real_tiles.clear();
            ui_carousel_rebuild_indicators(carousel);
        }
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();

    // Determine which sensors to show
    std::vector<std::string> display_sensors;
    if (sensors_.empty()) {
        // Show all available sensors
        for (const auto& s : tsm.get_sensors_sorted()) {
            display_sensors.push_back(s.klipper_name);
        }
    } else {
        display_sensors = sensors_;
    }

    if (display_sensors.empty()) {
        spdlog::debug("[ThermistorWidget] Carousel: no sensors available");
        return;
    }

    auto token = lifetime_.token();
    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    lv_color_t text_muted = theme_manager_get_color("text_muted");

    for (const auto& klipper_name : display_sensors) {
        // Resolve display name
        std::string display_name = klipper_name;
        for (const auto& s : tsm.get_sensors_sorted()) {
            if (s.klipper_name == klipper_name) {
                display_name = s.display_name;
                break;
            }
        }
        strip_temperature_suffix(display_name);

        // Create page: column layout with icon + temp + name
        lv_obj_t* page = lv_obj_create(lv_scr_act());
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_pad_gap(page, 0, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

        // Thermometer icon (use custom icon if configured)
        const char* icon_src = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        const char* icon_attrs[] = {"src", icon_src, "size", "sm", "variant", "secondary", nullptr};
        lv_xml_create(page, "icon", icon_attrs);

        // Temperature label
        lv_obj_t* temp_lbl = lv_label_create(page);
        lv_label_set_text(temp_lbl, "--\xC2\xB0"
                                    "C");
        lv_obj_set_style_text_font(temp_lbl, lv_font_get_default(), 0);

        // Sensor name label
        lv_obj_t* name_lbl = lv_label_create(page);
        lv_label_set_text(name_lbl, display_name.c_str());
        lv_obj_set_style_text_color(name_lbl, text_muted, 0);
        if (xs_font)
            lv_obj_set_style_text_font(name_lbl, xs_font, 0);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name_lbl, LV_PCT(100));
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        // Make page clickable to open multi-select picker
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(page, this);
        lv_obj_add_event_cb(
            page,
            [](lv_event_t* e) {
                auto* self = static_cast<ThermistorWidget*>(lv_event_get_user_data(e));
                if (self)
                    self->show_configure_picker();
            },
            LV_EVENT_CLICKED, this);

        ui_carousel_add_item(carousel, page);

        // Track page
        size_t page_idx = carousel_pages_.size();
        CarouselPage cp;
        cp.temp_label = temp_lbl;
        cp.name_label = name_lbl;
        std::strcpy(cp.temp_buffer, "--\xC2\xB0"
                                    "C");
        carousel_pages_.push_back(cp);

        // Observe temperature subject. Per-page lifetime is stored in a parallel
        // member vector so the token outlives the local stack frame ([L084]).
        SubjectLifetime& lifetime = carousel_lifetimes_.emplace_back();
        lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
        if (subject) {
            auto obs = helix::ui::observe_int_sync<ThermistorWidget>(
                subject, this,
                [token, page_idx](ThermistorWidget* self, int decidegrees) {
                    if (token.expired())
                        return;
                    if (page_idx >= self->carousel_pages_.size())
                        return;
                    auto& cp = self->carousel_pages_[page_idx];
                    float deg = deci_to_degrees_f(decidegrees);
                    format_temperature_f(deg, cp.temp_buffer, sizeof(cp.temp_buffer));
                    if (cp.temp_label)
                        lv_label_set_text(cp.temp_label, cp.temp_buffer);
                },
                lifetime);

            // Read current value immediately
            int current = lv_subject_get_int(subject);
            float deg = deci_to_degrees_f(current);
            format_temperature_f(deg, carousel_pages_.back().temp_buffer,
                                 sizeof(carousel_pages_.back().temp_buffer));
            lv_label_set_text(temp_lbl, carousel_pages_.back().temp_buffer);

            carousel_observers_.push_back(std::move(obs));
        } else {
            // Subject lookup failed — drop the unused lifetime slot to keep
            // carousel_lifetimes_ aligned with carousel_observers_.
            carousel_lifetimes_.pop_back();
        }
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[ThermistorWidget] Carousel bound {} sensor pages", page_count);

    binding_in_progress_ = false;
}

void ThermistorWidget::detach() {
    lifetime_.invalidate();
    sensor_picker_.hide();
    configure_picker_.hide();
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        version_observer_.reset();
        // Lifetimes reset BEFORE observers (#705 ordering rule).
        carousel_lifetimes_.clear();
        carousel_observers_.clear();
        carousel_pages_.clear();
        temp_lifetime_.reset();
        temp_observer_.reset();
    }

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    temp_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[ThermistorWidget] Detached");
}

void ThermistorWidget::handle_clicked() {
    if (is_carousel_mode()) {
        spdlog::info("[ThermistorWidget] Clicked carousel - showing configure picker");
        show_configure_picker();
    } else {
        spdlog::info("[ThermistorWidget] Clicked - showing sensor picker");
        show_sensor_picker();
    }
}

void ThermistorWidget::resolve_display_name() {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    display_name_ = selected_sensor_; // fallback
    for (const auto& s : tsm.get_sensors_sorted()) {
        if (s.klipper_name == selected_sensor_) {
            display_name_ = s.display_name;
            break;
        }
    }
    strip_temperature_suffix(display_name_);
}

void ThermistorWidget::select_sensor(const std::string& klipper_name) {
    if (klipper_name == selected_sensor_) {
        return;
    }

    // Reset existing observer + lifetime (lifetime first per #705 ordering rule)
    temp_lifetime_.reset();
    temp_observer_.reset();

    selected_sensor_ = klipper_name;
    if (!is_carousel_mode()) {
        sensors_ = {klipper_name};
    }
    resolve_display_name();

    // Subscribe to this sensor's temperature subject
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    lv_subject_t* subject = tsm.get_temp_subject(klipper_name, temp_lifetime_);
    if (subject) {
        auto token = lifetime_.token();
        temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
            subject, this,
            [token](ThermistorWidget* self, int temp) {
                if (token.expired())
                    return;
                self->on_temp_changed(temp);
            },
            temp_lifetime_);
    } else {
        spdlog::warn("[ThermistorWidget] No subject for sensor: {}", klipper_name);
    }

    update_display();
    save_config();

    spdlog::info("[ThermistorWidget] Selected sensor: {} ({})", display_name_, klipper_name);
}

void ThermistorWidget::select_icon(const std::string& name) {
    // Store the ON variant so display can derive the OFF icon from the pair table
    std::string canonical(to_on_variant(name.c_str()));
    icon_name_ = (canonical == DEFAULT_ICON) ? "" : canonical;
    save_config();

    // Update the widget icon immediately (single mode)
    lv_obj_t* icon_obj =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "thermistor_icon") : nullptr;
    if (icon_obj) {
        const char* effective = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        ui_icon_set_source(icon_obj, effective);
    }

    // Move the selection ring if the grid that raised this is still on screen
    configure_picker_.refresh_icon_highlights();

    spdlog::info("[ThermistorWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "thermometer (default)" : icon_name_);
}

void ThermistorWidget::on_temp_changed(int decidegrees) {
    float deg = deci_to_degrees_f(decidegrees);
    format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));

    if (temp_label_) {
        lv_label_set_text(temp_label_, temp_buffer_);
    }

    spdlog::trace("[ThermistorWidget] {} = {:.1f}°C", display_name_, deg);
}

void ThermistorWidget::update_display() {
    if (temp_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(temp_label_, "--\xC2\xB0"
                                           "C");
        } else {
            // Read current value from subject (no observer — use no-lifetime overload)
            auto& tsm = helix::sensors::TemperatureSensorManager::instance();
            lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_);
            if (subject) {
                float deg = deci_to_degrees_f(lv_subject_get_int(subject));
                format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));
                lv_label_set_text(temp_label_, temp_buffer_);
            } else {
                lv_label_set_text(temp_label_, "--\xC2\xB0"
                                               "C");
            }
        }
    }

    if (name_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(name_label_, lv_tr("Select sensor"));
        } else {
            lv_label_set_text(name_label_, display_name_.c_str());
        }
    }
}

void ThermistorWidget::set_config(const nlohmann::json& config) {
    config_ = config;

    // Read icon
    if (config.contains("icon") && config["icon"].is_string()) {
        icon_name_ = config["icon"].get<std::string>();
    }

    // Migrate old format: {"sensor": "x"} -> sensors_ vector
    if (config.contains("sensor") && config["sensor"].is_string()) {
        selected_sensor_ = config["sensor"].get<std::string>();
        sensors_ = {selected_sensor_};
        resolve_display_name();
        spdlog::debug("[ThermistorWidget] Config: sensor={}", selected_sensor_);
    }
    // New format: {"sensors": [...], "display_mode": "carousel"}
    if (config.contains("sensors") && config["sensors"].is_array()) {
        sensors_.clear();
        for (const auto& s : config["sensors"]) {
            if (s.is_string()) {
                sensors_.push_back(s.get<std::string>());
            }
        }
        if (!sensors_.empty()) {
            selected_sensor_ = sensors_.front();
            resolve_display_name();
        }
        spdlog::debug("[ThermistorWidget] Config: {} sensors, mode={} icon={}", sensors_.size(),
                      is_carousel_mode() ? "carousel" : "single",
                      icon_name_.empty() ? DEFAULT_ICON : icon_name_);
    }
}

std::string ThermistorWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_thermistor_carousel";
    }
    return "panel_widget_thermistor";
}

bool ThermistorWidget::on_edit_configure() {
    spdlog::info("[ThermistorWidget] Configure requested - showing sensor picker");
    show_configure_picker();
    return false; // no rebuild — picker updates and saves on Done
}

bool ThermistorWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void ThermistorWidget::save_config() {
    nlohmann::json config = config_;
    // Always write new format
    config["sensors"] = sensors_;
    config.erase("sensor"); // remove legacy key
    if (is_carousel_mode()) {
        config["display_mode"] = "carousel";
    }
    if (!icon_name_.empty()) {
        config["icon"] = icon_name_;
    } else {
        config.erase("icon");
    }
    config_ = config;
    save_widget_config(config);
    spdlog::debug("[ThermistorWidget] Saved config: {} sensors, mode={} icon={}", sensors_.size(),
                  is_carousel_mode() ? "carousel" : "single",
                  icon_name_.empty() ? DEFAULT_ICON : icon_name_);
}

void ThermistorWidget::show_sensor_picker() {
    if (sensor_picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    if (tsm.get_sensors_sorted().empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    sensor_picker_.show_below_widget(parent_screen_, widget_obj_,
                                     helix::ui::ContextMenu::AnchorAlign::Center);
}

void ThermistorWidget::SensorPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);

    // Cap the list at a share of the screen so a printer with a dozen sensors
    // scrolls the list instead of growing the card past the panel.
    lv_obj_set_style_max_height(sensor_list, screen_height_pct(35), 0);

    for (const auto& sensor : sensors) {
        bool is_selected = (sensor.klipper_name == owner_.selected_sensor_);
        lv_obj_t* row = create_sensor_row(sensor_list, sensor.display_name, sensor.klipper_name,
                                          is_selected, false, space_sm, space_xs);

        lv_obj_set_user_data(row, new RowPayload{this, sensor.klipper_name});

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_row_cb");
                auto* target = lv_event_get_current_target_obj(e);
                auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
                if (!payload)
                    return;

                // Copy the name: hide() takes the row - and this payload - with it.
                std::string sensor_name = payload->klipper_name;
                SensorPicker* picker = payload->picker;
                picker->hide();
                picker->owner_.select_sensor(sensor_name);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_row_delete_cb");
                auto* target = lv_event_get_current_target_obj(e);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    }

    spdlog::debug("[ThermistorWidget] Sensor picker built with {} sensors", sensors.size());
}

void ThermistorWidget::show_configure_picker() {
    if (configure_picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    if (tsm.get_sensors_sorted().empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    configure_picker_.show_below_widget(parent_screen_, widget_obj_,
                                        helix::ui::ContextMenu::AnchorAlign::Center);
}

void ThermistorWidget::ConfigurePicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);

    // Cap the list at a share of the screen so the icon grid below it stays on
    // screen however many sensors the printer reports.
    lv_obj_set_style_max_height(sensor_list, screen_height_pct(35), 0);

    // Build set of currently selected sensors
    std::set<std::string> selected_set(owner_.sensors_.begin(), owner_.sensors_.end());

    for (const auto& sensor : sensors) {
        bool selected = selected_set.count(sensor.klipper_name) > 0;
        lv_obj_t* row = create_sensor_row(sensor_list, sensor.display_name, sensor.klipper_name,
                                          selected, true, space_sm, space_xs);

        lv_obj_set_user_data(row, new RowPayload{this, sensor.klipper_name});

        // Click row to toggle checkbox
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_toggle_cb");
                auto* target = lv_event_get_current_target_obj(e);
                uint32_t count = lv_obj_get_child_count(target);
                for (uint32_t i = 0; i < count; ++i) {
                    lv_obj_t* child = lv_obj_get_child(target, static_cast<int32_t>(i));
                    if (lv_obj_check_type(child, &lv_checkbox_class)) {
                        if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                            lv_obj_remove_state(child, LV_STATE_CHECKED);
                        } else {
                            lv_obj_add_state(child, LV_STATE_CHECKED);
                        }
                        break;
                    }
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_row_delete_cb");
                auto* target = lv_event_get_current_target_obj(e);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    }

    // Icon picker section - the divider above it comes from the XML
    lv_obj_t* context_menu = card();
    if (context_menu) {
        // Icon section title
        lv_obj_t* icon_title = lv_label_create(context_menu);
        lv_label_set_text(icon_title, lv_tr("Icon"));
        lv_obj_set_style_text_font(icon_title, lv_font_get_default(), 0);
        lv_obj_set_style_text_color(icon_title, theme_manager_get_color("text"), 0);
        lv_obj_set_width(icon_title, LV_PCT(100));

        // Icon grid (wrap flow)
        lv_obj_t* icon_grid = lv_obj_create(context_menu);
        lv_obj_set_name(icon_grid, "thermistor_icon_grid");
        lv_obj_set_width(icon_grid, LV_PCT(100));
        lv_obj_set_height(icon_grid, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(icon_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(icon_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(icon_grid, 0, 0);
        lv_obj_set_style_pad_gap(icon_grid, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_style_bg_opa(icon_grid, 0, 0);
        lv_obj_set_style_border_width(icon_grid, 0, 0);
        lv_obj_remove_flag(icon_grid, LV_OBJ_FLAG_SCROLLABLE);

        std::string effective_icon =
            owner_.icon_name_.empty() ? std::string(DEFAULT_ICON) : owner_.icon_name_;

        for (size_t i = 0; i < THERMISTOR_ICON_COUNT; ++i) {
            lv_obj_t* cell = lv_obj_create(icon_grid);
            lv_obj_set_size(cell, ICON_CELL_SIZE, ICON_CELL_SIZE);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(cell, 0, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);

            // Pressed feedback
            lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                      LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

            apply_icon_cell_highlight(cell, THERMISTOR_ICONS[i] == effective_icon);

            // Icon glyph
            const char* cp = ui_icon::lookup_codepoint(THERMISTOR_ICONS[i]);
            if (cp) {
                lv_obj_t* icon = lv_label_create(cell);
                lv_label_set_text(icon, cp);
                lv_obj_set_style_text_font(icon, &mdi_icons_24, 0);
                lv_obj_set_style_text_color(icon, theme_manager_get_color("text"), 0);
                lv_obj_center(icon);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
            }

            // Store index as user_data
            lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

            // The cell's user_data is the icon index, so the picker travels as the
            // event's user_data instead. Both are needed to act on a tap.
            lv_obj_add_event_cb(
                cell,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] icon_cell_cb");
                    auto* picker = static_cast<ConfigurePicker*>(lv_event_get_user_data(e));
                    auto* target = lv_event_get_current_target_obj(e);
                    auto idx = static_cast<size_t>(
                        reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                    if (picker && idx < THERMISTOR_ICON_COUNT) {
                        picker->owner_.select_icon(THERMISTOR_ICONS[idx]);
                    }
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, this);
        }
    }

    spdlog::debug("[ThermistorWidget] Configure picker built with {} sensors", sensors.size());
}

void ThermistorWidget::ConfigurePicker::refresh_icon_highlights() {
    lv_obj_t* backdrop = menu();
    if (!backdrop) {
        return;
    }
    lv_obj_t* icon_grid = lv_obj_find_by_name(backdrop, "thermistor_icon_grid");
    if (!icon_grid) {
        return;
    }

    std::string effective_icon =
        owner_.icon_name_.empty() ? std::string(DEFAULT_ICON) : owner_.icon_name_;
    uint32_t grid_count = lv_obj_get_child_count(icon_grid);
    for (uint32_t i = 0; i < grid_count; ++i) {
        lv_obj_t* cell = lv_obj_get_child(icon_grid, i);
        auto idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
        if (idx < THERMISTOR_ICON_COUNT) {
            apply_icon_cell_highlight(cell, THERMISTOR_ICONS[idx] == effective_icon);
        }
    }
}

void ThermistorWidget::ConfigurePicker::on_backdrop_clicked() {
    commit();
}

void ThermistorWidget::ConfigurePicker::commit() {
    lv_obj_t* backdrop = menu();
    lv_obj_t* sensor_list = backdrop ? lv_obj_find_by_name(backdrop, "sensor_list") : nullptr;
    if (!sensor_list) {
        hide();
        return;
    }

    // Read the checkbox states back off the rows.
    std::vector<std::string> selected;
    uint32_t count = lv_obj_get_child_count(sensor_list);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* row = lv_obj_get_child(sensor_list, static_cast<int32_t>(i));
        auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(row));
        if (!payload)
            continue;

        uint32_t row_children = lv_obj_get_child_count(row);
        for (uint32_t j = 0; j < row_children; ++j) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(j));
            if (lv_obj_check_type(child, &lv_checkbox_class)) {
                if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                    selected.push_back(payload->klipper_name);
                }
                break;
            }
        }
    }

    // Close before applying: a selection that changes the display mode asks the
    // manager to rebuild the widget, and this menu belongs to it.
    hide();
    owner_.apply_sensor_selection(selected);
}

void ThermistorWidget::apply_sensor_selection(const std::vector<std::string>& selected) {
    if (selected.empty()) {
        spdlog::warn("[ThermistorWidget] Cannot apply empty sensor selection — keeping previous");
        return;
    }

    sensors_ = selected;

    // Auto-detect mode: multiple sensors → carousel, single → single
    bool was_carousel = is_carousel_mode();
    bool want_carousel = sensors_.size() > 1;
    if (want_carousel) {
        config_["display_mode"] = "carousel";
    } else {
        config_.erase("display_mode");
    }

    save_config();

    if (was_carousel != want_carousel) {
        // Mode changed — need a full widget rebuild (different XML component).
        // If we're in edit mode, the rebuild is deferred to edit-mode exit.
        // set_config() on the rebuilt widget will repopulate selected_sensor_
        // from the persisted sensors_.front().
        spdlog::info("[ThermistorWidget] Mode changed to {} — requesting rebuild",
                     want_carousel ? "carousel" : "single");
        PanelWidgetManager::instance().notify_config_changed(panel_id());
    } else if (is_carousel_mode()) {
        // Same mode, just different sensor selection — rebind inline.
        // bind_carousel_sensors() reads from sensors_ directly and does its own
        // per-page display-name lookup, so leaving selected_sensor_ alone is fine.
        bind_carousel_sensors();
    } else if (!sensors_.empty()) {
        // Single-mode, same-mode: let select_sensor() own selected_sensor_ so its
        // dedup guard at the top of the function can detect a real change.
        // Pre-mutating selected_sensor_ here makes that guard trivially match,
        // leaving temp_observer_ bound to the previous sensor's subject — the
        // displayed temperature only updates after a restart, when attach_single()
        // re-binds from the persisted config (#916).
        select_sensor(sensors_.front());
    }

    spdlog::info("[ThermistorWidget] Applied sensor selection: {} sensors (mode={})",
                 sensors_.size(), is_carousel_mode() ? "carousel" : "single");
}

// Static callbacks
void ThermistorWidget::thermistor_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_clicked_cb");
    auto* widget = panel_widget_from_event<ThermistorWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
