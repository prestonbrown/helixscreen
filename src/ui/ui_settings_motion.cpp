// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_motion.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_modal.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "jog_coalescer.h"
#include "lvgl.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace helix::settings {

// ============================================================================
// ROW DESCRIPTORS
// ============================================================================

namespace {

/// Settings store mm/min because that is what the motion API takes; the UI
/// speaks mm/s to match Extrude Speed and the web interfaces.
///
/// Truncation, not rounding: max_jog_mm_s() floors the printer's ceiling for
/// the same reason a move above it is rejected, and a rounded-up display would
/// read one above the slider's own maximum. The cost is that a stored feedrate
/// that is not a multiple of 60 - only reachable by editing settings.json by
/// hand - shows as the next mm/s down, and becomes that value if the user
/// touches the control. Nothing writes it back on its own: lv_slider_set_value
/// raises no value-changed event.
int mm_min_to_mm_s(int mm_per_min) {
    return mm_per_min / 60;
}

int mm_s_to_mm_min(int mm_per_sec) {
    return mm_per_sec * 60;
}

/// One row per control. `title` is both the keypad header and the row's
/// translation_tag in motion_settings_overlay.xml, so it resolves with no
/// extra keys. Indexed by Field.
enum class Kind {
    Speed,     ///< mm/s slider row
    Distance,  ///< jog step distance, mm
    Clearance, ///< Bed-tab Z clearance, whole mm
};

struct FieldSpec {
    const char* title; ///< Keypad header and row label
    Kind kind;
    bool is_z;    ///< speeds only: Z axis instead of XY
    JogMode mode; ///< distances only
    bool outer;   ///< distances only
};

constexpr FieldSpec kFieldSpecs[] = {
    {"Jog Speed XY", Kind::Speed, false, {}, false},
    {"Jog Speed Z", Kind::Speed, true, {}, false},
    {"Fine Inner", Kind::Distance, false, JogMode::Fine, false},
    {"Fine Outer", Kind::Distance, false, JogMode::Fine, true},
    {"Coarse Inner", Kind::Distance, false, JogMode::Coarse, false},
    {"Coarse Outer", Kind::Distance, false, JogMode::Coarse, true},
    {"Turbo Inner", Kind::Distance, false, JogMode::Turbo, false},
    {"Turbo Outer", Kind::Distance, false, JogMode::Turbo, true},
    {"Bed Map Clearance", Kind::Clearance, false, {}, false},
};

constexpr size_t kFieldCount = sizeof(kFieldSpecs) / sizeof(kFieldSpecs[0]);
static_assert(kFieldCount == static_cast<size_t>(Field::Count),
              "FIELD_SPECS must have one entry per Field");

/// Each mode occupies one adjacent inner/outer pair, starting at an even
/// index. Field, this table and the user_data numbers in
/// motion_settings_overlay.xml all share that order, so a row inserted in the
/// wrong place must fail here rather than mislabel a live control.
constexpr bool distance_pairs_alternate() {
    for (size_t i = static_cast<size_t>(Field::FineInner);
         i <= static_cast<size_t>(Field::TurboOuter); ++i) {
        if (kFieldSpecs[i].mode != kFieldSpecs[i ^ 1].mode) {
            return false;
        }
        if (kFieldSpecs[i].outer == kFieldSpecs[i ^ 1].outer) {
            return false;
        }
    }
    return true;
}
static_assert(static_cast<int>(Field::FineInner) % 2 == 0,
              "distance rows must start at an even Field index");
static_assert(distance_pairs_alternate(),
              "distance rows must pair same-mode inner/outer at i, i^1");

/// XML value_subject names, in Field order.
constexpr const char* const kSubjectNames[kFieldCount] = {
    "jog_speed_xy_display", "jog_speed_z_display",  "fine_inner_display",
    "fine_outer_display",   "coarse_inner_display", "coarse_outer_display",
    "turbo_inner_display",  "turbo_outer_display",  "bed_map_clearance_display",
};

bool field_in_range(int raw) {
    return raw >= 0 && raw < static_cast<int>(Field::Count);
}

} // namespace

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static MotionSettingsOverlay& get_motion_settings_overlay() {
    return lazy_global<MotionSettingsOverlay>("MotionSettingsOverlay");
}

