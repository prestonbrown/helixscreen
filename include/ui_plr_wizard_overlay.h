// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "core/static_panel_registry.h"
#include "display_awake_hold.h"
#include "jog_coalescer.h"
#include "lvgl/lvgl.h"
#include "moonraker_error.h"
#include "overlay_base.h"
#include "plr_wizard.h"
#include "subject_managed_panel.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::ui {

class PlrWizardOverlayTestAccess;

/// What the plugin is doing, for a helix:plr_progress step name; nullptr for a
/// step this app has no words for, which leaves the previous line up.
const char* plr_progress_step_text(const std::string& step);

/// True from a successful PlrWizardOverlay::open() until the wizard is dismissed.
bool plr_wizard_open();

/**
 * @brief Guided resume after a power loss, for the HelixPrint plugin's `plr`
 * feature (PlrBackendType::GENERIC).
 *
 * Steps per plr_next_wizard_step(): fetch the plan, ask the confirmations a
 * resume relies on, let the plugin heat the bed and the nozzle where it stopped,
 * then on the generic backend lift and home X/Y (prepare),
 * settle Z (tier 2 confirms the held height, tier 1 jogs down to the part),
 * then let the plugin heat the nozzle, move back and start (commit). The plugin renders and
 * sends every motion of the sequence; the one move this overlay sends is the
 * tier-1 Z jog, through the same coalesced relative move the motion panel uses.
 *
 * @pattern Overlay (lazy init, singleton via get_plr_wizard_overlay())
 * @threading Main thread; RPC replies and notifications return through
 *            lifetime_.bg_cb, and a reply from an earlier opening is dropped.
 */
class PlrWizardOverlay : public OverlayBase {
    friend class PlrWizardOverlayTestAccess;

  public:
    ~PlrWizardOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Power-Loss Resume";
    }
    const char* xml_component() const override {
        return "plr_wizard_overlay";
    }
    /// Freed on close; each resume starts from a fresh tree.
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void on_deactivating(DeactivateReason reason) override;
    void on_dismissed() override;

    /// Show the wizard and ask the plugin for its plan. @p api must outlive the
    /// overlay; the app-global IMoonrakerAPI does.
    void open(IMoonrakerAPI* api);

  private:
    struct Event {
        bool rpc_ok = false;
        bool rpc_failed = false;
        bool user_abort = false;
    };

    /// The one place the step moves: plr_next_wizard_step over the current
    /// plan, answers and link state, then the entry action of the new step.
    void advance(const Event& event);
    void enter_step(PlrWizardStep step);
    void set_step(PlrWizardStep step);
    PlrWizardStep step() const {
        return step_;
    }

    void request_plan();
    void on_status(const PlrPluginStatus& status);
    void on_plan(const PlrPlanReply& plan);
    void on_rpc_error(const MoonrakerError& err);
    void on_progress(const PlrProgressEvent& event);
    void on_plan_ready(const PlrPlanReady& ready);
    void on_connection_changed(int state);

    void handle_continue();
    void handle_resume();
    void handle_abort();
    void handle_jog(int direction);
    void handle_step_select(int index);
    void send_jog(const AxisMove& move);

    void publish_plan();
    void update_bed_live();
    void update_z_display(int centimm);
    std::vector<PlrConfirmation> given() const;

    void listen_for_notifications();
    void stop_listening();

    IMoonrakerAPI* api_ = nullptr;
    PlrWizardStep step_ = PlrWizardStep::Offer;
    PlrPluginStatus status_{};
    std::optional<PlrPlanView> plan_;
    std::optional<PlrPlanReply> plan_reply_;
    /// The Z confirmation the user gave on the Z step.
    std::optional<PlrConfirmation> z_given_;
    bool connected_ = true;
    std::string error_text_;

    /// Incremented per opening: a reply or event carries the session it was
    /// asked in, and one from an earlier opening is ignored.
    int session_ = 0;
    bool listening_ = false;
    /// helix:plr_plan_ready arrived this opening.
    bool plan_ready_seen_ = false;
    // Once the plugin reports the print starting, closing the wizard must not
    // abort: the abort's heaters-off would land in the running print.
    bool print_starting_ = false;

    JogCoalescer jog_;
    /// Held while the wizard is open: the machine waits mid-recovery on a
    /// step the user has to see.
    DisplayAwakeToken awake_;

    ObserverGuard conn_observer_;
    ObserverGuard z_observer_;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
    AsyncLifetimeGuard lifetime_;
    SubjectManager subjects_;
};

inline PlrWizardOverlay& get_plr_wizard_overlay() {
    return lazy_global<PlrWizardOverlay>("PlrWizardOverlay");
}

} // namespace helix::ui
