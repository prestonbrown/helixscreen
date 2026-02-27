// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_temp_control.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "observer_factory.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_temp_stack_widget() {
    register_widget_factory("temp_stack", []() {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TempControlPanel>();
        return std::make_unique<TempStackWidget>(ps, tcp);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "temp_stack_nozzle_cb",
                             TempStackWidget::temp_stack_nozzle_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_bed_cb", TempStackWidget::temp_stack_bed_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_chamber_cb",
                             TempStackWidget::temp_stack_chamber_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_long_press_cb",
                             TempStackWidget::temp_stack_long_press_cb);
    lv_xml_register_event_cb(nullptr, "temp_carousel_long_press_cb",
                             TempStackWidget::temp_carousel_long_press_cb);
    lv_xml_register_event_cb(nullptr, "temp_carousel_page_cb",
                             TempStackWidget::temp_carousel_page_cb);
}
} // namespace helix

namespace {
// File-local helper: get the shared PanelWidgetConfig instance for home panel
helix::PanelWidgetConfig& get_widget_config_ref() {
    static helix::PanelWidgetConfig config("home", *helix::Config::get_instance());
    config.load();
    return config;
}
// Recursively add long-press handler to all descendants of an LVGL object
static void add_long_press_recursive(lv_obj_t* obj, lv_event_cb_t cb, void* user_data) {
    if (!obj)
        return;
    lv_obj_add_event_cb(obj, cb, LV_EVENT_LONG_PRESSED, user_data);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        add_long_press_recursive(lv_obj_get_child(obj, i), cb, user_data);
    }
}

// Make all children of a page pass events through (not clickable, bubble to parent)
static void make_children_passthrough(lv_obj_t* parent) {
    if (!parent)
        return;
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (!child)
            continue;
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        make_children_passthrough(child);
    }
}
} // namespace

using namespace helix;

// Static instance pointer for callback dispatch (only one temp_stack widget at a time)
static TempStackWidget* s_active_instance = nullptr;

TempStackWidget::TempStackWidget(PrinterState& printer_state, TempControlPanel* temp_panel)
    : printer_state_(printer_state), temp_control_panel_(temp_panel) {}

TempStackWidget::~TempStackWidget() {
    detach();
}

void TempStackWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

std::string TempStackWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_temp_carousel";
    }
    return "panel_widget_temp_stack";
}

bool TempStackWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void TempStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
    s_active_instance = this;

    if (is_carousel_mode()) {
        attach_carousel(widget_obj);
    } else {
        attach_stack(widget_obj);
    }
}

void TempStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    using helix::ui::observe_int_sync;
    std::weak_ptr<bool> weak_alive = alive_;

    // Nozzle observers
    nozzle_temp_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_temp_subject(), this,
                                          [weak_alive](TempStackWidget* self, int temp) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_nozzle_temp_changed(temp);
                                          });
    nozzle_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_target_subject(), this,
                                          [weak_alive](TempStackWidget* self, int target) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_nozzle_target_changed(target);
                                          });

    // Bed observers
    bed_temp_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_temp_subject(), this, [weak_alive](TempStackWidget* self, int temp) {
            if (weak_alive.expired())
                return;
            self->on_bed_temp_changed(temp);
        });
    bed_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_bed_target_subject(), this,
                                          [weak_alive](TempStackWidget* self, int target) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_bed_target_changed(target);
                                          });

    // Attach nozzle animator - look for the glyph inside the nozzle_icon component
    lv_obj_t* nozzle_icon = lv_obj_find_by_name(widget_obj_, "nozzle_icon_glyph");
    if (nozzle_icon) {
        nozzle_animator_.attach(nozzle_icon);
        cached_nozzle_temp_ = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        cached_nozzle_target_ =
            lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
    }

    // Attach bed animator
    lv_obj_t* bed_icon = lv_obj_find_by_name(widget_obj_, "temp_stack_bed_icon_glyph");
    if (bed_icon) {
        bed_animator_.attach(bed_icon);
        cached_bed_temp_ = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        cached_bed_target_ = lv_subject_get_int(printer_state_.get_bed_target_subject());
        bed_animator_.update(cached_bed_temp_, cached_bed_target_);
    }

    spdlog::debug("[TempStackWidget] Attached stack with {} animators",
                  (nozzle_icon ? 1 : 0) + (bed_icon ? 1 : 0));
}

void TempStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "temp_carousel");
    if (!carousel) {
        spdlog::error("[TempStackWidget] Could not find temp_carousel in XML");
        return;
    }

    // Use carousel itself as temporary parent (ui_carousel_add_item reparents into tiles)
    lv_obj_t* page_parent = carousel;

    // Helper to create a carousel page with icon + temp_display
    auto create_temp_page = [&](const char* icon_src, const char* icon_name,
                                const char* bind_current, const char* bind_target,
                                const char* page_name) -> lv_obj_t* {
        // Create page container
        lv_obj_t* page = lv_obj_create(page_parent); // reparented by ui_carousel_add_item
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(page, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_name(page, page_name);

        // Click callback to open temp overlay; long-press to toggle mode
        lv_obj_add_event_cb(page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(page, temp_carousel_long_press_cb, LV_EVENT_LONG_PRESSED, nullptr);

        // Icon
        const char* icon_attrs[] = {"src",       icon_src, "size",    "sm",   "variant",
                                    "secondary", "name",   icon_name, nullptr};
        lv_xml_create(page, "icon", icon_attrs);

        // Temp display (larger, with target shown)
        const char* td_attrs[] = {"size",        "sm",           "show_target",
                                  "true",        "bind_current", bind_current,
                                  "bind_target", bind_target,    nullptr};
        lv_xml_create(page, "temp_display", td_attrs);

        // Make children pass events through to the page (clicks + long-press)
        make_children_passthrough(page);

        return page;
    };

    // Nozzle page (use nozzle_icon component for the icon instead)
    lv_obj_t* nozzle_page = lv_obj_create(page_parent);
    lv_obj_set_size(nozzle_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(nozzle_page, 0, 0);
    lv_obj_set_style_bg_opa(nozzle_page, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(nozzle_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nozzle_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nozzle_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nozzle_page, theme_manager_get_spacing("space_xs"), 0);
    lv_obj_add_flag(nozzle_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_name(nozzle_page, "nozzle");
    lv_obj_add_event_cb(nozzle_page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(nozzle_page, temp_carousel_long_press_cb, LV_EVENT_LONG_PRESSED, nullptr);

    const char* nozzle_icon_attrs[] = {
        "size", "sm", "badge_subject", "", "name", "carousel_nozzle_icon", nullptr};
    lv_xml_create(nozzle_page, "nozzle_icon", nozzle_icon_attrs);

    const char* nozzle_td_attrs[] = {
        "size",          "sm",          "show_target",     "true", "bind_current",
        "extruder_temp", "bind_target", "extruder_target", nullptr};
    lv_xml_create(nozzle_page, "temp_display", nozzle_td_attrs);
    make_children_passthrough(nozzle_page);
    ui_carousel_add_item(carousel, nozzle_page);

    // Attach nozzle heating animator
    lv_obj_t* nozzle_glyph = lv_obj_find_by_name(nozzle_page, "nozzle_icon_glyph");
    if (nozzle_glyph) {
        nozzle_animator_.attach(nozzle_glyph);
        cached_nozzle_temp_ = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        cached_nozzle_target_ =
            lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
    }

    // Bed page
    lv_obj_t* bed_page =
        create_temp_page("radiator", "carousel_bed_icon", "bed_temp", "bed_target", "bed");
    ui_carousel_add_item(carousel, bed_page);

    // Attach bed heating animator
    lv_obj_t* bed_glyph = lv_obj_find_by_name(bed_page, "carousel_bed_icon");
    if (bed_glyph) {
        // The icon component wraps a glyph child — try to find the actual glyph
        lv_obj_t* inner_glyph = lv_obj_get_child(bed_glyph, 0);
        if (inner_glyph) {
            bed_animator_.attach(inner_glyph);
        } else {
            bed_animator_.attach(bed_glyph);
        }
        cached_bed_temp_ = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        cached_bed_target_ = lv_subject_get_int(printer_state_.get_bed_target_subject());
        bed_animator_.update(cached_bed_temp_, cached_bed_target_);
    }

    // Chamber page (only if sensor present)
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber_sensor");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        lv_obj_t* chamber_page = create_temp_page("fridge_industrial", "carousel_chamber_icon",
                                                  "chamber_temp", "chamber_temp", "chamber");
        ui_carousel_add_item(carousel, chamber_page);
    }

    // Observe heating state for animators in carousel mode
    using helix::ui::observe_int_sync;
    std::weak_ptr<bool> weak_alive = alive_;

    nozzle_temp_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_temp_subject(), this,
                                          [weak_alive](TempStackWidget* self, int temp) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_nozzle_temp_changed(temp);
                                          });
    nozzle_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_target_subject(), this,
                                          [weak_alive](TempStackWidget* self, int target) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_nozzle_target_changed(target);
                                          });
    bed_temp_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_temp_subject(), this, [weak_alive](TempStackWidget* self, int temp) {
            if (weak_alive.expired())
                return;
            self->on_bed_temp_changed(temp);
        });
    bed_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_bed_target_subject(), this,
                                          [weak_alive](TempStackWidget* self, int target) {
                                              if (weak_alive.expired())
                                                  return;
                                              self->on_bed_target_changed(target);
                                          });

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[TempStackWidget] Attached carousel with {} pages", page_count);
}

