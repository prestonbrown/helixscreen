// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_widget.h"

#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {
void register_fan_widget() {
    register_widget_factory("fan",
                            [](const std::string& id) { return std::make_unique<FanWidget>(id); });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "fan_widget_clicked_cb", FanWidget::fan_widget_clicked_cb);
}
} // namespace helix

using namespace helix;

namespace {

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

} // anonymous namespace

FanWidget::FanWidget(const std::string& instance_id) : instance_id_(instance_id) {
    std::strcpy(speed_buffer_, "--");
}

FanWidget::~FanWidget() {
    detach();
}

void FanWidget::set_config(const nlohmann::json& config) {
    config_ = config;

    if (config.contains("fan") && config["fan"].is_string()) {
        selected_fan_ = config["fan"].get<std::string>();
        resolve_display_name();
        spdlog::debug("[FanWidget] Config: fan={}", selected_fan_);
    }
}

std::string FanWidget::get_component_name() const {
    return "panel_widget_fan";
}

void FanWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);
    }

    // Cache label pointers
    speed_label_ = lv_obj_find_by_name(widget_obj_, "fan_speed");
    name_label_ = lv_obj_find_by_name(widget_obj_, "fan_name");
    fan_icon_ = lv_obj_find_by_name(widget_obj_, "fan_widget_icon");

    // Only bind if fans are already discovered (e.g. reconnection, panel re-entry).
    // At startup, fan subjects don't exist yet — the version observer (below)
    // handles binding once init_fans() bumps fans_version.
    if (selected_fan_.empty()) {
        auto_select_first_fan();
    } else if (!get_printer_state().get_fans().empty()) {
        bind_speed_observer();
        update_display();
    } else {
        update_display();
    }

    // Observe fan version to detect fan discovery/reconnection
    auto token = lifetime_.token();
    version_observer_ = helix::ui::observe_int_sync<FanWidget>(
        get_printer_state().get_fans_version_subject(), this,
        [token](FanWidget* self, int /*version*/) {
            if (token.expired())
                return;
            if (self->selected_fan_.empty()) {
                self->auto_select_first_fan();
            } else {
                // Rebind in case subjects were recreated
                // after reconnection
                self->bind_speed_observer();
                self->update_display();
            }
        },
        get_printer_state().get_subjects_lifetime());

    spdlog::debug("[FanWidget] Attached (fan: {})", selected_fan_.empty() ? "none" : selected_fan_);
}

void FanWidget::detach() {
    lifetime_.invalidate();
    picker_.hide();
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        version_observer_.reset();
        speed_lifetime_.reset(); // Before observer: expire weak_ptr if subject already freed
        speed_observer_.reset();
    }

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    fan_control_panel_ = nullptr;
    speed_label_ = nullptr;
    name_label_ = nullptr;
    fan_icon_ = nullptr;

    spdlog::debug("[FanWidget] Detached");
}

void FanWidget::handle_clicked() {
    spdlog::debug("[FanWidget] Clicked - opening fan control overlay");

    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(get_moonraker_api());

        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            spdlog::error("[FanWidget] Failed to create fan control overlay");
            return;
        }
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        get_fan_control_overlay().set_api(get_moonraker_api());
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}

void FanWidget::resolve_display_name() {
    const auto& fans = get_printer_state().get_fans();
    for (const auto& fan : fans) {
        if (fan.object_name == selected_fan_) {
            display_name_ = fan.display_name;
            return;
        }
    }
    display_name_ = selected_fan_; // fallback
}

void FanWidget::auto_select_first_fan() {
    const auto& fans = get_printer_state().get_fans();
    if (!fans.empty()) {
        select_fan(fans.front().object_name);
    }
}

void FanWidget::bind_speed_observer() {
    // Reset lifetime BEFORE observer so the guard's weak_ptr expires and
    // skips lv_observer_remove() on a potentially-freed subject (#705).
    speed_lifetime_.reset();
    speed_observer_.reset();

    if (selected_fan_.empty())
        return;

    auto& ps = get_printer_state();
    lv_subject_t* subj = ps.get_fan_speed_subject(selected_fan_, speed_lifetime_);
    if (subj) {
        auto token = lifetime_.token();
        speed_observer_ = helix::ui::observe_int_sync<FanWidget>(
            subj, this,
            [token](FanWidget* self, int speed) {
                if (token.expired())
                    return;
                self->on_speed_changed(speed);
            },
            speed_lifetime_);
    } else {
        spdlog::debug("[FanWidget] No subject for fan: {} (will retry on discovery)",
                      selected_fan_);
    }
}

