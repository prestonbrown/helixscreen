// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_widget.h"

#include "ui_callback_helpers.h"
#include "ui_carousel.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_icon_picker.h"
#include "ui_panel_power.h"
#include "ui_row_text.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/subject.h"
#include "core/update_queue.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lvgl.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "power_device_icon.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "sensor_state.h"
#include "theme_manager.h"
#include "tile_color.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace helix {
void register_power_device_widget() {
    register_widget_factory("power_device", [](const std::string& id) {
        return std::make_unique<PowerDeviceWidget>(id);
    });
    lv_xml_register_event_cb(nullptr, "power_device_clicked_cb",
                             PowerDeviceWidget::power_device_clicked_cb);
    lv_xml_register_event_cb(nullptr, "power_device_color_cb", PowerDeviceWidget::color_swatch_cb);
    tile_color::register_subjects();
}
} // namespace helix

namespace {

// Power-related icons for the picker grid
static const char* const kPowerIcons[] = {
    // clang-format off
    // Power symbols
    "power_cycle",       "power",              "power_on",            "power_off",
    "power_standby",
    // Plugs
    "power_plug",        "power_plug_off",     "power_plug_outline",  "power_plug_battery",
    // Sockets
    "power_socket",      "power_socket_au",    "power_socket_ch",     "power_socket_de",
    "power_socket_eu",   "power_socket_fr",    "power_socket_it",     "power_socket_jp",
    "power_socket_uk",   "power_socket_us",
    // Device types (already in font)
    "lightbulb_outline", "lightbulb_on",       "led_strip",           "fan",
    "radiator",          "flash",              "electric_switch",
    // clang-format on
};
static constexpr size_t kPowerIconCount = std::size(kPowerIcons);
static constexpr const char* kDefaultIcon = "power_cycle";

/// The meter readings on the energy page, in the widget's readings_ order: the
/// sensor value key, the instance subject suffix and component prop, and the
/// text shown until the meter reports that key.
struct EnergyKey {
    const char* key;
    const char* suffix;
    const char* prop;
    const char* placeholder;
};
static constexpr EnergyKey kEnergyKeys[] = {
    {"power", "energy_power", "energy_power_subject", "-- W"},
    {"voltage", "energy_voltage", "energy_voltage_subject", "-- V"},
    {"current", "energy_current", "energy_current_subject", "-- A"},
    {"energy", "energy_energy", "energy_energy_subject", "-- kWh"},
};

} // namespace

using namespace helix;

// The state is the reading ("LOCKED" is the widest), the device name the label,
// drawn whatever show_widget_labels says, and the glyph sits in a disc that
// scales with it.
PowerDeviceWidget::PowerDeviceWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id, status_content(false)), instance_id_(instance_id) {
    // Registered before the manager parses the component, which drops a
    // binding whose subject is missing at parse time. The initial values are
    // the component's own defaults, so a tile reads the same before its first
    // update as it always has.
    has_status_subject_.init_int(0);
    sizing_.publish_instance_subject(subjects_, &has_status_subject_, "status_subject",
                                     "has_status");
    state_subject_.init_int(-1);
    sizing_.publish_instance_subject(subjects_, &state_subject_, "state_subject", "state");
    named_subject_.init_int(0);
    sizing_.publish_instance_subject(subjects_, &named_subject_, "named_subject", "named");
    name_subject_.init_string(name_buf_, nullptr, sizeof(name_buf_), "");
    sizing_.publish_instance_subject(subjects_, &name_subject_, "name_subject", "name");
    status_text_subject_.init_string(status_text_buf_, nullptr, sizeof(status_text_buf_), "");
    sizing_.publish_instance_subject(subjects_, &status_text_subject_, "status_text_subject",
                                     "status_text");
    accent_subject_.init_color(theme_manager_get_color("secondary"));
    sizing_.publish_instance_subject(subjects_, &accent_subject_, "accent_subject", "accent");
    badge_opa_subject_.init_int(40);
    sizing_.publish_instance_subject(subjects_, &badge_opa_subject_, "badge_opa_subject",
                                     "badge_opa");
    status_color_subject_.init_color(theme_manager_get_color("text_muted"));
    sizing_.publish_instance_subject(subjects_, &status_color_subject_, "status_color_subject",
                                     "status_color");
    static_assert(std::size(kEnergyKeys) == kEnergyReadingCount);
    for (size_t i = 0; i < kEnergyReadingCount; ++i) {
        EnergyReading& r = readings_[i];
        r.text.init_string(r.buf, nullptr, sizeof(r.buf), kEnergyKeys[i].placeholder);
        sizing_.publish_instance_subject(subjects_, &r.text, kEnergyKeys[i].prop,
                                         kEnergyKeys[i].suffix);
    }
}

