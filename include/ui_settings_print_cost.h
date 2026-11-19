// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_print_cost.h
 * @brief Settings > Printing > Print Cost: the currency, the price per kg per
 *        material and the optional electricity a print is costed at
 *        (prestonbrown/helixscreen#2132).
 */

#pragma once

#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "overlay_base.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>

namespace helix::settings {

class PrintCostOverlay : public OverlayBase {
  public:
    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Print Cost";
    }
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    /// Open the keypad for the field @p key names: a material, "default", "watts" or "kwh".
    void edit_field(const std::string& key);
    void handle_currency_changed(lv_event_t* e);

  protected:
    void before_show() override;

  private:
    /// The materials with their own price row, plus "default" first.
    static constexpr size_t kPriceRows = std::size(SettingsManager::kCostMaterials) + 1;

    void refresh_values();

    SubjectManager subjects_;
    helix::Subject currency_subject_{};
    std::array<helix::Subject, kPriceRows> price_subjects_{};
    char price_bufs_[kPriceRows][32]{};
    helix::Subject watts_subject_{};
    char watts_buf_[32]{};
    helix::Subject kwh_subject_{};
    char kwh_buf_[32]{};
    std::string editing_title_; ///< The open keypad's title
    /// The saved currency when the dropdown offers no such option; the dropdown
    /// shows this text, and keeps the pointer, until the user picks one.
    std::string unlisted_currency_;
};

inline PrintCostOverlay& get_print_cost_overlay() {
    return lazy_global<PrintCostOverlay>("PrintCostOverlay");
}

} // namespace helix::settings
