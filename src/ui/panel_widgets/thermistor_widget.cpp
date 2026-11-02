// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermistor_widget.h"

#include "ui_carousel.h"
#include "ui_component_keypad.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_icon_picker.h"
#include "ui_temperature_utils.h"
#include "ui_text.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <set>
#include <utility>

namespace helix {
void thermistor_widget_init_subjects();

void register_thermistor_widget() {
    register_widget_factory(
        "thermistor", [](const std::string& id) { return std::make_unique<ThermistorWidget>(id); });

    register_widget_subjects("thermistor", thermistor_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "thermistor_clicked_cb",
                             ThermistorWidget::thermistor_clicked_cb);
    lv_xml_register_event_cb(nullptr, "thermistor_picker_row_cb", ThermistorWidget::picker_row_cb);
    lv_xml_register_event_cb(nullptr, "thermistor_carousel_page_cb",
                             ThermistorWidget::carousel_page_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::deci_to_degrees_f;
using helix::ui::temperature::format_temperature_f;

/// Strip redundant " Temperature" suffix — the widget context already implies it
/// "Chamber Temperature" -> "Chamber", "Chamber Temperature (BME280)" -> "Chamber (BME280)".
static void strip_temperature_suffix(std::string& name) {
    const std::string word = " Temperature";
    const size_t chip =
        name.size() > 0 && name.back() == ')' ? name.rfind(" (") : std::string::npos;
    const size_t end = chip == std::string::npos ? name.size() : chip;
    if (end > word.size() && name.compare(end - word.size(), word.size(), word) == 0) {
        name.erase(end - word.size(), word.size());
    }
}

namespace {

// Thermistor-related icons for the picker grid
static const char* const kThermistorIcons[] = {
    // clang-format off
    "thermometer", "thermometer_lines", "thermometer_plus", "thermometer_minus",
    "thermometer_probe", "thermometer_off",
    "coolant_temperature", "home_thermometer",
    "heat_wave", "heater", "radiator", "cooldown",
    // clang-format on
};
static constexpr size_t kThermistorIconCount = std::size(kThermistorIcons);
static constexpr const char* kDefaultIcon = "thermometer";

// Icons with distinct on/off glyphs. Config stores the ON variant;
// resolve_icon_for_state() derives the OFF variant from this table.
struct IconPair {
    const char* on_icon;
    const char* off_icon;
};
static const IconPair kIconPairs[] = {
    {"thermometer", "thermometer_off"},
};

/// Map an off-variant icon name to its on-variant (e.g., "thermometer_off" -> "thermometer").
/// Returns the input unchanged if it's not an off-variant.
static const char* to_on_variant(const char* icon) {
    for (const auto& pair : kIconPairs) {
        if (std::strcmp(icon, pair.off_icon) == 0)
            return pair.on_icon;
    }
    return icon;
}

using helix::xml::IndexedSubjectPool;

/// The subjects behind one picker's rows (thermistor_picker_row): name and
/// reading text, a highlight flag (single-select) and a checked flag
/// (multi-select), one slot per sensor.
struct PickerRowSubjects {
    explicit PickerRowSubjects(const char* prefix)
        : name(prefix, IndexedSubjectPool::Type::String, 96, "_name"),
          temp(prefix, IndexedSubjectPool::Type::String, 16, "_temp"),
          highlight(prefix, IndexedSubjectPool::Type::Int, 0, "_hl"),
          checked(prefix, IndexedSubjectPool::Type::Int, 0, "_checked") {}

    void reclaim() {
        name.reclaim();
        temp.reclaim();
        highlight.reclaim();
        checked.reclaim();
    }

    IndexedSubjectPool name;
    IndexedSubjectPool temp;
    IndexedSubjectPool highlight;
    IndexedSubjectPool checked;
};

PickerRowSubjects s_sel_rows{"thermistor_sel"}; ///< single-select sensor picker
PickerRowSubjects s_cfg_rows{"thermistor_cfg"}; ///< multi-select configure picker
HELIX_PSRAM_BSS lv_subject_t s_sel_count_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cfg_count_subject{};
helix::WidgetSubjects s_subjects{"ThermistorWidgetSubjects"};

/// A sensor's reading as a picker row shows it.
std::string sensor_temp_text(const std::string& klipper_name) {
    auto state =
        helix::sensors::TemperatureSensorManager::instance().get_sensor_state(klipper_name);
    char buf[16];
    if (state && state->available) {
        helix::ui::temperature::format_temperature(static_cast<int>(state->temperature), buf,
                                                   sizeof(buf));
        return buf;
    }
    return "--\xC2\xB0"
           "C";
}

/// Fill a picker's row subjects from the sensor list, then publish the row
/// count. The count goes last: it drives the XML <repeat>, whose rows read the
/// other subjects as they are created.
/// @param checked  Klipper names ticked in a multi-select picker
/// @param chosen   The single-select picker's current sensor ("" for none)
void publish_picker_rows(PickerRowSubjects& rows, lv_subject_t* count_subject,
                         const std::vector<helix::sensors::TemperatureSensorConfig>& sensors,
                         const std::set<std::string>* checked, const std::string& chosen) {
    rows.name.ensure_size(sensors.size());
    rows.temp.ensure_size(sensors.size());
    rows.highlight.ensure_size(sensors.size());
    rows.checked.ensure_size(sensors.size());
    for (size_t i = 0; i < sensors.size(); ++i) {
        const auto& sensor = sensors[i];
        rows.name.set_string(i, sensor.display_name);
        rows.temp.set_string(i, sensor_temp_text(sensor.klipper_name));
        rows.highlight.set_int(i, !checked && sensor.klipper_name == chosen ? 1 : 0);
        rows.checked.set_int(i, checked && checked->count(sensor.klipper_name) ? 1 : 0);
    }
    lv_subject_set_int(count_subject, static_cast<int>(sensors.size()));
}

} // anonymous namespace

namespace helix {
void thermistor_widget_init_subjects() {
    s_subjects.ensure_init(
        [] {
            lv_subject_init_int(&s_sel_count_subject, 0);
            s_subjects.publish("thermistor_sel_rows", &s_sel_count_subject);
            lv_subject_init_int(&s_cfg_count_subject, 0);
            s_subjects.publish("thermistor_cfg_rows", &s_cfg_count_subject);
        },
        [] {
            s_sel_rows.reclaim();
            s_cfg_rows.reclaim();
        });
}
} // namespace helix

ThermistorWidget::ThermistorWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id,
                       TileSizing::Content{"110.0 / 888\u00B0C", "110.0\u00B0C", "Sensor", true, "",
                                           /*label_always_drawn=*/true, TileSizing::IconBox::Glyph,
                                           /*icon_animates=*/false, /*label_is_identity=*/true}),
      instance_id_(instance_id), temp_subject_name_(instance_id + "_thermistor_temp"),
      target_subject_name_(instance_id + "_thermistor_target"),
      available_subject_name_(instance_id + "_thermistor_available"),
      page_prefix_(instance_id + "_thermistor_page"),
      page_temp_(std::make_unique<IndexedSubjectPool>(
          page_prefix_.c_str(), IndexedSubjectPool::Type::String, 16, "_temp")),
      page_name_(std::make_unique<IndexedSubjectPool>(
          page_prefix_.c_str(), IndexedSubjectPool::Type::String, 96, "_name")) {
    // A widget built outside the panel-subject phase still publishes into live
    // subjects. A no-op once they exist.
    thermistor_widget_init_subjects();
    UI_MANAGED_SUBJECT_INT(temp_subject_, 0, temp_subject_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(target_subject_, 0, target_subject_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(available_subject_, 0, available_subject_name_.c_str(), subjects_);

    for (const char** a = sizing_.subject_attrs(); *a != nullptr; ++a) {
        attr_storage_.emplace_back(*a);
    }
    attr_storage_.insert(attr_storage_.end(),
                         {"temp_subject", temp_subject_name_, "target_subject",
                          target_subject_name_, "available_subject", available_subject_name_});
    for (const auto& s : attr_storage_) {
        attrs_.push_back(s.c_str());
    }
    attrs_.push_back(nullptr);
}

ThermistorWidget::~ThermistorWidget() {
    detach();
}

void ThermistorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    install_delete_hook(widget_obj);

    if (is_carousel_mode()) {
        attach_carousel();
    } else {
        attach_single();
    }
}

void ThermistorWidget::attach_single() {
    // Cache label pointers
    name_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_name");

    // Apply custom icon
    lv_obj_t* icon_obj = lv_obj_find_by_name(widget_obj_, "thermistor_icon");
    if (icon_obj) {
        const char* icon = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        helix::ui::icon::set_source(icon_obj, icon);
    }

    // If no sensor saved (set_config provided none), auto-select first available
    if (selected_sensor_.empty()) {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        auto sensors = tsm.get_sensors_sorted();
        if (!sensors.empty()) {
            select_sensor(sensors.front().klipper_name);
        }
    } else {
        bind_selected_sensor();
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
    sensor_count_seen_ = false;
    version_observer_ = helix::ui::observe<int>(
        tsm.count_subject(), this,
        [token](ThermistorWidget* self, int /*count*/) {
            if (token.expired() || !std::exchange(self->sensor_count_seen_, true))
                return;
            self->bind_carousel_sensors();
        },
        tsm.subjects_lifetime());

    // Bind immediately (deferred observer fire may be dropped during populate_widgets freeze)
    bind_carousel_sensors();

    spdlog::debug("[ThermistorWidget] Attached carousel ({} sensors)", sensors_.size());
}

void ThermistorWidget::bind_carousel_sensors() {
    if (!widget_obj_)
        return;

    // A nested bind would create pages that the outer call then cleans,
    // leaving page subjects out of step with the pages that exist.
    if (binding_in_progress_)
        return;
    binding_in_progress_ = true;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "thermistor_carousel");
    if (!carousel) {
        binding_in_progress_ = false;
        return;
    }

    // Lifetimes must clear BEFORE observers so weak_ptr-tracked subject-death
    // detection in ObserverGuard::reset() correctly skips lv_observer_remove()
    // on a recreated subject (#705 ordering rule). Clearing the observers drops
    // their queued callbacks, so none reaches the pages cleaned below.
    {
        carousel_lifetimes_.clear();
        carousel_observers_.clear();
        carousel_page_count_ = 0;

        auto* state_ptr = ui_carousel_get_state(carousel);
        if (state_ptr && state_ptr->scroll_container) {
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
        binding_in_progress_ = false;
        return;
    }

    auto token = lifetime_.token();

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

        // Page: icon, reading and name, bound to this page's subjects.
        const size_t page_idx = carousel_page_count_++;
        page_temp_->ensure_size(page_idx + 1);
        page_name_->ensure_size(page_idx + 1);
        page_name_->set_string(page_idx, display_name);
        page_temp_->set_string(page_idx, "--\xC2\xB0"
                                         "C");

        // Per-page lifetime is stored in a parallel member vector so the token
        // outlives the local stack frame ([L084]).
        SubjectLifetime& lifetime = carousel_lifetimes_.emplace_back();
        lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
        if (subject) {
            carousel_observers_.push_back(helix::ui::observe<int>(
                subject, this,
                [token, page_idx](ThermistorWidget* self, int decidegrees) {
                    if (token.expired())
                        return;
                    self->show_page_temp(page_idx, decidegrees);
                },
                lifetime));
            show_page_temp(page_idx, lv_subject_get_int(subject));
        } else {
            // Subject lookup failed: drop the unused lifetime slot to keep
            // carousel_lifetimes_ aligned with carousel_observers_.
            carousel_lifetimes_.pop_back();
        }

        const std::string index_text = std::to_string(page_idx);
        const char* icon_src = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        const char* page_attrs[] = {
            "prefix", page_prefix_.c_str(), "index", index_text.c_str(), "icon", icon_src, nullptr};
        // DECLARATIVE_OK: ui_carousel_add_item() owns its pages' scroll container and indicators,
        // so a <repeat> cannot build them.
        auto* page = static_cast<lv_obj_t*>(
            lv_xml_create(lv_scr_act(), "thermistor_carousel_page", page_attrs));
        if (!page) {
            spdlog::error("[ThermistorWidget] thermistor_carousel_page not registered");
            continue;
        }
        // The page's click handler finds this widget through its user_data.
        lv_obj_set_user_data(page, this); // DECLARATIVE_OK: carousel pages are C++-created (above)
        ui_carousel_add_item(carousel, page);
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
        version_observer_.reset();
        // Lifetimes reset BEFORE observers (#705 ordering rule).
        carousel_lifetimes_.clear();
        carousel_observers_.clear();
        temp_lifetime_.reset();
        temp_observer_.reset();
        target_observer_.reset();
    }
    uninstall_delete_hook();

    forget_tile_widgets();

    spdlog::debug("[ThermistorWidget] Detached");
}

void ThermistorWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the pending deferred observer
    // callbacks and drop the cached pointers only. The observers themselves
    // stay registered on their (still live) subjects until detach() or the
    // destructor resets them: every callback checks its token first, so a
    // drained on_temp_changed() no-ops instead of writing to the freed label.
    lifetime_.invalidate();
    forget_tile_widgets();
}

void ThermistorWidget::forget_tile_widgets() {
    name_label_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ThermistorWidget::handle_clicked() {
    if (is_carousel_mode()) {
        spdlog::info("[ThermistorWidget] Clicked carousel - showing configure picker");
        show_configure_picker();
    } else if (helix::sensors::klipper_object_has_target(selected_sensor_)) {
        spdlog::info("[ThermistorWidget] Clicked heater {} - showing target keypad",
                     selected_sensor_);
        open_target_keypad();
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

    selected_sensor_ = klipper_name;
    if (!is_carousel_mode()) {
        sensors_ = {klipper_name};
    }
    resolve_display_name();
    bind_selected_sensor();
    update_display();
    save_config();

    spdlog::info("[ThermistorWidget] Selected sensor: {} ({})", display_name_, klipper_name);
}

void ThermistorWidget::bind_selected_sensor() {
    // Lifetime first per #705 ordering rule
    temp_lifetime_.reset();
    temp_observer_.reset();
    target_observer_.reset();

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    lv_subject_t* temp = tsm.get_temp_subject(selected_sensor_, temp_lifetime_);
    if (!temp) {
        spdlog::warn("[ThermistorWidget] No subject for sensor: {}", selected_sensor_);
        return;
    }
    auto token = lifetime_.token();
    temp_observer_ = helix::ui::observe<int>(
        temp, this,
        [token](ThermistorWidget* self, int deci) {
            if (token.expired())
                return;
            self->on_temp_changed(deci);
        },
        temp_lifetime_);
    lv_subject_t* target = tsm.get_target_subject(selected_sensor_, temp_lifetime_);
    if (target) {
        target_observer_ = helix::ui::observe<int>(
            target, this,
            [token](ThermistorWidget* self, int deci) {
                if (token.expired())
                    return;
                lv_subject_set_int(&self->target_subject_, deci);
            },
            temp_lifetime_);
    }
}

namespace {
/// The heater the open target keypad sends to. One keypad shows at a time.
std::string g_target_keypad_heater;
/// Keypad ceiling when the heater's configured max_temp is not known.
constexpr float kTargetKeypadFallbackMax = 120.0f;
} // namespace

void ThermistorWidget::open_target_keypad() {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    SubjectLifetime unused;
    lv_subject_t* target = tsm.get_target_subject(selected_sensor_, unused);
    auto* controller = get_temperature_controller();
    g_target_keypad_heater = selected_sensor_;

    const float max_value =
        controller ? controller->keypad_max_for(selected_sensor_, kTargetKeypadFallbackMax)
                   : kTargetKeypadFallbackMax;
    ui_keypad_config_t config = {
        .initial_value = target ? deci_to_degrees_f(lv_subject_get_int(target)) : 0.0f,
        .min_value = 0.0f,
        .max_value = max_value,
        .title_label = display_name_.c_str(),
        .unit_label = "\u00B0C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = target_keypad_cb,
        .user_data = nullptr,
    };
    ui_keypad_show(&config);
}

void ThermistorWidget::target_keypad_cb(float value, void* /*user_data*/) {
    auto* controller = get_temperature_controller();
    if (!controller || g_target_keypad_heater.empty())
        return;
    spdlog::info("[ThermistorWidget] Setting {} target to {:.0f}°C", g_target_keypad_heater, value);
    controller->set_target(g_target_keypad_heater, static_cast<double>(static_cast<int>(value)),
                           {.toast = true});
}

void ThermistorWidget::select_icon(const std::string& name) {
    // Store the ON variant so display can derive the OFF icon from the pair table
    std::string canonical(to_on_variant(name.c_str()));
    icon_name_ = (canonical == kDefaultIcon) ? "" : canonical;
    save_config();

    // Update the widget icon immediately (single mode)
    lv_obj_t* icon_obj =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "thermistor_icon") : nullptr;
    if (icon_obj) {
        const char* effective = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        helix::ui::icon::set_source(icon_obj, effective);
    }

    // Move the selection ring if the grid that raised this is still on screen
    configure_picker_.refresh_icon_highlights();

    spdlog::info("[ThermistorWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "thermometer (default)" : icon_name_);
}

void ThermistorWidget::on_temp_changed(int decidegrees) {
    lv_subject_set_int(&temp_subject_, decidegrees);
    spdlog::trace("[ThermistorWidget] {} = {:.1f}°C", display_name_,
                  deci_to_degrees_f(decidegrees));
}

void ThermistorWidget::update_display() {
    // Seed the mirrored subjects from the sensor's current values; the
    // observers keep them current from here on.
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    SubjectLifetime unused;
    lv_subject_t* temp = tsm.get_temp_subject(selected_sensor_);
    lv_subject_t* target = tsm.get_target_subject(selected_sensor_, unused);
    lv_subject_set_int(&available_subject_, temp ? 1 : 0);
    lv_subject_set_int(&temp_subject_, temp ? lv_subject_get_int(temp) : 0);
    lv_subject_set_int(&target_subject_, target ? lv_subject_get_int(target) : 0);

    if (name_label_) {
        if (selected_sensor_.empty()) {
            // A tag, not lv_tr() text: the label then re-translates itself.
            lv_label_set_translation_tag(name_label_, "Select sensor");
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
                      icon_name_.empty() ? kDefaultIcon : icon_name_);
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
                  icon_name_.empty() ? kDefaultIcon : icon_name_);
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

    // The rows exist from the card's first frame, so its size is known when it is
    // placed: the subjects they bind are filled before the card is built.
    sensor_picker_.fill_rows();

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    sensor_picker_.show_below_widget(parent_screen_, widget_obj_,
                                     helix::ui::ContextMenu::AnchorAlign::Center);
}

void ThermistorWidget::SensorPicker::fill_rows() {
    auto sensors = helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted();
    row_sensors_.clear();
    for (const auto& sensor : sensors) {
        row_sensors_.push_back(sensor.klipper_name);
    }
    publish_picker_rows(s_sel_rows, &s_sel_count_subject, sensors, nullptr,
                        owner_.selected_sensor_);
}

void ThermistorWidget::SensorPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        return;
    }

    // Cap the list at a share of the screen so a printer with a dozen sensors
    // scrolls the list instead of growing the card past the panel.
    lv_obj_set_style_max_height(sensor_list, screen_height_pct(35), 0);

    spdlog::debug("[ThermistorWidget] Sensor picker built with {} sensors", row_sensors_.size());
}

void ThermistorWidget::SensorPicker::row_tapped(size_t row) {
    if (row >= row_sensors_.size())
        return;
    // Copy the name: hide() takes the rows with it.
    std::string sensor_name = row_sensors_[row];
    hide();
    owner_.select_sensor(sensor_name);
}

void ThermistorWidget::show_configure_picker() {
    if (configure_picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }
    if (helix::PanelWidgetManager::refuse_layout_edit()) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    if (tsm.get_sensors_sorted().empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    configure_picker_.fill_rows();
    configure_picker_.show_below_widget(parent_screen_, widget_obj_,
                                        helix::ui::ContextMenu::AnchorAlign::Center);
}

void ThermistorWidget::ConfigurePicker::fill_rows() {
    auto sensors = helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted();
    row_sensors_.clear();
    for (const auto& sensor : sensors) {
        row_sensors_.push_back(sensor.klipper_name);
    }
    const std::set<std::string> selected_set(owner_.sensors_.begin(), owner_.sensors_.end());
    publish_picker_rows(s_cfg_rows, &s_cfg_count_subject, sensors, &selected_set, "");
}

void ThermistorWidget::ConfigurePicker::row_tapped(size_t row) {
    if (row >= row_sensors_.size())
        return;
    lv_subject_t* checked = s_cfg_rows.checked.at(row);
    lv_subject_set_int(checked, lv_subject_get_int(checked) ? 0 : 1);
}

void ThermistorWidget::ConfigurePicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        return;
    }

    // Cap the list at a share of the screen so the icon grid below it stays on
    // screen however many sensors the printer reports.
    lv_obj_set_style_max_height(sensor_list, screen_height_pct(35), 0);

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
            owner_.icon_name_.empty() ? std::string(kDefaultIcon) : owner_.icon_name_;

        helix::ui::populate_icon_grid(icon_grid, kThermistorIcons, kThermistorIconCount,
                                      effective_icon,
                                      [this](const char* name) { owner_.select_icon(name); });
    }

