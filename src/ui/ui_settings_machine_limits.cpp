// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_machine_limits.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<MachineLimitsOverlay> g_machine_limits_overlay;

MachineLimitsOverlay& get_machine_limits_overlay() {
    if (!g_machine_limits_overlay) {
        g_machine_limits_overlay = std::make_unique<MachineLimitsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "MachineLimitsOverlay", []() { g_machine_limits_overlay.reset(); });
    }
    return *g_machine_limits_overlay;
}

void init_machine_limits_overlay(IMoonrakerAPI* api) {
    auto& overlay = get_machine_limits_overlay();
    overlay.set_api(api);
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

MachineLimitsOverlay::MachineLimitsOverlay() {
    spdlog::trace("[{}] Constructor", get_name());
}

MachineLimitsOverlay::~MachineLimitsOverlay() {
    if (apply_timer_) {
        lv_timer_delete(apply_timer_);
        apply_timer_ = nullptr;
    }
    deinit_subjects();
}

// ============================================================================
// CONFIGURATION
// ============================================================================

void MachineLimitsOverlay::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void MachineLimitsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize display subjects for XML binding
        // Use em-dash (—) for unknown values instead of double-hyphen (--)
        UI_MANAGED_SUBJECT_STRING(max_velocity_display_subject_, velocity_buf_, "— mm/s",
                                  "max_velocity_display", subjects_);

        UI_MANAGED_SUBJECT_STRING(max_accel_display_subject_, accel_buf_, "— mm/s²",
                                  "max_accel_display", subjects_);

        UI_MANAGED_SUBJECT_STRING(accel_to_decel_display_subject_, a2d_buf_, "— mm/s²",
                                  "accel_to_decel_display", subjects_);

        UI_MANAGED_SUBJECT_STRING(square_corner_velocity_display_subject_, scv_buf_, "— mm/s",
                                  "square_corner_velocity_display", subjects_);

        // Extrude speed display (persisted via SettingsManager, not a Klipper override)
        int extrude_speed = helix::SettingsManager::instance().get_extrude_speed();
        std::snprintf(extrude_speed_buf_, sizeof(extrude_speed_buf_), "%d mm/s", extrude_speed);
        UI_MANAGED_SUBJECT_STRING(extrude_speed_display_subject_, extrude_speed_buf_,
                                  extrude_speed_buf_, "extrude_speed_display", subjects_);
    });
}

void MachineLimitsOverlay::register_callbacks() {
    // Register slider change callbacks
    lv_xml_register_event_cb(nullptr, "on_max_velocity_changed", on_velocity_changed);
    lv_xml_register_event_cb(nullptr, "on_max_accel_changed", on_accel_changed);
    lv_xml_register_event_cb(nullptr, "on_accel_to_decel_changed", on_a2d_changed);
    lv_xml_register_event_cb(nullptr, "on_square_corner_velocity_changed", on_scv_changed);

    // Register button callbacks (Reset only - Apply removed for immediate mode)
    lv_xml_register_event_cb(nullptr, "on_limits_reset", on_reset);

    // Extrude speed slider (persisted setting, not a Klipper override)
    lv_xml_register_event_cb(nullptr, "on_extrude_speed_changed", on_extrude_speed_changed);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

void MachineLimitsOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* MachineLimitsOverlay::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] NULL parent", get_name());
        return nullptr;
    }

    // Create overlay from XML
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "machine_limits_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());

    return overlay_root_;
}

void MachineLimitsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Create overlay on first access (lazy initialization)
    if (!overlay_root_ && parent_screen) {
        create(parent_screen);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay", get_name());
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to load overlay"), 2000);
        return;
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push overlay onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void MachineLimitsOverlay::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Refresh data from printer
    query_and_show(nullptr);
}

void MachineLimitsOverlay::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Flush any pending debounced apply before leaving
    if (apply_timer_) {
        lv_timer_delete(apply_timer_);
        apply_timer_ = nullptr;
        apply_limits();
    }

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// DATA REFRESH
// ============================================================================