void TempStackWidget::toggle_display_mode() {
    auto& wc = get_widget_config_ref();
    nlohmann::json cfg = wc.get_widget_config("temp_stack");

    if (is_carousel_mode()) {
        cfg["display_mode"] = "stack";
    } else {
        cfg["display_mode"] = "carousel";
    }

    wc.set_widget_config("temp_stack", cfg);
    spdlog::info("[TempStackWidget] Toggled display mode to '{}'",
                 cfg["display_mode"].get<std::string>());

    // Defer rebuild to avoid destroying widgets during event processing
    helix::ui::async_call(
        [](void*) { PanelWidgetManager::instance().notify_config_changed("home"); }, nullptr);
}

void TempStackWidget::detach() {
    *alive_ = false;
    nozzle_animator_.detach();
    bed_animator_.detach();
    nozzle_temp_observer_.reset();
    nozzle_target_observer_.reset();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    // Clean up lazily-created overlays (children of parent_screen_, not widget container)
    if (nozzle_temp_panel_) {
        NavigationManager::instance().unregister_overlay_instance(nozzle_temp_panel_);
        helix::ui::safe_delete(nozzle_temp_panel_);
    }
    if (bed_temp_panel_) {
        NavigationManager::instance().unregister_overlay_instance(bed_temp_panel_);
        helix::ui::safe_delete(bed_temp_panel_);
    }
    if (chamber_temp_panel_) {
        NavigationManager::instance().unregister_overlay_instance(chamber_temp_panel_);
        helix::ui::safe_delete(chamber_temp_panel_);
    }

    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempStackWidget] Detached");
}

void TempStackWidget::on_nozzle_temp_changed(int temp_centi) {
    cached_nozzle_temp_ = temp_centi;
    nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
}

void TempStackWidget::on_nozzle_target_changed(int target_centi) {
    cached_nozzle_target_ = target_centi;
    nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
}

void TempStackWidget::on_bed_temp_changed(int temp_centi) {
    cached_bed_temp_ = temp_centi;
    bed_animator_.update(cached_bed_temp_, cached_bed_target_);
}

void TempStackWidget::on_bed_target_changed(int target_centi) {
    cached_bed_target_ = target_centi;
    bed_animator_.update(cached_bed_temp_, cached_bed_target_);
}

