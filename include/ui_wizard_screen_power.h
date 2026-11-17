// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

namespace helix::wizard {

/// Wizard page for the screen itself: brightness, the screensaver, when the screen
/// dims or starts the saver, when it sleeps, and the UI size. Every row is the
/// Settings > Display row, with its callback and option list, so there is no
/// second copy of the state. Rows the platform cannot honour hide themselves
/// through the subjects Settings uses; the UI size row shows only where more than
/// one size fits the screen.
class WizardScreenPowerStep : public Step {
  public:
    StepId id() const override {
        return StepId::ScreenPower;
    }
    const char* component_name() const override {
        return "wizard_screen_power";
    }
    const char* log_name() const override {
        return "WizardScreenPower";
    }

    WizardScreenPowerStep() = default;
    WizardScreenPowerStep(const WizardScreenPowerStep&) = delete;
    WizardScreenPowerStep& operator=(const WizardScreenPowerStep&) = delete;

    /// Every row is a global setting, and Display Sleep applies on every platform,
    /// so the page is skipped only for a printer added after the first run.
    bool should_skip(const StepContext& ctx) const override {
        return ctx.is_subsequent_printer;
    }

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;

  private:
    lv_obj_t* root_ = nullptr;
    SubjectManager subjects_;
    helix::Subject show_ui_scale_{};
};

WizardScreenPowerStep* get_wizard_screen_power_step();

} // namespace helix::wizard