void MachineLimitsOverlay::query_and_show(lv_obj_t* /*parent_screen*/) {
    if (api_) {
        // bg_cb marshals the response onto the main thread and drops it if the
        // overlay has since been deactivated or torn down, so the body below can
        // touch members and LVGL freely (#1165).
        api_->advanced().get_machine_limits(
            lifetime_.bg_cb("MachineLimitsOverlay::query_and_show",
                            [this](const MachineLimits& limits) {
                                spdlog::info(
                                    "[{}] Got machine limits: vel={}, accel={}, a2d={}, scv={}",
                                    get_name(), limits.max_velocity, limits.max_accel,
                                    limits.max_accel_to_decel, limits.square_corner_velocity);

                                // Store both current and original for reset
                                current_limits_ = limits;
                                original_limits_ = limits;

                                // Update display and sliders
                                update_display();
                                update_sliders();

                                // Update read-only Z values
                                if (overlay_root_) {
                                    lv_obj_t* z_vel_row =
                                        lv_obj_find_by_name(overlay_root_, "row_max_z_velocity");
                                    if (z_vel_row) {
                                        lv_obj_t* value = lv_obj_find_by_name(z_vel_row, "value");
                                        if (value) {
                                            char buf[32];
                                            helix::format::format_speed_mm_s(limits.max_z_velocity,
                                                                             buf, sizeof(buf));
                                            lv_label_set_text(value, buf);
                                        }
                                    }
                                    lv_obj_t* z_accel_row =
                                        lv_obj_find_by_name(overlay_root_, "row_max_z_accel");
                                    if (z_accel_row) {
                                        lv_obj_t* value = lv_obj_find_by_name(z_accel_row, "value");
                                        if (value) {
                                            char buf[32];
                                            helix::format::format_accel_mm_s2(limits.max_z_accel,
                                                                              buf, sizeof(buf));
                                            lv_label_set_text(value, buf);
                                        }
                                    }
                                }
                            }),
            lifetime_.bg_cb(
                "MachineLimitsOverlay::query_and_show_error", [this](const MoonrakerError& err) {
                    spdlog::error("[{}] Failed to get machine limits: {}", get_name(), err.message);
                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                  lv_tr("Failed to get limits"), 2000);
                }));
    } else {
        // No API - overlay is already shown via push_overlay
        spdlog::warn("[{}] No API available, showing defaults", get_name());
    }
}

// ============================================================================
// DISPLAY UPDATES
// ============================================================================

void MachineLimitsOverlay::update_display() {
    // Update max velocity display
    helix::format::format_speed_mm_s(current_limits_.max_velocity, velocity_buf_,
                                     sizeof(velocity_buf_));
    lv_subject_copy_string(&max_velocity_display_subject_, velocity_buf_);

    // Update max accel display
    helix::format::format_accel_mm_s2(current_limits_.max_accel, accel_buf_, sizeof(accel_buf_));
    lv_subject_copy_string(&max_accel_display_subject_, accel_buf_);

    // Update accel to decel display
    helix::format::format_accel_mm_s2(current_limits_.max_accel_to_decel, a2d_buf_,
                                      sizeof(a2d_buf_));
    lv_subject_copy_string(&accel_to_decel_display_subject_, a2d_buf_);

    // Update square corner velocity display
    helix::format::format_speed_mm_s(current_limits_.square_corner_velocity, scv_buf_,
                                     sizeof(scv_buf_));
    lv_subject_copy_string(&square_corner_velocity_display_subject_, scv_buf_);
}

