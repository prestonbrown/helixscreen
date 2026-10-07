// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_widget.h"

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
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "power_device_icon.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "sensor_state.h"
#include "theme_manager.h"
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
}
} // namespace helix

namespace {

// Power-related icons for the picker grid
static const char* const POWER_ICONS[] = {
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
static constexpr size_t POWER_ICON_COUNT = std::size(POWER_ICONS);
static constexpr const char* DEFAULT_ICON = "power_cycle";

} // namespace

using namespace helix;

// The state is the reading ("LOCKED" is the widest), the device name the label,
// drawn whatever show_widget_labels says, and the glyph sits in a disc that
// scales with it.
PowerDeviceWidget::PowerDeviceWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id, status_content(false)), instance_id_(instance_id) {
    // Registered before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    UI_MANAGED_SUBJECT_INT(has_status_subject_, 0, has_status_name_.c_str(), subjects_);
    sizing_.add_subject_attr("status_subject", has_status_name_);
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
    if (config.contains("device") && config["device"].is_string()) {
        device_name_ = config["device"].get<std::string>();
    }
    if (config.contains("icon") && config["icon"].is_string()) {
        icon_name_ = config["icon"].get<std::string>();
    }
    if (config.contains("sensor") && config["sensor"].is_string()) {
        sensor_id_ = config["sensor"].get<std::string>();
    }
    spdlog::debug("[PowerDeviceWidget] Config: {}={} icon={}", instance_id_,
                  device_name_.empty() ? "(unconfigured)" : device_name_,
                  icon_name_.empty() ? DEFAULT_ICON : icon_name_);
    apply_status_presence();
}

void PowerDeviceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        // Pressed feedback
        lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Cache LVGL object pointers from XML
    badge_obj_ = lv_obj_find_by_name(widget_obj_, "power_badge");
    icon_obj_ = lv_obj_find_by_name(widget_obj_, "power_icon");
    name_label_ = lv_obj_find_by_name(widget_obj_, "power_device_name");
    status_label_ = lv_obj_find_by_name(widget_obj_, "power_device_status");
    lock_icon_ = lv_obj_find_by_name(widget_obj_, "power_lock_icon");

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

        if (name_label_) {
            lv_label_set_text(name_label_, lv_tr("All Devices"));
        }
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

            // Set initial display name
            if (name_label_) {
                std::string display =
                    helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE);
                lv_label_set_text(name_label_, display.c_str());
            }
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
    setup_carousel();

    spdlog::debug("[PowerDeviceWidget] Attached {} (device: {})", instance_id_,
                  device_name_.empty() ? "none" : device_name_);
}

void PowerDeviceWidget::detach() {
    teardown_carousel();
    lifetime_.invalidate();
    picker_.hide();

    status_observer_.reset();
    power_count_observer_.reset();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    badge_obj_ = nullptr;
    icon_obj_ = nullptr;
    name_label_ = nullptr;
    status_label_ = nullptr;
    lock_icon_ = nullptr;

    spdlog::debug("[PowerDeviceWidget] Detached");
}

