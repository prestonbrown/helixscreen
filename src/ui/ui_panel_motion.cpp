// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_motion.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_jog_pad.h"
#include "ui_motors_off.h"
#include "ui_panel_common.h"
#include "ui_panel_controls.h"
#include "ui_settings_motion.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "jog_coalescer.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "toolhead_homing.h"
#include "ui/ui_widget_helpers.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include "hv/json.hpp"

using namespace helix;
using helix::ui::find_required;

namespace {
/// How far Park lifts the nozzle away from the plate before moving over it.
constexpr double PARK_Z_LIFT_MM = 10.0;

/// Bed tab: finger travel up to this is still a tap at the press point.
constexpr int BED_MAP_DRAG_THRESHOLD_PX = 8;
} // namespace

/// Trim trailing zeros so 0.1 reads "0.1" and 10.0 reads "10"; the default
/// distances must render as 0.1, 1, 10 and 50.
static void format_distance_label(char* buf, size_t n, float mm) {
    if (mm == std::floor(mm)) {
        std::snprintf(buf, n, "%.0f", static_cast<double>(mm));
    } else {
        std::snprintf(buf, n, "%g", static_cast<double>(mm));
    }
}

/// Axis readouts share one line in the header (and the portrait strip under
/// it), so they carry no unit suffix; digit alignment comes from the mono
/// font the readout labels use, not from padding in the string.
static void format_axis_value(char* buf, size_t n, float mm) {
    std::snprintf(buf, n, "%.2f", static_cast<double>(mm));
}

helix::JogModeDistances helix::get_jog_mode_distances(JogMode mode) {
    auto& settings = SettingsManager::instance();
    JogModeDistances d{};
    d.inner = settings.get_jog_distance(mode, /*outer=*/false);
    d.outer = settings.get_jog_distance(mode, /*outer=*/true);
    format_distance_label(d.inner_label, sizeof(d.inner_label), d.inner);
    format_distance_label(d.outer_label, sizeof(d.outer_label), d.outer);
    return d;
}

std::optional<helix::AxisKeypadParams>
helix::keypad_params_for_axis(const AxisBounds& bounds, Axis axis, double commanded_mm) {
    float min = 0.0f, max = 0.0f;
    switch (axis) {
    case Axis::X:
        if (!bounds.has_x)
            return std::nullopt;
        min = bounds.x_min;
        max = bounds.x_max;
        break;
    case Axis::Y:
        if (!bounds.has_y)
            return std::nullopt;
        min = bounds.y_min;
        max = bounds.y_max;
        break;
    case Axis::Z:
        if (!bounds.has_z)
            return std::nullopt;
        min = bounds.z_min;
        max = bounds.z_max;
        break;
    }
    return AxisKeypadParams{min, max, static_cast<float>(commanded_mm), min < 0.0f};
}

bool helix::z_direction_blocked(bool z_homed, bool bounds_known, double z, double z_min,
                                double z_max, double direction_mm) {
    if (!z_homed || !bounds_known || direction_mm == 0.0) {
        return false;
    }
    // The reading is truncated to POSITION_RESOLUTION_MM, so a head parked at
    // the bound can read up to that much short of it. What a press could still
    // move from there is below what the clamp's margin already gives away.
    const double tolerance = POSITION_RESOLUTION_MM + AxisMove::EPSILON_MM;
    if (direction_mm > 0.0) {
        return z >= z_max - tolerance;
    }
    return z <= z_min + tolerance;
}

// Strip Klipper error prefixes, parse JSON error objects, and truncate for toast display.
// Some Klipper builds (e.g. K1C) send errors as JSON:
//   {"code":"key585","msg":"Move out of range: ...","values":[...]}
static std::string clean_gcode_error(const std::string& msg) {
    std::string cleaned = msg;

    // Strip Klipper "!! " error prefix
    if (cleaned.size() > 3 && cleaned.substr(0, 3) == "!! ") {
        cleaned = cleaned.substr(3);
    }

    // Parse JSON error objects - extract the "msg" field; anything that does
    // not parse or lacks a string "msg" is used as-is
    if (!cleaned.empty() && cleaned[0] == '{') {
        auto j = nlohmann::json::parse(cleaned, nullptr, false);
        if (!j.is_discarded() && j.contains("msg") && j["msg"].is_string()) {
            cleaned = j["msg"].get<std::string>();
        }
    }

    // Provide friendly messages for common error patterns
    if (cleaned.find("Must home axis") != std::string::npos ||
        cleaned.find("must home") != std::string::npos) {
        return lv_tr("Must home axes first");
    }
    // "Move out of range" — let through with truncation so user sees the limits

    // Truncate long messages for toast display
    constexpr size_t MAX_LEN = 80;
    if (cleaned.size() > MAX_LEN) {
        cleaned = cleaned.substr(0, MAX_LEN - 3) + "...";
    }

    return cleaned;
}

/// Bed-grid preset keys as they appear in motion_panel.xml user_data, in
/// MotionPreset declaration order (rear row first, front row last).
static std::optional<helix::MotionPreset> preset_from_key(const char* key) {
    static const struct {
        const char* key;
        helix::MotionPreset preset;
    } table[] = {
        {"rear_left", helix::MotionPreset::RearLeft},     {"rear", helix::MotionPreset::Rear},
        {"rear_right", helix::MotionPreset::RearRight},   {"left", helix::MotionPreset::Left},
        {"center", helix::MotionPreset::Center},          {"right", helix::MotionPreset::Right},
        {"front_left", helix::MotionPreset::FrontLeft},   {"front", helix::MotionPreset::Front},
        {"front_right", helix::MotionPreset::FrontRight},
    };
    for (const auto& row : table) {
        if (std::strcmp(key, row.key) == 0) {
            return row.preset;
        }
    }
    return std::nullopt;
}

// ============================================================================
// Constructor
// ============================================================================

MotionPanel::MotionPanel() {
    // Initialize buffer contents (axis labels are in XML, values only here)
    std::strcpy(pos_x_buf_, "—");
    std::strcpy(pos_y_buf_, "—");
    std::strcpy(pos_z_buf_, "—");
    std::strcpy(z_axis_label_buf_, "Z Axis"); // Default before kinematics detected
    std::strcpy(z_up_icon_buf_, "arrow_up");
    std::strcpy(z_down_icon_buf_, "arrow_down");
    std::strcpy(z_large_label_buf_, "10mm");
    std::strcpy(z_small_label_buf_, "1mm");

    // Load persisted jog mode (default: coarse)
    // Migrate from old bool /motion/jog_mode_fine to new int /motion/jog_mode
    auto* cfg = Config::get_instance();
    int mode = cfg->get<int>("/motion/jog_mode", -1);
    if (mode >= 0 && mode < JOG_MODE_COUNT) {
        current_mode_ = static_cast<JogMode>(mode);
    } else {
        // Migrate legacy bool setting and persist new key
        bool fine = cfg->get<bool>("/motion/jog_mode_fine", false);
        current_mode_ = fine ? JogMode::Fine : JogMode::Coarse;
        cfg->set("/motion/jog_mode", static_cast<int>(current_mode_));
        cfg->save();
    }
}