void PowerDeviceWidget::set_name_text(const char* text) {
    lv_subject_set_string(&name_subject_, text);
    lv_subject_set_int(&named_subject_, 1);
}

void PowerDeviceWidget::apply_status_presence() {
    const bool has_status = !device_name_.empty();
    lv_subject_set_int(&has_status_subject_, has_status ? 1 : 0);
    sizing_.set_content(status_content(has_status));
    relayout_for_granted_size();
}

PowerDeviceWidget::~PowerDeviceWidget() {
    detach();
}

void PowerDeviceWidget::set_config(const nlohmann::json& config) {
    device_name_ = json_util::read_field<std::string>(config, "device").value_or(device_name_);
    icon_name_ = json_util::read_field<std::string>(config, "icon").value_or(icon_name_);
    sensor_id_ = json_util::read_field<std::string>(config, "sensor").value_or(sensor_id_);
    color_ = tile_color::color_from_config(config);
    spdlog::debug("[PowerDeviceWidget] Config: {}={} icon={} color=0x{:06X}", instance_id_,
                  device_name_.empty() ? "(unconfigured)" : device_name_,
                  icon_name_.empty() ? kDefaultIcon : icon_name_, color_);
    apply_status_presence();
}

void PowerDeviceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        // Pressed feedback
        lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    icon_obj_ = lv_obj_find_by_name(widget_obj_, "power_icon");

    if (is_all_devices()) {
        // __all__ mode: aggregate toggle for all selected power panel devices.
        // Observe power_device_count to refresh when devices are discovered.
        auto token = lifetime_.token();
        power_count_observer_ = helix::ui::observe<int>(
            get_printer_state().capabilities_state().subject(Capability::PowerDeviceCount), this,
            [token](PowerDeviceWidget* self, int /*count*/) {
                if (token.expired())
                    return;
                self->refresh_all_devices_state();
            },
            get_printer_state().get_subjects_lifetime());

        set_name_text(lv_tr("All Devices"));
    } else if (!device_name_.empty()) {
        // Observe the device status subject. Use a LOCAL lifetime variable
        // so the ObserverGuard's weak_ptr expires when deinit_subjects()
        // destroys the PowerDeviceState's copy (shutdown safety).
        SubjectLifetime lifetime;
        lv_subject_t* subj =
            PowerDeviceState::instance().get_status_subject(device_name_, lifetime);
        if (subj) {
            auto token = lifetime_.token();
            status_observer_ = helix::ui::observe<int>(
                subj, this,
                [token](PowerDeviceWidget* self, int status) {
                    if (token.expired())
                        return;
                    self->update_display(status);
                },
                lifetime);

            set_name_text(
                helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE).c_str());
        } else {
            spdlog::warn("[PowerDeviceWidget] No status subject for device '{}'", device_name_);
            update_display(-1);
        }
    } else {
        // Unconfigured state
        update_display(-1);
    }

    // Auto-match sensor if none configured and only one energy sensor + one power device
    if (sensor_id_.empty() && !device_name_.empty()) {
        sensor_id_ = auto_match_sensor();
        if (!sensor_id_.empty()) {
            save_config();
        }
    }
    carousel_ = helix::ui::find_required(widget_obj_, "power_carousel", "PowerDeviceWidget");
    show_energy_page();

    spdlog::debug("[PowerDeviceWidget] Attached {} (device: {})", instance_id_,
                  device_name_.empty() ? "none" : device_name_);
}

