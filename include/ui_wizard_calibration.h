// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/subject.h"
#include "wizard_step.h"

/// The setup wizard's last page: start the guided calibration run, or skip it.
/// Setup is marked complete before the run opens, so a run that fails or is
/// stopped never sends the owner back through setup.
class WizardCalibrationStep : public helix::wizard::Step {
  public:
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Calibration;
    }
    const char* component_name() const override {
        return "wizard_calibration";
    }
    const char* log_name() const override {
        return "WizardCalibration";
    }
    /// Nothing to calibrate without a printer to talk to.
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.api == nullptr;
    }
    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;

  private:
    lv_obj_t* root_ = nullptr;
    helix::Subject steps_text_{};
    char steps_text_buf_[256]{};
    helix::Subject time_text_{};
    char time_text_buf_[128]{};
};

WizardCalibrationStep* get_wizard_calibration_step();
