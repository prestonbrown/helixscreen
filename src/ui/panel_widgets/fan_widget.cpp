// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_widget.h"

#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_picker_option_row.h"
#include "ui_text.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "json_utils.h"
#include "lvgl.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "hv/json.hpp"

namespace helix {
void register_fan_widget() {
    register_widget_factory("fan",
                            [](const std::string& id) { return std::make_unique<FanWidget>(id); });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "fan_widget_clicked_cb", FanWidget::fan_widget_clicked_cb);
}
void FanWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    (void)colspan;
    (void)rowspan;
    sizing_.measure_and_publish(width_px, height_px);
}

} // namespace helix

using namespace helix;

FanWidget::FanWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id), instance_id_(instance_id) {
    // Worst cases, not a live reading: a size accepted while the fan reads 5%
    // must still draw 100%.
    TileSizing::Content content{"100%",
                                "100%",
                                "Fan",
                                /*has_value=*/true,
                                "",
                                /*label_always_drawn=*/true};
    content.label_is_identity = true;
    sizing_.set_content(std::move(content));

    // Published before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    speed_subject_.init_string(speed_buffer_, nullptr, sizeof(speed_buffer_), "--");
    sizing_.publish_instance_subject(subjects_, &speed_subject_, "speed_subject", "speed");
    name_subject_.init_string(name_buffer_, nullptr, sizeof(name_buffer_), "");
    sizing_.publish_instance_subject(subjects_, &name_subject_, "name_subject", "name");
    named_subject_.init_int(0);
    sizing_.publish_instance_subject(subjects_, &named_subject_, "named_subject", "named");
}

FanWidget::~FanWidget() {
    detach();
}

void FanWidget::set_config(const nlohmann::json& config) {
    config_ = config;

    if (auto fan = helix::json_util::read_field<std::string>(config, "fan")) {
        selected_fan_ = *fan;
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

    // Only bind if fans are already discovered (e.g. reconnection, panel re-entry).
    // At startup, fan subjects don't exist yet — the version observer (below)
    // handles binding once init_fans() bumps fans_version.
    if (selected_fan_.empty()) {
        auto_select_first_fan();
    } else if (!get_printer_state().fan_state().get_fans().empty()) {
        bind_speed_observer();
        update_display();
    } else {
        update_display();
    }

    // Observe fan version to detect fan discovery/reconnection
    auto token = lifetime_.token();
    version_observer_ = helix::ui::observe<int>(
        get_printer_state().fan_state().get_fans_version_subject(), this,
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
        version_observer_.reset();
        speed_lifetime_.reset(); // Before observer: expire weak_ptr if subject already freed
        speed_observer_.reset();
    }

    if (widget_obj_) {
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[FanWidget] Detached");
}

void FanWidget::handle_clicked() {
    spdlog::debug("[FanWidget] Clicked - opening fan control overlay");
    open_fan_control_overlay(parent_screen_);
}

void FanWidget::resolve_display_name() {
    const auto& fans = get_printer_state().fan_state().get_fans();
    for (const auto& fan : fans) {
        if (fan.object_name == selected_fan_) {
            display_name_ = fan.display_name;
            return;
        }
    }
    display_name_ = selected_fan_; // fallback
}

void FanWidget::auto_select_first_fan() {
    const auto& fans = get_printer_state().fan_state().get_fans();
    if (!fans.empty()) {
        select_fan(fans.front().object_name);
    }
}

void FanWidget::bind_speed_observer() {
    // Reset lifetime BEFORE observer so the guard's weak_ptr expires and
    // skips lv_observer_delete() on a potentially-freed subject (#705).
    speed_lifetime_.reset();
    speed_observer_.reset();

    if (selected_fan_.empty())
        return;

    auto& ps = get_printer_state();
    lv_subject_t* subj = ps.fan_state().get_fan_speed_subject(selected_fan_, speed_lifetime_);
    if (subj) {
        auto token = lifetime_.token();
        speed_observer_ = helix::ui::observe<int>(
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

void FanWidget::set_speed_text(const char* text) {
    lv_subject_set_string(&speed_subject_, text);
}

void FanWidget::on_speed_changed(int speed_pct) {
    char text[16];
    snprintf(text, sizeof(text), "%d%%", speed_pct);
    set_speed_text(text);

    spdlog::trace("[FanWidget] {} = {}%", display_name_, speed_pct);
}

void FanWidget::update_display() {
    lv_subject_t* subj = selected_fan_.empty()
                             ? nullptr
                             : get_printer_state().fan_state().get_fan_speed_subject(selected_fan_);
    if (subj) {
        on_speed_changed(lv_subject_get_int(subj));
    } else {
        set_speed_text("--");
    }

    // An empty selection shows the component's own "Select fan" label, which
    // re-translates itself.
    if (!selected_fan_.empty()) {
        lv_subject_set_string(&name_subject_, display_name_.c_str());
    }
    lv_subject_set_int(&named_subject_, selected_fan_.empty() ? 0 : 1);
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

    if (get_printer_state().fan_state().get_fans().empty()) {
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

    const auto& fans = get_printer_state().fan_state().get_fans();

    // Cap the list at a share of the screen so a printer with a dozen fans
    // scrolls the list instead of growing the card past the panel.
    lv_obj_set_style_max_height(fan_list, screen_height_pct(66), 0);

    for (const auto& fan : fans) {
        char speed[16];
        snprintf(speed, sizeof(speed), "%d%%", fan.speed_percent);
        const std::string fan_name = fan.object_name;
        helix::ui::create_picker_option_row(fan_list, fan.display_name.c_str(),
                                            fan_name == owner_.selected_fan_,
                                            [this, fan_name]() {
                                                owner_.select_fan(fan_name);
                                                hide();
                                            },
                                            {"value", speed});
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
