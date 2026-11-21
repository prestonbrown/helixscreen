// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "runout_other_head.h"

#include <functional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Pick another loaded toolhead to finish a runout-paused print on (#2137).
 *
 * Lists every other head with why it can or cannot take over
 * (helix::runout_other_head::assess()), warns on a different colour, and on a
 * confirmed tap sends the filament system's plan (runout_other_head::build_plan()).
 * Offered only where the active filament system has a plan row.
 */
class RunoutOtherHeadModal : public Modal {
  public:
    /// What a sent switch reports to whoever offered it. Each runs on the main
    /// thread at most once per switch; the caller guards its own lifetime.
    struct SwitchHooks {
        /// The switch to @p target_name was sent. The printer is busy with it
        /// until one of the other two runs.
        std::function<void(const std::string& target_name)> on_sent;
        /// The routing moved the print off the runout head, resumed or not.
        std::function<void()> on_moved;
        /// The switch ended without moving the print, or its watch ended
        /// without an outcome.
        std::function<void()> on_not_moved;
    };

    explicit RunoutOtherHeadModal(int runout_head, SwitchHooks hooks = {});
    ~RunoutOtherHeadModal() override;

    const char* get_name() const override {
        return "Runout Other Head";
    }
    const char* component_name() const override {
        return "runout_other_head_modal";
    }

    /// Whether the active filament system can continue a runout on @p runout_head
    /// elsewhere: it has a plan row and reports the facts. Main thread only.
    [[nodiscard]] static bool available(int runout_head);

    /// Show the chooser for a runout on @p runout_head, owned by ModalStack.
    /// False when available() is not. Main thread only.
    static bool show_owned(int runout_head, SwitchHooks hooks = {});

    /// Apply or forget a restore a switch left pending (a setting it turned on
    /// for itself and could not put back: a failed script, a restart, a lost
    /// link). Call whenever the printer's state may have moved; also re-runs
    /// itself when the firmware's runout switching changes. Main thread only.
    static void settle_pending_restore();

    /// Register the subjects and the row callback the XML binds. Idempotent.
    static void init_subjects();

  protected:
    void on_show() override;
    void on_ok() override {
        hide();
    }

  private:
    void publish();
    void pick(int row);
    void start(int head);

    /// Rows take no tap while a pick's confirmation is up.
    static void set_confirming(bool confirming);

    int runout_head_;
    SwitchHooks hooks_;
    std::vector<helix::runout_other_head::HeadVerdict> verdicts_;
    static RunoutOtherHeadModal* shown_;
};

} // namespace helix::ui