MotionPanel::~MotionPanel() {
    // SubjectManager (subjects_) handles deinit automatically via RAII
    // No need to call deinit_subjects() manually
    // StaticPanelRegistry::destroy_all() can run before lv_deinit(), so the
    // hold timer must be cancelled here too, not only in the cleanup hooks.
    stop_hold_repeat();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void MotionPanel::init_subjects() {
    // SubjectInitializer initializes this at boot, ahead of the first show().
    if (subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize position subjects with default placeholder values
    // Axis labels are in XML, subjects contain values only
    UI_MANAGED_SUBJECT_STRING(pos_x_subject_, pos_x_buf_, "—", "motion_pos_x", subjects_);
    UI_MANAGED_SUBJECT_STRING(pos_y_subject_, pos_y_buf_, "—", "motion_pos_y", subjects_);
    UI_MANAGED_SUBJECT_STRING(pos_z_subject_, pos_z_buf_, "—", "motion_pos_z", subjects_);

    // Z-axis label: "Bed" (corexy/corexz) or "Print Head" (cartesian/delta)
    UI_MANAGED_SUBJECT_STRING(z_axis_label_subject_, z_axis_label_buf_, "Z Axis",
                              "motion_z_axis_label", subjects_);

    // Z button icons: expand variants for bed-moves, regular for head-moves
    UI_MANAGED_SUBJECT_STRING(z_up_icon_subject_, z_up_icon_buf_, "arrow_up", "motion_z_up_icon",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(z_down_icon_subject_, z_down_icon_buf_, "arrow_down",
                              "motion_z_down_icon", subjects_);

    // Z button labels — dynamic based on jog mode
    const auto& mode_dist = get_jog_mode_distances(current_mode_);
    snprintf(z_large_label_buf_, sizeof(z_large_label_buf_), "%smm", mode_dist.outer_label);
    snprintf(z_small_label_buf_, sizeof(z_small_label_buf_), "%smm", mode_dist.inner_label);
    UI_MANAGED_SUBJECT_STRING(z_large_label_subject_, z_large_label_buf_, z_large_label_buf_,
                              "motion_z_large_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(z_small_label_subject_, z_small_label_buf_, z_small_label_buf_,
                              "motion_z_small_label", subjects_);

    // Jog mode toggle subjects for button styling
    UI_MANAGED_SUBJECT_INT(jog_mode_fine_active_, (current_mode_ == JogMode::Fine) ? 1 : 0,
                           "motion_jog_mode_fine_active", subjects_);
    UI_MANAGED_SUBJECT_INT(jog_mode_coarse_active_, (current_mode_ == JogMode::Coarse) ? 1 : 0,
                           "motion_jog_mode_coarse_active", subjects_);
    UI_MANAGED_SUBJECT_INT(jog_mode_turbo_active_, (current_mode_ == JogMode::Turbo) ? 1 : 0,
                           "motion_jog_mode_turbo_active", subjects_);

    // Per-axis homed flags (0=unhomed, 1=homed). Prefixed with motion_ to
    // avoid collision with ControlsPanel's x_homed/y_homed/z_homed.
    UI_MANAGED_SUBJECT_INT(motion_x_homed_, 0, "motion_x_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_y_homed_, 0, "motion_y_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_z_homed_, 0, "motion_z_homed", subjects_);

    // Z button limit state (0=enabled, 1=disabled); XML binds these with
    // bind_state_if_eq state="disabled" on every Z button in both layouts.
    UI_MANAGED_SUBJECT_INT(motion_z_up_blocked_, 0, "motion_z_up_blocked", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_z_down_blocked_, 0, "motion_z_down_blocked", subjects_);

    // Content tab (0=Jog, 1=Move, 2=Bed) plus the strip state the shared
    // zone_tab component binds: one active flag per tab, one label each.
    UI_MANAGED_SUBJECT_INT(motion_tab_subject_, motion_tab_, "motion_tab", subjects_);
    refresh_tab_labels(); // fills the buffers the label subjects start from
    for (int i = 0; i < 3; ++i) {
        char subject_name[32];
        snprintf(subject_name, sizeof(subject_name), "motion_tab_label_%d", i);
        UI_MANAGED_SUBJECT_STRING(motion_tab_label_[i], motion_tab_label_buf_[i],
                                  motion_tab_label_buf_[i], subject_name, subjects_);
        snprintf(subject_name, sizeof(subject_name), "motion_tab_active_%d", i);
        UI_MANAGED_SUBJECT_INT(motion_tab_active_[i], i == motion_tab_ ? 1 : 0, subject_name,
                               subjects_);
    }

    UI_MANAGED_SUBJECT_STRING(motion_bed_readout_, motion_bed_readout_buf_, "",
                              "motion_bed_readout", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_bed_circular_, 0, "motion_bed_circular", subjects_);

    // Register PrinterState observers (RAII - auto-removed on destruction)
    register_position_observers();

    subjects_initialized_ = true;

    // Sync initial position values (observers only fire on change, not on subscribe)
    // Without this, panel shows dashes until next position update even if printer is homed
    current_x_ = static_cast<float>(helix::units::from_centimm(
        lv_subject_get_int(get_printer_state().motion_state().get_gcode_position_x_subject())));
    current_y_ = static_cast<float>(helix::units::from_centimm(
        lv_subject_get_int(get_printer_state().motion_state().get_gcode_position_y_subject())));
    gcode_z_centimm_ =
        lv_subject_get_int(get_printer_state().motion_state().get_gcode_position_z_subject());
    current_z_ = static_cast<float>(helix::units::from_centimm(gcode_z_centimm_));
    live_x_ = static_cast<float>(helix::units::from_centimm(
        lv_subject_get_int(get_printer_state().motion_state().get_live_position_x_subject())));
    live_y_ = static_cast<float>(helix::units::from_centimm(
        lv_subject_get_int(get_printer_state().motion_state().get_live_position_y_subject())));
    live_z_ = static_cast<float>(helix::units::from_centimm(
        lv_subject_get_int(get_printer_state().motion_state().get_live_position_z_subject())));
    show_actual_ = SettingsManager::instance().get_motion_show_actual_position();
    refresh_position_display();

    int bed_moves =
        lv_subject_get_int(get_printer_state().capabilities_state().subject(Capability::BedMoves));

    // Update Z axis label
    update_z_axis_label(bed_moves != 0);

    update_z_button_blocked();

    spdlog::debug("[{}] Subjects initialized: X/Y/Z position + Z-axis label + observers ({} "
                  "subjects managed)",
                  get_name(), subjects_.count());
}

void MotionPanel::deinit_subjects() {
    // NOTE: This method exists for API symmetry with init_subjects() and to support
    // explicit cleanup if needed. However, it's NOT called in the destructor because
    // SubjectManager handles cleanup via RAII. This is intentional - RAII is preferred.
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Deinitializing subjects", get_name());

    // SubjectManager handles deinitialization of all registered subjects
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

/// Forward a pointer event on the Bed tab's surface with the point it fired at.
static void on_bed_touch_event(lv_event_t* e, helix::BedTouch phase) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    get_global_motion_panel().handle_bed_touch(phase, lv_event_get_current_target_obj(e), point);
}

void MotionPanel::register_callbacks() {
    register_xml_callbacks({
        // Unified Z-axis button callback (user_data from XML distinguishes buttons)
        {"on_motion_z_button",
         [](lv_event_t* e) {
             MotionPanel& panel = get_global_motion_panel();
             // A hold that repeated must not add one extra jog on release; a plain
             // tap (no repeat) jogs exactly once through this path as before.
             const bool swallow = panel.z_hold_timer().swallow_click();
             panel.z_hold_timer().cancel();
             if (swallow) {
                 spdlog::debug("[MotionPanel] Z click swallowed after hold repeat");
             } else if (const char* button_id =
                            static_cast<const char*>(lv_event_get_user_data(e))) {
                 panel.handle_z_button(button_id);
             }
         }},
        // Hold-to-repeat arms on press and stops on release / press lost
        {"on_motion_z_button_pressed",
         [](lv_event_t* e) {
             if (const char* button_id = static_cast<const char*>(lv_event_get_user_data(e))) {
                 get_global_motion_panel().begin_z_hold(
                     button_id, static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
             }
         }},
        {"on_motion_z_button_released",
         [](lv_event_t*) { get_global_motion_panel().z_hold_timer().release(); }},
        {"on_motion_z_button_press_lost",
         [](lv_event_t*) { get_global_motion_panel().z_hold_timer().cancel(); }},
        // Leveling buttons delegate to the ControlsPanel singleton
        {"on_motion_qgl", [](lv_event_t*) { get_global_controls_panel().handle_qgl(); }},
        {"on_motion_z_tilt", [](lv_event_t*) { get_global_controls_panel().handle_z_tilt(); }},
        {"on_jog_mode_fine",
         [](lv_event_t*) { get_global_motion_panel().set_jog_mode(JogMode::Fine); }},
        {"on_jog_mode_coarse",
         [](lv_event_t*) { get_global_motion_panel().set_jog_mode(JogMode::Coarse); }},
        {"on_jog_mode_turbo",
         [](lv_event_t*) { get_global_motion_panel().set_jog_mode(JogMode::Turbo); }},
        // Header cog: opens the Motion settings overlay through its single opener
        {"on_motion_header_settings_clicked",
         [](lv_event_t*) { helix::settings::show_motion_settings_overlay(); }},
        // Coordinate readouts: tap an axis pair for the keypad (user_data is the axis
        // letter), tap the swap icon to flip commanded/actual display.
        {"on_motion_pos_clicked",
         [](lv_event_t* e) {
             const char* axis = static_cast<const char*>(lv_event_get_user_data(e));
             if (axis && axis[0]) {
                 get_global_motion_panel().open_axis_keypad(axis[0]);
             }
         }},
        {"on_motion_swap_coords_clicked",
         [](lv_event_t*) { get_global_motion_panel().toggle_coordinate_source(); }},
        // Move tab: tab strip (user_data is the tab index), the nine bed presets
        // (user_data is the preset key), Park and Motors off.
        {"on_motion_tab_clicked",
         [](lv_event_t* e) {
             if (auto tab = helix::ui::event_user_int(e))
                 get_global_motion_panel().set_motion_tab(*tab);
         }},
        {"on_motion_preset_clicked",
         [](lv_event_t* e) {
             const char* key = static_cast<const char*>(lv_event_get_user_data(e));
             if (auto preset = key ? preset_from_key(key) : std::nullopt) {
                 get_global_motion_panel().handle_preset(*preset);
             }
         }},
        {"on_motion_park_clicked", [](lv_event_t*) { get_global_motion_panel().handle_park(); }},
        {"on_motion_motors_off_clicked",
         [](lv_event_t*) { get_global_motion_panel().handle_motors_off(); }},
        // Bed tab: the touch surface's pointer events, its size, and Home.
        {"on_motion_bed_pressed",
         [](lv_event_t* e) { on_bed_touch_event(e, helix::BedTouch::Pressed); }},
        {"on_motion_bed_pressing",
         [](lv_event_t* e) { on_bed_touch_event(e, helix::BedTouch::Pressing); }},
        {"on_motion_bed_released",
         [](lv_event_t* e) { on_bed_touch_event(e, helix::BedTouch::Released); }},
        {"on_motion_bed_press_lost",
         [](lv_event_t* e) { on_bed_touch_event(e, helix::BedTouch::Lost); }},
        {"on_motion_bed_size_changed",
         [](lv_event_t*) { get_global_motion_panel().layout_bed_map(); }},
        {"on_motion_bed_home", [](lv_event_t*) { get_global_motion_panel().home('A'); }},
    });
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* MotionPanel::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    setup_jog_pad();
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void MotionPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Every open starts on Jog: a stale Move/Bed selection from a previous
    // visit would hide the pad the user came here for.
    set_motion_tab(0);

    // Recalculate jog pad size — the wrapper dimensions may differ after re-layout
    if (jog_pad_ && overlay_root_) {
        fit_jog_pad();
        // Jog step distances are settings, and both the header cog and
        // Settings > Printing > Motion can change them while this panel sits on
        // the stack. The ring labels are painted from the draw callback, so a
        // repaint is all they need to re-read the new values.
        lv_obj_invalidate(jog_pad_);
    }

    // The Z button labels are subject-bound and have no repaint to ride in on.
    update_z_button_labels();
    // The clearance setting can change while this panel sits on the stack.
    refresh_bed_readout();
}

void MotionPanel::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[{}] on_deactivating({})", get_name(), deactivate_reason_name(reason));

    // The base invalidates lifetime_ on the way out of this hook, dropping any
    // in-flight ack callback — fully reset the coalescer so it can't get stuck
    // in_flight forever.
    jog_coalescer_.reset();
    stop_hold_repeat();
    // A release arriving after the panel comes back belongs to no gesture.
    end_bed_gesture();
}

void MotionPanel::on_ui_destroyed() {
    // Null raw child pointers so persistent observers (jog_ready_observer_
    // dereferences jog_pad_ on every connection/klippy flap) can't UAF if the
    // overlay widget tree is ever destroyed (destroy-on-close, parent screen
    // teardown). Peers do the same — see BedMeshPanel::on_ui_destroyed.
    jog_pad_ = nullptr;
    jog_coalescer_.reset();
    stop_hold_repeat();
    end_bed_gesture();
}

// ============================================================================
// Hold-to-Repeat (Z buttons; the pad owns its own repeat)
// ============================================================================

void MotionPanel::begin_z_hold(const char* button_name, lv_obj_t* button) {
    if (!button_name)
        return;
    snprintf(z_hold_button_, sizeof(z_hold_button_), "%s", button_name);
    z_hold_obj_ = button;
    z_hold_repeated_ = false;
    z_hold_timer_.begin(&MotionPanel::z_hold_fire, this);
}

bool MotionPanel::z_hold_fire(void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    // The first tick is this press's initial jog; later ones are repeats.
    const bool is_repeat = self->z_hold_repeated_;
    self->z_hold_repeated_ = true;
    const bool keep_going = self->handle_z_button(self->z_hold_button_, is_repeat);
    lv_obj_t* held = self->z_hold_obj_.get();
    if (!keep_going && held && lv_obj_has_state(held, LV_STATE_DISABLED)) {
        lv_obj_remove_state(held, LV_STATE_PRESSED);
    }
    return keep_going;
}

void MotionPanel::stop_hold_repeat() {
    z_hold_timer_.cancel();
    if (jog_pad_)
        ui_jog_pad_stop_repeat(jog_pad_);
}

// ============================================================================
// Jog Pad Setup
// ============================================================================

void MotionPanel::fit_jog_pad() {
    lv_obj_t* jog_wrapper = lv_obj_get_parent(jog_pad_);
    if (!jog_wrapper)
        return;

    lv_obj_update_layout(overlay_root_);
    const lv_coord_t wrapper_h = lv_obj_get_height(jog_wrapper);

    // Portrait only: the Z column grows beside the wrapper, so the wrapper's
    // resolved share under-reports the width a square pad could claim. Size
    // against the row minus the column's floor instead, then clamp the
    // wrapper to the square so the growing column absorbs the leftover (no
    // dead band beside a height-bound pad). Landscape keeps its growing
    // wrapper: the pad centres in it and the Z controls live in a separate
    // right-hand column.
    lv_obj_t* pad_row = lv_obj_get_parent(jog_wrapper);
    lv_coord_t wrapper_w = lv_obj_get_width(jog_wrapper);
    const bool portrait_pad_row = lv_streq(lv_obj_get_name(pad_row), "pad_row");
    if (portrait_pad_row) {
        lv_obj_t* z_column = lv_obj_get_child_by_name(pad_row, "z_column");
        if (z_column) {
            const lv_coord_t z_floor = lv_obj_get_style_min_width(z_column, LV_PART_MAIN);
            wrapper_w = lv_obj_get_content_width(pad_row) - z_floor -
                        lv_obj_get_style_pad_column(pad_row, LV_PART_MAIN);
        }
    }

    const lv_coord_t jog_size = LV_MIN(wrapper_w, wrapper_h);
    if (portrait_pad_row) {
        lv_obj_set_width(jog_wrapper, jog_size);
        lv_obj_set_flex_grow(jog_wrapper, 0);
    }
    lv_obj_set_width(jog_pad_, jog_size);
    lv_obj_set_height(jog_pad_, jog_size);
}

void MotionPanel::setup_jog_pad() {
    // Find overlay_content to access motion panel widgets
    lv_obj_t* overlay_content = find_required(overlay_root_, "overlay_content", get_name());
    // Find jog pad container from XML and replace it with the widget
    lv_obj_t* jog_pad_container = find_required(overlay_content, "jog_pad_container", get_name());
    if (!jog_pad_container) {
        return;
    }

    // Get parent container (jog_pad_wrapper, which flex_grows inside left_column)
    lv_obj_t* jog_wrapper = lv_obj_get_parent(jog_pad_container);

    // Each orientation's placeholder carries the pad's alignment: portrait
    // left-hugs the pad row (a height-bound square would otherwise float
    // centred with dead bands), landscape centres it in the wider wrapper.
    const lv_align_t pad_align = lv_obj_get_style_align(jog_pad_container, LV_PART_MAIN);

    // Delete placeholder container
    helix::ui::safe_delete(jog_pad_container);

    // Create jog pad widget
    jog_pad_ = ui_jog_pad_create(jog_wrapper);
    if (jog_pad_) {
        lv_obj_set_name(jog_pad_, "jog_pad");
        lv_obj_set_align(jog_pad_, pad_align);

        fit_jog_pad();

        // Set callbacks - pass 'this' as user_data
        ui_jog_pad_set_jog_callback(jog_pad_, jog_pad_jog_cb, this);
        ui_jog_pad_set_home_callback(jog_pad_, jog_pad_home_cb, this);

        // Set initial jog mode
        ui_jog_pad_set_mode(jog_pad_, current_mode_);

        // Apply initial enabled/dimmed state (the observer only fires on change)
        update_jog_pad_enabled();

        spdlog::debug("[{}] Jog pad widget created", get_name());
    } else {
        spdlog::error("[{}] Failed to create jog pad widget!", get_name());
    }
}

// ============================================================================
// Position Observers
// ============================================================================

void MotionPanel::register_position_observers() {
    // Subscribe to PrinterState position updates so UI reflects real printer position
    // Using observer factory for type-safe lambda-based observers with RAII cleanup

    using helix::ui::observe;

    // Use gcode position (commanded) for X/Y display and jog calculations
    position_x_observer_ = observe<int>(
        get_printer_state().motion_state().get_gcode_position_x_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->current_x_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->update_bed_marker();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());

    position_y_observer_ = observe<int>(
        get_printer_state().motion_state().get_gcode_position_y_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->current_y_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->update_bed_marker();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());

    // The readout shows the commanded (gcode) Z; mesh-compensated toolhead Z
    // has no display here.
    gcode_z_observer_ = observe<int>(
        get_printer_state().motion_state().get_gcode_position_z_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->gcode_z_centimm_ = centimm;
            self->current_z_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->update_z_button_blocked();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());

    // A G-code offset change (SET_GCODE_OFFSET, saved Z offset, toolchanger
    // tool offsets) shifts the G-code envelope the Z buttons clamp against,
    // while the commanded position can round to the same centimillimeter and
    // fire nothing: this subject is homing_origin[2] and moves with it.
    gcode_z_offset_observer_ = observe<int>(
        get_printer_state().motion_state().get_gcode_z_offset_subject(), this,
        [](MotionPanel* self, int) {
            if (!self->subjects_initialized_)
                return;
            self->update_z_button_blocked();
        },
        get_printer_state().get_subjects_lifetime());

    // Actual (live) positions from motion_report.live_position; the readouts
    // show these while the coordinate preference is "actual".
    live_position_observer_x_ = observe<int>(
        get_printer_state().motion_state().get_live_position_x_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->live_x_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());
    live_position_observer_y_ = observe<int>(
        get_printer_state().motion_state().get_live_position_y_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->live_y_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());
    live_position_observer_z_ = observe<int>(
        get_printer_state().motion_state().get_live_position_z_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->live_z_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->refresh_position_display();
            self->refresh_bed_readout();
        },
        get_printer_state().get_subjects_lifetime());

    // Coordinate source preference: commanded (default) or actual (live).
    // Flipping the persisted setting re-renders the readouts.
    // Tab labels are translated into subject buffers, so a live language
    // switch has to re-fill them; XML text around them re-translates itself.
    language_observer_ = helix::ui::observe_language_change(this, [](MotionPanel* self) {
        self->refresh_tab_labels();
        self->update_z_axis_label(self->bed_moves_);
    });

    coordinate_mode_observer_ = observe<int>(
        SettingsManager::instance().subject_motion_show_actual_position(), this,
        [](MotionPanel* self, int show_actual) {
            self->show_actual_ = show_actual != 0;
            self->refresh_position_display();
            self->refresh_bed_readout();
        },
        SettingsManager::instance().get_subjects_lifetime());

    // Watch for kinematics changes to update Z-axis label ("Bed" vs "Print Head")
    // Use Dispatch::Immediate — label/icon updates are safe to do immediately,
    // and the deferred callback can be lost during panel recreation (#610)
    bed_moves_observer_ = helix::ui::observe<int>(
        get_printer_state().capabilities_state().subject(Capability::BedMoves), this,
        [](MotionPanel* self, int bed_moves) {
            if (!self->subjects_initialized_)
                return;
            self->update_z_axis_label(bed_moves != 0);
        },
        get_printer_state().get_subjects_lifetime(), helix::ui::Dispatch::Immediate);

    // Observe homed_axes from PrinterState to publish the per-axis homed
    // subjects (the readouts mute an unhomed axis) and recolor the
    // custom-drawn center home button: warning tint until all axes are homed.
    homed_axes_observer_ = observe<const char*>(
        get_printer_state().motion_state().get_homed_axes_subject(), this,
        [](MotionPanel* self, const char* axes) {
            if (!self->subjects_initialized_)
                return;
            int x = (strchr(axes, 'x') != nullptr) ? 1 : 0;
            int y = (strchr(axes, 'y') != nullptr) ? 1 : 0;
            int z = (strchr(axes, 'z') != nullptr) ? 1 : 0;
            lv_subject_set_int(&self->motion_x_homed_, x);
            lv_subject_set_int(&self->motion_y_homed_, y);
            lv_subject_set_int(&self->motion_z_homed_, z);
            self->update_z_button_blocked();
            self->refresh_bed_readout();
            if (self->jog_pad_)
                ui_jog_pad_set_homed(self->jog_pad_, x && y && z);
        },
        get_printer_state().get_subjects_lifetime());

    // Dim/enable the jog pad to track connection + klippy readiness. The same
    // subject greys the surrounding panel content via motion_panel.xml, but the
    // custom-drawn jog pad has no XML binding, so drive it here.
    jog_ready_observer_ = observe<int>(
        get_printer_state().network_state().get_nav_buttons_enabled_subject(), this,
        [](MotionPanel* self, int) {
            if (!self->subjects_initialized_)
                return;
            self->update_jog_pad_enabled();
            // Bounds land with the connect/klippy-ready frames; recompute so a
            // fresh envelope re-enables or disables the Z buttons and re-fits
            // the Bed tab's plate.
            self->update_z_button_blocked();
            self->layout_bed_map();
        },
        get_printer_state().get_subjects_lifetime());

    spdlog::debug("[{}] Position + kinematics + homing observers registered (observer factory)",
                  get_name());
}

void MotionPanel::update_jog_pad_enabled() {
    if (!jog_pad_)
        return;
    bool ready = lv_subject_get_int(
                     get_printer_state().network_state().get_nav_buttons_enabled_subject()) != 0;
    ui_jog_pad_set_enabled(jog_pad_, ready);
}

void MotionPanel::update_z_button_blocked() {
    if (!subjects_initialized_)
        return;
    const auto bounds = clamp_bounds();
    const bool z_homed = helix::axis_is_homed(get_printer_state(), helix::Axis::Z);
    // Commanded, not predicted: an in-flight move leaves no recompute trigger
    // when it acks, so a prediction here could disable a button for good.
    const double z = current_z_;
    // The G-code direction the on-screen up buttons drive, after the bed_moves
    // inversion: screen-up is +Z unless the bed moves.
    const double up_dir = bed_moves_ ? -1.0 : 1.0;
    lv_subject_set_int(
        &motion_z_up_blocked_,
        z_direction_blocked(z_homed, bounds.has_z, z, bounds.z_min, bounds.z_max, up_dir) ? 1 : 0);
    lv_subject_set_int(
        &motion_z_down_blocked_,
        z_direction_blocked(z_homed, bounds.has_z, z, bounds.z_min, bounds.z_max, -up_dir) ? 1 : 0);
}

// Observer callbacks migrated to lambda-based observer factory pattern
// See register_position_observers() for inline observers

void MotionPanel::update_z_axis_label(bool bed_moves) {
    bed_moves_ = bed_moves; // Store for Z button direction inversion
    const char* label = bed_moves ? lv_tr("Bed") : lv_tr("Print Head");
    std::strncpy(z_axis_label_buf_, label, sizeof(z_axis_label_buf_) - 1);
    z_axis_label_buf_[sizeof(z_axis_label_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_axis_label_subject_, z_axis_label_buf_);

    // Update Z button icons: expand variants (with platform line) for bed-moves
    const char* up_icon = bed_moves ? "arrow_expand_up" : "arrow_up";
    const char* down_icon = bed_moves ? "arrow_expand_down" : "arrow_down";
    std::strncpy(z_up_icon_buf_, up_icon, sizeof(z_up_icon_buf_) - 1);
    z_up_icon_buf_[sizeof(z_up_icon_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_up_icon_subject_, z_up_icon_buf_);
    std::strncpy(z_down_icon_buf_, down_icon, sizeof(z_down_icon_buf_) - 1);
    z_down_icon_buf_[sizeof(z_down_icon_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_down_icon_subject_, z_down_icon_buf_);

    // The inversion flips which bound each on-screen direction drives toward.
    update_z_button_blocked();

    spdlog::debug("[{}] Z-axis updated: label={}, icons={}/{} (bed_moves={})", get_name(), label,
                  up_icon, down_icon, bed_moves);
}

void MotionPanel::refresh_position_display() {
    if (!subjects_initialized_)
        return;
    const float x = show_actual_ ? live_x_ : current_x_;
    const float y = show_actual_ ? live_y_ : current_y_;
    const float z = show_actual_ ? live_z_ : current_z_;
    format_axis_value(pos_x_buf_, sizeof(pos_x_buf_), x);
    format_axis_value(pos_y_buf_, sizeof(pos_y_buf_), y);
    format_axis_value(pos_z_buf_, sizeof(pos_z_buf_), z);
    lv_subject_copy_string(&pos_x_subject_, pos_x_buf_);
    lv_subject_copy_string(&pos_y_subject_, pos_y_buf_);
    lv_subject_copy_string(&pos_z_subject_, pos_z_buf_);
}

// ============================================================================
// Z Button Handler
// ============================================================================

bool MotionPanel::handle_z_button(const char* name, bool is_repeat) {
    spdlog::debug("[{}] Z button callback fired! Button name: '{}'", get_name(),
                  name ? name : "(null)");

    if (!name) {
        spdlog::error("[{}] Button has no name!", get_name());
        return false;
    }

    // Z distance from current jog mode (Fine: 0.1/1, Coarse: 1/10, Turbo: 10/50)
    const auto& mode_dist = get_jog_mode_distances(current_mode_);
    double large_dist = static_cast<double>(mode_dist.outer);
    double small_dist = static_cast<double>(mode_dist.inner);

    double distance = 0.0;
    if (strcmp(name, "z_up_large") == 0) {
        distance = large_dist;
    } else if (strcmp(name, "z_up_small") == 0) {
        distance = small_dist;
    } else if (strcmp(name, "z_down_small") == 0) {
        distance = -small_dist;
    } else if (strcmp(name, "z_down_large") == 0) {
        distance = -large_dist;
    } else {
        spdlog::error("[{}] Unknown button name: '{}'", get_name(), name);
        return false;
    }

    // For bed-moves printers (CoreXY etc), invert direction so arrows match physical motion:
    // - Up arrow = bed moves UP toward nozzle = G-code Z- (bed rises, gap decreases)
    // - Down arrow = bed moves DOWN away from nozzle = G-code Z+ (bed lowers, gap increases)
    if (bed_moves_) {
        distance = -distance;
        spdlog::debug("[{}] Bed-moves printer: inverted Z direction for bed movement", get_name());
    }

    // Bounds are in gcode space, so this must follow the inversion above.
    const auto bounds = clamp_bounds();
    if (bounds.has_z && helix::axis_is_homed(get_printer_state(), helix::Axis::Z)) {
        distance = clamp_axis_delta(helix::Axis::Z, current_z_,
                                    jog_coalescer_.predicted_z(current_z_) - current_z_, distance,
                                    bounds.z_min, bounds.z_max, /*fresh_press=*/!is_repeat);
        if (distance == 0.0) {
            return false;
        }
    }

    spdlog::debug("[{}] Z jog: {:+.2f}mm (bed_moves={})", get_name(), distance, bed_moves_);

    return dispatch_jog({0.0, 0.0, distance});
}

// ============================================================================
// Jog Pad Callbacks
// ============================================================================

bool MotionPanel::jog_pad_jog_cb(JogDirection direction, float distance_mm, bool is_repeat,
                                 void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (!self) {
        return false;
    }
    return self->jog(direction, distance_mm, is_repeat);
}

void MotionPanel::jog_pad_home_cb(void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (self) {
        self->home('A'); // Home all axes
    }
}

// ============================================================================
// Public API
// ============================================================================

void MotionPanel::set_position(float x, float y, float z) {
    current_x_ = x;
    current_y_ = y;
    current_z_ = z;

    int z_centimm = helix::units::to_centimm(static_cast<double>(z));
    gcode_z_centimm_ = z_centimm;

    if (!subjects_initialized_)
        return;

    // Update subjects (will automatically update bound UI elements)
    refresh_position_display();
}

bool MotionPanel::jog(JogDirection direction, float distance_mm, bool is_repeat) {
    const char* dir_names[] = {"N(+Y)",    "S(-Y)",    "E(+X)",    "W(-X)",
                               "NE(+X+Y)", "NW(-X+Y)", "SE(+X-Y)", "SW(-X-Y)"};

    spdlog::debug("[{}] Jog command: {} {:.1f}mm", get_name(),
                  dir_names[static_cast<int>(direction)], distance_mm);

    // Calculate dx/dy from direction
    float dx = 0.0f, dy = 0.0f;

    switch (direction) {
    case JogDirection::N:
        dy = distance_mm;
        break;
    case JogDirection::S:
        dy = -distance_mm;
        break;
    case JogDirection::E:
        dx = distance_mm;
        break;
    case JogDirection::W:
        dx = -distance_mm;
        break;
    case JogDirection::NE:
        dx = distance_mm;
        dy = distance_mm;
        break;
    case JogDirection::NW:
        dx = -distance_mm;
        dy = distance_mm;
        break;
    case JogDirection::SE:
        dx = distance_mm;
        dy = -distance_mm;
        break;
    case JogDirection::SW:
        dx = -distance_mm;
        dy = -distance_mm;
        break;
    }

    // Soft-stop: clamp against the PREDICTED position (current + uncommitted
    // coalescer travel) so queued taps can't walk past the envelope. Skip when
    // bounds aren't known yet (fresh connect) or the axis isn't homed.
    const auto bounds = clamp_bounds();

    double ddx = static_cast<double>(dx);
    double ddy = static_cast<double>(dy);

    if (ddx != 0.0 && bounds.has_x && helix::axis_is_homed(get_printer_state(), helix::Axis::X)) {
        ddx = clamp_axis_delta(helix::Axis::X, current_x_,
                               jog_coalescer_.predicted_x(current_x_) - current_x_, ddx,
                               bounds.x_min, bounds.x_max, /*fresh_press=*/false);
    }
    if (ddy != 0.0 && bounds.has_y && helix::axis_is_homed(get_printer_state(), helix::Axis::Y)) {
        ddy = clamp_axis_delta(helix::Axis::Y, current_y_,
                               jog_coalescer_.predicted_y(current_y_) - current_y_, ddy,
                               bounds.y_min, bounds.y_max, /*fresh_press=*/false);
    }

    if (ddx == 0.0 && ddy == 0.0) {
        // Warn only when the whole press is refused: a diagonal along a wall
        // still moves the other axis, and partial travel is silent.
        if (!is_repeat) {
            if (dx != 0.0f) {
                warn_axis_limit(helix::Axis::X, dx > 0.0f ? bounds.x_max : bounds.x_min, dx > 0.0f);
            } else if (dy != 0.0f) {
                warn_axis_limit(helix::Axis::Y, dy > 0.0f ? bounds.y_max : bounds.y_min, dy > 0.0f);
            }
        }
        return false;
    }
    return dispatch_jog({ddx, ddy, 0.0});
}

double MotionPanel::clamp_axis_delta(helix::Axis axis, double current, double uncommitted,
                                     double delta, float min, float max, bool fresh_press) {
    const double allowed = helix::clamp_jog_delta(
        current, uncommitted, delta, static_cast<double>(min), static_cast<double>(max));
    if (std::abs(allowed) > helix::AxisMove::EPSILON_MM) {
        // Full or partial travel: silent either way.
        return allowed;
    }
    if (fresh_press) {
        // Repeat ticks into a limit stop silently; only the initial jog of a
        // press warns.
        warn_axis_limit(axis, delta > 0.0 ? max : min, delta > 0.0);
    }
    return 0.0;
}

helix::AxisBounds MotionPanel::clamp_bounds() {
    return helix::inset_bounds(get_printer_state().motion_state().get_gcode_axis_bounds(),
                               helix::GCODE_EDGE_MARGIN_MM);
}

void MotionPanel::warn_axis_limit(helix::Axis axis, float clamp_bound, bool at_max) {
    // The clamp stops GCODE_EDGE_MARGIN_MM inside; tell the user the limit.
    const auto limit = static_cast<float>(at_max ? clamp_bound + helix::GCODE_EDGE_MARGIN_MM
                                                 : clamp_bound - helix::GCODE_EDGE_MARGIN_MM);
    // Three literals rather than an assembled string: the translation
    // extractor scans for lv_tr() literals and cannot see a runtime key.
    char limit_buf[16];
    format_axis_value(limit_buf, sizeof(limit_buf), limit);
    switch (axis) {
    case helix::Axis::X:
        NOTIFY_WARNING(lv_tr("X is at its limit ({}mm)"), limit_buf);
        break;
    case helix::Axis::Y:
        NOTIFY_WARNING(lv_tr("Y is at its limit ({}mm)"), limit_buf);
        break;
    case helix::Axis::Z:
        NOTIFY_WARNING(lv_tr("Z is at its limit ({}mm)"), limit_buf);
        break;
    }
}

bool MotionPanel::dispatch_jog(const helix::AxisMove& delta) {
    if (auto immediate = jog_coalescer_.on_tap(delta)) {
        return send_jog_move(*immediate);
    }
    spdlog::debug("[{}] Jog coalesced: predicted x={:+.2f} y={:+.2f} z={:+.2f}", get_name(),
                  jog_coalescer_.predicted_x(current_x_), jog_coalescer_.predicted_y(current_y_),
                  jog_coalescer_.predicted_z(current_z_));
    return true;
}

bool MotionPanel::dispatch_target(const helix::AxisTarget& target) {
    // Soft-stop for absolute moves, same bounds source the jog clamp uses: a
    // set axis without a known envelope cannot be clamped, and sending it
    // unclamped would trust exactly the value that is missing.
    const auto bounds = clamp_bounds();
    if ((target.x && !bounds.has_x) || (target.y && !bounds.has_y) || (target.z && !bounds.has_z)) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return false;
    }
    std::optional<std::pair<double, double>> z_range;
    if (bounds.has_z) {
        z_range =
            std::make_pair(static_cast<double>(bounds.z_min), static_cast<double>(bounds.z_max));
    }
    const helix::AxisTarget clamped = helix::clamp_target_to_bounds(
        target, static_cast<double>(bounds.x_min), static_cast<double>(bounds.x_max),
        static_cast<double>(bounds.y_min), static_cast<double>(bounds.y_max), z_range);

    target_start_z_ = jog_coalescer_.target_start_z(current_z_);
    if (auto immediate = jog_coalescer_.on_target(clamped)) {
        return send_jog_move(*immediate);
    }
    spdlog::debug("[{}] Target coalesced: predicted x={:+.2f} y={:+.2f} z={:+.2f}", get_name(),
                  jog_coalescer_.predicted_x(current_x_), jog_coalescer_.predicted_y(current_y_),
                  jog_coalescer_.predicted_z(current_z_));
    return true;
}

bool MotionPanel::send_jog_move(const helix::JogCoalescer::CoalescedMove& move) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        jog_coalescer_.on_error();
        return false;
    }
    auto& settings = SettingsManager::instance();
    // Storage keeps the user's choice; emission is clamped to what the printer
    // currently permits, or move_relative/move_to would reject the move outright.
    const SafetyLimits& limits = api->get_safety_limits();
    const double xy_feedrate = static_cast<double>(helix::effective_jog_speed_mm_min(
        settings.get_jog_speed_xy(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));
    const double z_feedrate = static_cast<double>(helix::effective_jog_speed_mm_min(
        settings.get_jog_speed_z(), limits.min_feedrate_mm_min, limits.max_feedrate_mm_min));

    auto on_ack = lifetime_.bg_cb("MotionPanel::on_jog_ack", [this]() {
        if (auto flush = jog_coalescer_.on_ack()) {
            send_jog_move(*flush);
        }
        // An ack changes where the next target starts (target_start_z), and
        // with it the lift the Bed tab announces.
        refresh_bed_readout();
    });
    auto on_error = lifetime_.bg_cb("MotionPanel::on_jog_error", [this](const MoonrakerError& err) {
        jog_coalescer_.on_error();
        // A Bed-tab drag would send the next sample into the same refusal.
        bed_gesture_failed_ = true;
        // The printer refused the move: a hold-to-repeat still ticking would
        // re-send it every interval and raise one error toast per tick.
        stop_hold_repeat();
        NOTIFY_ERROR(lv_tr("Jog failed: {}"), clean_gcode_error(err.localized_message()));
    });

    if (const auto* delta = std::get_if<helix::AxisMove>(&move)) {
        api->motion().move_relative(delta->dx, delta->dy, delta->dz, xy_feedrate, z_feedrate,
                                    std::move(on_ack), std::move(on_error));
    } else if (const auto* target = std::get_if<helix::AxisTarget>(&move)) {
        api->motion().move_to(*target, xy_feedrate, z_feedrate, std::move(on_ack),
                              std::move(on_error), target_start_z_);
    }
    return true;
}

void MotionPanel::home(char axis) {
    spdlog::debug("[{}] Home command: {} axis", get_name(), axis);

    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        // Convert axis char to string for API ("" for all, "X", "Y", "Z", or "XY")
        std::string axes_str;
        if (axis == 'A') {
            axes_str = ""; // Empty string = home all
        } else {
            axes_str = std::string(1, axis);
        }

        api->motion().home_axes(
            axes_str,
            [axis]() {
                if (axis == 'A') {
                    helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("All axes homed"));
                } else {
                    helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("{} axis homed"), axis);
                }
            },
            [](const MoonrakerError& err) {
                // home_axes() answers on the WebSocket thread; the toast is main-thread only.
                helix::ui::run_on_main("MotionPanel::home_failed", [err]() {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"),
                                 clean_gcode_error(err.localized_message()));
                });
            });
    }
}