void PowerDeviceWidget::update_display(int status) {
    // Status values: 0=off, 1=on, 2=locked, -1=unconfigured

    if (badge_obj_) {
        switch (status) {
        case 1: // ON
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("danger"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 40, 0);
            break;
        case 0: // OFF
        case 2: // LOCKED
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 20, 0);
            break;
        default: // Unconfigured
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("secondary"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 20, 0);
            break;
        }
    }

    if (icon_obj_) {
        // Apply icon — for paired icons, toggle between on/off variants
        const char* base_icon = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        const char* effective_icon = power_resolve_icon_for_state(base_icon, status);
        helix::ui::icon::set_source(icon_obj_, effective_icon);

        switch (status) {
        case 1:
            helix::ui::icon::set_variant(icon_obj_, "danger");
            break;
        case 0:
        case 2:
            helix::ui::icon::set_variant(icon_obj_, "muted");
            break;
        default:
            helix::ui::icon::set_variant(icon_obj_, "secondary");
            break;
        }
    }

    if (lock_icon_) {
        if (status == 2) {
            lv_obj_remove_flag(lock_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lock_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (status_label_) {
        switch (status) {
        case 1:
            lv_label_set_text(status_label_, lv_tr("ON"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("danger"), 0);
            break;
        case 0:
            lv_label_set_text(status_label_, lv_tr("OFF"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("text_muted"), 0);
            break;
        case 2:
            lv_label_set_text(status_label_, lv_tr("LOCKED"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("text_muted"), 0);
            break;
        default:
            lv_label_set_text(status_label_, "");
            break;
        }
    }

    if (name_label_ && status == -1) {
        // A tag, not lv_tr() text: the label then re-translates itself.
        lv_label_set_translation_tag(name_label_, "Configure");
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
            EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);
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

    std::string effective_icon = owner_.icon_name_.empty() ? DEFAULT_ICON : owner_.icon_name_;
    helix::ui::populate_icon_grid(icon_grid, POWER_ICONS, POWER_ICON_COUNT, effective_icon,
                                  [this](const char* name) { owner_.select_icon(name); });

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
            auto* info = SensorState::instance().get_sensor_info(sid);
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
                                 owner_.icon_name_.empty() ? DEFAULT_ICON : owner_.icon_name_);
}

void PowerDeviceWidget::select_sensor(const std::string& sensor_id) {
    teardown_carousel();
    sensor_id_ = sensor_id;
    save_config();
    setup_carousel();
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

            if (name_label_) {
                lv_label_set_text(name_label_, lv_tr("All Devices"));
            }
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
            }

            // Update display name
            if (name_label_) {
                std::string display =
                    helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE);
                lv_label_set_text(name_label_, display.c_str());
            }
        }
    }

    spdlog::info("[PowerDeviceWidget] {} selected device: {}", instance_id_, name);
}

void PowerDeviceWidget::select_icon(const std::string& name) {
    // Store the ON variant so update_display can derive the OFF icon from the pair table
    std::string canonical(power_icon_to_on_variant(name.c_str()));
    icon_name_ = (canonical == DEFAULT_ICON) ? "" : canonical;
    save_config();

    // Update the widget icon immediately
    if (icon_obj_) {
        const char* effective = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        helix::ui::icon::set_source(icon_obj_, effective);
    }

    picker_.refresh_icon_highlights();

    spdlog::info("[PowerDeviceWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "power_cycle (default)" : icon_name_);
}

void PowerDeviceWidget::save_config() {
    nlohmann::json config;
    config["device"] = device_name_;
    if (!icon_name_.empty())
        config["icon"] = icon_name_;
    if (!sensor_id_.empty())
        config["sensor"] = sensor_id_;
    save_widget_config(config);
    spdlog::debug("[PowerDeviceWidget] Saved config: {}={} icon={} sensor={}", instance_id_,
                  device_name_, icon_name_.empty() ? DEFAULT_ICON : icon_name_,
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
            if (token.expired())
                return;
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
        EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);
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

    if (badge_obj_) {
        if (any_on) {
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("danger"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 40, 0);
        } else {
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 20, 0);
        }
    }

    if (icon_obj_) {
        const char* base_icon = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        const char* effective_icon = power_resolve_icon_for_state(base_icon, any_on ? 1 : 0);
        helix::ui::icon::set_source(icon_obj_, effective_icon);
        helix::ui::icon::set_variant(icon_obj_, any_on ? "danger" : "muted");
    }

    if (lock_icon_) {
        lv_obj_add_flag(lock_icon_, LV_OBJ_FLAG_HIDDEN);
    }

    if (status_label_) {
        lv_label_set_text(status_label_, any_on ? lv_tr("ON") : lv_tr("OFF"));
        lv_obj_set_style_text_color(status_label_,
                                    theme_manager_get_color(any_on ? "danger" : "text_muted"), 0);
    }

    if (name_label_) {
        lv_label_set_text(name_label_, lv_tr("All Devices"));
    }
}

