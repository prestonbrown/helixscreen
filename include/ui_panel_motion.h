// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "axis.h"
#include "bed_plate_gesture.h"
#include "core/static_panel_registry.h"
#include "hold_repeat_timer.h"
#include "motion_presets.h"
#include "overlay_base.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"
#include "toolhead_move_sender.h"

#include <optional>
#include <string>
#include <vector>

/**
 * @file ui_panel_motion.h
 * @brief Motion panel - XYZ movement and homing control
 *
 * Overlay panel for jogging the printer head in X/Y/Z directions and homing axes.
 * Uses OverlayBase pattern with lifecycle hooks.
 */

// Jog mode: determines inner/outer ring distances for XY pad and Z buttons
namespace helix {

struct AxisBounds;

/// Keypad parameters for tapping one axis readout in the motion header.
struct AxisKeypadParams {
    float min_value;     ///< Axis envelope minimum (mm)
    float max_value;     ///< Axis envelope maximum (mm)
    float seed;          ///< COMMANDED position (mm); always commanded, even in actual mode
    bool allow_negative; ///< True only when the axis minimum is below zero
};

/// Seed and bounds for one axis' coordinate keypad. Empty when the axis
/// envelope is unknown: an absolute move would have nothing to clamp against.
std::optional<AxisKeypadParams> keypad_params_for_axis(const AxisBounds& bounds, Axis axis,
                                                       double commanded_mm);

/// Whether a Z button driving G-code direction `direction_mm` (after the
/// bed_moves inversion) can still move: true only when Z is homed with known
/// bounds and the commanded `z` already
/// sits at the bound that direction drives toward, within AxisMove::kEpsilonMm.
/// A partial move is possible anywhere else, so the button stays enabled.
bool z_direction_blocked(bool z_homed, bool bounds_known, double z, double z_min, double z_max,
                         double direction_mm);

/// A block's size in px.
struct BlockSize {
    int w = 0;
    int h = 0;
};

/// Sizes for one of the alternative Jog-tab layouts: the key size and the
/// blocks MotionPanel::fit_jog_layout() sets. Arrows layouts fill `xy`, `z`,
/// `home` and `steps_w`; Axis Rows fills `row` (each of its four rows).
struct JogLayoutFit {
    int key_w = 0;
    int key_h = 0;
    BlockSize xy;
    BlockSize z;
    BlockSize home;
    BlockSize row;
    int steps_w = 0;
};

/**
 * @brief Block sizes for a Jog-tab layout in a `box_w` x `box_h` view
 *
 * Keys come out near square (fit_key_grid()); every block is a whole number of
 * keys plus the `gap` between them, so the keys inside fill it. Arrows and
 * Arrows + Home put the home block beside the keys in landscape and under
 * them in portrait, and leave `step_h` plus a gap under everything for the
 * step selector, whose `step_count` keys stay at most 1.4 times as wide as tall.
 * Axis Rows stacks four rows of two minus keys, the axis key and two plus
 * keys. Empty for MotionLayout::Pad, which sizes its own pad.
 */
JogLayoutFit jog_layout_fit(MotionLayout layout, bool portrait, int box_w, int box_h, int gap,
                            int step_h, int step_count, int min_key);

enum class JogMode { Fine = 0, Coarse = 1, Turbo = 2 };
constexpr int kJogModeCount = 3;

// Inner/outer distance pair for each mode. Labels are owned buffers so the
// struct can travel by value out of the settings lookup.
struct JogModeDistances {
    float inner;
    float outer;
    char inner_label[16];
    char outer_label[16];
};

/// Inner and outer ring distances for a jog mode, in millimetres.
/// Values come from settings; the labels are formatted for display.
JogModeDistances get_jog_mode_distances(JogMode mode);

/// Most steps an arrow layout's selector offers: each mode's two rings.
constexpr int kJogStepMax = 2 * kJogModeCount;

/// The step list the arrow layouts offer: @p distances ascending, with a value
/// within 0.001mm of a smaller one merged into it.
std::vector<float> unique_sorted_steps(std::vector<float> distances);

/// Every configured jog distance (each mode's inner and outer ring) as a step
/// list, so the one set of distances in settings drives every layout.
std::vector<float> jog_step_list();

/// Index of the step closest to @p mm; 0 for an empty list.
size_t nearest_step_index(const std::vector<float>& steps, float mm);

/// Which distance a jog button moves.
enum class JogButtonStep {
    Inner,    ///< the jog mode's inner ring
    Outer,    ///< the jog mode's outer ring
    Selected, ///< the arrow layouts' selected step
};

/// A jog button, decoded from its XML user_data.
struct JogButton {
    Axis axis;
    int sign; ///< +1 or -1; for Z a SCREEN direction, before the bed_moves inversion
    JogButtonStep step;
};

/// Decode "<axis>_<direction>_<size>": axis x, y or z; direction plus/minus
/// for X and Y, up/down for Z; size small (inner ring), large (outer ring) or
/// step (the selected step). Empty for anything else.
std::optional<JogButton> parse_jog_button(const char* name);

inline const char* jog_mode_name(JogMode mode) {
    static const char* names[] = {"Fine", "Coarse", "Turbo"};
    int idx = static_cast<int>(mode);
    if (idx < 0 || idx >= kJogModeCount)
        return "Unknown";
    return names[idx];
}

// Jog direction
enum class JogDirection {
    N,  // +Y
    S,  // -Y
    E,  // +X
    W,  // -X
    NE, // +X+Y
    NW, // -X+Y
    SE, // +X-Y
    SW  // -X-Y
};
} // namespace helix