// ============================================================================
// Coordinate Source Preference + Axis Keypad
// ============================================================================

void MotionPanel::toggle_coordinate_source() {
    auto& settings = SettingsManager::instance();
    settings.set_motion_show_actual_position(!settings.get_motion_show_actual_position());
    // The settings-subject observer re-renders; refresh here too so the panel
    // is correct even where that subject was never initialized (unit tests).
    show_actual_ = settings.get_motion_show_actual_position();
    refresh_position_display();
}

void MotionPanel::open_axis_keypad(char axis) {
    // Same gate as the jog controls: while the printer is not connected or
    // klippy is not ready, a jog would be refused, so the keypad must not
    // open either.
    if (lv_subject_get_int(get_printer_state().network_state().get_nav_buttons_enabled_subject()) ==
        0) {
        spdlog::debug("[{}] Axis keypad refused: printer not ready", get_name());
        return;
    }

    const helix::Axis axis_enum = axis == 'x'   ? helix::Axis::X
                                  : axis == 'y' ? helix::Axis::Y
                                                : helix::Axis::Z;
    const double commanded = axis == 'x' ? current_x_ : axis == 'y' ? current_y_ : current_z_;
    const auto params = helix::keypad_params_for_axis(
        get_printer_state().motion_state().get_gcode_axis_bounds(), axis_enum, commanded);
    if (!params) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    }

    keypad_axis_ = axis;
    ui_keypad_config_t config = {};
    config.initial_value = params->seed;
    config.min_value = params->min_value;
    config.max_value = params->max_value;
    config.allow_decimal = true;
    config.allow_negative = params->allow_negative;
    config.unit_label = "mm";
    // The axis letter alone: the keypad header is narrow at small sizes and
    // the display already carries the mm unit.
    config.title_label = axis == 'x' ? "X" : axis == 'y' ? "Y" : "Z";
    config.callback = &MotionPanel::on_axis_keypad_value;
    config.user_data = this;
    spdlog::debug("[{}] Axis keypad for {} ({}-{}, seed {:.2f})", get_name(), axis,
                  config.min_value, config.max_value, config.initial_value);
    ui_keypad_show(&config);
}

