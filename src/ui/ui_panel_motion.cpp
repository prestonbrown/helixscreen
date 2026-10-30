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
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "bed_plate_draw.h"
#include "config.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "sort_small.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "tile_grid_fit.h"
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

using namespace helix;
using helix::ui::find_required;

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

std::vector<float> helix::unique_sorted_steps(std::vector<float> distances) {
    // Settings store floats typed on a keypad, so two "equal" distances can
    // differ in the last bits; a step button per bit pattern would repeat.
    constexpr float kSameStepMm = 0.001f;
    helix::sort_small(distances.begin(), distances.end());
    std::vector<float> steps;
    for (float mm : distances) {
        if (steps.empty() || mm - steps.back() > kSameStepMm) {
            steps.push_back(mm);
        }
    }
    return steps;
}

std::vector<float> helix::jog_step_list() {
    auto& settings = SettingsManager::instance();
    std::vector<float> distances;
    distances.reserve(kJogStepMax);
    for (int m = 0; m < kJogModeCount; ++m) {
        const auto mode = static_cast<JogMode>(m);
        distances.push_back(settings.get_jog_distance(mode, /*outer=*/false));
        distances.push_back(settings.get_jog_distance(mode, /*outer=*/true));
    }
    return unique_sorted_steps(std::move(distances));
}

size_t helix::nearest_step_index(const std::vector<float>& steps, float mm) {
    size_t best = 0;
    for (size_t i = 1; i < steps.size(); ++i) {
        if (std::abs(steps[i] - mm) < std::abs(steps[best] - mm)) {
            best = i;
        }
    }
    return best;
}

std::optional<helix::JogButton> helix::parse_jog_button(const char* name) {
    if (!name) {
        return std::nullopt;
    }
    const char* dir = std::strchr(name, '_');
    if (!dir || dir - name != 1) {
        return std::nullopt;
    }
    ++dir;
    const char* size = std::strchr(dir, '_');
    if (!size) {
        return std::nullopt;
    }
    const std::string direction(dir, static_cast<size_t>(size - dir));
    ++size;

    JogButton button{};
    switch (name[0]) {
    case 'x':
        button.axis = Axis::X;
        break;
    case 'y':
        button.axis = Axis::Y;
        break;
    case 'z':
        button.axis = Axis::Z;
        break;
    default:
        return std::nullopt;
    }

    // X and Y read in G-code terms; Z reads as the arrow on screen, which the
    // bed_moves inversion maps to a G-code direction at jog time.
    const bool is_z = button.axis == Axis::Z;
    if (direction == (is_z ? "up" : "plus")) {
        button.sign = 1;
    } else if (direction == (is_z ? "down" : "minus")) {
        button.sign = -1;
    } else {
        return std::nullopt;
    }

    if (std::strcmp(size, "small") == 0) {
        button.step = JogButtonStep::Inner;
    } else if (std::strcmp(size, "large") == 0) {
        button.step = JogButtonStep::Outer;
    } else if (std::strcmp(size, "step") == 0) {
        button.step = JogButtonStep::Selected;
    } else {
        return std::nullopt;
    }
    return button;
}

helix::JogLayoutFit helix::jog_layout_fit(MotionLayout layout, bool portrait, int box_w, int box_h,
                                          int gap, int step_h, int step_count, int min_key) {
    JogLayoutFit fit;
    if (layout == MotionLayout::Pad) {
        return fit;
    }
    if (layout == MotionLayout::AxisRows) {
        // Four rows of five keys (the axis key in the middle), four gaps
        // across, three down.
        const auto key = ui::fit_key_grid(5, 4, box_w - 4 * gap, box_h - 3 * gap, min_key);
        fit.key_w = key.key_w;
        fit.key_h = key.key_h;
        fit.row = {5 * key.key_w + 4 * gap, key.key_h};
        return fit;
    }

    // The crosses: Arrows is 3 keys by 2 (X- and X+ sit between the Y rows),
    // Arrows + Home 3 by 3; a Z column beside them either way. The home block
    // is 2 keys wide beside them in landscape, one row under them in portrait.
    const int cross_rows = layout == MotionLayout::Arrows ? 2 : 3;
    const double cols = portrait ? 4 : 6;
    const int rows = portrait ? cross_rows + 1 : cross_rows;
    const int gaps_x = portrait ? 3 : 5;
    const int gaps_y = rows; // between key rows, plus one above the step bar
    const auto key =
        ui::fit_key_grid(cols, rows, box_w - gaps_x * gap, box_h - gaps_y * gap - step_h, min_key);
    fit.key_w = key.key_w;
    fit.key_h = key.key_h;
    const int cross_h = cross_rows * key.key_h + (cross_rows - 1) * gap;
    fit.xy = {3 * key.key_w + 2 * gap, cross_h};
    fit.z = {key.key_w, cross_h};
    fit.home = portrait ? BlockSize{4 * key.key_w + 3 * gap, key.key_h}
                        : BlockSize{2 * key.key_w + gap, cross_h};
    const int body_w = portrait ? fit.home.w : fit.xy.w + gap + fit.z.w + gap + fit.home.w;
    // Step keys are as tall as the bar; past 1.4x that they would read as bars.
    const int widest = step_count > 0 ? step_count * (step_h * 7 / 5) + (step_count - 1) * gap : 0;
    fit.steps_w = std::min(body_w, widest);
    return fit;
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
    // The reading is truncated to kPositionResolutionMm, so a head parked at
    // the bound can read up to that much short of it. What a press could still
    // move from there is below what the clamp's margin already gives away.
    const double tolerance = kPositionResolutionMm + AxisMove::kEpsilonMm;
    if (direction_mm > 0.0) {
        return z >= z_max - tolerance;
    }
    return z <= z_min + tolerance;
}