class MotionPanel : public OverlayBase {
  public:
    MotionPanel();
    ~MotionPanel() override;

    // === OverlayBase interface ===
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    const char* xml_component() const override {
        return "motion_panel";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Motion Panel";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void on_ui_destroyed() override;

    // === Public API ===
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }
    void set_position(float x, float y, float z);
    helix::JogMode get_jog_mode() const {
        return current_mode_;
    }

    /// Jog one zone. Returns false when nothing was dispatched (fully clamped
    /// at a limit, or the send was refused) - a hold-to-repeat reading false
    /// stops repeating. is_repeat is false for a tap and for the first jog of
    /// a hold (a fresh press); a fresh press fully clamped at a limit warns.
    bool jog(helix::JogDirection direction, float distance_mm, bool is_repeat = false);
    void home(char axis);

    /// Home the named axes ("X", "XY", ...; empty homes all).
    void home_axes(const std::string& axes);

    /// Jog one button named in parse_jog_button() form. Same
    /// false-means-refused contract as jog(), and the same is_repeat meaning
    /// (false = the initial jog of this press).
    bool handle_jog_button(const char* name, bool is_repeat = false);
    void set_jog_mode(helix::JogMode mode); // Switch between Fine/Coarse/Turbo jog mode

    /// Arrow layouts: select the step at @p index in the step list (persisted).
    void select_jog_step(int index);

    /// The arrow layouts' selected step, in mm.
    float selected_jog_step() const;

    /// Rebuild the step selector from the configured distances, keeping the
    /// selection on the step nearest the one in use.
    void refresh_step_list();

    /// The jog-button hold timer, for the XML pressed/released/press_lost
    /// callbacks. Also the test seam: poll() drives the repeat without a
    /// periodic timer, which the unit-test harness never runs.
    helix::HoldRepeatTimer& jog_hold_timer() {
        return jog_hold_timer_;
    }

    /// Arm the hold repeat for a named jog button press (the XML pressed
    /// event). `button` is the pressed widget, when known.
    void begin_jog_hold(const char* button_name, lv_obj_t* button = nullptr);

    /// Flip the persisted commanded/actual coordinate preference. The panel's
    /// observer on the settings subject re-renders the readouts.
    void toggle_coordinate_source();

    /// Open the coordinate keypad for one axis ('x'/'y'/'z'), seeded with the
    /// COMMANDED position and bounded by the axis envelope. Refuses (no-op)
    /// while jogging is gated off (not connected or klippy not ready) or the
    /// axis is unhomed.
    void open_axis_keypad(char axis);