void MotionPanel::on_axis_keypad_value(float value, void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (self) {
        self->request_axis_target(self->keypad_axis_, static_cast<double>(value));
    }
}

void MotionPanel::request_axis_target(char axis, double mm) {
    helix::AxisTarget target;
    switch (axis) {
    case 'x':
        target.x = mm;
        break;
    case 'y':
        target.y = mm;
        break;
    default:
        target.z = mm;
        break;
    }

    const helix::Axis axis_enum = axis == 'x'   ? helix::Axis::X
                                  : axis == 'y' ? helix::Axis::Y
                                                : helix::Axis::Z;
    if (helix::axis_is_homed(get_printer_state(), axis_enum)) {
        dispatch_target(target);
        return;
    }

    // Unhomed: offer homing instead of letting Klipper refuse the move.
    helix::ensure_homed_then(
        get_moonraker_api(), lifetime_, [this, target]() { dispatch_target(target); },
        lifetime_.bg_cb("MotionPanel::coord_home_failed", [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), clean_gcode_error(err.localized_message()));
        }));
}

// ============================================================================
// Content Tabs (Jog / Move / Bed)
// ============================================================================

void MotionPanel::set_motion_tab(int tab) {
    if (tab < 0 || tab > 2) {
        return;
    }
    motion_tab_ = tab;
    sync_motion_tab_subjects();
    if (tab == 2) {
        // The surface was hidden until now, so it has no size to fit to yet.
        if (overlay_root_) {
            lv_obj_update_layout(overlay_root_);
        }
        layout_bed_map();
        refresh_bed_readout();
    }
}

