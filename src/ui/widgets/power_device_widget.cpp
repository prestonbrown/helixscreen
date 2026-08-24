// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_widget.h"

#include "ui_carousel.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_panel_power.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "sensor_state.h"
#include "theme_manager.h"

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

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

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
static constexpr int ICON_CELL_SIZE = 36;
static constexpr const char* DEFAULT_ICON = "power_cycle";

// Icons with distinct on/off glyphs. Config always stores the ON variant;
// resolve_icon_for_state() derives the OFF variant from this table.
struct IconPair {
    const char* on_icon;
    const char* off_icon;
};
static const IconPair ICON_PAIRS[] = {
    {"power_on", "power_off"},
    {"power_plug", "power_plug_off"},
    {"lightbulb_on", "lightbulb_outline"},
    {"fan", "fan_off"},
};

/// Map an off-variant icon name to its on-variant (e.g., "fan_off" → "fan").
/// Returns the input unchanged if it's not an off-variant.
static const char* to_on_variant(const char* icon) {
    for (const auto& pair : ICON_PAIRS) {
        if (std::strcmp(icon, pair.off_icon) == 0)
            return pair.on_icon;
    }
    return icon;
}

/// Return the icon to display for a given power status.
/// For paired icons, returns the off-variant when the device is off/locked.
static const char* resolve_icon_for_state(const char* base_icon, int status) {
    if (status == 1)
        return base_icon;
    for (const auto& pair : ICON_PAIRS) {
        if (std::strcmp(base_icon, pair.on_icon) == 0)
            return pair.off_icon;
    }
    return base_icon;
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

} // namespace

using namespace helix;

PowerDeviceWidget* PowerDeviceWidget::s_active_picker_ = nullptr;

PowerDeviceWidget::PowerDeviceWidget(const std::string& instance_id) : instance_id_(instance_id) {}

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
}

void PowerDeviceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);

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
        power_count_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
            get_printer_state().get_power_device_count_subject(), this,
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
            status_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
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
    dismiss_device_picker();

    status_observer_.reset();
    power_count_observer_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
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
        const char* effective_icon = resolve_icon_for_state(base_icon, status);
        ui_icon_set_source(icon_obj_, effective_icon);

        switch (status) {
        case 1:
            ui_icon_set_variant(icon_obj_, "danger");
            break;
        case 0:
        case 2:
            ui_icon_set_variant(icon_obj_, "muted");
            break;
        default:
            ui_icon_set_variant(icon_obj_, "secondary");
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
        lv_label_set_text(name_label_, lv_tr("Configure"));
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
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    // Dismiss any other widget's picker
    if (s_active_picker_ && s_active_picker_ != this) {
        s_active_picker_->dismiss_device_picker();
    }

    auto device_names = PowerDeviceState::instance().device_names();
    if (device_names.empty()) {
        spdlog::warn("[PowerDeviceWidget] No power devices available");
        return;
    }
    std::sort(device_names.begin(), device_names.end());

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);
    int space_md = resolve_space_token("space_md", 10);

    int screen_w = lv_obj_get_width(parent_screen_);
    int screen_h = lv_obj_get_height(parent_screen_);

    // Backdrop (full screen, transparent, catches clicks to dismiss)
    picker_backdrop_ = lv_obj_create(parent_screen_);
    lv_obj_set_size(picker_backdrop_, screen_w, screen_h);
    lv_obj_set_pos(picker_backdrop_, 0, 0);
    lv_obj_set_style_bg_color(picker_backdrop_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(picker_backdrop_, LV_OPA_50, 0);
    lv_obj_set_style_border_width(picker_backdrop_, 0, 0);
    lv_obj_set_style_radius(picker_backdrop_, 0, 0);
    lv_obj_remove_flag(picker_backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(picker_backdrop_, LV_OBJ_FLAG_CLICKABLE);

    // Backdrop click dismisses picker
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] backdrop_cb");
            if (s_active_picker_) {
                s_active_picker_->dismiss_device_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    // Card container — two-column layout
    lv_obj_t* card = lv_obj_create(picker_backdrop_);
    int card_w = std::clamp(screen_w * 60 / 100, 260, 420);
    int card_h = std::clamp(screen_h * 65 / 100, 200, 380);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_style_bg_color(card, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, theme_manager_get_color("border"), 0);
    lv_obj_set_style_pad_all(card, space_md, 0);
    lv_obj_set_style_pad_gap(card, space_sm, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // === Left column: Device list (scrollable) ===
    lv_obj_t* left_col = lv_obj_create(card);
    lv_obj_set_width(left_col, 1);
    lv_obj_set_flex_grow(left_col, 2);
    lv_obj_set_height(left_col, LV_PCT(100));
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left_col, 0, 0);
    lv_obj_set_style_pad_gap(left_col, space_xs, 0);
    lv_obj_set_style_bg_opa(left_col, 0, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);

    // "Device" header
    lv_obj_t* dev_title = lv_label_create(left_col);
    lv_label_set_text(dev_title, lv_tr("Device"));
    lv_obj_set_style_text_font(dev_title, theme_manager_get_font("xs"), 0);
    lv_obj_set_style_text_color(dev_title, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_width(dev_title, LV_PCT(100));

    // Scrollable device list
    lv_obj_t* list = lv_obj_create(left_col);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_gap(list, theme_manager_get_spacing("space_xxs"), 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    // Helper lambda to create a device row in the picker
    auto create_device_row = [&](const std::string& device_id, const std::string& display,
                                 bool is_selected) {
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(row, 6, 0);

        // Highlight selected row
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);
        if (is_selected) {
            lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
        }

        // Pressed feedback
        lv_obj_set_style_bg_color(row, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        // Device display name
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store device name for click handler
        auto* name_copy = new std::string(device_id);
        lv_obj_set_user_data(row, name_copy);

        // Free heap string when row is deleted
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) { delete static_cast<std::string*>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, name_copy);

        // Click selects device and dismisses picker
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] device_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(ev));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                if (PowerDeviceWidget::s_active_picker_) {
                    std::string selected = *name_ptr;
                    PowerDeviceWidget::s_active_picker_->select_device(selected);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    };

    // "All Devices" option at top of list
    create_device_row("__all__", lv_tr("All Devices"), device_name_ == "__all__");

    // Individual device entries
    for (const auto& name : device_names) {
        std::string display = helix::get_display_name(name, helix::DeviceType::POWER_DEVICE);
        create_device_row(name, display, name == device_name_);
    }

    // Vertical divider between columns
    lv_obj_t* v_divider = lv_obj_create(card);
    lv_obj_set_width(v_divider, 1);
    lv_obj_set_height(v_divider, LV_PCT(100));
    lv_obj_set_style_bg_color(v_divider, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_bg_opa(v_divider, 38, 0);
    lv_obj_set_style_pad_all(v_divider, 0, 0);
    lv_obj_set_style_border_width(v_divider, 0, 0);
    lv_obj_remove_flag(v_divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v_divider, LV_OBJ_FLAG_CLICKABLE);

    // === Right column: Icon grid + Sensor ===
    lv_obj_t* right_col = lv_obj_create(card);
    lv_obj_set_width(right_col, 1);
    lv_obj_set_flex_grow(right_col, 4);
    lv_obj_set_height(right_col, LV_PCT(100));
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right_col, 0, 0);
    lv_obj_set_style_pad_gap(right_col, space_xs, 0);
    lv_obj_set_style_bg_opa(right_col, 0, 0);
    lv_obj_set_style_border_width(right_col, 0, 0);

    // "Icon" header
    lv_obj_t* icon_title = lv_label_create(right_col);
    lv_label_set_text(icon_title, lv_tr("Icon"));
    lv_obj_set_style_text_font(icon_title, theme_manager_get_font("xs"), 0);
    lv_obj_set_style_text_color(icon_title, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_width(icon_title, LV_PCT(100));

    // Icon grid (wrap flow)
    lv_obj_t* icon_grid = lv_obj_create(right_col);
    lv_obj_set_name(icon_grid, "picker_icon_grid");
    lv_obj_set_width(icon_grid, LV_PCT(100));
    lv_obj_set_height(icon_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icon_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(icon_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(icon_grid, 0, 0);
    lv_obj_set_style_pad_gap(icon_grid, theme_manager_get_spacing("space_xxs"), 0);
    lv_obj_set_style_bg_opa(icon_grid, 0, 0);
    lv_obj_set_style_border_width(icon_grid, 0, 0);
    lv_obj_remove_flag(icon_grid, LV_OBJ_FLAG_SCROLLABLE);

    std::string effective_icon = icon_name_.empty() ? DEFAULT_ICON : icon_name_;

    for (size_t i = 0; i < POWER_ICON_COUNT; ++i) {
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

        apply_icon_cell_highlight(cell, POWER_ICONS[i] == effective_icon);

        // Icon glyph
        const char* cp = ui_icon::lookup_codepoint(POWER_ICONS[i]);
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

        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] icon_cell_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto idx =
                    static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                if (idx < POWER_ICON_COUNT && PowerDeviceWidget::s_active_picker_) {
                    PowerDeviceWidget::s_active_picker_->select_icon(POWER_ICONS[idx]);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // === Sensor section (in right column, below icon grid) ===
    auto energy_ids = SensorState::instance().energy_sensor_ids();
    if (!energy_ids.empty()) {
        std::sort(energy_ids.begin(), energy_ids.end());

        // "Sensor" header
        lv_obj_t* sensor_title = lv_label_create(right_col);
        lv_label_set_text(sensor_title, lv_tr("Sensor"));
        lv_obj_set_style_text_font(sensor_title, theme_manager_get_font("xs"), 0);
        lv_obj_set_style_text_color(sensor_title, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_width(sensor_title, LV_PCT(100));

        // Sensor chip container (wrap flow)
        lv_obj_t* sensor_grid = lv_obj_create(right_col);
        lv_obj_set_width(sensor_grid, LV_PCT(100));
        lv_obj_set_height(sensor_grid, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sensor_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(sensor_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(sensor_grid, 0, 0);
        lv_obj_set_style_pad_gap(sensor_grid, space_xs, 0);
        lv_obj_set_style_bg_opa(sensor_grid, 0, 0);
        lv_obj_set_style_border_width(sensor_grid, 0, 0);
        lv_obj_remove_flag(sensor_grid, LV_OBJ_FLAG_SCROLLABLE);

        // Helper to create a sensor chip
        auto make_chip = [&](const char* label_text, const std::string& sensor_id, bool selected) {
            lv_obj_t* chip = lv_obj_create(sensor_grid);
            lv_obj_set_height(chip, LV_SIZE_CONTENT);
            lv_obj_set_width(chip, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_ver(chip, space_xs, 0);
            lv_obj_set_style_pad_hor(chip, space_sm, 0);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

            // Highlight if selected
            if (selected) {
                lv_obj_set_style_border_width(chip, 2, 0);
                lv_obj_set_style_border_color(chip, theme_manager_get_color("primary"), 0);
                lv_obj_set_style_bg_opa(chip, 20, 0);
                lv_obj_set_style_bg_color(chip, theme_manager_get_color("primary"), 0);
            } else {
                lv_obj_set_style_border_width(chip, 1, 0);
                lv_obj_set_style_border_color(chip, theme_manager_get_color("border"), 0);
                lv_obj_set_style_bg_opa(chip, 0, 0);
            }

            // Pressed feedback
            lv_obj_set_style_bg_color(chip, theme_manager_get_color("text_muted"),
                                      LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(chip, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

            lv_obj_t* lbl = lv_label_create(chip);
            lv_label_set_text(lbl, label_text);
            lv_obj_set_style_text_font(lbl, lv_font_get_default(), 0);
            lv_obj_set_style_text_color(lbl, theme_manager_get_color("text"), 0);
            lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

            // Store sensor ID as user_data (heap-allocated string)
            auto* id_copy = new std::string(sensor_id);
            lv_obj_set_user_data(chip, id_copy);

            // Free heap string on deletion (L069 pattern)
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t* ev) {
                    delete static_cast<std::string*>(lv_event_get_user_data(ev));
                },
                LV_EVENT_DELETE, id_copy);

            // Click handler
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t* ev) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] sensor_chip_cb");
                    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(ev));
                    auto* id_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                    if (!id_ptr || !PowerDeviceWidget::s_active_picker_)
                        return;

                    auto* self = PowerDeviceWidget::s_active_picker_;
                    std::string new_sensor = *id_ptr;

                    // Teardown old carousel, update sensor, setup new one
                    self->teardown_carousel();
                    self->sensor_id_ = new_sensor;
                    self->save_config();
                    self->setup_carousel();
                    self->dismiss_device_picker();
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, nullptr);
        };

        // "None" chip
        make_chip(lv_tr("None"), "", sensor_id_.empty());

        // One chip per energy sensor
        for (const auto& sid : energy_ids) {
            auto* info = SensorState::instance().get_sensor_info(sid);
            std::string label = info ? info->friendly_name : sid;
            make_chip(label.c_str(), sid, sid == sensor_id_);
        }
    }

    s_active_picker_ = this;

    // Self-clearing delete callback for parent deletion safety
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* ev) {
            auto* self = static_cast<PowerDeviceWidget*>(lv_event_get_user_data(ev));
            if (!self)
                return;
            self->picker_backdrop_ = nullptr;
            if (s_active_picker_ == self) {
                s_active_picker_ = nullptr;
            }
        },
        LV_EVENT_DELETE, this);

    // Position card near the widget
    if (card && widget_obj_) {
        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;

        // Clamp to screen bounds
        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;

        int card_max_h = screen_h * 70 / 100;
        if (card_y + card_max_h > screen_h - space_md) {
            card_y = widget_area.y1 - card_max_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[PowerDeviceWidget] Picker shown with {} devices", device_names.size());
}

void PowerDeviceWidget::dismiss_device_picker() {
    if (!picker_backdrop_) {
        return;
    }

    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;
    s_active_picker_ = nullptr;

    if (lv_obj_is_valid(backdrop)) {
        helix::ui::safe_delete_deferred(backdrop);
    }

    spdlog::debug("[PowerDeviceWidget] Picker dismissed");
}

void PowerDeviceWidget::select_device(const std::string& name) {
    device_name_ = name;
    save_config();
    dismiss_device_picker();

    // Re-attach to start observing the new device
    if (widget_obj_ && parent_screen_) {
        // Reset all observers before re-attaching
        status_observer_.reset();
        power_count_observer_.reset();

        if (is_all_devices()) {
            // __all__ mode: observe device count for aggregate refresh
            auto token = lifetime_.token();
            power_count_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
                get_printer_state().get_power_device_count_subject(), this,
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
                status_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
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
    std::string canonical(to_on_variant(name.c_str()));
    icon_name_ = (canonical == DEFAULT_ICON) ? "" : canonical;
    save_config();

    // Update the widget icon immediately
    if (icon_obj_) {
        const char* effective = icon_name_.empty() ? DEFAULT_ICON : icon_name_.c_str();
        ui_icon_set_source(icon_obj_, effective);
    }

    // Update icon grid highlights if picker is still open
    if (picker_backdrop_) {
        lv_obj_t* icon_grid = lv_obj_find_by_name(picker_backdrop_, "picker_icon_grid");
        if (icon_grid) {
            std::string effective_icon = icon_name_.empty() ? DEFAULT_ICON : icon_name_;
            uint32_t grid_count = lv_obj_get_child_count(icon_grid);
            for (uint32_t i = 0; i < grid_count; ++i) {
                lv_obj_t* cell = lv_obj_get_child(icon_grid, i);
                auto idx =
                    static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
                if (idx < POWER_ICON_COUNT) {
                    apply_icon_cell_highlight(cell, POWER_ICONS[idx] == effective_icon);
                }
            }
        }
    }

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
        const char* effective_icon = resolve_icon_for_state(base_icon, any_on ? 1 : 0);
        ui_icon_set_source(icon_obj_, effective_icon);
        ui_icon_set_variant(icon_obj_, any_on ? "danger" : "muted");
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
    energy_power_label_ = lv_obj_find_by_name(energy_page_, "energy_power_label");
    energy_voltage_label_ = lv_obj_find_by_name(energy_page_, "energy_voltage_label");
    energy_current_label_ = lv_obj_find_by_name(energy_page_, "energy_current_label");
    energy_energy_label_ = lv_obj_find_by_name(energy_page_, "energy_energy_label");

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
        guard = helix::ui::observe_int_sync<PowerDeviceWidget>(
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
