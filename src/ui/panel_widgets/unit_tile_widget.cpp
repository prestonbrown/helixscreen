// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/unit_tile_widget.h"

#include "ui_event_safety.h"

#include "core/observer_factory.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "humidity_sensor_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/unit_override.h"
#include "widget_subjects.h"
#include "width_sensor_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

namespace helix {

namespace {

/// The configure card's Unit row: the text it shows, and 1 while an override is set.
helix::Subject s_cfg_unit_subject{};
char s_cfg_unit_buf[48] = {};
helix::Subject s_cfg_unit_custom_subject{};
helix::WidgetSubjects s_subjects{"UnitTileWidgetSubjects"};

void unit_tile_init_subjects() {
    s_subjects.ensure_init([] {
        s_cfg_unit_subject.init_string(s_cfg_unit_buf, nullptr, sizeof(s_cfg_unit_buf), "");
        s_subjects.publish("unit_tile_cfg_unit", &s_cfg_unit_subject);
        s_cfg_unit_custom_subject.init_int(0);
        s_subjects.publish("unit_tile_cfg_unit_custom", &s_cfg_unit_custom_subject);
    });
}

TileSizing::Content sizing_content(const UnitTileWidget::Spec& spec, const std::string& unit) {
    const std::string reading =
        std::string(spec.widest_number) + (unit.empty() ? "" : spec.unit_gap + unit);
    TileSizing::Content content;
    content.widest_value = reading;
    content.widest_current = reading;
    content.label = spec.label;
    content.has_value = true;
    return content;
}

/// Humidity subject: percent x 10, -1 without a sensor.
bool format_humidity_number(int raw, char* buf, size_t size) {
    if (raw < 0) {
        return false;
    }
    std::snprintf(buf, size, "%d", raw / 10);
    return true;
}

/// Width subject: mm x 1000, -1 without a sensor.
bool format_width_number(int raw, char* buf, size_t size) {
    if (raw < 0) {
        return false;
    }
    std::snprintf(buf, size, "%.2f", static_cast<double>(raw) / 1000.0);
    return true;
}

lv_subject_t* humidity_raw() {
    return helix::sensors::HumiditySensorManager::instance().get_chamber_humidity_subject();
}
SubjectLifetime humidity_lifetime() {
    return helix::sensors::HumiditySensorManager::instance().subjects_lifetime();
}
lv_subject_t* width_raw() {
    return helix::sensors::WidthSensorManager::instance().get_diameter_subject();
}
SubjectLifetime width_lifetime() {
    return helix::sensors::WidthSensorManager::instance().subjects_lifetime();
}

const UnitTileWidget::Spec kHumiditySpec{"humidity",
                                         "%",
                                         "",
                                         "Humidity",
                                         "100",
                                         humidity_raw,
                                         humidity_lifetime,
                                         format_humidity_number};
const UnitTileWidget::Spec kWidthSpec{
    "width_sensor", "mm", " ", "Width", "8.88", width_raw, width_lifetime, format_width_number};

} // namespace

const UnitTileWidget::Spec& UnitTileWidget::humidity_spec() {
    return kHumiditySpec;
}

const UnitTileWidget::Spec& UnitTileWidget::width_spec() {
    return kWidthSpec;
}

void UnitTileWidget::register_xml() {
    unit_tile_init_subjects();
    lv_xml_register_event_cb(nullptr, "unit_tile_unit_row_cb", UnitTileWidget::unit_row_cb);
    lv_xml_register_event_cb(nullptr, "unit_tile_unit_reset_cb", UnitTileWidget::unit_reset_cb);
}

void register_unit_tile_widgets() {
    for (const UnitTileWidget::Spec* spec : {&kHumiditySpec, &kWidthSpec}) {
        register_widget_factory(spec->id, [spec](const std::string& id) {
            return std::make_unique<UnitTileWidget>(id, *spec);
        });
        register_widget_subjects(spec->id, unit_tile_init_subjects);
    }
    UnitTileWidget::register_xml();
}

UnitTileWidget::UnitTileWidget(const std::string& instance_id, const Spec& spec)
    : TiledPanelWidget(instance_id, sizing_content(spec, spec.default_unit)),
      instance_id_(instance_id), spec_(spec) {
    unit_tile_init_subjects();
    value_subject_.init_string(value_buffer_, nullptr, sizeof(value_buffer_), "--");
    sizing_.publish_instance_subject(subjects_, &value_subject_, "value_subject",
                                     "unit_tile_value");
}

UnitTileWidget::~UnitTileWidget() {
    detach();
}

std::string UnitTileWidget::unit_text(const std::optional<std::string>& unit) const {
    return unit_override::resolve(unit, spec_.default_unit);
}

void UnitTileWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    unit_override_ = unit_override::from_config(config);
    apply_unit();
}