void TempStackWidget::handle_nozzle_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Nozzle click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Nozzle clicked - opening nozzle temp panel");

    if (!temp_control_panel_) {
        spdlog::error("[TempStackWidget] TempControlPanel not initialized");
        NOTIFY_ERROR("Temperature panel not available");
        return;
    }

    if (!nozzle_temp_panel_ && parent_screen_) {
        nozzle_temp_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "nozzle_temp_panel", nullptr));
        if (nozzle_temp_panel_) {
            temp_control_panel_->setup_nozzle_panel(nozzle_temp_panel_, parent_screen_);
            NavigationManager::instance().register_overlay_instance(
                nozzle_temp_panel_, temp_control_panel_->get_nozzle_lifecycle());
            lv_obj_add_flag(nozzle_temp_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[TempStackWidget] Nozzle temp panel created");
        } else {
            spdlog::error("[TempStackWidget] Failed to create nozzle temp panel");
            NOTIFY_ERROR("Failed to load temperature panel");
            return;
        }
    }

    if (nozzle_temp_panel_) {
        NavigationManager::instance().push_overlay(nozzle_temp_panel_);
    }
}

void TempStackWidget::handle_bed_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Bed click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Bed clicked - opening bed temp panel");

    if (!temp_control_panel_) {
        spdlog::error("[TempStackWidget] TempControlPanel not initialized");
        NOTIFY_ERROR("Temperature panel not available");
        return;
    }

    if (!bed_temp_panel_ && parent_screen_) {
        bed_temp_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "bed_temp_panel", nullptr));
        if (bed_temp_panel_) {
            temp_control_panel_->setup_bed_panel(bed_temp_panel_, parent_screen_);
            NavigationManager::instance().register_overlay_instance(
                bed_temp_panel_, temp_control_panel_->get_bed_lifecycle());
            lv_obj_add_flag(bed_temp_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[TempStackWidget] Bed temp panel created");
        } else {
            spdlog::error("[TempStackWidget] Failed to create bed temp panel");
            NOTIFY_ERROR("Failed to load temperature panel");
            return;
        }
    }

    if (bed_temp_panel_) {
        NavigationManager::instance().push_overlay(bed_temp_panel_);
    }
}

void TempStackWidget::handle_chamber_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Chamber click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Chamber clicked - opening chamber temp panel");

    if (!temp_control_panel_) {
        spdlog::error("[TempStackWidget] TempControlPanel not initialized");
        NOTIFY_ERROR("Temperature panel not available");
        return;
    }

    if (!chamber_temp_panel_ && parent_screen_) {
        chamber_temp_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "chamber_temp_panel", nullptr));
        if (chamber_temp_panel_) {
            temp_control_panel_->setup_chamber_panel(chamber_temp_panel_, parent_screen_);
            NavigationManager::instance().register_overlay_instance(
                chamber_temp_panel_, temp_control_panel_->get_chamber_lifecycle());
            lv_obj_add_flag(chamber_temp_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[TempStackWidget] Chamber temp panel created");
        } else {
            spdlog::error("[TempStackWidget] Failed to create chamber temp panel");
            NOTIFY_ERROR("Failed to load temperature panel");
            return;
        }
    }

    if (chamber_temp_panel_) {
        NavigationManager::instance().push_overlay(chamber_temp_panel_);
    }
}

void TempStackWidget::temp_stack_nozzle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_nozzle_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_nozzle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_bed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_bed_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_bed_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_chamber_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_chamber_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_chamber_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_long_press_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_long_press_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->long_pressed_ = true;
        s_active_instance->toggle_display_mode();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_carousel_long_press_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_carousel_long_press_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->long_pressed_ = true;
        s_active_instance->toggle_display_mode();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_carousel_page_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_carousel_page_cb");
    if (s_active_instance) {
        if (s_active_instance->long_pressed_) {
            s_active_instance->long_pressed_ = false;
            spdlog::debug("[TempStackWidget] Carousel page click suppressed (follows long-press)");
        } else {
            auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            const char* page_id = lv_obj_get_name(target);
            if (page_id) {
                if (std::strcmp(page_id, "nozzle") == 0) {
                    s_active_instance->handle_nozzle_clicked();
                } else if (std::strcmp(page_id, "bed") == 0) {
                    s_active_instance->handle_bed_clicked();
                } else if (std::strcmp(page_id, "chamber") == 0) {
                    s_active_instance->handle_chamber_clicked();
                }
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
