// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_observer_guard.h"

#include "filament_macro_profiles.h"
#include "guided_load_flow.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <string>

namespace helix::ui {

/// Walks the user through a stock load wizard once its first macro has run:
/// push the filament in, extrude until new filament shows, then finish.
/// GuidedLoadFlow decides every step; this class shows it and sends it.
class GuidedLoadModal : public Modal {
  public:
    GuidedLoadModal(filament_macros::GuidedLoad steps, bool waits_for_push);
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

    /// Whether the wizard may open with the print in @p lifecycle.
    [[nodiscard]] static bool opens_during(PrintState lifecycle);

    /// The stage the XML binds, as GuidedLoadModal publishes it.
    enum class View : int { Push = 0, Working = 1, Confirm = 2 };
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

    /// Send @p script and feed its outcome back to the flow.
    void send(const std::string& script, bool report_outcome);
    void on_command_finished(bool ok);
    void publish();

    GuidedLoadFlow flow_;
    SubjectManager subjects_;
    lv_subject_t view_subject_{};
    bool subjects_initialized_ = false;
    ObserverGuard runout_observer_;
    int last_runout_ = -1;
};

} // namespace helix::ui