void show_motion_settings_overlay() {
    auto& overlay = get_motion_settings_overlay();
    overlay.set_api(get_moonraker_api());
    overlay.show(lv_screen_active());
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

MotionSettingsOverlay::~MotionSettingsOverlay() {
    // persist_timer_ cancels itself as a member; a still-pending write dies
    // with the overlay, which on_deactivating already flushed on the way out.
    deinit_subjects();
}

void MotionSettingsOverlay::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void MotionSettingsOverlay::init_subjects() {
    for (size_t i = 0; i < kFieldCount; ++i) {
        format_display(i);
        UI_MANAGED_SUBJECT_STRING(display_subjects_[i], display_buffers_[i], display_buffers_[i],
                                  kSubjectNames[i], subjects_);
    }
    UI_MANAGED_SUBJECT_STRING(jog_speed_max_subject_, jog_speed_max_buf_, "500",
                              "jog_speed_max_display", subjects_);
}

void MotionSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_jog_speed_xy_changed",
         [](lv_event_t* e) {
             get_motion_settings_overlay().handle_jog_speed_changed(
                 /*is_z=*/false, lv_slider_get_value(lv_event_get_current_target_obj(e)));
         }},
        {"on_jog_speed_z_changed",
         [](lv_event_t* e) {
             get_motion_settings_overlay().handle_jog_speed_changed(
                 /*is_z=*/true, lv_slider_get_value(lv_event_get_current_target_obj(e)));
         }},
        {"on_motion_field_clicked",
         [](lv_event_t* e) {
             const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
             if (!index_str) {
                 return;
             }
             const int raw = static_cast<int>(std::strtol(index_str, nullptr, 10));
             if (field_in_range(raw)) {
                 get_motion_settings_overlay().handle_field_clicked(static_cast<Field>(raw));
             } else {
                 spdlog::warn("[MotionSettingsOverlay] Ignoring out-of-range field index {}", raw);
             }
         }},
        {"on_motion_layout_changed",
         [](lv_event_t* e) {
             const int index = helix::ui::event_selected(e);
             if (index >= 0 && index < kMotionLayoutCount) {
                 SettingsManager::instance().set_motion_layout(static_cast<MotionLayout>(index));
             }
         }},
        {"on_reset_distances",
         [](lv_event_t*) { get_motion_settings_overlay().handle_reset_distances(); }},
        {"on_z_offset_save_toggled",
         [](lv_event_t* e) { helix::zoffset::set_save_enabled(ui::event_checked(e)); }},
    });
}

void MotionSettingsOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void MotionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Returning from our own keypad is not a fresh entry. The keypad confirm
    // updates the display and schedules the debounced persist, so re-reading
    // SettingsManager here would put the pre-persist value back on screen.
    if (returning_from_keypad_) {
        returning_from_keypad_ = false;
        spdlog::debug("[{}] Keeping typed value: returned from keypad", get_name());
        return;
    }

    refresh_displays();
    refresh_sliders();
}

void MotionSettingsOverlay::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] on_deactivating()", get_name());

    // Flush a pending debounced write before leaving. Cancelling alone would
    // drop the user's last drag: the timer cancels, it does not run.
    if (persist_timer_.pending()) {
        persist_timer_.cancel();
        persist_pending_speed();
    }
}

// ============================================================================
// DISPLAY REFRESH
// ============================================================================

void MotionSettingsOverlay::format_display(size_t i) {
    const FieldSpec& spec = kFieldSpecs[i];
    auto& settings = SettingsManager::instance();

    if (spec.kind == Kind::Speed) {
        const int mm_min = spec.is_z ? settings.get_jog_speed_z() : settings.get_jog_speed_xy();
        std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%d mm/s",
                      mm_min_to_mm_s(effective_mm_min(mm_min)));
    } else if (spec.kind == Kind::Clearance) {
        std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%d mm",
                      settings.get_bed_map_clearance_mm());
    } else {
        // %g trims trailing zeros, so the defaults read 0.1, 1, 10 and 50.
        std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%g mm",
                      static_cast<double>(settings.get_jog_distance(spec.mode, spec.outer)));
    }
}

