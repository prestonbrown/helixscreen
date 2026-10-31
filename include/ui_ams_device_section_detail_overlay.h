// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_section_detail_overlay.h
 * @brief AMS Device Section Detail overlay for progressive disclosure
 *
 * This overlay displays controls for a single device section (e.g., "calibration"
 * or "speed") in the AMS device operations progressive disclosure pattern.
 * It is opened from the device operations overlay when the user taps a section
 * row, showing only the actions belonging to that section.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "ui_observer_guard.h"

#include "ams_error.h"
#include "ams_types.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <any>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class AmsBackend;
} // namespace helix

namespace helix::ui {

/**
 * @class AmsDeviceSectionDetailOverlay
 * @brief Overlay for a single AMS device section's actions
 *
 * Shows the actions for one section (identified by section_id) from the
 * backend's get_device_actions() list. The title is set to the section label
 * passed in via show().
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::ui::get_ams_device_section_detail_overlay();
 * overlay.show(parent_screen, "calibration", "Calibration");
 * @endcode
 */
class AmsDeviceSectionDetailOverlay : public OverlayBase {
  public:
    /**
     * @brief Destructor
     */
    ~AmsDeviceSectionDetailOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Register the constant subject a disabled action's row binds to
     */
    void init_subjects() override;

    void register_callbacks() override;

    /**
     * @brief Get human-readable overlay name
     * @return "Section Detail"
     */
    const char* get_name() const override {
        return "Section Detail";
    }

    const char* xml_component() const override {
        return "ams_device_section_detail";
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    //
    // === Public API ===
    //

    /**
     * @brief Show the overlay for a specific section
     *
     * Sets the title to the section label, builds controls for the backend
     * actions matching section_id, and pushes the overlay.
     *
     * @param parent_screen The parent screen for overlay creation
     * @param section_id The section ID to filter actions by (e.g., "calibration")
     * @param section_label Human-readable label for the overlay title
     */
    void show(lv_obj_t* parent_screen, const std::string& section_id,
              const std::string& section_label);

    /**
     * @brief Refresh the overlay from backend
     *
     * Re-queries backend and recreates controls for the current section.
     */
    void refresh();

    /**
     * @brief Follow the backend while open: rebuild when this section's actions
     *        change value or availability
     */
    void on_activate() override;

  protected:
    void before_show() override;
    void on_deactivating(DeactivateReason reason) override;

  private:
    friend class AmsDeviceSectionDetailOverlayTestAccess;

    /// The always-1 subject a disabled action's row names as its `disabled` subject.
    static constexpr const char* kActionDisabledSubject = "ams_device_action_disabled";
    /// What a dropdown shows when the backend reports no value it lists.
    static constexpr const char* kUnknownDropdownText = "-";

    //
    // === Internal Methods ===
    //

    /**
     * @brief Create the setting_*_row for a TOGGLE, SLIDER, DROPDOWN or INFO action
     *
     * The row is named after the action id. A disabled action shows its
     * disable_reason where the description goes.
     */
    void create_setting_row(lv_obj_t* parent, const helix::printer::DeviceAction& action);

    /// Create a BUTTON action's device_action_button inside a two-button row.
    void create_button(lv_obj_t* row, const helix::printer::DeviceAction& action);

    /// The action whose row (or button) the event's target sits in, or nullptr.
    [[nodiscard]] const helix::printer::DeviceAction* action_for_event(lv_event_t* e) const;

    /// The row or button built for @p action_id, or nullptr.
    [[nodiscard]] lv_obj_t* row_for(const std::string& action_id) const;

    /// Run @p action through the backend, reporting a failure, and on success record
    /// @p value as the one its row shows. Returns the result.
    AmsError execute(const helix::printer::DeviceAction& action, std::any value);

    /// Show slider @p position as @p action's value in its row's value display.
    void show_slider_value(lv_obj_t* row, const helix::printer::DeviceAction& action,
                           int32_t position) const;

    /// Set @p action's slider to @p value (clamped to its range and step) and send it.
    /// A refused value puts the slider back on the action's current value.
    void apply_slider_value(const helix::printer::DeviceAction& action, double value);

    /// Show @p choice in @p action's dropdown and send it. A refused choice puts
    /// the section back on what the backend reports.
    void apply_dropdown_choice(const helix::printer::DeviceAction& action,
                               const std::string& choice);

    /// Ask @p confirm's question before @p choice is sent. Confirmed, it goes
    /// through on_choice_confirmed(); cancelled or dismissed, nothing is sent and
    /// the section goes back to what the backend reports.
    void confirm_dropdown_choice(const helix::printer::DeviceAction& action,
                                 const std::string& choice,
                                 const helix::printer::DeviceAction::OptionConfirm& confirm);

    /// Send a confirmed @p choice, unless @p action_id has since gone or become
    /// unavailable (or no longer lists it), when the pick is dropped.
    void on_choice_confirmed(const std::string& action_id, const std::string& choice);

    //
    // === Static Callbacks ===
    //

    static void on_action_clicked(lv_event_t* e);
    static void on_toggle_changed(lv_event_t* e);
    /// The value display follows the drag; nothing is sent.
    static void on_slider_changed(lv_event_t* e);
    /// Sends the slider's value, once, on release.
    static void on_slider_released(lv_event_t* e);
    /// Opens the numeric keypad for an exact value.
    static void on_value_clicked(lv_event_t* e);
    static void on_keypad_value(float value, void* user_data);
    static void on_dropdown_changed(lv_event_t* e);

    //
    // === State ===
    //

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Container for section action controls
    lv_obj_t* actions_container_ = nullptr;

    /// The section ID this overlay is currently showing
    std::string section_id_;

    /// Its human-readable label, shown in the header
    std::string section_label_;

    /// Cached actions from backend
    std::vector<helix::printer::DeviceAction> cached_actions_;

    /// Re-reads the backend on every AMS data tick while the overlay is open.
    ObserverGuard data_observer_;

    /// refresh(), only when the backend's actions for this section differ from the shown ones.
    void refresh_if_changed();

    /// The slider action the open keypad edits, and the keypad's title
    std::string keypad_action_id_;
    std::string keypad_title_;

    lv_subject_t action_disabled_subject_{};
    /// Declared after the subject it deinits, so it is destroyed first.
    SubjectManager subjects_;
};

/**
 * @brief What a DeviceAction's control is labeled: its translated label, then the
 *        tool it applies to in the app's one tool-label form ("Tool 1 (T0)").
 */
[[nodiscard]] std::string device_action_label(const helix::printer::DeviceAction& action);

/**
 * @brief Whether @p section's actions in @p fresh differ from those in @p shown in
 *        anything a row displays: which actions, their value, or their availability.
 */
[[nodiscard]] bool device_section_changed(const std::vector<helix::printer::DeviceAction>& shown,
                                          const std::vector<helix::printer::DeviceAction>& fresh,
                                          const std::string& section);

/**
 * @brief The bind_state_if expression device_action_button greys a button with,
 *        beyond moves_machine: for a motion action still allowed on a paused
 *        print, "motion blocked and not paused"; otherwise one that is never true.
 */
[[nodiscard]] std::string device_action_disabled_cond(const helix::printer::DeviceAction& action);

inline AmsDeviceSectionDetailOverlay& get_ams_device_section_detail_overlay() {
    return lazy_global<AmsDeviceSectionDetailOverlay>("AmsDeviceSectionDetailOverlay");
}

} // namespace helix::ui
