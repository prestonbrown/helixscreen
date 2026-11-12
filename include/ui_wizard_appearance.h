// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "display_settings_manager.h" // helix::TimeFormat
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

#include <string>

namespace helix::wizard {

/// The 12/24-hour default a language implies: English reads a 12-hour clock,
/// every other offered language a 24-hour one.
TimeFormat default_time_format_for_language(const std::string& language);

/// Whether the timezone row should draw attention: the zone still follows the
/// system and the system has none, so every clock reads UTC by accident.
bool timezone_needs_attention(bool follows_system, bool system_zone_known);

/// Wizard page for how HelixScreen looks and reads: timezone, clock format,
/// theme and units. Each row is bound to the same subject and callback as its
/// settings-overlay twin, so there is no second copy of the state.
class WizardAppearanceStep : public Step {
  public:
    StepId id() const override {
        return StepId::Appearance;
    }
    const char* component_name() const override {
        return "wizard_appearance";
    }
    const char* log_name() const override {
        return "WizardAppearance";
    }

    WizardAppearanceStep() = default;
    WizardAppearanceStep(const WizardAppearanceStep&) = delete;
    WizardAppearanceStep& operator=(const WizardAppearanceStep&) = delete;

    /// Every row is a global setting, so a printer added later skips the page.
    bool should_skip(const StepContext& ctx) const override {
        return ctx.is_subsequent_printer;
    }

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;

    /// Save the theme at @p index and redraw every screen in it.
    static void apply_theme_by_index(int index);

    /// Save the region defaults for the current language and timezone into each
    /// unit row that still follows the region: one nobody had saved when the page
    /// opened and nobody has picked on it since.
    void apply_region_defaults();

    /// A pick on a row: it stops following the region.
    void on_temp_unit_picked(int index);
    void on_date_format_picked(int index);

  private:
    lv_obj_t* root_ = nullptr;
    SubjectManager subjects_;
    lv_subject_t tz_attention_{};
    bool temp_unit_follows_region_ = false;
    bool date_format_follows_region_ = false;
};

WizardAppearanceStep* get_wizard_appearance_step();

} // namespace helix::wizard