void MotionPanel::sync_motion_tab_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&motion_tab_subject_, motion_tab_);
    for (int i = 0; i < 3; ++i) {
        lv_subject_set_int(&motion_tab_active_[i], i == motion_tab_ ? 1 : 0);
    }
}

bool MotionPanel::moves_allowed() const {
    auto& ps = get_printer_state();
    return lv_subject_get_int(ps.network_state().get_nav_buttons_enabled_subject()) != 0 &&
           lv_subject_get_int(ps.print_state().get_machine_motion_blocked_subject()) == 0;
}

/// Run `then` once X and Y are homed: homed runs it directly, anything else
/// homes first and the move only fires once homing succeeded. Presets move XY
/// only; Park needs every axis and uses ensure_homed_then directly.
static void ensure_xy_homed_then(AsyncLifetimeGuard& lifetime, std::function<void()> then) {
    auto& ps = get_printer_state();
    if (helix::axis_is_homed(ps, helix::Axis::X) && helix::axis_is_homed(ps, helix::Axis::Y)) {
        then();
        return;
    }
    helix::ensure_homed_then(
        get_moonraker_api(), lifetime, std::move(then),
        lifetime.bg_cb("MotionPanel::xy_home_failed", [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), clean_gcode_error(err.localized_message()));
        }));
}