void MotionSettingsOverlay::refresh_displays() {
    for (size_t i = 0; i < kFieldCount; ++i) {
        format_display(i);
        lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
    }
}

int MotionSettingsOverlay::effective_mm_min(int stored_mm_min) const {
    if (api_) {
        // The same bounds is_safe_feedrate() enforces at emission, so what the
        // field shows is what the move uses.
        const SafetyLimits& limits = api_->get_safety_limits();
        return helix::effective_jog_speed_mm_min(stored_mm_min, limits.min_feedrate_mm_min,
                                                 limits.max_feedrate_mm_min);
    }
    // Settings clamp range for callers with no API to ask: stored values
    // already sit inside it, so this is a passthrough in practice.
    return helix::effective_jog_speed_mm_min(stored_mm_min, 0.0, 60000.0);
}

int MotionSettingsOverlay::max_jog_mm_s() const {
    if (api_) {
        // IMoonrakerAPI::get_safety_limits() is the same source is_safe_feedrate()
        // checks against, so the slider cannot offer a speed the move would reject.
        const SafetyLimits& limits = api_->get_safety_limits();
        return std::max(1, static_cast<int>(limits.max_feedrate_mm_min / 60.0));
    }
    // Settings clamp ceiling, for callers with no API to ask.
    return mm_min_to_mm_s(60000);
}

lv_obj_t* MotionSettingsOverlay::speed_slider(bool is_z) const {
    if (!overlay_root_) {
        return nullptr;
    }
    return lv_obj_find_by_name(overlay_root_, is_z ? "jog_speed_z_slider" : "jog_speed_xy_slider");
}