void UnitTileWidget::attach(lv_obj_t* /*widget_obj*/, lv_obj_t* /*parent_screen*/) {
    if (lv_subject_t* raw = spec_.raw_subject()) {
        auto token = lifetime_.token();
        raw_observer_ = helix::ui::observe<int>(
            raw, this,
            [token](UnitTileWidget* self, int /*raw*/) {
                if (!token.expired()) {
                    self->refresh_value();
                }
            },
            spec_.raw_lifetime());
    }
    refresh_value();
}

void UnitTileWidget::detach() {
    raw_observer_.reset();
    configure_card_.hide();
}

void UnitTileWidget::refresh_value() {
    char number[16];
    lv_subject_t* raw = spec_.raw_subject();
    std::string text = "--";
    if (raw && spec_.format_number(lv_subject_get_int(raw), number, sizeof(number))) {
        const std::string unit = unit_text(unit_override_);
        text = number + (unit.empty() ? std::string() : spec_.unit_gap + unit);
    }
    lv_subject_set_string(&value_subject_, text.c_str());
}

void UnitTileWidget::apply_unit() {
    // The widest reading changes with the unit, so the rung it fits at may too.
    sizing_.set_content(sizing_content(spec_, unit_text(unit_override_)));
    sizing_.remeasure();
    refresh_value();
}

void UnitTileWidget::set_unit_override(std::optional<std::string> unit) {
    unit_override_ = unit_override::clamp(std::move(unit));
    save_config();
    apply_unit();
    configure_card_.fill_unit_row();
    spdlog::info("[UnitTileWidget] {} unit: {}", instance_id_,
                 unit_override_ ? "\"" + *unit_override_ + "\"" : std::string("default"));
}

void UnitTileWidget::save_config() {
    nlohmann::json config = config_;
    unit_override::to_config(config, unit_override_);
    config_ = config;
    save_widget_config(config);
}

bool UnitTileWidget::on_edit_configure() {
    lv_obj_t* tile = root();
    if (configure_card_.is_visible() || !tile) {
        return false;
    }
    configure_card_.fill_unit_row();
    configure_card_.show_below_widget(lv_obj_get_screen(tile), tile,
                                      helix::ui::ContextMenu::AnchorAlign::Center);
    return false; // no rebuild: the card saves as it edits
}

void UnitTileWidget::ConfigureCard::fill_unit_row() {
    const auto& unit = owner_.unit_override_;
    lv_subject_set_string(&s_cfg_unit_subject,
                          unit_override::unit_row_value(unit, owner_.spec_.default_unit).c_str());
    lv_subject_set_int(&s_cfg_unit_custom_subject, unit ? 1 : 0);
}

void UnitTileWidget::ConfigureCard::edit_unit() {
    // The dialog needs the keyboard and the screen, so the card closes first.
    UnitTileWidget* owner = &owner_;
    LifetimeToken token = owner_.lifetime_.token();
    std::string panel = owner_.panel_id();
    std::string id = owner_.instance_id_;
    std::string initial = owner_.unit_text(owner_.unit_override_);
    hide();
    unit_override::show_entry([owner](const std::string& text) { owner->set_unit_override(text); },
                              std::move(token), std::move(panel), std::move(id),
                              std::move(initial));
}

void UnitTileWidget::ConfigureCard::reset_unit() {
    owner_.set_unit_override(std::nullopt);
}

void UnitTileWidget::unit_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UnitTileWidget] unit_row_cb");
    (void)e;
    if (auto* card = helix::ui::ContextMenu::active_as<ConfigureCard>()) {
        card->edit_unit();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void UnitTileWidget::unit_reset_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UnitTileWidget] unit_reset_cb");
    (void)e;
    if (auto* card = helix::ui::ContextMenu::active_as<ConfigureCard>()) {
        card->reset_unit();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