void MotionPanel::refresh_tab_labels() {
    // Literal lv_tr calls, not an array of keys: the translation extractor
    // only sees string literals inside lv_tr().
    const char* const tab_labels[] = {lv_tr("Jog"), lv_tr("Move"), lv_tr("Bed")};
    for (int i = 0; i < 3; ++i) {
        snprintf(motion_tab_label_buf_[i], sizeof(motion_tab_label_buf_[i]), "%s", tab_labels[i]);
        if (subjects_initialized_) {
            lv_subject_copy_string(&motion_tab_label_[i], motion_tab_label_buf_[i]);
        }
    }
}

void MotionPanel::handle_preset(helix::MotionPreset preset) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api || !moves_allowed()) {
        return;
    }
    // Computed at tap time: bounds can change (settings, calibration) between
    // the panel opening and the tap.
    const auto plate = bed_plate();
    const auto target =
        plate ? helix::motion_preset_target(preset, plate->area, plate->circular) : std::nullopt;
    if (!target) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    }
    ensure_xy_homed_then(lifetime_, [this, t = *target]() { dispatch_target(t); });
}

void MotionPanel::handle_park() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api || !moves_allowed()) {
        return;
    }
    const auto info = StandardMacros::instance().get(StandardMacroSlot::ParkToolhead);
    // Both paths may lift Z, so they need every axis homed.
    if (info.is_empty()) {
        // The commanded Z the panel holds predates a G28 that runs first, so a
        // lift computed from it could be a descent; homing already leaves Z at
        // a safe height, so the lift only applies to a machine already homed.
        const bool lift_z = helix::toolhead_is_homed(get_printer_state());
        helix::ensure_homed_then(
            api, lifetime_, [this, lift_z]() { park_over_plate(lift_z); },
            lifetime_.bg_cb("MotionPanel::park_home_failed", [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Homing failed: {}"),
                             clean_gcode_error(err.localized_message()));
            }));
        return;
    }
    const std::string name = info.translated_name();
    helix::ensure_homed_then(
        api, lifetime_,
        [this, name]() {
            NOTIFY_INFO(lv_tr("Running {}..."), name.c_str());
            if (!StandardMacros::instance().execute(
                    StandardMacroSlot::ParkToolhead, get_moonraker_api(), {},
                    [name]() { NOTIFY_SUCCESS(lv_tr("{} complete"), name.c_str()); },
                    lifetime_.bg_cb("MotionPanel::park_failed", [name](const MoonrakerError& err) {
                        NOTIFY_ERROR(lv_tr("Macro failed: {}"),
                                     clean_gcode_error(err.localized_message()));
                    }))) {
                NOTIFY_WARNING(lv_tr("{} macro not configured"), name.c_str());
            }
        },
        lifetime_.bg_cb("MotionPanel::park_home_failed", [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), clean_gcode_error(err.localized_message()));
        }));
}