void PowerDeviceWidget::detach() {
    detach_sensor_observers();
    carousel_ = nullptr;
    lifetime_.invalidate();
    picker_.hide();

    status_observer_.reset();
    power_count_observer_.reset();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    icon_obj_ = nullptr;

    spdlog::debug("[PowerDeviceWidget] Detached");
}

void PowerDeviceWidget::update_display(int status) {
    // Status values: 0=off, 1=on, 2=locked, -1=unconfigured
    last_status_ = status;
    apply_state_colors(status);

    if (icon_obj_) {
        // Apply icon — for paired icons, toggle between on/off variants
        const char* base_icon = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        const char* effective_icon = power_resolve_icon_for_state(base_icon, status);
        helix::ui::icon::set_source(icon_obj_, effective_icon);
    }

    lv_subject_set_int(&state_subject_, status);
    const char* text = status == 1   ? lv_tr("ON")
                       : status == 0 ? lv_tr("OFF")
                       : status == 2 ? lv_tr("LOCKED")
                                     : "";
    lv_subject_set_string(&status_text_subject_, text);

    if (status == -1) {
        // The component's own "Configure" label, which re-translates itself.
        lv_subject_set_int(&named_subject_, 0);
    } else if (!device_name_.empty() && !is_all_devices()) {
        set_name_text(
            helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE).c_str());
    }
}

void PowerDeviceWidget::apply_state_colors(int status) {
    tile_color::TileState state = tile_color::TileState::Unconfigured;
    if (status == 1) {
        state = tile_color::TileState::Active;
    } else if (status == 0) {
        state = tile_color::TileState::Inactive;
    } else if (status == 2) {
        state = tile_color::TileState::Locked;
    }
    const tile_color::Paint paint = tile_color::paint_for(color_, state);

    // The state colours: danger while on, muted while off or locked, secondary
    // while unconfigured. A user colour replaces them on and off only.
    const char* token = status == 1 ? "danger" : status == -1 ? "secondary" : "text_muted";
    const char* variant = status == 1 ? "danger" : status == -1 ? "secondary" : "muted";
    lv_color_t accent = theme_manager_get_color(token);
    if (paint == tile_color::Paint::Custom) {
        accent = lv_color_hex(color_);
    } else if (paint == tile_color::Paint::CustomMuted) {
        accent = tile_color::muted(lv_color_hex(color_), theme_manager_get_color("text_muted"));
    }

    lv_subject_set_color(&accent_subject_, accent);
    lv_subject_set_int(&badge_opa_subject_, status == 1 ? 40 : 20);

    if (icon_obj_) {
        if (paint == tile_color::Paint::Theme) {
            helix::ui::icon::set_variant(icon_obj_, variant);
        } else {
            helix::ui::icon::set_color(icon_obj_, accent, LV_OPA_COVER);
        }
    }

    // The state line keeps the muted face while off, so OFF still reads as off.
    if (status != -1) {
        lv_subject_set_color(&status_color_subject_, paint == tile_color::Paint::Custom
                                                         ? accent
                                                         : theme_manager_get_color(token));
    }
}

