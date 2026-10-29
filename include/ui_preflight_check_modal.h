// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "preflight_validator.h"
#include "subject_managed_panel.h"

#include <functional>
#include <vector>

namespace helix::ui {

/**
 * @file ui_preflight_check_modal.h
 * @brief Enriched pre-flight filament check modal
 *
 * Replaces the simple confirmation dialog the print-start gate used to show.
 * For each required tool it renders a row with:
 *   - "Tx" label
 *   - the slicer-intended color swatch
 *   - an arrow
 *   - the actually-seated slot color swatch (or an "EMPTY" label)
 *   - a severity glyph (✓ / ⚠ / ✗)
 * plus an explanation line and three actions:
 *   - Map Filament… (tertiary; only when the active backend supports remap)
 *   - Cancel        (secondary)
 *   - Print Anyway  (primary)
 *
 * Seated color/material is looked up from
 * AmsState::collect_available_slots() by ToolCheck::mapped_slot — the
 * ToolCheck itself only carries the intended color/material.
 *
 * Mirrors SpaghettiDetectionModal: buttons wired programmatically in
 * on_show() via wire_*_button(). One-shot: shown through Modal::show_owned(),
 * which hands the instance to ModalStack to free when its entry goes (#1382).
 */
class PreflightCheckModal : public Modal {
  public:
    using Action = std::function<void()>;

    PreflightCheckModal();
    ~PreflightCheckModal() override;

    const char* get_name() const override {
        return "Preflight Check";
    }
    const char* component_name() const override {
        return "preflight_check_modal";
    }

    /// Copy in the check data to render.
    void set_checks(const helix::PreflightResult& result) {
        result_ = result;
    }

    /// "Print Anyway" callback.
    void set_on_force(Action a) {
        on_force_ = std::move(a);
    }
    /// "Map Filament…" callback (only invoked when the Map Filament button is shown).
    void set_on_remap(Action a) {
        on_remap_ = std::move(a);
    }
    /// Runs once the modal closes, however it closed, after any Print Anyway
    /// or Remap callback.
    void set_on_closed(Action a) {
        on_closed_ = std::move(a);
    }

  protected:
    void on_show() override;
    void on_ok() override { // Print Anyway
        if (on_force_)
            on_force_();
        hide();
    }
    void on_cancel() override { // Cancel
        hide();
    }
    void on_tertiary() override { // Map Filament…
        if (on_remap_)
            on_remap_();
        hide();
    }
    void on_hide() override {
        if (auto closed = std::move(on_closed_)) {
            on_closed_ = nullptr;
            closed();
        }
    }

  private:
    /// Write the file's material to the slot material_fix() names, then close:
    /// the next Print checks again against what the slot now holds.
    void apply_material_fix();
    static void on_set_material_cb(lv_event_t* e);

    void build_rows(lv_obj_t* list);
    lv_obj_t* create_tool_row(lv_obj_t* list, const helix::ToolCheck& check,
                              const std::vector<helix::AvailableSlot>& slots);

    helix::PreflightResult result_;
    Action on_force_, on_remap_, on_closed_;

    SubjectManager subjects_;
    lv_subject_t fix_shown_subject_{};
    lv_subject_t fix_text_subject_{};
    char fix_text_buf_[96] = {0};
    /// The shown modal the material-fix button acts on; one at a time.
    static PreflightCheckModal* active_instance_;
};

} // namespace helix::ui
