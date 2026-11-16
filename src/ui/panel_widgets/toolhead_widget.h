// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_coalesced_timer.h"

#include "async_lifetime_guard.h"
#include "axis_move.h"
#include "bed_plate_gesture.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/toolhead_layout.h"
#include "src/ui/panel_widgets/toolhead_plate.h"
#include "toolhead_move_sender.h"
#include "toolhead_preset_slots.h"

#include <memory>
#include <optional>

#include "hv/json.hpp" // JSON_HPP_OK: config_ held by value

namespace helix {

class PrinterState;
class ToolheadPresetConfigModal;

/// Home widget for the toolhead's position and homing. Measures what each rung would
/// draw, decides the rung with decide_toolhead_layout(), and publishes the verdict and
/// the readings as subjects that panel_widget_toolhead.xml binds. Wide and up draw a
/// top-down plate with the crosshair and a Z bar on a plain object of that component.
/// A tap opens the Motion panel; the Full rung adds Home All while an axis is unhomed.
///
/// While homed and idle the plate is interactive: a tap drops a target that Move here
/// sends, and a drag moves the head live, both through the BedPlateGesture and the lift
/// rule the Motion panel's Bed tab uses. A tap on the Z bar or beside the plate, or on an
/// inert plate, still opens the Motion panel.
///
/// A Full tile with room to spare (a 2x3) adds a row of three configurable preset slots
/// under the plate and a line saying what lift the next move takes; the edit-mode gear
/// configures the slots.
class ToolheadWidget : public PanelWidget {
  public:
    explicit ToolheadWidget(PrinterState& printer_state);
    ~ToolheadWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "toolhead";
    }

    static void clicked_cb(lv_event_t* e);

    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Run preset slot @p index: a position or Park through the plate's lift rule, a Z
    /// step, or a macro. Does nothing unless the row is on screen, and a slot that moves
    /// the toolhead does nothing while the plate is inert.
    void run_preset(std::size_t index);
    [[nodiscard]] const ToolheadPresetSlots& preset_slots() const {
        return slots_;
    }
    /// The open preset picker, or nullptr.
    [[nodiscard]] ToolheadPresetConfigModal* preset_config_modal() const {
        return config_modal_.get();
    }

    /// Send G28. Does nothing unless Home All is on screen, which it is only while
    /// idle with an axis not homed.
    void home_all();

    void on_activate() override;
    void on_deactivate() override;

    /// A pointer event on the plate at @p screen_point. Starts nothing unless the plate is
    /// interactive and the press lands on the drawn plate (not the Z bar or beside it).
    void handle_plate_touch(BedTouch phase, lv_point_t screen_point);
    /// The plate's CLICKED: a tap the gesture took stops here, any other reaches the tile,
    /// which opens the Motion panel.
    void plate_clicked(lv_event_t* e);
    /// Send the tapped target and clear it. Does nothing without one.
    void move_here();
    /// Clear the tapped target without moving.
    void clear_ghost();

    /// Homed, idle, the motion gate open and a plate on screen.
    [[nodiscard]] bool plate_interactive() const;
    /// The tapped target awaiting Move here.
    [[nodiscard]] const std::optional<AxisTarget>& ghost_target() const {
        return ghost_;
    }

    /// The plate the canvas would draw now, or nullopt while it is not on screen.
    [[nodiscard]] std::optional<ToolheadPlateGeometry> plate_geometry() const;

    /// What the plate's draw hook will draw from, or nullopt before the canvas exists.
    [[nodiscard]] std::optional<ToolheadPlateInput> drawn_plate_input() const;

    /// How long a tapped target waits for Move here.
    static constexpr uint32_t kGhostTimeoutMs = 5000;

  private:
    struct LiveState;
    LiveState read_live() const;
    ToolheadVerdict decide(int width_px, int height_px, bool printing, bool homed) const;
    void publish_state(const LiveState& live);
    void publish_values(const LiveState& live);
    void on_state_changed();
    void schedule_refresh();
    void refresh();
    void resolve_bed();
    bool uses_positions() const;
    bool plate_visible() const;
    ToolheadPlateInput plate_input(const LiveState& live, int box_w, int box_h) const;
    void open_motion_panel();
    void refresh_outlines();
    void publish_interaction();
    void publish_target_text();
    void publish_presets();
    void publish_lift_text();
    void save_slots(const ToolheadPresetSlots& slots);
    void step_z(double delta_mm);
    void park();
    void set_ghost(const AxisTarget& target);
    void send_plate_target(const AxisTarget& target);
    void invalidate_plate();
    [[nodiscard]] double current_z_mm() const;
    /// The plate target under a screen point, clamped to the plate; nullopt without a plate.
    [[nodiscard]] std::optional<AxisTarget> target_at(int screen_x, int screen_y) const;
    /// Whether a screen point lies on the drawn plate.
    [[nodiscard]] bool on_plate(int screen_x, int screen_y) const;

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* plate_ = nullptr;

    ToolheadVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the
    /// content being measured changed.
    bool decided_printing_ = false;
    bool decided_homed_ = false;
    bool moving_ = false;
    /// The owning panel is offscreen: nothing redraws until it returns.
    bool panel_hidden_ = false;
    BedDimensions bed_{};
    bool circular_ = false;
    /// The plate area in G-code space that bed_ was built from; nullopt with no plate to
    /// move over, which leaves the plate drawn but inert.
    std::optional<AxisBounds> plate_area_;
    /// The press in progress was taken by the gesture, so its CLICKED stops at the plate.
    bool press_taken_ = false;
    std::optional<AxisTarget> ghost_;
    ToolheadPresetSlots slots_ = default_toolhead_preset_slots();
    nlohmann::json config_ = nlohmann::json::object();
    std::unique_ptr<ToolheadPresetConfigModal> config_modal_;

    ObserverGuard homed_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard position_observers_[6];
    ObserverGuard velocity_observer_;
    ObserverGuard bed_moves_observer_;
    ObserverGuard build_volume_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;
    ObserverGuard nav_enabled_observer_;
    ObserverGuard motion_blocked_observer_;
    ObserverGuard occupancy_observer_;
    ObserverGuard language_observer_;

    /// Every position and homing observer funnels here, so the readouts and the canvas
    /// refresh at about 4 Hz however fast the printer reports.
    ui::CoalescedTimer refresh_timer_{250};
    /// Clears a tapped target nobody confirmed.
    ui::CoalescedTimer ghost_timer_{kGhostTimeoutMs};

    ToolheadMoveSender move_sender_;
    BedPlateGesture gesture_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_toolhead_widget();

} // namespace helix
