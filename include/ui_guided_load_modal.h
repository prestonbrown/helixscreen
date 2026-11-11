// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "core/observer_guard.h"
#include "filament_macro_profiles.h"
#include "guided_load_flow.h"
#include "guided_load_session.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <string>

namespace helix::ui {

/// Walks the user through a guided load: a stock load wizard once its first
/// macro has run, or a manual load that heats the nozzle first (#2000). Push the
/// filament in, extrude until new filament shows, then finish.
/// GuidedLoadSession runs it; this class shows it.
class GuidedLoadModal : public Modal {
  public:
    explicit GuidedLoadModal(std::unique_ptr<GuidedLoadSession> session);
    ~GuidedLoadModal() override;

    const char* get_name() const override {
        return "Guided Load";
    }
    const char* component_name() const override {
        return "guided_load_modal";
    }
    HELIX_KIND_TAG(GuidedLoadModal)

    /// Show on the active screen and hand the instance to ModalStack. Declines
    /// while a print job holds the machine: the wizard's buttons move the
    /// printer and its endings cool the nozzle and reset the printer's status.
    static bool show_owned(filament_macros::GuidedLoad steps, bool waits_for_push);

    /// Open a manual load for @p slot (negative: the active nozzle): heat it, then
    /// the hand push. A refusal (manual_load_refusal_now()) is toasted and opens
    /// nothing. True when it opened.
    static bool show_manual(int slot);

    /// Whether the wizard may open with the print in @p lifecycle.
    [[nodiscard]] static bool opens_during(PrintState lifecycle);

    /// The stage the XML binds, as GuidedLoadModal publishes it.
    enum class View : int { Push = 0, Working = 1, Confirm = 2, Heating = 3 };
    [[nodiscard]] static View view_for(GuidedLoadStage stage);

  protected:
    void on_show() override;
    void on_hide() override;
    void on_dismissed() override;
    /// ESC cancels like the Cancel button: it must not close with the nozzle hot.
    void on_escape() override;

  private:
    void init_subjects();
    void deinit_subjects();
    static void register_callbacks();

    void handle_extrude();
    void handle_done();
    void handle_cancel();
    void on_runout_value(int value);
    void publish();
    static bool open(std::unique_ptr<GuidedLoadSession> session);

    std::unique_ptr<GuidedLoadSession> session_;
    SubjectManager subjects_;
    lv_subject_t view_subject_{};
    lv_subject_t heat_text_subject_{};
    char heat_text_buf_[48] = {};
    bool subjects_initialized_ = false;
    ObserverGuard runout_observer_;
    int last_runout_ = -1;
};

} // namespace helix::ui