void FanWidget::select_fan(const std::string& object_name) {
    if (object_name == selected_fan_) {
        return;
    }

    selected_fan_ = object_name;
    resolve_display_name();
    bind_speed_observer();
    update_display();
    save_config();

    spdlog::info("[FanWidget] Selected fan: {} ({})", display_name_, object_name);
}

void FanWidget::on_speed_changed(int speed_pct) {
    snprintf(speed_buffer_, sizeof(speed_buffer_), "%d%%", speed_pct);

    if (speed_label_) {
        lv_label_set_text(speed_label_, speed_buffer_);
    }

    spdlog::trace("[FanWidget] {} = {}%", display_name_, speed_pct);
}

void FanWidget::update_display() {
    if (speed_label_) {
        if (selected_fan_.empty()) {
            lv_label_set_text(speed_label_, "--");
        } else {
            // Read current value from subject
            auto& ps = get_printer_state();
            lv_subject_t* subj = ps.get_fan_speed_subject(selected_fan_);
            if (subj) {
                int speed = lv_subject_get_int(subj);
                snprintf(speed_buffer_, sizeof(speed_buffer_), "%d%%", speed);
                lv_label_set_text(speed_label_, speed_buffer_);
            } else {
                lv_label_set_text(speed_label_, "--");
            }
        }
    }

    if (name_label_) {
        if (selected_fan_.empty()) {
            lv_label_set_text(name_label_, lv_tr("Select fan"));
        } else {
            lv_label_set_text(name_label_, display_name_.c_str());
        }
    }
}

void FanWidget::save_config() {
    nlohmann::json config;
    config["fan"] = selected_fan_;
    config_ = config;
    save_widget_config(config);
    spdlog::debug("[FanWidget] Saved config: fan={}", selected_fan_);
}

bool FanWidget::on_edit_configure() {
    spdlog::info("[FanWidget] Configure requested - showing fan picker");
    show_fan_picker();
    return false; // no rebuild — picker updates inline
}

void FanWidget::show_fan_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    if (get_printer_state().get_fans().empty()) {
        spdlog::warn("[FanWidget] No fans available for picker");
        return;
    }

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Center);
}

void FanWidget::FanPicker::on_created(lv_obj_t* menu_obj) {
    lv_obj_t* fan_list = lv_obj_find_by_name(menu_obj, "fan_list");
    if (!fan_list) {
        spdlog::error("[FanWidget] fan_list not found in picker XML");
        return;
    }

    const auto& fans = get_printer_state().get_fans();

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);

    // Cap the list at a share of the screen so a printer with a dozen fans
    // scrolls the list instead of growing the card past the panel.
    lv_obj_set_style_max_height(fan_list, screen_height_pct(66), 0);

    for (const auto& fan : fans) {
        bool is_selected = (fan.object_name == owner_.selected_fan_);

        lv_obj_t* row = lv_obj_create(fan_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Highlight for single-select
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);

        // Fan display name
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, fan.display_name.c_str());
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, lv_font_get_default(), 0);

        // Current speed
        char speed_buf[16];
        snprintf(speed_buf, sizeof(speed_buf), "%d%%", fan.speed_percent);
        lv_obj_t* speed = lv_label_create(row);
        lv_label_set_text(speed, speed_buf);
        lv_obj_set_style_text_font(speed, lv_font_get_default(), 0);
        lv_obj_set_style_text_opa(speed, 180, 0);

        lv_obj_set_user_data(row, new RowPayload{this, fan.object_name});

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanWidget] fan_row_cb");
                auto* target = lv_event_get_current_target_obj(e);
                auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
                if (!payload)
                    return;

                // Copy the name: hide() takes the row - and this payload - with it.
                std::string fan_name = payload->object_name;
                FanPicker* picker = payload->picker;
                picker->owner_.select_fan(fan_name);
                picker->hide();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FanWidget] fan_row_delete_cb");
                auto* target = lv_event_get_current_target_obj(e);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    }

    spdlog::debug("[FanWidget] Fan picker built with {} fans", fans.size());
}

// Static callbacks
void FanWidget::fan_widget_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FanWidget] fan_widget_clicked_cb");
    auto* widget = panel_widget_from_event<FanWidget>(e);
    if (widget) {
        widget->record_interaction();
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