void MotionPanel::park_over_plate(bool lift_z) {
    if (!get_moonraker_api()) {
        return;
    }
    const AxisBounds gcode = get_printer_state().motion_state().get_gcode_axis_bounds();
    const auto plate = bed_plate();
    auto target = plate ? helix::plate_rear_park(plate->area) : std::nullopt;
    if (!target) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    }
    if (lift_z && gcode.has_z) {
        target->z = std::min(static_cast<double>(current_z_) + PARK_Z_LIFT_MM,
                             static_cast<double>(gcode.z_max));
    }
    dispatch_target(*target);
}

// ============================================================================
// Bed Tab
// ============================================================================

/// Mapper over the Bed tab surface's content box, where the plate and the
/// marker are positioned.
static helix::BedCoordMapper bed_surface_mapper(lv_obj_t* area, const helix::AxisBounds& plate) {
    return helix::bed_map_mapper(plate, lv_obj_get_content_width(area),
                                 lv_obj_get_content_height(area));
}

/// Both targets set and naming the same XY.
static bool same_xy(const std::optional<helix::AxisTarget>& a,
                    const std::optional<helix::AxisTarget>& b) {
    return a && b && a->x == b->x && a->y == b->y;
}

std::optional<MotionPanel::BedPlate> MotionPanel::bed_plate() const {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return std::nullopt;
    }
    const auto& ps = get_printer_state();
    const auto area = helix::preset_area(ps.motion_state().get_axis_bounds(),
                                         ps.motion_state().get_gcode_axis_bounds(),
                                         api->hardware().build_volume());
    if (!area.has_x || !area.has_y || area.x_max <= area.x_min || area.y_max <= area.y_min) {
        return std::nullopt;
    }
    return BedPlate{area, helix::circular_bed_kinematics(api->hardware().kinematics())};
}

std::optional<helix::AxisTarget> MotionPanel::bed_target_at(lv_obj_t* area,
                                                            lv_point_t screen_point) const {
    const auto plate = bed_plate();
    if (!plate || !area) {
        return std::nullopt;
    }
    lv_area_t coords;
    lv_obj_get_coords(area, &coords);
    const int32_t left = coords.x1 + lv_obj_get_style_pad_left(area, LV_PART_MAIN);
    const int32_t top = coords.y1 + lv_obj_get_style_pad_top(area, LV_PART_MAIN);
    return helix::bed_map_target(
        static_cast<float>(screen_point.x - left), static_cast<float>(screen_point.y - top),
        bed_surface_mapper(area, plate->area), plate->area, plate->circular);
}

std::optional<double> MotionPanel::bed_lift_z() {
    const auto bounds = clamp_bounds();
    if (!bounds.has_z || !helix::axis_is_homed(get_printer_state(), helix::Axis::Z)) {
        return std::nullopt;
    }
    return helix::bed_map_lift_z(
        jog_coalescer_.target_start_z(current_z_),
        static_cast<double>(SettingsManager::instance().get_bed_map_clearance_mm()),
        static_cast<double>(bounds.z_max));
}

namespace {
/// The plate's rectangle inside the Bed tab surface's content box, rounded
/// to the pixels layout_bed_map() gives it.
struct PlateRect {
    int32_t x, y, w, h;
};
PlateRect plate_rect(const helix::BedCoordMapper& mapper, const helix::AxisBounds& area) {
    const auto [x1, y1] = mapper.mm_to_px(area.x_min, area.y_max);
    const auto [x2, y2] = mapper.mm_to_px(area.x_max, area.y_min);
    return {static_cast<int32_t>(std::lround(x1)), static_cast<int32_t>(std::lround(y1)),
            static_cast<int32_t>(std::lround(x2 - x1)), static_cast<int32_t>(std::lround(y2 - y1))};
}
} // namespace

void MotionPanel::layout_bed_map() {
    if (!overlay_root_ || motion_tab_ != 2) {
        return;
    }
    lv_obj_t* area = helix::ui::find_required(overlay_root_, "bed_map_area", get_name());
    lv_obj_t* plate_obj = helix::ui::find_required(area, "bed_map_plate", get_name());
    const auto plate = bed_plate();
    if (!plate_obj || !plate) {
        return;
    }
    if (lv_obj_get_content_width(area) <= 0 || lv_obj_get_content_height(area) <= 0) {
        return;
    }
    const auto rect = plate_rect(bed_surface_mapper(area, plate->area), plate->area);
    lv_obj_set_pos(plate_obj, rect.x, rect.y);
    lv_obj_set_size(plate_obj, rect.w, rect.h);
    lv_subject_set_int(&motion_bed_circular_, plate->circular ? 1 : 0);
    update_bed_marker();
}

void MotionPanel::update_bed_marker() {
    if (!overlay_root_ || motion_tab_ != 2) {
        return;
    }
    lv_obj_t* area = helix::ui::find_required(overlay_root_, "bed_map_area", get_name());
    lv_obj_t* marker = helix::ui::find_required(area, "bed_map_marker", get_name());
    lv_obj_t* plate_obj = helix::ui::find_required(area, "bed_map_plate", get_name());
    const auto plate = bed_plate();
    if (!marker || !plate_obj || !plate) {
        return;
    }
    const auto mapper = bed_surface_mapper(area, plate->area);
    const auto [fx, fy] = mapper.mm_to_px(current_x_, current_y_);
    const auto px = static_cast<int32_t>(std::lround(fx));
    const auto py = static_cast<int32_t>(std::lround(fy));
    lv_obj_set_pos(marker, px - lv_obj_get_width(marker) / 2, py - lv_obj_get_height(marker) / 2);

    // The guides live in the plate's content box, which clips them to its
    // rectangle; on a round plate their length is the chord through the marker.
    lv_obj_t* guide_x = helix::ui::find_required(plate_obj, "bed_map_guide_x", get_name());
    lv_obj_t* guide_y = helix::ui::find_required(plate_obj, "bed_map_guide_y", get_name());
    if (!guide_x || !guide_y) {
        return;
    }
    const auto rect = plate_rect(mapper, plate->area);
    const int32_t border = lv_obj_get_style_border_width(plate_obj, LV_PART_MAIN);
    const int32_t cw = rect.w - 2 * border;
    const int32_t ch = rect.h - 2 * border;
    const float lx = static_cast<float>(px - rect.x - border);
    const float ly = static_cast<float>(py - rect.y - border);
    const float half_h = helix::bed_map_guide_half_span(lx - cw / 2.0f, ch / 2.0f, plate->circular);
    const float half_w = helix::bed_map_guide_half_span(ly - ch / 2.0f, cw / 2.0f, plate->circular);
    lv_obj_set_pos(guide_x, static_cast<int32_t>(lx) - lv_obj_get_width(guide_x) / 2,
                   static_cast<int32_t>(std::lround(ch / 2.0f - half_h)));
    lv_obj_set_height(guide_x, static_cast<int32_t>(std::lround(2.0f * half_h)));
    lv_obj_set_pos(guide_y, static_cast<int32_t>(std::lround(cw / 2.0f - half_w)),
                   static_cast<int32_t>(ly) - lv_obj_get_height(guide_y) / 2);
    lv_obj_set_width(guide_y, static_cast<int32_t>(std::lround(2.0f * half_w)));
}