void PowerDeviceWidget::handle_clicked() {
    if (device_name_.empty()) {
        spdlog::info("[PowerDeviceWidget] {} clicked (unconfigured) - showing picker",
                     instance_id_);
        show_device_picker();
        return;
    }

    if (is_all_devices()) {
        handle_all_devices_toggle();
        return;
    }

    // Check current status
    SubjectLifetime lt;
    lv_subject_t* subj = PowerDeviceState::instance().get_status_subject(device_name_, lt);
    if (subj) {
        int status = lv_subject_get_int(subj);
        if (status == 2) {
            spdlog::debug("[PowerDeviceWidget] {} - device '{}' is locked, ignoring click",
                          instance_id_, device_name_);
            return;
        }
    }

    IMoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[PowerDeviceWidget] No API available");
        return;
    }

    // Suppress "Printer Firmware Disconnected" dialog when turning off a power device.
    // The device may have bound_services: klipper, causing an expected Klipper disconnect.
    // Check current state: if on (1), toggle means turning off.
    {
        SubjectLifetime lt;
        lv_subject_t* status_subj =
            PowerDeviceState::instance().get_status_subject(device_name_, lt);
        if (status_subj && lv_subject_get_int(status_subj) == 1) {
            EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::NORMAL);
        }
    }

    spdlog::info("[PowerDeviceWidget] {} toggling device '{}'", instance_id_, device_name_);
    auto token = lifetime_.token();
    api->set_device_power(
        device_name_, "toggle",
        [name = device_name_]() {
            spdlog::debug("[PowerDeviceWidget] Device '{}' toggled successfully", name);
        },
        [token, name = device_name_](const MoonrakerError& err) {
            // Invoked on the calling thread for validation rejects and on an
            // HttpExecutor worker for transport failures, so the notification —
            // which touches LVGL — has to go through the lifetime token.
            spdlog::error("[PowerDeviceWidget] Failed to toggle device '{}': {}", name,
                          err.message);
            token.defer("PowerDeviceWidget::toggle_error",
                        [name]() { NOTIFY_ERROR("Failed to toggle {}", name); });
        });
}

void PowerDeviceWidget::color_swatch_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] color_swatch_cb");
    const int index = helix::ui::event_user_int(e).value_or(-1);
    auto* picker = helix::ui::ContextMenu::active_as<DevicePicker>();
    if (picker && index >= 0 && index < static_cast<int>(tile_color::palette_size())) {
        picker->owner().select_color(tile_color::palette_at(index));
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PowerDeviceWidget::power_device_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] power_device_clicked_cb");
    auto* widget = panel_widget_from_event<PowerDeviceWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

bool PowerDeviceWidget::on_edit_configure() {
    spdlog::info("[PowerDeviceWidget] {} configure requested - showing picker", instance_id_);
    show_device_picker();
    return false;
}

IMoonrakerAPI* PowerDeviceWidget::get_api() const {
    return get_moonraker_api();
}

void PowerDeviceWidget::show_device_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    if (PowerDeviceState::instance().device_names().empty()) {
        spdlog::warn("[PowerDeviceWidget] No power devices available");
        return;
    }

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Center);
}

void PowerDeviceWidget::DevicePicker::add_row(lv_obj_t* parent, const char* component,
                                              const char* label_name, const std::string& label,
                                              const std::string& value, bool selected,
                                              PickFn on_pick) {
    const char* attrs[] = {
        "selected",
        selected ? "true" : "false",
        nullptr,
    };
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, component, attrs));
    if (!row) {
        return;
    }
    helix::ui::set_row_label_text(row, label_name, label.c_str());

    lv_obj_set_user_data(row, new RowPayload{this, value, on_pick});

    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] picker_row_cb");
            auto* target = lv_event_get_current_target_obj(e);
            auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
            if (!payload)
                return;

            // Copy out: hide() takes the row - and this payload - with it.
            RowPayload pick = *payload;
            pick.picker->hide();
            pick.on_pick(pick.picker->owner_, pick.value);
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] picker_row_delete_cb");
            auto* target = lv_event_get_current_target_obj(e);
            delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
            lv_obj_set_user_data(target, nullptr);
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_DELETE, nullptr);
}