void MotionSettingsOverlay::refresh_sliders() {
    if (!overlay_root_) {
        return;
    }

    auto& settings = SettingsManager::instance();
    const int max_mm_s = max_jog_mm_s();
    const struct {
        const char* slider;
        int mm_min;
    } rows[] = {
        {"jog_speed_xy_slider", settings.get_jog_speed_xy()},
        {"jog_speed_z_slider", settings.get_jog_speed_z()},
    };

    std::snprintf(jog_speed_max_buf_, sizeof(jog_speed_max_buf_), "%d", max_mm_s);
    lv_subject_copy_string(&jog_speed_max_subject_, jog_speed_max_buf_);

    for (const auto& row : rows) {
        lv_obj_t* slider = lv_obj_find_by_name(overlay_root_, row.slider);
        if (slider) {
            lv_slider_set_range(slider, 1, max_mm_s);
            // The effective value, not the stored one: LVGL clamps the slider
            // to this range anyway, and the field must agree with it.
            lv_slider_set_value(slider, mm_min_to_mm_s(effective_mm_min(row.mm_min)), LV_ANIM_OFF);
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MotionSettingsOverlay::handle_jog_speed_changed(bool is_z, int mm_s) {
    const size_t i =
        is_z ? static_cast<size_t>(Field::JogSpeedZ) : static_cast<size_t>(Field::JogSpeedXY);

    pending_mm_min_[is_z ? 1 : 0] = mm_s_to_mm_min(mm_s);
    // Trailing-edge debounce: a drag re-requests per pixel, and the write
    // fires once, 250ms after the burst stops, with the latest value.
    persist_timer_.schedule([this]() { persist_pending_speed(); });

    std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%d mm/s", mm_s);
    lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
}

void MotionSettingsOverlay::persist_pending_speed() {
    auto& settings = SettingsManager::instance();
    if (pending_mm_min_[0] != 0) {
        settings.set_jog_speed_xy(pending_mm_min_[0]);
        pending_mm_min_[0] = 0;
    }
    if (pending_mm_min_[1] != 0) {
        settings.set_jog_speed_z(pending_mm_min_[1]);
        pending_mm_min_[1] = 0;
    }
}

void MotionSettingsOverlay::handle_field_clicked(Field field) {
    const size_t i = static_cast<size_t>(field);
    const FieldSpec& spec = kFieldSpecs[i];
    auto& settings = SettingsManager::instance();
    pending_keypad_field_ = field;

    ui_keypad_config_t config = {};
    if (spec.kind == Kind::Speed) {
        lv_obj_t* slider = speed_slider(spec.is_z);
        if (!slider) {
            spdlog::warn("[{}] No slider for field {}", get_name(), static_cast<int>(field));
            return;
        }
        // Bounds come from the row's slider, whose range was derived from the
        // printer's reported feedrate limit, so the two cannot disagree.
        const KeypadBounds bounds =
            keypad_bounds(field, static_cast<float>(lv_slider_get_min_value(slider)),
                          static_cast<float>(lv_slider_get_max_value(slider)));
        config.min_value = bounds.min;
        config.max_value = bounds.max;
        config.initial_value = static_cast<float>(mm_min_to_mm_s(effective_mm_min(
            spec.is_z ? settings.get_jog_speed_z() : settings.get_jog_speed_xy())));
        config.allow_decimal = false;
        config.unit_label = "mm/s";
    } else if (spec.kind == Kind::Clearance) {
        config.initial_value = static_cast<float>(settings.get_bed_map_clearance_mm());
        config.min_value = static_cast<float>(kBedMapClearanceMinMm);
        config.max_value = static_cast<float>(kBedMapClearanceMaxMm);
        config.allow_decimal = false;
        config.unit_label = "mm";
    } else {
        const KeypadBounds bounds =
            keypad_bounds(field, settings.get_jog_distance(spec.mode, false),
                          settings.get_jog_distance(spec.mode, true));
        config.initial_value = settings.get_jog_distance(spec.mode, spec.outer);
        config.min_value = bounds.min;
        config.max_value = bounds.max;
        config.allow_decimal = true;
        config.unit_label = "mm";
    }
    config.title_label = lv_tr(spec.title);
    config.allow_negative = false;
    config.callback = on_keypad_value;
    config.user_data = this;

    spdlog::debug("[{}] Keypad for {} ({}-{})", get_name(), spec.title, config.min_value,
                  config.max_value);
    ui_keypad_show(&config);
}

void MotionSettingsOverlay::handle_keypad_value(Field field, double value) {
    // Set on confirm, not when the keypad opens: the keypad invokes this
    // callback before it hides, so the flag is always consumed by the
    // on_activate() that follows.
    returning_from_keypad_ = true;

    const FieldSpec& spec = kFieldSpecs[static_cast<size_t>(field)];
    if (spec.kind == Kind::Clearance) {
        SettingsManager::instance().set_bed_map_clearance_mm(static_cast<int>(value));
        const size_t i = static_cast<size_t>(field);
        format_display(i);
        lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
        return;
    }
    if (spec.kind == Kind::Speed) {
        lv_obj_t* slider = speed_slider(spec.is_z);
        if (!slider) {
            spdlog::warn("[{}] No slider for field {}; dropping typed value {}", get_name(),
                         static_cast<int>(field), value);
            return;
        }
        const int mm_s = static_cast<int>(value);
        lv_slider_set_value(slider, mm_s, LV_ANIM_OFF);
        // Same path a drag takes: relabel, then persist. One code path for both.
        handle_jog_speed_changed(spec.is_z, mm_s);
    } else {
        SettingsManager::instance().set_jog_distance(spec.mode, spec.outer,
                                                     static_cast<float>(value));
        const size_t i = static_cast<size_t>(field);
        format_display(i);
        lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
    }
}

void MotionSettingsOverlay::handle_reset_distances() {
    helix::ui::ConfirmOptions opts;
    // The dialog outlives its exit animation, so the confirm callback needs a
    // token that survives this overlay being deactivated behind the modal.
    opts.owner_token = object_lifetime_.token();

    helix::ui::modal_confirm(
        lv_tr("Reset Distances?"),
        lv_tr("This will restore the default jog step distances for all three modes."),
        ModalSeverity::Warning, lv_tr("Reset"),
        [this] {
            spdlog::info("[{}] Resetting jog distances to defaults", get_name());
            SettingsManager::instance().reset_jog_distances();
            refresh_displays();
        },
        opts);
}

// ============================================================================
// KEYPAD
// ============================================================================

void MotionSettingsOverlay::on_keypad_value(float value, void* user_data) {
    auto* self = static_cast<MotionSettingsOverlay*>(user_data);
    if (!self) {
        return;
    }
    self->handle_keypad_value(self->pending_keypad_field_, static_cast<double>(value));
}

} // namespace helix::settings
