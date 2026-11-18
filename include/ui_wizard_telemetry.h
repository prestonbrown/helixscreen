// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "wizard_step.h"

/**
 * @class WizardTelemetryStep
 * @brief Updates & Data step for preset wizard mode
 *
 * The update channel and the anonymous statistics opt-in, shown only in preset
 * mode where the summary step is skipped. Both rows are Settings rows, with
 * their callbacks.
 */
class WizardTelemetryStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Telemetry;
    }
    const char* component_name() const override {
        return "wizard_telemetry";
    }
    const char* log_name() const override {
        return "WizardTelemetry";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return !ctx.preset.first_run;
    }

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;
    bool is_validated() const override {
        return true;
    }
    const char* get_name() const {
        return "WizardTelemetry";
    }
    bool should_skip() const;

    /**
     * @brief Register the privacy text as the `telemetry_info_text` subject
     *
     * The one owner of that subject: the summary step's info modal binds to it
     * too, so both steps call this before creating their screens.
     */
    void init_info_text_subject();
    lv_subject_t* info_text_subject() {
        return &telemetry_info_text_;
    }

  private:
    lv_obj_t* root_ = nullptr;

    // Telemetry info modal text subject
    helix::Subject telemetry_info_text_{};
    char telemetry_info_text_buffer_[2048];
    bool subjects_initialized_ = false;
};

WizardTelemetryStep* get_wizard_telemetry_step();