    spdlog::debug("[ThermistorWidget] Configure picker built with {} sensors", row_sensors_.size());
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
        owner_.icon_name_.empty() ? std::string(kDefaultIcon) : owner_.icon_name_;
    helix::ui::refresh_icon_grid(icon_grid, effective_icon);
}

void ThermistorWidget::ConfigurePicker::on_backdrop_clicked() {
    commit();
}

void ThermistorWidget::ConfigurePicker::commit() {
    // The checked subjects are the edit; read them back in row order.
    std::vector<std::string> selected;
    for (size_t i = 0; i < row_sensors_.size(); ++i) {
        if (lv_subject_get_int(s_cfg_rows.checked.at(i)))
            selected.push_back(row_sensors_[i]);
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

void ThermistorWidget::picker_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] picker_row_cb");
    // XML event user_data is the row's index as a string.
    const char* index = static_cast<const char*>(lv_event_get_user_data(e));
    if (!index)
        return;
    const auto row = static_cast<size_t>(std::atoi(index));
    if (auto* picker = helix::ui::ContextMenu::active_as<SensorPicker>()) {
        picker->row_tapped(row);
    } else if (auto* config = helix::ui::ContextMenu::active_as<ConfigurePicker>()) {
        config->row_tapped(row);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ThermistorWidget::carousel_page_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] carousel_page_cb");
    if (auto* widget = panel_widget_from_event<ThermistorWidget>(e)) {
        widget->show_configure_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ThermistorWidget::show_page_temp(size_t page, int decidegrees) {
    if (page >= page_temp_->size())
        return;
    char buf[16];
    format_temperature_f(deci_to_degrees_f(decidegrees), buf, sizeof(buf));
    page_temp_->set_string(page, buf);
}