void PowerDeviceWidget::DevicePicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* device_list = helix::ui::find_required(backdrop, "device_list", "PowerDeviceWidget");
    lv_obj_t* icon_grid = helix::ui::find_required(backdrop, "icon_grid", "PowerDeviceWidget");
    if (!device_list || !icon_grid) {
        return;
    }

    // Cap the list at a share of the screen so a printer with many devices
    // scrolls the list instead of growing the card past the panel.
    lv_obj_set_style_max_height(device_list, screen_height_pct(50), 0);

    auto pick_device = [](PowerDeviceWidget& w, const std::string& v) { w.select_device(v); };
    add_row(device_list, "picker_option_row", "option_label", lv_tr("All Devices"), "__all__",
            owner_.is_all_devices(), pick_device);

    auto device_names = PowerDeviceState::instance().device_names();
    std::sort(device_names.begin(), device_names.end());
    for (const auto& name : device_names) {
        add_row(device_list, "picker_option_row", "option_label",
                helix::get_display_name(name, helix::DeviceType::POWER_DEVICE), name,
                name == owner_.device_name_, pick_device);
    }

    std::string effective_icon = owner_.icon_name_.empty() ? kDefaultIcon : owner_.icon_name_;
    helix::ui::populate_icon_grid(icon_grid, kPowerIcons, kPowerIconCount, effective_icon,
                                  [this](const char* name) { owner_.select_icon(name); });
    tile_color::publish_palette(owner_.color_);

    lv_obj_t* sensor_section =
        helix::ui::find_required(backdrop, "sensor_section", "PowerDeviceWidget");
    lv_obj_t* sensor_grid = helix::ui::find_required(backdrop, "sensor_grid", "PowerDeviceWidget");
    auto energy_ids = SensorState::instance().energy_sensor_ids();
    if (energy_ids.empty() || !sensor_grid) {
        // Nothing to choose between, so the section is not built at all.
        if (sensor_section) {
            lv_obj_delete(sensor_section);
        }
    } else {
        std::sort(energy_ids.begin(), energy_ids.end());
        auto pick_sensor = [](PowerDeviceWidget& w, const std::string& v) { w.select_sensor(v); };
        add_row(sensor_grid, "picker_chip", "chip_label", lv_tr("None"), "",
                owner_.sensor_id_.empty(), pick_sensor);
        for (const auto& sid : energy_ids) {
            const auto info = SensorState::instance().get_sensor_info(sid);
            add_row(sensor_grid, "picker_chip", "chip_label", info ? info->friendly_name : sid, sid,
                    sid == owner_.sensor_id_, pick_sensor);
        }
    }

    spdlog::debug("[PowerDeviceWidget] Picker shown with {} devices", device_names.size());
}

// DECLARATIVE_OK: the grid cells are created in C++, so their selection border has
// no XML layer to bind to.
void PowerDeviceWidget::DevicePicker::refresh_icon_highlights() {
    lv_obj_t* icon_grid =
        menu() ? helix::ui::find_required(menu(), "icon_grid", "PowerDeviceWidget") : nullptr;
    if (!icon_grid) {
        return;
    }
    helix::ui::refresh_icon_grid(icon_grid,
                                 owner_.icon_name_.empty() ? kDefaultIcon : owner_.icon_name_);
}

void PowerDeviceWidget::select_sensor(const std::string& sensor_id) {
    sensor_id_ = sensor_id;
    save_config();
    show_energy_page();
}

void PowerDeviceWidget::select_device(const std::string& name) {
    device_name_ = name;
    apply_status_presence();
    save_config();

    // Re-attach to start observing the new device
    if (widget_obj_ && parent_screen_) {
        // Reset all observers before re-attaching
        status_observer_.reset();
        power_count_observer_.reset();

        if (is_all_devices()) {
            // __all__ mode: observe device count for aggregate refresh
            auto token = lifetime_.token();
            power_count_observer_ = helix::ui::observe<int>(
                get_printer_state().capabilities_state().subject(Capability::PowerDeviceCount),
                this,
                [token](PowerDeviceWidget* self, int /*count*/) {
                    if (token.expired())
                        return;
                    self->refresh_all_devices_state();
                },
                get_printer_state().get_subjects_lifetime());

            set_name_text(lv_tr("All Devices"));
        } else {
            // Single device mode: observe the specific device status
            SubjectLifetime lifetime;
            lv_subject_t* subj =
                PowerDeviceState::instance().get_status_subject(device_name_, lifetime);
            if (subj) {
                auto token = lifetime_.token();
                status_observer_ = helix::ui::observe<int>(
                    subj, this,
                    [token](PowerDeviceWidget* self, int status) {
                        if (token.expired())
                            return;
                        self->update_display(status);
                    },
                    lifetime);
            } else {
                // Nothing to report for this device yet: drop the previous
                // device's state rather than show it under the new name.
                update_display(-1);
            }

            set_name_text(
                helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE).c_str());
        }
    }

    spdlog::info("[PowerDeviceWidget] {} selected device: {}", instance_id_, name);
}