void MachineLimitsOverlay::update_sliders() {
    if (!overlay_root_) {
        return;
    }

    // Update max velocity slider
    lv_obj_t* vel_slider = lv_obj_find_by_name(overlay_root_, "max_velocity_slider");
    if (vel_slider) {
        lv_slider_set_value(vel_slider, static_cast<int>(current_limits_.max_velocity),
                            LV_ANIM_OFF);
    }

    // Update max accel slider
    lv_obj_t* accel_slider = lv_obj_find_by_name(overlay_root_, "max_accel_slider");
    if (accel_slider) {
        lv_slider_set_value(accel_slider, static_cast<int>(current_limits_.max_accel), LV_ANIM_OFF);
    }

    // Update accel to decel slider
    lv_obj_t* a2d_slider = lv_obj_find_by_name(overlay_root_, "accel_to_decel_slider");
    if (a2d_slider) {
        lv_slider_set_value(a2d_slider, static_cast<int>(current_limits_.max_accel_to_decel),
                            LV_ANIM_OFF);
    }

    // Update square corner velocity slider
    lv_obj_t* scv_slider = lv_obj_find_by_name(overlay_root_, "square_corner_velocity_slider");
    if (scv_slider) {
        lv_slider_set_value(scv_slider, static_cast<int>(current_limits_.square_corner_velocity),
                            LV_ANIM_OFF);
    }

    // Update extrude speed slider (persisted setting, not from Klipper)
    lv_obj_t* extrude_slider = lv_obj_find_by_name(overlay_root_, "extrude_speed_slider");
    if (extrude_slider) {
        int speed = helix::SettingsManager::instance().get_extrude_speed();
        lv_slider_set_value(extrude_slider, speed, LV_ANIM_OFF);
        std::snprintf(extrude_speed_buf_, sizeof(extrude_speed_buf_), "%d mm/s", speed);
        lv_subject_copy_string(&extrude_speed_display_subject_, extrude_speed_buf_);
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MachineLimitsOverlay::handle_velocity_changed(int value) {
    current_limits_.max_velocity = static_cast<double>(value);
    helix::format::format_speed_mm_s(static_cast<double>(value), velocity_buf_,
                                     sizeof(velocity_buf_));
    lv_subject_copy_string(&max_velocity_display_subject_, velocity_buf_);
    schedule_apply_limits();
}

void MachineLimitsOverlay::handle_accel_changed(int value) {
    current_limits_.max_accel = static_cast<double>(value);
    helix::format::format_accel_mm_s2(static_cast<double>(value), accel_buf_, sizeof(accel_buf_));
    lv_subject_copy_string(&max_accel_display_subject_, accel_buf_);
    schedule_apply_limits();
}

void MachineLimitsOverlay::handle_a2d_changed(int value) {
    current_limits_.max_accel_to_decel = static_cast<double>(value);
    helix::format::format_accel_mm_s2(static_cast<double>(value), a2d_buf_, sizeof(a2d_buf_));
    lv_subject_copy_string(&accel_to_decel_display_subject_, a2d_buf_);
    schedule_apply_limits();
}

void MachineLimitsOverlay::handle_scv_changed(int value) {
    current_limits_.square_corner_velocity = static_cast<double>(value);
    helix::format::format_speed_mm_s(static_cast<double>(value), scv_buf_, sizeof(scv_buf_));
    lv_subject_copy_string(&square_corner_velocity_display_subject_, scv_buf_);
    schedule_apply_limits();
}

void MachineLimitsOverlay::handle_reset() {
    spdlog::info("[{}] Resetting limits to original values", get_name());
    current_limits_ = original_limits_;
    update_display();
    update_sliders();
    apply_limits(); // Send original values back to printer
}

void MachineLimitsOverlay::schedule_apply_limits() {
    // Debounce slider changes: reset/create a 250ms one-shot timer.
    // Display subjects update immediately (user sees the number change),
    // but the G-code command only fires after 250ms of inactivity.
    static constexpr uint32_t DEBOUNCE_MS = 250;

    if (apply_timer_) {
        lv_timer_reset(apply_timer_);
    } else {
        apply_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<MachineLimitsOverlay*>(lv_timer_get_user_data(t));
                self->apply_timer_ = nullptr;
                self->apply_limits();
            },
            DEBOUNCE_MS, this);
        lv_timer_set_repeat_count(apply_timer_, 1);
    }
}

void MachineLimitsOverlay::apply_limits() {
    spdlog::debug("[{}] Applying machine limits: vel={}, accel={}, a2d={}, scv={}", get_name(),
                  current_limits_.max_velocity, current_limits_.max_accel,
                  current_limits_.max_accel_to_decel, current_limits_.square_corner_velocity);

    if (!api_) {
        spdlog::warn("[{}] No API available - cannot apply limits", get_name());
        return;
    }

    // bg_cb, like query_and_show above: the whole body runs on the main thread
    // after a generation re-check, so no liveness test belongs on the bg side.
    // The hand-rolled guards these replaced re-checked on the background thread
    // before deferring, which was dead code and tripped the runtime L081
    // detector on every reply (#1165).
    api_->advanced().set_machine_limits(
        current_limits_,
        lifetime_.bg_cb(
            "MachineLimitsOverlay::apply_limits",
            [this]() { spdlog::debug("[{}] Machine limits applied successfully", get_name()); }),
        lifetime_.bg_cb(
            "MachineLimitsOverlay::apply_limits_error", [this](const MoonrakerError& err) {
                spdlog::error("[{}] Failed to apply machine limits: {}", get_name(), err.message);
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to apply limits"),
                                              2000);
            }));
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void MachineLimitsOverlay::on_velocity_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_velocity_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_machine_limits_overlay().handle_velocity_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::on_accel_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_accel_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_machine_limits_overlay().handle_accel_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::on_a2d_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_a2d_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_machine_limits_overlay().handle_a2d_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::on_scv_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_scv_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_machine_limits_overlay().handle_scv_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::on_reset(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_reset");
    get_machine_limits_overlay().handle_reset();
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::on_extrude_speed_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MachineLimitsOverlay] on_extrude_speed_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_machine_limits_overlay().handle_extrude_speed_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void MachineLimitsOverlay::handle_extrude_speed_changed(int value) {
    helix::SettingsManager::instance().set_extrude_speed(value);
    std::snprintf(extrude_speed_buf_, sizeof(extrude_speed_buf_), "%d mm/s", value);
    lv_subject_copy_string(&extrude_speed_display_subject_, extrude_speed_buf_);
}

} // namespace helix::settings
