// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "action_prompt_manager.h" // PromptData / PromptButton
#include "action_prompt_modal.h"   // helix::ui::ActionPromptModal
#include "error_event.h"
#include "lvgl.h"

#include <memory>
#include <string>
#include <vector>

class IMoonrakerAPI;

// Forward declaration so the presenter can grant white-box test access.
struct RecoveryModalPresenterTestAccess;

namespace helix::ui {

/// Source-agnostic owner of the recovery modal shown for CRITICAL errors that
/// carry recovery actions (e.g. AFC jam, CFS key840). Lives as a member of
/// Application and is passed by reference to GcodeErrorRouter so the router
/// can delegate the modal presentation without owning any LVGL state.
///
/// Decoupling: the modal is created and shown regardless of whether api_ is
/// set. The api_ pointer is only used in the gcode-execution callback that
/// fires when the user taps a recovery button. This makes the presenter fully
/// testable with api_==nullptr.
///
/// Cold-nozzle gate: an action flagged RecoveryAction::needs_hot_nozzle is not
/// sent while the hotend is below Klipper's min_extrude_temp. The presenter
/// commands a preheat and polls until the nozzle arrives, then sends — bounded,
/// so a hotend that never heats gives up instead of waiting forever.
///
/// Lifetime: must outlive any GcodeErrorRouter that holds a reference to it.
/// The presenter outlives individual modals, and the preheat poll timer outlives
/// the modal that started it (the modal closes on the tap), so the destructor
/// cancels the timer.
class RecoveryModalPresenter {
  public:
    explicit RecoveryModalPresenter(IMoonrakerAPI* api);
    ~RecoveryModalPresenter();

    RecoveryModalPresenter(const RecoveryModalPresenter&) = delete;
    RecoveryModalPresenter& operator=(const RecoveryModalPresenter&) = delete;

    /// Show the recovery modal for this event, or replace content if already
    /// visible. Deduplicates identical e.detail while the modal is on screen,
    /// and suppresses a fault the user has already answered (see
    /// handled_detail_). Falls back to ui_notification_error when no screen is
    /// available.
    void present(const helix::ErrorEvent& e);

    /// Hide the modal if visible and clear shown-detail state so a subsequent
    /// present() with the same detail is not suppressed. Also ends the current
    /// fault episode: whatever the user had already answered stops being
    /// suppressed, because the next present() belongs to a new episode.
    ///
    /// Deliberately does NOT abort a preheat already running for a tapped
    /// action. dismiss() fires on the AMS action leaving ERROR, which is also
    /// what a preheat's own heater command can provoke; cancelling there would
    /// discard the recovery the user explicitly asked for. Only the bounded
    /// wait, a later tap, or destruction ends a preheat.
    void dismiss();

    /// Forget which fault the user has already answered, without touching the
    /// dialog. The suppression in present() is scoped to one fault episode, and
    /// this is the opening boundary: AmsErrorBridge calls it on the rising edge
    /// into AmsAction::ERROR, so a fault identical to one dismissed during an
    /// EARLIER episode is presented again rather than silently swallowed.
    /// dismiss() is the closing boundary and clears the same state.
    void forget_handled_fault();

    [[nodiscard]] bool is_visible() const;

  private:
    friend struct ::RecoveryModalPresenterTestAccess;

    /// Body of the modal's gcode callback: runs on the main thread when the user
    /// taps a recovery button.
    void on_recovery_tapped(const std::string& gcode);

    /// Send @p gcode to the printer with the recovery error/toast handling.
    void dispatch_recovery(const std::string& gcode, const std::string& tag);

    /// True when the hotend may extrude right now.
    [[nodiscard]] bool nozzle_ready_for_extrusion() const;

    /// Temperature to preheat to before a deferred recovery.
    [[nodiscard]] int resolve_preheat_target() const;

    /// Command the heater and start polling for arrival.
    void begin_preheat(const std::string& gcode, const std::string& tag, const std::string& label);

    /// One poll tick: dispatch on arrival, give up once the budget is spent.
    void poll_preheat();

    /// Cancel the poll timer. Safe from the destructor and from inside the
    /// timer's own callback.
    void cancel_preheat_timer();

    /// Cancel the timer and forget the deferred action.
    void clear_preheat();

    /// Called by the modal itself whenever it hides for a reason we did not
    /// initiate — the dismiss button, a backdrop tap, ESC. Records the fault on
    /// screen as answered.
    void on_modal_hidden();

    /// Remember the fault currently on screen as one the user has answered, so
    /// present() will not put it back up unchanged. No-op when nothing is shown.
    void mark_handled();

    static void preheat_timer_cb(lv_timer_t* timer);

    IMoonrakerAPI* api_;
    std::unique_ptr<helix::ui::ActionPromptModal> modal_;
    std::string shown_detail_;
    std::vector<helix::RecoveryAction> active_actions_;

    // === The user's answer to the fault currently being shown ===
    //
    // A dismiss-only event carries one {"OK", ""} action; ActionPromptModal
    // treats an empty gcode as "close and send nothing", so the tap never
    // reaches our gcode callback. Nothing else in this class can see that the
    // user said "I've read it", which is why the modal reports its own hides
    // here rather than the dedup below inferring it from is_visible():
    // visibility is false for a hide we performed, a hide the modal stack
    // performed, and a hide the user performed, and only the last of those means
    // the fault must stay down.
    std::string handled_detail_;
    std::vector<helix::RecoveryAction> handled_actions_;
    /// True while this class is the one hiding the modal (dismiss(), or the
    /// implicit hide Modal::show() does when replacing visible content). Keeps
    /// those out of on_modal_hidden().
    bool suppress_hide_notice_ = false;

    // Deferred (preheating) recovery. preheat_timer_ != nullptr is the "a tap is
    // waiting on the nozzle" state.
    lv_timer_t* preheat_timer_ = nullptr;
    std::string pending_gcode_;
    std::string pending_tag_;
    std::string pending_label_;
    int pending_target_c_ = 0;
    /// Polls left before the wait is abandoned. Counting ticks rather than wall
    /// clock keeps the bound honest when the main loop stalls.
    int32_t polls_remaining_ = 0;
    uint32_t preheat_budget_ms_;
};

} // namespace helix::ui
