// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "subject_managed_panel.h"
#include "target_nozzle_observers.h"
#include "target_tool_selection.h"

#include <string>

namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief The one target tool every temperature and filament surface aims at.
 *
 * The Controls temperature card, the Filament panel and the temperature graph
 * all show and heat this tool, so a pick on any of them is a pick on all of
 * them. It follows TargetToolSelection's rule: a tool change anywhere moves it
 * to the mounted tool, an empty carriage keeps the pick, and it never changes
 * tools by itself.
 *
 * It publishes what the surfaces bind:
 *  - `target_tool` (tool index) and `target_tool_multi_head` (the printer
 *    has several heads);
 *  - `target_tool_name` ("Tool 2" or a configured name), `target_tool_number`
 *    ("2", empty with one head) and `target_nozzle_label` ("Nozzle 2"), all
 *    in the display numbering, so a chip, a segment and a row agree;
 *  - `target_nozzle_temp` / `target_nozzle_target` (decidegrees) and
 *    `target_nozzle_status_state` / `target_nozzle_status` (classified as
 *    every heater row is) for the target's own nozzle.
 *
 * Main thread only.
 */
class TargetTool {
  public:
    static TargetTool& instance();

    /// Register the subjects; a no-op once they exist.
    void init_subjects();
    void deinit_subjects();

    /// Read @p printer_state's temperatures and follow ToolState. The app
    /// attaches its PrinterState at startup; a surface attaches the one it
    /// reads, which also resubscribes after ToolState rebuilds its subjects.
    /// Keeps the pick.
    void attach(PrinterState& printer_state);

    /// The tool index, clamped to the tool list: the mounted tool until a
    /// pick or a tool change says otherwise, and 0 past a shrunk list.
    [[nodiscard]] int tool() const;
    /// The physical head (ToolState::physical_heads()) the tool prints through.
    [[nodiscard]] int head() const;
    /// The target's extruder ("extruder" when it names none).
    [[nodiscard]] std::string extruder_name() const;
    /// Whether the target is the tool on the carriage, by tool index: after a
    /// park Klipper still names the last tool's extruder active. With one
    /// head the target is always mounted.
    [[nodiscard]] bool mounted() const;

    /// Aim at @p tool. Never changes tools.
    void pick(int tool);
    /// Aim at head @p head through the tool that stands for it.
    void pick_head(int head);
    /// Aim at the tool whose extruder is @p extruder; unknown names are ignored.
    void pick_extruder(const std::string& extruder);

    /// Bumped on every change of the target, for C++ observers.
    lv_subject_t* subject() {
        return &tool_subject_;
    }
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /// Rebind the nozzle mirrors (the target's extruder subjects can be
    /// rebuilt) and republish everything.
    void refresh();

  private:
    TargetTool() = default;

    void publish();
    void publish_status();
    void rebind_power();
    /// Whether the attached PrinterState's temperature subjects still live.
    [[nodiscard]] bool printer_alive() const;

    TargetToolSelection selection_;
    static constexpr int kNoActiveSeen = -2;
    /// The mounted tool last seen, so a subscription's replay is not a change.
    int last_active_ = kNoActiveSeen;
    PrinterState* printer_state_ = nullptr;
    SubjectLifetime printer_lifetime_;
    bool subjects_initialized_ = false;

    SubjectManager subjects_;
    lv_subject_t tool_subject_{};
    lv_subject_t multi_head_subject_{};
    lv_subject_t name_subject_{};
    char name_buf_[64] = {};
    lv_subject_t number_subject_{};
    char number_buf_[48] = {};
    lv_subject_t label_subject_{};
    char label_buf_[64] = {};
    lv_subject_t temp_subject_{};
    lv_subject_t target_subject_{};
    lv_subject_t status_state_subject_{};
    lv_subject_t status_subject_{};
    char status_buf_[16] = {};

    int power_ = -1;
    TargetNozzleObservers nozzle_observers_;
    ObserverGuard power_observer_;
    ObserverGuard active_tool_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard extruder_version_observer_;
    ObserverGuard language_observer_;
};

} // namespace helix::ui