/// Bed-grid preset keys as they appear in motion_panel.xml user_data, in
/// MotionPreset declaration order (rear row first, front row last).
// ============================================================================
// Constructor
// ============================================================================

MotionPanel::MotionPanel()
    : move_sender_(lifetime_, "MotionPanel",
                   {[this]() {
                        // An ack changes where the next target starts (target_start_z), and
                        // with it the lift the Bed tab announces.
                        refresh_bed_readout();
                    },
                    [this](const MoonrakerError&) {
                        // A Bed-tab drag would send the next sample into the same refusal.
                        bed_gesture_.mark_failed();
                        // A hold-to-repeat still ticking would re-send the refused move
                        // every interval and raise one error toast per tick.
                        stop_hold_repeat();
                    },
                    [this](const helix::JogCoalescer::CoalescedMove& move) {
                        // A target is a plate, preset or coordinate move, held to the plate's
                        // gate; a jog delta only needs the panel's.
                        if (std::holds_alternative<helix::AxisTarget>(move)) {
                            return helix::plate_moves_allowed(get_printer_state());
                        }
                        return moves_allowed();
                    }}),
      bed_gesture_(
          {[this](int x, int y) { return bed_target_at(bed_touch_area_, lv_point_t{x, y}); },
           []() {
               return get_moonraker_api() != nullptr &&
                      helix::plate_moves_allowed(get_printer_state());
           },
           [this]() {
               return helix::current_plate_lift(get_printer_state(),
                                                move_sender_.target_start_z(current_z_));
           },
           [this](const helix::AxisTarget& target) { send_bed_target(target); },
           []() { notify_part_too_tall(); }}) {
    // Initialize buffer contents (axis labels are in XML, values only here)
    std::strcpy(pos_x_buf_, "—");
    std::strcpy(pos_y_buf_, "—");
    std::strcpy(pos_z_buf_, "—");
    std::strcpy(z_axis_label_buf_, "Z Axis"); // Default before kinematics detected
    std::strcpy(z_up_icon_buf_, "arrow_up");
    std::strcpy(z_down_icon_buf_, "arrow_down");
    std::strcpy(jog_outer_label_buf_, "10mm");
    std::strcpy(jog_inner_label_buf_, "1mm");
    std::memset(step_label_buf_, 0, sizeof(step_label_buf_));

    // Load persisted jog mode (default: coarse)
    // Migrate from old bool /motion/jog_mode_fine to new int /motion/jog_mode
    auto* cfg = Config::get_instance();
    int mode = cfg->get<int>("/motion/jog_mode", -1);
    if (mode >= 0 && mode < kJogModeCount) {
        current_mode_ = static_cast<JogMode>(mode);
    } else {
        // Migrate legacy bool setting and persist new key
        bool fine = cfg->get<bool>("/motion/jog_mode_fine", false);
        current_mode_ = fine ? JogMode::Fine : JogMode::Coarse;
        cfg->set("/motion/jog_mode", static_cast<int>(current_mode_));
        cfg->save();
    }
    step_mm_ = cfg->get<float>("/motion/jog_step", 1.0f);
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

    // The jog mode's two distances: the Z column and the axis rows label
    // their buttons with them.
    const auto& mode_dist = get_jog_mode_distances(current_mode_);
    snprintf(jog_outer_label_buf_, sizeof(jog_outer_label_buf_), "%smm", mode_dist.outer_label);
    snprintf(jog_inner_label_buf_, sizeof(jog_inner_label_buf_), "%smm", mode_dist.inner_label);
    UI_MANAGED_SUBJECT_STRING(jog_outer_label_subject_, jog_outer_label_buf_, jog_outer_label_buf_,
                              "motion_jog_outer_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(jog_inner_label_subject_, jog_inner_label_buf_, jog_inner_label_buf_,
                              "motion_jog_inner_label", subjects_);

    // Arrow layouts' step selector: one label and one active flag per slot;
    // the count drives how many slots the XML repeat builds.
    UI_MANAGED_SUBJECT_INT(step_count_subject_, 0, "motion_step_count", subjects_);
    for (int i = 0; i < kJogStepMax; ++i) {
        char subject_name[32];
        snprintf(subject_name, sizeof(subject_name), "motion_step_label_%d", i);
        UI_MANAGED_SUBJECT_STRING(step_label_subject_[i], step_label_buf_[i], "", subject_name,
                                  subjects_);
        snprintf(subject_name, sizeof(subject_name), "motion_step_active_%d", i);
        UI_MANAGED_SUBJECT_INT(step_active_subject_[i], 0, subject_name, subjects_);
    }

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

    refresh_step_list();

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
        // Every jog button (Z column, arrows, axis rows) shares one callback set;
        // user_data names the button in parse_jog_button() form.
        {"on_motion_jog_button",
         [](lv_event_t* e) {
             MotionPanel& panel = get_global_motion_panel();
             // A hold that repeated must not add one extra jog on release; a
             // plain tap (no repeat) jogs exactly once through this path.
             const bool swallow = panel.jog_hold_timer().swallow_click();
             panel.jog_hold_timer().cancel();
             if (swallow) {
                 spdlog::debug("[MotionPanel] Jog click swallowed after hold repeat");
             } else if (const char* button_id =
                            static_cast<const char*>(lv_event_get_user_data(e))) {
                 panel.handle_jog_button(button_id);
             }
         }},
        // Hold-to-repeat arms on press and stops on release / press lost
        {"on_motion_jog_button_pressed",
         [](lv_event_t* e) {
             if (const char* button_id = static_cast<const char*>(lv_event_get_user_data(e))) {
                 get_global_motion_panel().begin_jog_hold(
                     button_id, static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
             }
         }},
        {"on_motion_jog_button_released",
         [](lv_event_t*) { get_global_motion_panel().jog_hold_timer().release(); }},
        {"on_motion_jog_button_press_lost",
         [](lv_event_t*) { get_global_motion_panel().jog_hold_timer().cancel(); }},
        // Arrow layouts: the step selector (user_data is the step index).
        {"on_motion_step_clicked",
         [](lv_event_t* e) {
             if (auto index = helix::ui::event_user_int(e))
                 get_global_motion_panel().select_jog_step(*index);
         }},
        {"on_motion_jog_layout_resized",
         [](lv_event_t* e) {
             get_global_motion_panel().fit_jog_layout(lv_event_get_current_target_obj(e));
         }},
        // Layout home buttons: user_data names the axes, "all" homes everything.
        {"on_motion_home",
         [](lv_event_t* e) {
             const char* axes = static_cast<const char*>(lv_event_get_user_data(e));
             if (axes) {
                 get_global_motion_panel().home_axes(lv_streq(axes, "all") ? "" : axes);
             }
         }},
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
             if (auto preset = key ? helix::motion_preset_from_token(key) : std::nullopt) {
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
        {"on_motion_bed_cleared", [](lv_event_t*) { helix::confirm_bed_cleared(); }},
        {"on_motion_bed_plate_draw",
         [](lv_event_t* e) { get_global_motion_panel().draw_bed_plate(e); }},
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

    // The button labels are subject-bound and have no repaint to ride in on.
    update_jog_distance_labels();
    refresh_step_list();
    // The clearance setting can change while this panel sits on the stack.
    refresh_bed_readout();
    observe_bed_occupancy();
}

void MotionPanel::observe_bed_occupancy() {
    // The state belongs to the printer session, which a printer switch replaces, so the
    // observer is taken fresh on every open.
    bed_occupancy_observer_.reset();
    auto* occupancy = helix::get_bed_occupancy_state();
    if (!occupancy) {
        return;
    }
    bed_occupancy_observer_ = helix::ui::observe<int>(
        occupancy->get_version_subject(), this,
        [](MotionPanel* self, int) {
            if (!self->subjects_initialized_ || !self->overlay_root_) {
                return;
            }
            if (lv_obj_t* plate = lv_obj_find_by_name(self->overlay_root_, "bed_map_plate")) {
                lv_obj_invalidate(plate);
            }
            self->refresh_bed_readout();
        },
        occupancy->get_subjects_lifetime());
}

void MotionPanel::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[{}] on_deactivating({})", get_name(), deactivate_reason_name(reason));

    // The base invalidates lifetime_ on the way out of this hook, dropping any
    // in-flight ack callback — fully reset the coalescer so it can't get stuck
    // in_flight forever.
    move_sender_.reset();
    stop_hold_repeat();
    // A release arriving after the panel comes back belongs to no gesture.
    end_bed_gesture();
    bed_occupancy_observer_.reset();
}

void MotionPanel::on_ui_destroyed() {
    // Null raw child pointers so persistent observers (jog_ready_observer_
    // dereferences jog_pad_ on every connection/klippy flap) can't UAF if the
    // overlay widget tree is ever destroyed (destroy-on-close, parent screen
    // teardown). Peers do the same — see BedMeshPanel::on_ui_destroyed.
    jog_pad_ = nullptr;
    move_sender_.reset();
    stop_hold_repeat();
    end_bed_gesture();
}

// ============================================================================
// Hold-to-Repeat (Z buttons; the pad owns its own repeat)
// ============================================================================

void MotionPanel::begin_jog_hold(const char* button_name, lv_obj_t* button) {
    if (!button_name)
        return;
    snprintf(jog_hold_button_, sizeof(jog_hold_button_), "%s", button_name);
    jog_hold_obj_ = button;
    jog_hold_has_obj_ = button != nullptr;
    jog_hold_repeated_ = false;
    jog_hold_timer_.begin(&MotionPanel::jog_hold_fire, this);
}

bool MotionPanel::jog_hold_fire(void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (self->jog_hold_has_obj_ && !self->jog_hold_obj_.get()) {
        spdlog::debug("[MotionPanel] Held jog button deleted; ending the hold");
        return false;
    }
    // The first tick is this press's initial jog; later ones are repeats.
    const bool is_repeat = self->jog_hold_repeated_;
    self->jog_hold_repeated_ = true;
    const bool keep_going = self->handle_jog_button(self->jog_hold_button_, is_repeat);
    lv_obj_t* held = self->jog_hold_obj_.get();
    if (!keep_going && held && lv_obj_has_state(held, LV_STATE_DISABLED)) {
        lv_obj_remove_state(held, LV_STATE_PRESSED);
    }
    return keep_going;
}

void MotionPanel::stop_hold_repeat() {
    jog_hold_timer_.cancel();
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
    } else {
        // Landscape: the Z column's keys take their height from the column, so
        // its width stops at 1.4x that height rather than drawing wide bars.
        lv_obj_t* right_column = lv_obj_find_by_name(overlay_root_, "right_column");
        lv_obj_t* z_key = right_column ? lv_obj_find_by_name(right_column, "z_up_large") : nullptr;
        if (z_key && lv_obj_get_height(z_key) > 0) {
            const lv_coord_t max_w = lv_obj_get_style_max_width(right_column, LV_PART_MAIN);
            const lv_coord_t near_square = lv_obj_get_height(z_key) * 7 / 5;
            lv_obj_set_width(right_column, LV_MIN(max_w, near_square));
            lv_obj_set_flex_grow(right_column, 0);
            // A key too short to stack keeps its label beside the arrow, which
            // a near-square key cannot hold: the column keeps its flex share.
            lv_obj_update_layout(overlay_root_);
            if (lv_obj_get_scroll_right(z_key) > 0) {
                lv_obj_set_width(right_column, 0);
                lv_obj_set_flex_grow(right_column, 1);
            }
        }
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

    // A layout swap deletes the held button without a RELEASED. A pad built
    // while another layout showed was fitted to a hidden, zero-size wrapper,
    // so it is re-fitted once the swap has laid out.
    layout_observer_ = observe<int>(
        SettingsManager::instance().subject_motion_layout(), this,
        [](MotionPanel* self, int) {
            self->stop_hold_repeat();
            self->lifetime_.defer("MotionPanel::refit_pad", [self]() {
                if (self->jog_pad_ && self->overlay_root_) {
                    self->fit_jog_pad();
                    lv_obj_invalidate(self->jog_pad_);
                }
            });
        },
        SettingsManager::instance().get_subjects_lifetime());

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
    const auto bounds = helix::move_clamp_bounds(get_printer_state());
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
// Jog Button Handler
// ============================================================================

bool MotionPanel::handle_jog_button(const char* name, bool is_repeat) {
    const auto button = parse_jog_button(name);
    if (!button) {
        spdlog::error("[{}] Unknown jog button: '{}'", get_name(), name ? name : "(null)");
        return false;
    }

    double distance = 0.0;
    if (button->step == JogButtonStep::Selected) {
        distance = static_cast<double>(selected_jog_step());
    } else {
        const auto& mode_dist = get_jog_mode_distances(current_mode_);
        distance = static_cast<double>(button->step == JogButtonStep::Outer ? mode_dist.outer
                                                                            : mode_dist.inner);
    }
    distance *= button->sign;

    if (button->axis == helix::Axis::X) {
        return jog(button->sign > 0 ? JogDirection::E : JogDirection::W,
                   static_cast<float>(std::abs(distance)), is_repeat);
    }
    if (button->axis == helix::Axis::Y) {
        return jog(button->sign > 0 ? JogDirection::N : JogDirection::S,
                   static_cast<float>(std::abs(distance)), is_repeat);
    }

    // For bed-moves printers (CoreXY etc), invert direction so arrows match physical motion:
    // - Up arrow = bed moves UP toward nozzle = G-code Z- (bed rises, gap decreases)
    // - Down arrow = bed moves DOWN away from nozzle = G-code Z+ (bed lowers, gap increases)
    if (bed_moves_) {
        distance = -distance;
        spdlog::debug("[{}] Bed-moves printer: inverted Z direction for bed movement", get_name());
    }

    // Bounds are in gcode space, so this must follow the inversion above.
    const auto bounds = helix::move_clamp_bounds(get_printer_state());
    if (bounds.has_z && helix::axis_is_homed(get_printer_state(), helix::Axis::Z)) {
        distance =
            clamp_axis_delta(helix::Axis::Z, current_z_,
                             move_sender_.coalescer().predicted_z(current_z_) - current_z_,
                             distance, bounds.z_min, bounds.z_max, /*fresh_press=*/!is_repeat);
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
    const auto bounds = helix::move_clamp_bounds(get_printer_state());

    double ddx = static_cast<double>(dx);
    double ddy = static_cast<double>(dy);

    if (ddx != 0.0 && bounds.has_x && helix::axis_is_homed(get_printer_state(), helix::Axis::X)) {
        ddx = clamp_axis_delta(helix::Axis::X, current_x_,
                               move_sender_.coalescer().predicted_x(current_x_) - current_x_, ddx,
                               bounds.x_min, bounds.x_max, /*fresh_press=*/false);
    }
    if (ddy != 0.0 && bounds.has_y && helix::axis_is_homed(get_printer_state(), helix::Axis::Y)) {
        ddy = clamp_axis_delta(helix::Axis::Y, current_y_,
                               move_sender_.coalescer().predicted_y(current_y_) - current_y_, ddy,
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
    if (std::abs(allowed) > helix::AxisMove::kEpsilonMm) {
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

void MotionPanel::warn_axis_limit(helix::Axis axis, float clamp_bound, bool at_max) {
    // The clamp stops kGcodeEdgeMarginMm inside; tell the user the limit.
    const auto limit = static_cast<float>(at_max ? clamp_bound + helix::kGcodeEdgeMarginMm
                                                 : clamp_bound - helix::kGcodeEdgeMarginMm);
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
    return move_sender_.send_delta(get_moonraker_api(), delta);
}

bool MotionPanel::dispatch_target(const helix::AxisTarget& target) {
    return move_sender_.send_target(get_moonraker_api(), target,
                                    helix::move_clamp_bounds(get_printer_state()), current_z_);
}

void MotionPanel::home(char axis) {
    // 'A' homes every axis; the API takes an empty axis list for that.
    home_axes(axis == 'A' ? std::string() : std::string(1, axis));
}

void MotionPanel::home_axes(const std::string& axes) {
    spdlog::debug("[{}] Home command: '{}'", get_name(), axes.empty() ? "all" : axes);

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    api->motion().home_axes(
        axes,
        [axes]() {
            if (axes.empty()) {
                helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("All axes homed"));
            } else {
                if (axes.size() == 1) {
                    helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("{} axis homed"), axes);
                } else {
                    helix::ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("{} axes homed"), axes);
                }
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

namespace {
helix::Axis axis_from_letter(char axis) {
    return axis == 'x' ? helix::Axis::X : axis == 'y' ? helix::Axis::Y : helix::Axis::Z;
}
} // namespace

void MotionPanel::open_axis_keypad(char axis) {
    // Same gate as the jog controls: while the printer is not connected or
    // klippy is not ready, a jog would be refused, so the keypad must not
    // open either.
    if (lv_subject_get_int(get_printer_state().network_state().get_nav_buttons_enabled_subject()) ==
        0) {
        spdlog::debug("[{}] Axis keypad refused: printer not ready", get_name());
        return;
    }

    const helix::Axis axis_enum = axis_from_letter(axis);
    // An unhomed axis has no position to seed from, and only an explicit Home homes.
    if (!helix::axis_is_homed(get_printer_state(), axis_enum)) {
        spdlog::debug("[{}] Axis keypad refused: {} unhomed", get_name(), axis);
        return;
    }
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

    // Only an explicit Home homes; the coordinate is disabled while its axis is unhomed.
    if (!helix::axis_is_homed(get_printer_state(), axis_from_letter(axis))) {
        spdlog::debug("[{}] Coordinate target refused: {} unhomed", get_name(), axis);
        return;
    }
    dispatch_target(target);
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
    const auto plate = helix::current_bed_plate(get_printer_state(), get_moonraker_api());
    const auto target =
        plate ? helix::motion_preset_target(preset, plate->area, plate->circular) : std::nullopt;
    if (!target) {
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    }
    // The plate's send path: the lift floor, the Refuse notice and the homed gate.
    bed_gesture_.send_once(*target);
}

void MotionPanel::handle_park() {
    IMoonrakerAPI* api = get_moonraker_api();
    // Both paths may lift Z, so they need every axis homed; only an explicit Home homes.
    if (!api || !helix::plate_moves_allowed(get_printer_state())) {
        return;
    }
    if (!helix::run_park_toolhead_macro(api, lifetime_, "MotionPanel::park_failed")) {
        park_over_plate();
    }
}

void MotionPanel::park_over_plate() {
    if (!get_moonraker_api() || !helix::plate_moves_allowed(get_printer_state())) {
        return;
    }
    const auto park = helix::park_over_plate_target(get_printer_state(), get_moonraker_api(),
                                                    move_sender_.target_start_z(current_z_));
    switch (park.kind) {
    case helix::ParkOverPlate::Kind::Unknown:
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    case helix::ParkOverPlate::Kind::Refuse:
        notify_part_too_tall();
        return;
    case helix::ParkOverPlate::Kind::Move:
        dispatch_target(park.target);
        return;
    }
}

void MotionPanel::notify_part_too_tall() {
    NOTIFY_WARNING(helix::part_too_tall_text());
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

std::optional<helix::AxisTarget> MotionPanel::bed_target_at(lv_obj_t* area,
                                                            lv_point_t screen_point) const {
    const auto plate = helix::current_bed_plate(get_printer_state(), get_moonraker_api());
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
    const auto plate = helix::current_bed_plate(get_printer_state(), get_moonraker_api());
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

void MotionPanel::fit_jog_layout(lv_obj_t* root) {
    if (!root) {
        return;
    }
    struct Named {
        const char* view;
        MotionLayout layout;
        const char* prefix;
    };
    static constexpr Named kLayouts[] = {
        {"motion_jog_arrows", MotionLayout::Arrows, "arrows"},
        {"motion_jog_arrows_home", MotionLayout::ArrowsHome, "cross"},
        {"motion_jog_rows", MotionLayout::AxisRows, "rows"},
    };
    const Named* named = nullptr;
    for (const auto& n : kLayouts) {
        if (lv_streq(lv_obj_get_name(root), n.view)) {
            named = &n;
        }
    }
    if (!named) {
        return;
    }

    lv_subject_t* portrait_subject = lv_xml_get_subject(nullptr, "ui_is_portrait");
    const bool portrait = portrait_subject && lv_subject_get_int(portrait_subject) != 0;
    const int min_key = static_cast<int>(theme_manager_get_spacing("touch_target_min"));
    const int step_h =
        std::max(static_cast<int>(theme_manager_get_spacing("button_height")), min_key);
    const auto fit = jog_layout_fit(named->layout, portrait, lv_obj_get_content_width(root),
                                    lv_obj_get_content_height(root),
                                    lv_obj_get_style_pad_row(root, LV_PART_MAIN), step_h,
                                    static_cast<int>(steps_.size()), min_key);

    auto set = [root, named](const char* suffix, const BlockSize& size) {
        char name[32];
        std::snprintf(name, sizeof(name), "%s_%s", named->prefix, suffix);
        if (lv_obj_t* obj = lv_obj_find_by_name(root, name)) {
            lv_obj_set_size(obj, size.w, size.h);
        }
    };
    if (named->layout == MotionLayout::AxisRows) {
        for (const char* row : {"x", "y", "z", "bottom"}) {
            set(row, fit.row);
        }
    } else {
        set("xy", fit.xy);
        set("z", fit.z);
        set("home", fit.home);
        char steps[32];
        std::snprintf(steps, sizeof(steps), "%s_steps", named->prefix);
        if (lv_obj_t* bar = lv_obj_find_by_name(root, steps)) {
            lv_obj_set_width(bar, fit.steps_w);
        }
    }
    spdlog::trace("[{}] {} fit: key {}x{} ({})", get_name(), named->view, fit.key_w, fit.key_h,
                  portrait ? "portrait" : "landscape");
}

void MotionPanel::draw_bed_plate(lv_event_t* e) const {
    lv_obj_t* plate_obj = lv_event_get_current_target_obj(e);
    lv_obj_t* area = plate_obj ? lv_obj_get_parent(plate_obj) : nullptr;
    const auto plate = helix::current_bed_plate(get_printer_state(), get_moonraker_api());
    if (!area || !plate) {
        return;
    }
    const auto& a = plate->area;
    helix::BedPlateLayersInput in;
    in.x_min = a.x_min;
    in.x_max = a.x_max;
    in.y_min = a.y_min;
    in.y_max = a.y_max;
    in.circular = plate->circular;
    if (const auto* occupancy = helix::get_bed_occupancy_state()) {
        in.outlines = occupancy->outlines();
    }
    const auto layers = helix::plan_bed_plate_layers(in, bed_surface_mapper(area, a));

    lv_area_t coords;
    lv_obj_get_coords(area, &coords);
    const lv_point_t origin{coords.x1 + lv_obj_get_style_pad_left(area, LV_PART_MAIN),
                            coords.y1 + lv_obj_get_style_pad_top(area, LV_PART_MAIN)};

    // The plate's own border color, faded: the grid follows the live/dim
    // styling the XML gives the plate.
    constexpr lv_opa_t kGridOpaFraction = LV_OPA_30;
    helix::BedPlateStyle style;
    style.grid_color = lv_obj_get_style_border_color(plate_obj, LV_PART_MAIN);
    style.grid_opa = static_cast<lv_opa_t>(lv_obj_get_style_border_opa(plate_obj, LV_PART_MAIN) *
                                           kGridOpaFraction / LV_OPA_COVER);
    style.outline_color = theme_manager_get_color("warning");
    helix::draw_bed_plate_layers(lv_event_get_layer(e), origin, layers, style);
}

void MotionPanel::update_bed_marker() {
    if (!overlay_root_ || motion_tab_ != 2) {
        return;
    }
    lv_obj_t* area = helix::ui::find_required(overlay_root_, "bed_map_area", get_name());
    lv_obj_t* marker = helix::ui::find_required(area, "bed_map_marker", get_name());
    lv_obj_t* plate_obj = helix::ui::find_required(area, "bed_map_plate", get_name());
    const auto plate = helix::current_bed_plate(get_printer_state(), get_moonraker_api());
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
    const auto& drag_target = bed_gesture_.drag_target();
    const bool dragging = drag_target && drag_target->x && drag_target->y;
    std::string text;
    if (!dragging && !helix::toolhead_is_homed(get_printer_state())) {
        text = lv_tr("Home all axes to use the map");
    } else {
        // The target under a dragging finger; at rest the header already
        // shows where the head is, so only a pending lift is worth saying.
        if (dragging) {
            text = fmt::format("X {:.1f}  Y {:.1f}", *drag_target->x, *drag_target->y);
        }
        // Announced before the tap: an unrequested Z move is acceptable only
        // when the user was told about it first.
        const auto lift = helix::axis_is_homed(get_printer_state(), helix::Axis::Z)
                              ? helix::current_plate_lift(get_printer_state(),
                                                          move_sender_.target_start_z(current_z_))
                              : helix::PlateLift{};
        const std::string lift_text =
            helix::plate_lift_readout(show_actual_ ? live_z_ : current_z_, lift);
        if (!lift_text.empty()) {
            if (!text.empty()) {
                text += "  ";
            }
            text += lift_text;
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
    // The gesture maps points through this surface while the call lasts.
    bed_touch_area_ = area;
    const int x = static_cast<int>(screen_point.x);
    const int y = static_cast<int>(screen_point.y);
    switch (phase) {
    case helix::BedTouch::Pressed:
        bed_gesture_.press(x, y);
        break;
    case helix::BedTouch::Pressing: {
        if (bed_gesture_.pressing(x, y)) {
            refresh_bed_readout();
        }
        break;
    }
    case helix::BedTouch::Released: {
        const auto release = bed_gesture_.release(x, y);
        if (!release.was_drag && release.tap_target) {
            bed_gesture_.send_once(*release.tap_target);
        }
        refresh_bed_readout(); // a lift now in flight is no longer pending
        break;
    }
    case helix::BedTouch::Lost:
        end_bed_gesture();
        break;
    }
    bed_touch_area_ = nullptr;
}

void MotionPanel::end_bed_gesture() {
    bed_gesture_.lost();
    refresh_bed_readout();
}

void MotionPanel::send_bed_target(const helix::AxisTarget& target) {
    spdlog::info("[{}] Plate target X{:.2f} Y{:.2f}{}", get_name(), target.x.value_or(0.0),
                 target.y.value_or(0.0),
                 target.z ? fmt::format(" (lift Z{:.2f})", *target.z) : std::string());
    if (!dispatch_target(target)) {
        bed_gesture_.mark_failed();
    }
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

    update_jog_distance_labels();

    // Persist setting
    auto* cfg = Config::get_instance();
    cfg->set("/motion/jog_mode", static_cast<int>(mode));
    cfg->save();

    spdlog::info("[MotionPanel] Jog mode: {}", jog_mode_name(mode));
}

void MotionPanel::update_jog_distance_labels() {
    if (!subjects_initialized_)
        return;

    const auto& mode_dist = get_jog_mode_distances(current_mode_);
    snprintf(jog_outer_label_buf_, sizeof(jog_outer_label_buf_), "%smm", mode_dist.outer_label);
    lv_subject_copy_string(&jog_outer_label_subject_, jog_outer_label_buf_);

    snprintf(jog_inner_label_buf_, sizeof(jog_inner_label_buf_), "%smm", mode_dist.inner_label);
    lv_subject_copy_string(&jog_inner_label_subject_, jog_inner_label_buf_);
}

// ============================================================================
// Step Selector (arrow layouts)
// ============================================================================

void MotionPanel::refresh_step_list() {
    steps_ = jog_step_list();
    step_index_ = nearest_step_index(steps_, step_mm_);
    if (!subjects_initialized_)
        return;

    const int count = static_cast<int>(std::min<size_t>(steps_.size(), kJogStepMax));
    for (int i = 0; i < count; ++i) {
        format_distance_label(step_label_buf_[i], sizeof(step_label_buf_[i]), steps_[i]);
        lv_subject_copy_string(&step_label_subject_[i], step_label_buf_[i]);
    }
    for (int i = 0; i < kJogStepMax; ++i) {
        lv_subject_set_int(&step_active_subject_[i], i == static_cast<int>(step_index_) ? 1 : 0);
    }
    // Last: a changed count rebuilds the selector, whose new buttons bind the
    // labels and flags already in place.
    if (lv_subject_get_int(&step_count_subject_) != count) {
        lv_subject_set_int(&step_count_subject_, count);
        // The step bar's width follows the count; the layout keeps its size.
        if (overlay_root_) {
            for (const char* view : {"motion_jog_arrows", "motion_jog_arrows_home"}) {
                fit_jog_layout(lv_obj_find_by_name(overlay_root_, view));
            }
        }
    }
}

void MotionPanel::select_jog_step(int index) {
    if (index < 0 || static_cast<size_t>(index) >= steps_.size()) {
        spdlog::warn("[{}] Ignoring out-of-range step index {}", get_name(), index);
        return;
    }
    step_index_ = static_cast<size_t>(index);
    step_mm_ = steps_[step_index_];
    if (subjects_initialized_) {
        for (int i = 0; i < kJogStepMax; ++i) {
            lv_subject_set_int(&step_active_subject_[i], i == index ? 1 : 0);
        }
    }

    auto* cfg = Config::get_instance();
    cfg->set("/motion/jog_step", step_mm_);
    cfg->save();
    spdlog::info("[MotionPanel] Jog step: {}mm", step_mm_);
}

float MotionPanel::selected_jog_step() const {
    return steps_.empty() ? step_mm_ : steps_[step_index_];
}
