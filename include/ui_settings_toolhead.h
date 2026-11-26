// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_toolhead.h
 * @brief Toolhead settings overlay: one card per physical head, with its nozzle size
 *        and, where the printer has flow variants, its flow type.
 *
 * What a head offers comes from helix::nozzle_size (capability(), plan_change()); this
 * overlay lays the heads out, asks for the confirmation a plan calls for, and executes
 * the plan: a G-code command, the filament backend's own nozzle call, or a config edit
 * with a Klipper restart. A head is a physical extruder, labeled through
 * head_identity() and the head_*_label() family.
 *
 * @pattern Overlay (lazy init, dynamic card creation)
 * @threading Main thread only
 */

#pragma once

#include "ui_widget_ref.h"

#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "klipper_config_editor.h"
#include "nozzle_size.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <string>

namespace helix::settings {

class ToolheadSettingsOverlay : public OverlayBase {
  public:
    void register_callbacks() override;
    void init_subjects() override;

    const char* get_name() const override {
        return "Toolhead";
    }
    const char* xml_component() const override {
        return "toolhead_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    /// Rewrites the Printing settings row's description ("0.4 mm nozzle", or each
    /// head's size). Needs the subjects, not the screen.
    void refresh_summary();

    /// The tap on a head's nozzle size or flow dropdown: @p option indexes the
    /// choices the row listed.
    void on_size_picked(int extruder_index, int option);
    void on_flow_picked(int extruder_index, int option);

  private:
    void populate();

    /// Executes @p plan, after the confirmation it calls for.
    /// What a pick asks for: a size in mm, or a flow variant, on one head.
    struct Request {
        int extruder_index = 0;
        bool flow = false;
        double mm = 0.0;
        std::string variant;
        /// The kind of plan the user was asked about; unset until asked.
        nozzle_size::ChangePlan::Kind kind = nozzle_size::ChangePlan::Kind::Refused;
    };
    /// The plan for @p request from the printer's state now.
    [[nodiscard]] static nozzle_size::ChangePlan plan_for(const Request& request);
    /// Plans @p request, asks where the plan calls for it, then plans it again from the
    /// state at the answer (a print may have started meanwhile) and executes that.
    void apply(const Request& request, const std::string& head_label);
    /// Plans @p request afresh and executes it, or reports why it cannot be.
    void confirmed(const Request& request);
    void run(const nozzle_size::ChangePlan& plan);
    void finish(const std::string& error);

    SubjectManager subjects_; // declared ahead: tears down after the subjects it owns
    char summary_buf_[160] = {};
    helix::Subject summary_{};
    helix::Subject head_count_{};
    helix::system::KlipperConfigEditor config_editor_;

    void on_ui_destroyed() override {
        list_.reset();
    }
    helix::ui::WidgetRef list_;
};

inline ToolheadSettingsOverlay& get_toolhead_settings_overlay() {
    return lazy_global<ToolheadSettingsOverlay>("ToolheadSettingsOverlay");
}

} // namespace helix::settings