void PowerDeviceWidget::select_icon(const std::string& name) {
    // Store the ON variant so update_display can derive the OFF icon from the pair table
    std::string canonical(power_icon_to_on_variant(name.c_str()));
    icon_name_ = (canonical == kDefaultIcon) ? "" : canonical;
    save_config();

    // Update the widget icon immediately
    if (icon_obj_) {
        const char* effective = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        helix::ui::icon::set_source(icon_obj_, effective);
    }

    picker_.refresh_icon_highlights();

    spdlog::info("[PowerDeviceWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "power_cycle (default)" : icon_name_);
}

void PowerDeviceWidget::select_color(uint32_t rgb) {
    color_ = rgb;
    save_config();
    apply_state_colors(last_status_);
    tile_color::publish_selection(color_);
    spdlog::info("[PowerDeviceWidget] {} selected color: 0x{:06X}", instance_id_, color_);
}

void PowerDeviceWidget::save_config() {
    nlohmann::json config;
    config["device"] = device_name_;
    if (!icon_name_.empty())
        config["icon"] = icon_name_;
    if (!sensor_id_.empty())
        config["sensor"] = sensor_id_;
    if (color_ != 0)
        config["color"] = color_;
    save_widget_config(config);
    spdlog::debug("[PowerDeviceWidget] Saved config: {}={} icon={} sensor={}", instance_id_,
                  device_name_, icon_name_.empty() ? kDefaultIcon : icon_name_,
                  sensor_id_.empty() ? "(none)" : sensor_id_);
}

void PowerDeviceWidget::refresh_all_devices_state() {
    IMoonrakerAPI* api = get_api();
    if (!api)
        return;

    // Capture selected devices on UI thread before async API call
    auto& power_panel = get_global_power_panel();
    const auto& selected = power_panel.get_selected_devices();
    if (selected.empty()) {
        update_all_devices_display(false);
        return;
    }
    std::set<std::string> selected_set(selected.begin(), selected.end());

    auto token = lifetime_.token();
    api->get_power_devices(
        [this, token, selected_set](const std::vector<PowerDevice>& devices) {
            bool any_on = false;
            for (const auto& dev : devices) {
                if (selected_set.count(dev.device) > 0 && dev.status == "on") {
                    any_on = true;
                    break;
                }
            }

            token.defer("PowerDeviceWidget::refresh_all",
                        [this, any_on]() { update_all_devices_display(any_on); });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[PowerDeviceWidget] Failed to refresh all-devices state: {}",
                         err.message);
        });
}

void PowerDeviceWidget::handle_all_devices_toggle() {
    IMoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[PowerDeviceWidget] No API available for all-devices toggle");
        return;
    }

    auto& power_panel = get_global_power_panel();
    const auto& selected = power_panel.get_selected_devices();
    if (selected.empty()) {
        spdlog::warn("[PowerDeviceWidget] All-devices toggle: no devices selected");
        return;
    }

    const char* action = all_power_on_ ? "off" : "on";
    bool new_state = !all_power_on_;

    // Suppress recovery dialog when turning off (devices may have bound_services: klipper)
    if (!new_state) {
        EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::NORMAL);
    }

    spdlog::info("[PowerDeviceWidget] {} toggling all selected devices {}", instance_id_, action);
    auto token = lifetime_.token();
    for (const auto& device : selected) {
        api->set_device_power(
            device, action,
            [device]() {
                spdlog::debug("[PowerDeviceWidget] Power device '{}' set successfully", device);
            },
            [token, device](const MoonrakerError& err) {
                // See handle_clicked(): this callback runs on either the calling
                // thread or an HttpExecutor worker, so defer the LVGL-touching
                // notification through the token.
                spdlog::error("[PowerDeviceWidget] Failed to set power device '{}': {}", device,
                              err.message);
                token.defer("PowerDeviceWidget::all_toggle_error",
                            [device]() { NOTIFY_ERROR("Failed to toggle {}", device); });
            });
    }

    // Optimistically update display state
    all_power_on_ = new_state;
    update_all_devices_display(all_power_on_);
}