    /// Keypad confirm: dispatch the absolute target for that one axis. An
    /// unhomed axis sends nothing; nothing in the panel homes except Home.
    void request_axis_target(char axis, double mm);

    /// Select the content tab: 0=Jog, 1=Move, 2=Bed. Jog is re-selected every
    /// time the panel opens.
    void set_motion_tab(int tab);

    /// Move tab: send the toolhead to one of the nine bed grid positions. The
    /// target is computed at tap time; unknown axis bounds warn instead of
    /// moving. It goes out through the plate gesture's send_once(), so it lifts
    /// to the plate floor, refuses a part too tall to clear, and sends nothing
    /// unless every axis is homed.
    void handle_preset(helix::MotionPreset preset);

    /// Move tab: run the ParkToolhead macro when the slot resolves to one,
    /// else lift Z and park over the rear of the plate (plate_rear_park()).
    /// Either way every axis must be homed, or nothing is sent.
    void handle_park();

    /// Park with no macro: over the rear of the plate at park_plate_lift()'s
    /// height. A part too tall to clear refuses with a notice; unhomed sends
    /// nothing.
    void park_over_plate();

    /// Move tab: raise the shared Disable Motors confirmation.
    void handle_motors_off();

    /// The Refuse notice: a part on the bed is too tall to clear. The home Toolhead
    /// widget's plate raises it too.
    static void notify_part_too_tall();

    /// Bed tab: one pointer event at @p screen_point on the touch surface
    /// @p area. A tap moves on release to the press point; travel past a few
    /// px makes it a drag, and the head follows the finger (latest target
    /// wins), with the release point sent only if it differs from the last.
    void handle_bed_touch(helix::BedTouch phase, lv_obj_t* area, lv_point_t screen_point);

    /// Bed tab: size the plate to the bed's aspect inside the touch surface
    /// and place the marker. No-op while another tab is showing.
    void layout_bed_map();

    /// Size an alternative Jog-tab layout's blocks for its current size
    /// (jog_layout_fit()). `root` is the layout component's view; its
    /// SIZE_CHANGED event calls this, and so does a change in the step count.
    void fit_jog_layout(lv_obj_t* root);

    /// Bed tab: draw the reference grid and the print's outlines on the plate
    /// (its DRAW_MAIN hook).
    void draw_bed_plate(lv_event_t* e) const;

    /// Clamp one axis against its bounds. Partial travel is silent; a fully
    /// clamped FRESH press (anything but a hold repeat tick) warns every time
    /// with the limit it hit. Returns the permitted delta, 0.0 when blocked.
    double clamp_axis_delta(helix::Axis axis, double current, double uncommitted, double delta,
                            float min, float max, bool fresh_press);
    /// Raise the "at its limit" warning for an axis refused at the clamp bound
    /// on its max (`at_max`) or min side.
    void warn_axis_limit(helix::Axis axis, float clamp_bound, bool at_max);

  private:
    // RAII subject manager - auto-deinits all registered subjects on destruction
    SubjectManager subjects_;

