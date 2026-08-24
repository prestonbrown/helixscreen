// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "jog_coalescer.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

/**
 * @file ui_panel_motion.h
 * @brief Motion panel - XYZ movement and homing control
 *
 * Overlay panel for jogging the printer head in X/Y/Z directions and homing axes.
 * Uses OverlayBase pattern with lifecycle hooks.
 */

// Jog mode: determines inner/outer ring distances for XY pad and Z buttons
namespace helix {
enum class JogMode { Fine = 0, Coarse = 1, Turbo = 2 };
constexpr int JOG_MODE_COUNT = 3;

// Inner/outer distance pair for each mode
struct JogModeDistances {
    float inner;
    float outer;
    const char* inner_label;
    const char* outer_label;
};

// Mode → distance mapping (Fine: 0.1/1, Coarse: 1/10, Turbo: 10/50)
inline const JogModeDistances& get_jog_mode_distances(JogMode mode) {
    static const JogModeDistances modes[] = {
        {0.1f, 1.0f, "0.1", "1"},
        {1.0f, 10.0f, "1", "10"},
        {10.0f, 50.0f, "10", "50"},
    };
    static_assert(sizeof(modes) / sizeof(modes[0]) == JOG_MODE_COUNT,
                  "modes[] size must match JOG_MODE_COUNT");
    int idx = static_cast<int>(mode);
    if (idx < 0 || idx >= JOG_MODE_COUNT)
        idx = 1; // default Coarse
    return modes[idx];
}

inline const char* jog_mode_name(JogMode mode) {
    static const char* names[] = {"Fine", "Coarse", "Turbo"};
    int idx = static_cast<int>(mode);
    if (idx < 0 || idx >= JOG_MODE_COUNT)
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
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Motion Panel";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivate() override;
    void on_ui_destroyed() override;

    // === Public API ===
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }
    void set_position(float x, float y, float z);
    helix::JogMode get_jog_mode() const {
        return current_mode_;
    }
    void jog(helix::JogDirection direction, float distance_mm);
    void home(char axis);
    void handle_z_button(const char* name);
    void set_jog_mode(helix::JogMode mode); // Switch between Fine/Coarse/Turbo jog mode

  private:
    // RAII subject manager - auto-deinits all registered subjects on destruction
    SubjectManager subjects_;

    lv_subject_t pos_x_subject_;
    lv_subject_t pos_y_subject_;
    lv_subject_t pos_z_subject_;           // Commanded Z position
    lv_subject_t pos_z_actual_subject_;    // Actual Z position (with mesh compensation)
    lv_subject_t motion_z_actual_visible_; // 1 when actual differs from commanded
    lv_subject_t z_axis_label_subject_;    // "Bed" or "Print Head"
    lv_subject_t z_up_icon_subject_;       // "arrow_expand_up" or "arrow_up"
    lv_subject_t z_down_icon_subject_;     // "arrow_expand_down" or "arrow_down"
    lv_subject_t z_large_label_subject_;   // "10mm" or "1mm" (large Z button label)
    lv_subject_t z_small_label_subject_;   // "1mm" or "0.1mm" (small Z button label)
    lv_subject_t jog_mode_fine_active_;    // 1 when Fine mode active
    lv_subject_t jog_mode_coarse_active_;  // 1 when Coarse mode active
    lv_subject_t jog_mode_turbo_active_;   // 1 when Turbo mode active
    char pos_x_buf_[32];
    char pos_y_buf_[32];
    char pos_z_buf_[32];
    char pos_z_actual_buf_[32];
    char z_axis_label_buf_[16];
    char z_up_icon_buf_[24];
    char z_down_icon_buf_[24];
    char z_large_label_buf_[8];
    char z_small_label_buf_[8];
    bool bed_moves_ = false; // If true, invert Z direction (arrows match bed movement)

    helix::JogMode current_mode_ = helix::JogMode::Coarse;
    float current_x_ = 0.0f;
    float current_y_ = 0.0f;
    float current_z_ = 0.0f; // Gcode (commanded) Z position

    // For Z display: track both commanded and actual positions
    int gcode_z_centimm_ = 0;
    int actual_z_centimm_ = 0;

    lv_obj_t* jog_pad_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    bool callbacks_registered_ = false;

    helix::JogCoalescer jog_coalescer_;
    bool x_edge_warned_ = false; // dedupe "blocked at bed edge" warnings
    bool y_edge_warned_ = false;

    // Route a tap/flush through the coalescer and send if idle.
    void dispatch_jog(const helix::AxisMove& delta);
    // Send one relative move; ack/error callbacks re-enter the coalescer.
    void send_jog_move(const helix::AxisMove& move);

    // Homing state subjects (0=unhomed, 1=homed) for declarative XML bind_style
    lv_subject_t motion_x_homed_;
    lv_subject_t motion_y_homed_;
    lv_subject_t motion_z_homed_;

    ObserverGuard position_x_observer_;
    ObserverGuard position_y_observer_;
    ObserverGuard gcode_z_observer_;
    ObserverGuard actual_z_observer_;
    ObserverGuard bed_moves_observer_;
    ObserverGuard homed_axes_observer_;
    ObserverGuard jog_ready_observer_;

    void setup_jog_pad();
    void register_position_observers();

    // Enable/dim the jog pad from the current nav_buttons_enabled state
    // (connected AND klippy ready). Jogging while not ready is refused at the
    // API layer; this makes the pad visibly unavailable to match.
    void update_jog_pad_enabled();

    static void jog_pad_jog_cb(helix::JogDirection direction, float distance_mm, void* user_data);
    static void jog_pad_home_cb(void* user_data);
    // Position observers use lambda-based observer factory (no static callbacks needed)

    void update_z_axis_label(bool bed_moves);
    void update_z_display();       // Updates Z label with actual in brackets when different
    void update_z_button_labels(); // Update Z button text for current mode
};

MotionPanel& get_global_motion_panel();