void MotionPanel::refresh_bed_readout() {
    if (!subjects_initialized_) {
        return;
    }
    const bool dragging = bed_drag_target_ && bed_drag_target_->x && bed_drag_target_->y;
    std::string text;
    if (!dragging && !helix::toolhead_is_homed(get_printer_state())) {
        text = lv_tr("Home all axes to use the map");
    } else {
        // The target under a dragging finger; at rest the header already
        // shows where the head is, so only a pending lift is worth saying.
        if (dragging) {
            text = fmt::format("X {:.1f}  Y {:.1f}", *bed_drag_target_->x, *bed_drag_target_->y);
        }
        // Announced before the tap: an unrequested Z move is acceptable only
        // when the user was told about it first.
        if (const auto lift = bed_lift_z()) {
            char z_buf[16];
            char lift_buf[16];
            format_axis_value(z_buf, sizeof(z_buf), show_actual_ ? live_z_ : current_z_);
            format_distance_label(lift_buf, sizeof(lift_buf), static_cast<float>(*lift));
            if (!text.empty()) {
                text += "  ";
            }
            text += fmt::format(fmt::runtime(lv_tr("Z {}, will lift to {}mm")), z_buf, lift_buf);
        }
    }
    // Position frames arrive many times a second; most change nothing shown.
    if (text == motion_bed_readout_buf_) {
        return;
    }
    snprintf(motion_bed_readout_buf_, sizeof(motion_bed_readout_buf_), "%s", text.c_str());
    lv_subject_copy_string(&motion_bed_readout_, motion_bed_readout_buf_);
}

void MotionPanel::handle_bed_touch(helix::BedTouch phase, lv_obj_t* area, lv_point_t screen_point) {
    switch (phase) {
    case helix::BedTouch::Pressed:
        // Another operation (homing, leveling, a long macro) holds the head.
        // Our own moves never count: the surface keeps a gesture it started.
        if (get_printer_state().is_external_blocking_operation_active()) {
            return;
        }
        bed_pressing_ = true;
        bed_dragging_ = false;
        bed_press_point_ = screen_point;
        bed_sent_target_.reset();
        bed_gesture_failed_ = false;
        return;
    case helix::BedTouch::Pressing: {
        if (!bed_pressing_) {
            return;
        }
        if (!bed_dragging_ &&
            std::abs(screen_point.x - bed_press_point_.x) <= BED_MAP_DRAG_THRESHOLD_PX &&
            std::abs(screen_point.y - bed_press_point_.y) <= BED_MAP_DRAG_THRESHOLD_PX) {
            return;
        }
        bed_dragging_ = true;
        // PRESSING repeats on every input read while the finger rests.
        auto target = bed_target_at(area, screen_point);
        if (!target || same_xy(target, bed_drag_target_)) {
            return;
        }
        bed_drag_target_ = target;
        refresh_bed_readout();
        // The head follows the finger, coalesced latest-wins: one move in
        // flight, the newest target pending. Only on a homed machine: an
        // unhomed one would queue a home per pointer sample, so its drag
        // commits once, on release.
        if (!bed_gesture_failed_ && helix::toolhead_is_homed(get_printer_state())) {
            send_bed_gesture_target(*target);
        }
        return;
    }
    case helix::BedTouch::Released: {
        if (!bed_pressing_) {
            return;
        }
        const auto target = bed_target_at(area, bed_dragging_ ? screen_point : bed_press_point_);
        end_bed_gesture();
        // A drag that already streamed the point it ended on sends nothing more.
        if (target && !bed_gesture_failed_ && !same_xy(target, bed_sent_target_)) {
            send_bed_gesture_target(*target);
            refresh_bed_readout(); // a lift now in flight is no longer pending
        }
        return;
    }
    case helix::BedTouch::Lost:
        end_bed_gesture();
        return;
    }
}

void MotionPanel::end_bed_gesture() {
    bed_pressing_ = false;
    bed_dragging_ = false;
    bed_drag_target_.reset();
    refresh_bed_readout();
}

void MotionPanel::send_bed_gesture_target(const helix::AxisTarget& target) {
    // The lift is decided once, at the gesture's first move, and stamped on
    // every later one: a sample replacing a pending target replaces it
    // wholesale, so a lift carried by the first alone could be dropped. A
    // repeated G0 to the same height is no motion, never a descent.
    if (!bed_sent_target_) {
        bed_gesture_lift_z_ = bed_lift_z();
    }
    commit_bed_target(target, bed_gesture_lift_z_);
    bed_sent_target_ = target;
}

void MotionPanel::commit_bed_target(helix::AxisTarget target, std::optional<double> lift_z) {
    if (!get_moonraker_api() || !moves_allowed()) {
        return;
    }
    // Every axis, like Park: the lift needs a known Z.
    if (!helix::toolhead_is_homed(get_printer_state())) {
        // No lift: the Z this panel holds predates the G28 about to run, and
        // homing leaves the nozzle clear of the plate.
        helix::ensure_homed_then(
            get_moonraker_api(), lifetime_, [this, target]() { dispatch_target(target); },
            lifetime_.bg_cb("MotionPanel::bed_home_failed", [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Homing failed: {}"),
                             clean_gcode_error(err.localized_message()));
            }));
        return;
    }
    // The lift rides in the same target as the XY: move_to raises Z on its
    // own line before the XY travel.
    if (lift_z) {
        target.z = *lift_z;
    }
    spdlog::info("[{}] Bed map target X{:.2f} Y{:.2f}{}", get_name(), target.x.value_or(0.0),
                 target.y.value_or(0.0),
                 target.z ? fmt::format(" (lift Z{:.2f})", *target.z) : std::string());
    dispatch_target(target);
}

void MotionPanel::handle_motors_off() {
    if (!moves_allowed()) {
        return;
    }
    helix::ui::show_motors_off_confirm(get_moonraker_api(), motors_off_dialog_, lifetime_.token());
}

// ============================================================================
// Jog Mode (Fine/Coarse/Turbo)
// ============================================================================

void MotionPanel::set_jog_mode(JogMode mode) {
    if (current_mode_ == mode)
        return;

    current_mode_ = mode;

    // Update jog pad
    if (jog_pad_) {
        ui_jog_pad_set_mode(jog_pad_, current_mode_);
        lv_obj_invalidate(jog_pad_); // Redraw ring labels
    }

    // Update toggle button styles
    if (subjects_initialized_) {
        lv_subject_set_int(&jog_mode_fine_active_, (mode == JogMode::Fine) ? 1 : 0);
        lv_subject_set_int(&jog_mode_coarse_active_, (mode == JogMode::Coarse) ? 1 : 0);
        lv_subject_set_int(&jog_mode_turbo_active_, (mode == JogMode::Turbo) ? 1 : 0);
    }

    // Update Z button labels
    update_z_button_labels();

    // Persist setting
    auto* cfg = Config::get_instance();
    cfg->set("/motion/jog_mode", static_cast<int>(mode));
    cfg->save();

    spdlog::info("[MotionPanel] Jog mode: {}", jog_mode_name(mode));
}

void MotionPanel::update_z_button_labels() {
    if (!subjects_initialized_)
        return;

    const auto& mode_dist = get_jog_mode_distances(current_mode_);
    snprintf(z_large_label_buf_, sizeof(z_large_label_buf_), "%smm", mode_dist.outer_label);
    lv_subject_copy_string(&z_large_label_subject_, z_large_label_buf_);

    snprintf(z_small_label_buf_, sizeof(z_small_label_buf_), "%smm", mode_dist.inner_label);
    lv_subject_copy_string(&z_small_label_subject_, z_small_label_buf_);
}