    lv_subject_t pos_x_subject_{};
    lv_subject_t pos_y_subject_{};
    lv_subject_t pos_z_subject_{};           // Commanded Z position
    lv_subject_t z_axis_label_subject_{};    // "Bed" or "Print Head"
    lv_subject_t z_up_icon_subject_{};       // "arrow_expand_up" or "arrow_up"
    lv_subject_t z_down_icon_subject_{};     // "arrow_expand_down" or "arrow_down"
    lv_subject_t jog_outer_label_subject_{}; // jog mode's outer distance, "10mm"
    lv_subject_t jog_inner_label_subject_{}; // jog mode's inner distance, "1mm"
    lv_subject_t jog_mode_fine_active_{};    // 1 when Fine mode active
    lv_subject_t jog_mode_coarse_active_{};  // 1 when Coarse mode active
    lv_subject_t jog_mode_turbo_active_{};   // 1 when Turbo mode active
    // Per-axis homing state (0=unhomed, 1=homed) for declarative bind_style:
    // the coordinate readouts mute an axis whose position is not trustworthy.
    lv_subject_t motion_x_homed_{};
    lv_subject_t motion_y_homed_{};
    lv_subject_t motion_z_homed_{};
    // Z button limit state (1=render disabled): a further move in that
    // button's G-code direction is impossible. up/down are screen directions,
    // the bed_moves inversion inside update_z_button_blocked() maps them.
    lv_subject_t motion_z_up_blocked_{};
    lv_subject_t motion_z_down_blocked_{};
    // Content tab selection (0=Jog, 1=Move, 2=Bed). zone_tab instances bind
    // one active subject each and their labels are subject-bound, matching the
    // AMS environment strip that shares the component.
    lv_subject_t motion_tab_subject_{};
    lv_subject_t motion_tab_active_[3]{};
    lv_subject_t motion_tab_label_[3]{};
    char motion_tab_label_buf_[3][32];
    // Bed tab: readout text under the plate, and 1 when the plate is round.
    lv_subject_t motion_bed_readout_{};
    char motion_bed_readout_buf_[128];
    lv_subject_t motion_bed_circular_{};
    /// Fill the tab label buffers (and subjects, once they exist) in the
    /// current language.
    void refresh_tab_labels();
    int motion_tab_ = 0;
    char pos_x_buf_[32];
    char pos_y_buf_[32];
    char pos_z_buf_[32];
    char z_axis_label_buf_[64];
    char z_up_icon_buf_[24];
    char z_down_icon_buf_[24];
    char jog_outer_label_buf_[16];
    char jog_inner_label_buf_[16];
    // Arrow layouts' step selector: how many steps show, each one's label and
    // whether it is the selected one.
    lv_subject_t step_count_subject_{};
    lv_subject_t step_label_subject_[helix::kJogStepMax]{};
    lv_subject_t step_active_subject_[helix::kJogStepMax]{};
    char step_label_buf_[helix::kJogStepMax][16];
    std::vector<float> steps_;
    size_t step_index_ = 0;
    /// The selected step as persisted; survives a distance edit that drops it
    /// from the list, so the nearest remaining step is chosen.
    float step_mm_ = 1.0f;
    bool bed_moves_ = false; // If true, invert Z direction (arrows match bed movement)

    helix::JogMode current_mode_ = helix::JogMode::Coarse;
    float current_x_ = 0.0f;
    float current_y_ = 0.0f;
    float current_z_ = 0.0f; // Gcode (commanded) Z position

    int gcode_z_centimm_ = 0;

    lv_obj_t* jog_pad_ = nullptr;

    /// One move in flight, the newest coalesced behind it, for jogs and targets alike.
    helix::ToolheadMoveSender move_sender_;

    /// The Move tab's Disable Motors confirmation. The guard hides the dialog
    /// if the panel is destroyed while it is open.
    helix::ui::WidgetRef motors_off_dialog_;

    /// Whether a repeat tick has fired for the current jog-button hold: the
    /// first tick is that press's initial jog, later ones are repeats.
    bool jog_hold_repeated_ = false;

    /// Hold-to-repeat for the jog buttons (Z column, arrows, axis rows). The
    /// pad owns its own repeat (it is the only one that can see the press
    /// point move between zones); both use helix::HoldRepeatTimer so the
    /// timing logic is shared.
    helix::HoldRepeatTimer jog_hold_timer_;
    char jog_hold_button_[16] = {};
    /// The held button. A button disabled under the finger never receives
    /// RELEASED, so LVGL would leave it drawn pressed.
    helix::ui::WidgetRef jog_hold_obj_;
    /// A widget was recorded for this hold. Deleting it (a layout swap, a
    /// reload) sends no RELEASED, so a recorded widget gone null ends the hold.
    bool jog_hold_has_obj_ = false;

    /// stop_hold_repeat() sweeps both this timer and the pad's on every
    /// teardown path.
    void stop_hold_repeat();
    static bool jog_hold_fire(void* user_data);

    /// Target under @p screen_point on the touch surface @p area.
    std::optional<helix::AxisTarget> bed_target_at(lv_obj_t* area, lv_point_t screen_point) const;

    /// Move the toolhead marker to the commanded XY.
    void update_bed_marker();

    /// Re-render the Bed tab readout: the target under a dragging finger and
    /// the clearance lift a commit would make.
    void refresh_bed_readout();