void PowerDeviceWidget::update_all_devices_display(bool any_on) {
    all_power_on_ = any_on;
    last_status_ = any_on ? 1 : 0;
    apply_state_colors(last_status_);

    if (icon_obj_) {
        const char* base_icon = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        const char* effective_icon = power_resolve_icon_for_state(base_icon, any_on ? 1 : 0);
        helix::ui::icon::set_source(icon_obj_, effective_icon);
    }

    lv_subject_set_int(&state_subject_, last_status_);
    lv_subject_set_string(&status_text_subject_, any_on ? lv_tr("ON") : lv_tr("OFF"));
    set_name_text(lv_tr("All Devices"));
}

std::string PowerDeviceWidget::auto_match_sensor() const {
    auto energy_ids = SensorState::instance().energy_sensor_ids();
    auto device_names = PowerDeviceState::instance().device_names();
    if (energy_ids.size() == 1 && device_names.size() == 1) {
        return energy_ids[0];
    }
    return "";
}

void PowerDeviceWidget::show_energy_page() {
    hide_energy_page();
    if (sensor_id_.empty() || !carousel_)
        return;
    if (!SensorState::instance().get_sensor_info(sensor_id_)) {
        spdlog::debug("[PowerDeviceWidget] Sensor '{}' not found, no energy page", sensor_id_);
        return;
    }
    helix::ui::carousel_set_trailing_tiles_reachable(carousel_, true);
    ui_carousel_set_real_page_count(carousel_, -1);
    attach_sensor_observers();
    spdlog::debug("[PowerDeviceWidget] Energy page shown for sensor '{}'", sensor_id_);
}

void PowerDeviceWidget::hide_energy_page() {
    detach_sensor_observers();
    for (size_t i = 0; i < kEnergyReadingCount; ++i) {
        lv_subject_set_string(&readings_[i].text, kEnergyKeys[i].placeholder);
    }
    if (!carousel_)
        return;
    helix::ui::carousel_set_trailing_tiles_reachable(carousel_, false);
    ui_carousel_set_real_page_count(carousel_, 1);
    ui_carousel_goto_page(carousel_, 0, false);
}

void PowerDeviceWidget::attach_sensor_observers() {
    if (sensor_id_.empty())
        return;
    auto token = lifetime_.token();
    for (size_t i = 0; i < kEnergyReadingCount; ++i) {
        EnergyReading& r = readings_[i];
        r.lifetime = {};
        lv_subject_t* subj =
            SensorState::instance().get_value_subject(sensor_id_, kEnergyKeys[i].key, r.lifetime);
        if (!subj)
            continue;
        r.observer = helix::ui::observe<int>(
            subj, this,
            [token, i](PowerDeviceWidget* self, int centi_value) {
                if (token.expired())
                    return;
                lv_subject_set_string(
                    &self->readings_[i].text,
                    SensorState::format_value(kEnergyKeys[i].key, centi_value).c_str());
            },
            r.lifetime);
    }
}

void PowerDeviceWidget::detach_sensor_observers() {
    // Lifetimes reset BEFORE observers so the guard's weak_ptr expires and
    // skips lv_observer_delete() on potentially-freed subjects (#705).
    for (auto& r : readings_) {
        r.lifetime = {};
    }
    for (auto& r : readings_) {
        r.observer.reset();
    }
}
