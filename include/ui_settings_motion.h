// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_motion.h
 * @brief Motion Settings overlay - jog feedrates and step distances
 *
 * Persisted settings owned by SettingsManager:
 * - Jog Speed XY / Z (stored mm/min, displayed mm/s)
 * - Six step distances (Fine/Coarse/Turbo x inner/outer, mm)
 * - Bed map clearance (mm below which a Bed-tab move lifts Z first)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsManager jog accessors
 */

#pragma once

#include "ui_coalesced_timer.h"
#include "ui_panel_motion.h"

#include "overlay_base.h"
#include "subject_managed_panel.h"

namespace helix {
class IMoonrakerAPI;
}

namespace helix::settings {

/**
 * @brief One row per control. The value doubles as the user_data on each
 * setting_value_field in motion_settings_overlay.xml, so the order here
 * and the numbers there must agree. kFieldSpecs in the .cpp is indexed by
 * the same value.
 */
enum class Field : int {
    JogSpeedXY = 0,
    JogSpeedZ,
    FineInner,
    FineOuter,
    CoarseInner,
    CoarseOuter,
    TurboInner,
    TurboOuter,
    BedMapClearance,
    Count
};

/**
 * @class MotionSettingsOverlay
 * @brief Overlay for jog speed and step distance settings
 *
 * Opened only through show_motion_settings_overlay(), so the settings row and
 * the motion panel header cog share one entry point.
 */
class MotionSettingsOverlay : public OverlayBase {
  public:
    ~MotionSettingsOverlay() override;

    void set_api(IMoonrakerAPI* api);

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Motion Settings";
    }
    const char* xml_component() const override {
        return "motion_settings_overlay";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    //
    // === Event Handlers (public for static callbacks) ===
    //

    /// Slider drag or typed speed, in mm/s. is_z picks the axis.
    void handle_jog_speed_changed(bool is_z, int mm_s);

    /// Keypad-confirmed value for one row (speed in mm/s or distance in mm).
    void handle_keypad_value(Field field, double value);

    /// Open the keypad for one row (speed or distance).
    void handle_field_clicked(Field field);

    /// Reset button - restore the six shipped step distances.
    void handle_reset_distances();

  private:
    // The instance accessor lives in ui_settings_motion.cpp: opening the
    // overlay any other way skips set_api(), and the speed sliders silently
    // lose their ceiling.

    /// Format one row's display buffer from SettingsManager.
    void format_display(size_t i);

    /// Re-read SettingsManager into the display subjects.
    void refresh_displays();

    /// Re-read SettingsManager into the sliders and their range labels.
    void refresh_sliders();

    /// Persist any pending slider value (called by the debounce timer).
    void persist_pending_speed();

    /// Stored mm/min clamped to what the printer currently permits, so the
    /// field, the slider and the emitted move always agree.
    int effective_mm_min(int stored_mm_min) const;

    /// One of the overlay's speed sliders, or nullptr before create().
    lv_obj_t* speed_slider(bool is_z) const;

    /// Slider ceiling in mm/s, from the printer's reported feedrate limit.
    int max_jog_mm_s() const;

    /// Deinitialize subjects for clean shutdown.
    void deinit_subjects();

    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_{nullptr};

    //
    // === State Tracking ===
    //

    /// Set while our own numeric keypad is open: returning from it
    /// re-activates this overlay, and a refresh there would read back the
    /// pre-debounce value and clobber the one just typed.
    bool returning_from_keypad_{false};

    /// Row the open keypad is editing. The overlay stack guarantees one
    /// keypad at a time, so a single slot is enough.
    Field pending_keypad_field_{Field::JogSpeedXY};

    /// Debounced SettingsManager write for slider drags: a drag fires
    /// value_changed per pixel and each write persists to disk. Indexed
    /// [XY, Z]; 0 means nothing pending (the settings clamp floor is 60).
    static constexpr uint32_t kPersistDebounceMs = 250;
    helix::ui::CoalescedTimer persist_timer_{kPersistDebounceMs};
    int pending_mm_min_[2]{0, 0};

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    /// One string display subject per Field, named in kSubjectNames.
    lv_subject_t display_subjects_[static_cast<size_t>(Field::Count)]{};
    char display_buffers_[static_cast<size_t>(Field::Count)][24]{};

    /// Slider ceiling text, bound to both speed sliders' max end labels.
    lv_subject_t jog_speed_max_subject_{};
    char jog_speed_max_buf_[16]{};

    //
    // === Static Callbacks ===
    //

    /// ui_keypad_callback_t; user_data carries the overlay.
    static void on_keypad_value(float value, void* user_data);
};

/**
 * @brief The single opener for the Motion settings overlay
 *
 * Called by the Printing settings row and the motion panel header cog, so
 * both share one creation/registration path.
 */
void show_motion_settings_overlay();

/// SettingsManager's step-distance clamp range, in mm. It clamps with inline
/// literals and exposes no accessor, so this is the one spelling the keypad
/// rules below see; keep the two in step.
constexpr float kJogDistanceMinMm = 0.01f;
constexpr float kJogDistanceMaxMm = 200.0f;

/// Min/max for one row's numeric keypad.
struct KeypadBounds {
    float min;
    float max;
};

/// Bounds for one row's keypad. Distance rows are coupled: an outer row's
/// floor is its mode's current inner step and an inner row's ceiling is its
/// mode's current outer, so inner can never exceed outer. Speed rows have no
/// coupled pair — the two arguments carry the row's slider range and pass
/// through unchanged.
inline KeypadBounds keypad_bounds(Field field, float inner_mm, float outer_mm) {
    switch (field) {
    case Field::FineOuter:
    case Field::CoarseOuter:
    case Field::TurboOuter:
        return {inner_mm, kJogDistanceMaxMm};
    case Field::FineInner:
    case Field::CoarseInner:
    case Field::TurboInner:
        return {kJogDistanceMinMm, outer_mm};
    default: // speed rows: passthrough of the slider range
        return {inner_mm, outer_mm};
    }
}

} // namespace helix::settings