    /// The Bed tab's press / drag / release decisions, shared with the home
    /// Toolhead widget.
    helix::BedPlateGesture bed_gesture_;
    /// The touch surface handle_bed_touch() is handling, for the gesture's
    /// target_at hook; null outside that call.
    lv_obj_t* bed_touch_area_ = nullptr;
    /// Stop tracking the finger: no press, no drag target, readout at rest.
    void end_bed_gesture();

    /// The gesture's send hook: one plate target (Bed tab or preset), lift
    /// already applied.
    void send_bed_target(const helix::AxisTarget& target);

    /// Gating backstop behind the Move tab's XML disabled bindings: commands
    /// are allowed only while nav buttons are enabled (connected + klippy
    /// ready) and no print is active.
    bool moves_allowed() const;

    /// Write motion_tab_ into the tab subject and the three per-tab active
    /// subjects.
    void sync_motion_tab_subjects();

    // Route a jog delta through move_sender_. Returns whether the move was
    // dispatched or accepted as pending.
    bool dispatch_jog(const helix::AxisMove& delta);
    // Route an absolute target through move_sender_, clamped to clamp_bounds().
    bool dispatch_target(const helix::AxisTarget& target);

    ObserverGuard position_x_observer_;
    ObserverGuard position_y_observer_;
    ObserverGuard gcode_z_observer_;
    ObserverGuard gcode_z_offset_observer_;
    ObserverGuard live_position_observer_x_;
    ObserverGuard live_position_observer_y_;
    ObserverGuard live_position_observer_z_;
    ObserverGuard coordinate_mode_observer_;
    ObserverGuard language_observer_;
    ObserverGuard bed_moves_observer_;
    ObserverGuard homed_axes_observer_;
    ObserverGuard jog_ready_observer_;
    ObserverGuard layout_observer_;
    /// Redraws the plate when the bed-occupancy verdict or outlines change.
    ObserverGuard bed_occupancy_observer_;
    /// (Re)take bed_occupancy_observer_ on the current session's state.
    void observe_bed_occupancy();

    // Actual (live) positions in mm; the readouts show these when the
    // coordinate preference is "actual", the commanded current_x_/y_/z_
    // otherwise. Keypad seed and jog math always use the commanded values.
    float live_x_ = 0.0f;
    float live_y_ = 0.0f;
    float live_z_ = 0.0f;

    /// Axis whose keypad is open ('x'/'y'/'z'); the keypad callback is a bare
    /// function pointer, so the axis travels through the panel.
    char keypad_axis_ = 'x';

    /// Re-render all three readout subjects from the selected source.
    void refresh_position_display();

    /// Recompute the Z-button disabled subjects from the predicted Z, homed
    /// state, bounds and bed_moves inversion.
    void update_z_button_blocked();

    /// ui_keypad_show() confirm callback.
    static void on_axis_keypad_value(float value, void* user_data);

    void setup_jog_pad();

    /// Re-fit the pad square inside its wrapper. Portrait also clamps the
    /// wrapper to the square so the growing Z column absorbs the leftover
    /// width; landscape centres the pad in its growing wrapper untouched.
    void fit_jog_pad();
    void register_position_observers();

    // Enable/dim the jog pad from the current nav_buttons_enabled state
    // (connected AND klippy ready). Jogging while not ready is refused at the
    // API layer; this makes the pad visibly unavailable to match.
    void update_jog_pad_enabled();

    static bool jog_pad_jog_cb(helix::JogDirection direction, float distance_mm, bool is_repeat,
                               void* user_data);
    static void jog_pad_home_cb(void* user_data);
    // Position observers use lambda-based observer factory (no static callbacks needed)

    void update_z_axis_label(bool bed_moves);
    void update_jog_distance_labels(); // Inner/outer labels for the current mode

    /// Which source the readouts render: 1 = actual (live) position,
    /// 0 = commanded. Mirrors the persisted preference subject.
    bool show_actual_ = false;
};

inline MotionPanel& get_global_motion_panel() {
    return helix::lazy_global<MotionPanel>("MotionPanel");
}