std::string PowerDeviceWidget::auto_match_sensor() const {
    auto energy_ids = SensorState::instance().energy_sensor_ids();
    auto device_names = PowerDeviceState::instance().device_names();
    if (energy_ids.size() == 1 && device_names.size() == 1) {
        return energy_ids[0];
    }
    return "";
}

void PowerDeviceWidget::setup_carousel() {
    if (sensor_id_.empty() || !widget_obj_)
        return;

    // Verify sensor exists
    auto* info = SensorState::instance().get_sensor_info(sensor_id_);
    if (!info) {
        spdlog::debug("[PowerDeviceWidget] Sensor '{}' not found, skipping carousel", sensor_id_);
        return;
    }

    // Collect existing children of widget_ before creating carousel
    std::vector<lv_obj_t*> existing_children;
    uint32_t child_count = lv_obj_get_child_count(widget_obj_);
    for (uint32_t i = 0; i < child_count; ++i) {
        existing_children.push_back(lv_obj_get_child(widget_obj_, static_cast<int32_t>(i)));
    }

    // Create carousel as child of widget_
    carousel_ = ui_carousel_create_obj(widget_obj_);
    if (!carousel_) {
        spdlog::warn("[PowerDeviceWidget] Failed to create carousel");
        return;
    }

    lv_obj_set_size(carousel_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(carousel_, 0, 0);
    lv_obj_set_style_bg_opa(carousel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(carousel_, 0, 0);

    auto* state = ui_carousel_get_state(carousel_);
    if (state) {
        state->wrap = true;
        state->show_indicators = true;
    }

    // Page 1 (control): create a container and reparent existing children into it
    lv_obj_t* control_page = lv_obj_create(carousel_);
    lv_obj_set_size(control_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(control_page, 0, 0);
    lv_obj_set_style_bg_opa(control_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(control_page, 0, 0);
    lv_obj_remove_flag(control_page, LV_OBJ_FLAG_SCROLLABLE);

    // Copy the original widget's flex layout to the control page
    lv_obj_set_flex_flow(control_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(control_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Allow click events to bubble up from control page
    lv_obj_add_flag(control_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(control_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (auto* child : existing_children) {
        lv_obj_set_parent(child, control_page);
    }

    ui_carousel_add_item(carousel_, control_page);

    // Page 2 (energy): create container and instantiate XML component
    lv_obj_t* energy_container = lv_obj_create(carousel_);
    lv_obj_set_size(energy_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(energy_container, 0, 0);
    lv_obj_set_style_bg_opa(energy_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(energy_container, 0, 0);
    lv_obj_remove_flag(energy_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(energy_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(energy_container, LV_OBJ_FLAG_EVENT_BUBBLE);

    energy_page_ = static_cast<lv_obj_t*>(
        lv_xml_create(energy_container, "power_device_energy_page", nullptr));
    if (!energy_page_) {
        spdlog::warn("[PowerDeviceWidget] Failed to create energy page XML");
        // Remove the carousel and restore children
        for (auto* child : existing_children) {
            lv_obj_set_parent(child, widget_obj_);
        }
        // Same indev-dispatch constraint as teardown_carousel(): setup_carousel()
        // also runs from the sensor-chip click handler.
        helix::ui::safe_delete_deferred(carousel_);
        return;
    }

    ui_carousel_add_item(carousel_, energy_container);

    // Cache label pointers from the energy page XML
    energy_power_label_ =
        helix::ui::find_required(energy_page_, "energy_power_label", "PowerDeviceWidget");
    energy_voltage_label_ =
        helix::ui::find_required(energy_page_, "energy_voltage_label", "PowerDeviceWidget");
    energy_current_label_ =
        helix::ui::find_required(energy_page_, "energy_current_label", "PowerDeviceWidget");
    energy_energy_label_ =
        helix::ui::find_required(energy_page_, "energy_energy_label", "PowerDeviceWidget");

    // Rebuild indicators to show 2 dots
    ui_carousel_rebuild_indicators(carousel_);

    attach_sensor_observers();

    spdlog::debug("[PowerDeviceWidget] Carousel setup complete for sensor '{}'", sensor_id_);
}

void PowerDeviceWidget::teardown_carousel() {
    detach_sensor_observers();

    if (carousel_ && widget_obj_) {
        // Get carousel state to access the scroll container
        auto* state = ui_carousel_get_state(carousel_);
        if (state && state->scroll_container) {
            // Page 0 is the control page — reparent its children back to widget_
            lv_obj_t* first_tile = lv_obj_get_child(state->scroll_container, 0);
            if (first_tile) {
                // The tile wraps a control_page container; get the control_page
                lv_obj_t* control_page = lv_obj_get_child(first_tile, 0);
                if (control_page) {
                    // Collect children before reparenting (iteration invalidation)
                    std::vector<lv_obj_t*> children;
                    uint32_t count = lv_obj_get_child_count(control_page);
                    for (uint32_t i = 0; i < count; ++i) {
                        children.push_back(lv_obj_get_child(control_page, static_cast<int32_t>(i)));
                    }
                    for (auto* child : children) {
                        lv_obj_set_parent(child, widget_obj_);
                    }
                }
            }
        }

        // Reached from the sensor-chip LV_EVENT_CLICKED handler, i.e. from inside
        // LVGL's indev dispatch — a synchronous delete corrupts the parent's child
        // iteration. safe_delete_deferred() detaches the carousel to lv_layer_top()
        // right now (so the setup_carousel() that follows never sees it among
        // widget_obj_'s children) and async-deletes it on the next tick.
        helix::ui::safe_delete_deferred(carousel_);
    }

    energy_page_ = nullptr;
    energy_power_label_ = nullptr;
    energy_voltage_label_ = nullptr;
    energy_current_label_ = nullptr;
    energy_energy_label_ = nullptr;
    carousel_ = nullptr;
}

void PowerDeviceWidget::attach_sensor_observers() {
    if (sensor_id_.empty())
        return;
    auto& sensor_state = SensorState::instance();
    auto token = lifetime_.token();

    auto observe_key = [&](const std::string& key, lv_obj_t* label, ObserverGuard& guard,
                           SubjectLifetime& lt) {
        lt = {};
        auto* subj = sensor_state.get_value_subject(sensor_id_, key, lt);
        if (!subj || !label)
            return;
        std::string key_copy = key;
        guard = helix::ui::observe<int>(
            subj, this,
            [token, key_copy, label](PowerDeviceWidget* self, int centi_value) {
                if (token.expired())
                    return;
                self->update_energy_label(key_copy, label, centi_value);
            },
            lt);
    };

    observe_key("power", energy_power_label_, power_observer_, power_lifetime_);
    observe_key("voltage", energy_voltage_label_, voltage_observer_, voltage_lifetime_);
    observe_key("current", energy_current_label_, current_observer_, current_lifetime_);
    observe_key("energy", energy_energy_label_, energy_observer_, energy_lifetime_);
}

void PowerDeviceWidget::detach_sensor_observers() {
    // Reset lifetimes BEFORE observers so the guard's weak_ptr expires and
    // skips lv_observer_remove() on potentially-freed subjects (#705).
    power_lifetime_ = {};
    voltage_lifetime_ = {};
    current_lifetime_ = {};
    energy_lifetime_ = {};
    power_observer_.reset();
    voltage_observer_.reset();
    current_observer_.reset();
    energy_observer_.reset();
}

void PowerDeviceWidget::update_energy_label(const std::string& key, lv_obj_t* label,
                                            int centi_value) {
    if (!label)
        return;
    auto text = SensorState::format_value(key, centi_value);
    lv_label_set_text(label, text.c_str());
}
