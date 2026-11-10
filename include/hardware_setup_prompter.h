// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_guard.h"
#include "i_moonraker_api.h"
#include "lvgl/lvgl.h"
#include "wizard_step.h" // helix::wizard::StepId

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix {

class AsyncLifetimeGuard;
class PrinterDiscovery;

/// Notification-history action id that reopens the printer type mismatch review.
inline constexpr const char* kReviewPrinterTypeAction = "review_printer_type";
inline constexpr const char* kMismatchToastClosedAction = "type_mismatch_closed";

/// Which hardware prompt a discovery pass launches. At most one per pass: the gates
/// below make the three reasons mutually exclusive.
enum class HardwarePrompt {
    None,
    Reconfig,       ///< targeted wizard for guided roles with no live match
    DeferredOffer,  ///< offer the hardware steps a Klipper-down setup skipped
    DeferredSilent, ///< that debt, but nothing is left to ask: settle it quietly
    TypeMismatch,   ///< saved printer type contradicts detected hardware
};

/// Everything choose_prompt() reads. All of it is known by the time the discovery
/// pass reaches its prompts.
struct HardwarePromptGates {
    bool hw_changed = false;              ///< hardware shape differs from the last discovery
    bool print_active = false;            ///< a print is running, so no wizard may open over it
    bool wizard_blocked = false;          ///< the first-run wizard is required or running
    bool reconfig_steps_pending = false;  ///< guided roles left unresolved
    bool hardware_setup_deferred = false; ///< a Klipper-down setup left steps behind
    bool reconfig_shown = false;          ///< reconfig already launched this connection
    bool deferred_prompt_shown = false;   ///< deferred offer already made this session
    bool type_mismatch_shown = false;     ///< mismatch already prompted this session
};

/// Pure decision for the discovery-time prompt. @p deferred_steps_empty is consulted
/// only when a deferred offer is otherwise due: computing the steps has side effects
/// on filament-sensor discovery, so the answer must not be fetched when nothing could
/// use it.
HardwarePrompt choose_prompt(const HardwarePromptGates& gates,
                             const std::function<bool()>& deferred_steps_empty);

/// Owns the hardware-setup prompts a discovery pass can raise (reconfig wizard, the
/// deferred-setup offer, the type-mismatch warning), the guards that keep each to
/// once per session or connection, and the wizard sessions they launch. Main thread.
class HardwareSetupPrompter {
  public:
    /// @p screen and @p api are read at use, since both change across a printer switch.
    HardwareSetupPrompter(AsyncLifetimeGuard& lifetime, std::function<lv_obj_t*()> screen,
                          std::function<IMoonrakerAPI*()> api)
        : lifetime_(lifetime), screen_(std::move(screen)), api_(std::move(api)) {}
    ~HardwareSetupPrompter();

    HardwareSetupPrompter(const HardwareSetupPrompter&) = delete;
    HardwareSetupPrompter& operator=(const HardwareSetupPrompter&) = delete;

    /// A discovery cycle is starting: a reconnect may offer the reconfig wizard again.
    void begin_discovery_cycle() {
        guards.reconfig_shown = false;
    }

    /// Once-per-session and once-per-connection guards. Public so a test can prove a
    /// printer switch re-arms them.
    struct Guards {
        /// The discovery-triggered targeted reconfig wizard launches at most once per
        /// connection; re-armed when a new discovery cycle begins.
        bool reconfig_shown = false;
        /// The deferred hardware-setup offer (#1160) is made at most once per session, so
        /// a reconnect cannot re-ask. The persisted per-printer marker stops it across
        /// sessions: cleared as soon as the user answers either way.
        bool deferred_prompt_shown = false;
        /// The saved-vs-detected type mismatch warning (toast + history entry) is raised
        /// at most once per session. The persisted answered pair (TYPE_MISMATCH_SHOWN_FOR
        /// and kTypeMismatchShownForDetected), written by an answer in the review modal or
        /// by closing the toast, covers cross-boot for that saved and detected pair. Not
        /// re-armed on reconnect, only on a new printer.
        bool type_mismatch_shown = false;
    };
    Guards guards;

    /// Re-arm every guard and drop pending wizard steps. A new printer's first
    /// discovery is a first discovery with its own prompts to show.
    void reset_for_new_connection();

    /// The prompt pass of a completed discovery. Launches at most one prompt.
    /// @param hw discovered hardware (already stored in the API)
    /// @return which prompt was launched
    HardwarePrompt run_discovery_prompts(const PrinterDiscovery& hw, bool hw_changed,
                                         bool print_active, bool hardware_setup_deferred);

    /// Re-resolve and persist fan AND heater roles, then rebind fan UI to the new
    /// mapping, after a targeted wizard session finishes. Marshals to the main thread
    /// internally; safe to call from a main-thread on_complete.
    void reapply_hardware_roles();

    /// Open the Choose Model / Keep current review for the mismatch warning numbered
    /// @p generation. Reached from that warning's toast button and history entry. Shows
    /// nothing, and says so at info, when that warning is no longer the pending one (a
    /// printer switch or a newer warning replaced it), or the saved type has since
    /// changed or been settled.
    void show_type_mismatch_review(uint32_t generation);

    /// Remote-control demo: raise the mismatch warning for these names without the
    /// detection that decides it. @p open_review opens the review at once, skipping
    /// the saved-type checks a demo's names would fail against the mock's config. A
    /// type change does not retire the demo's toast.
    void show_type_mismatch_demo(const std::string& detected, int confidence,
                                 const std::string& saved, bool open_review);

  private:
    friend class HardwareSetupPrompterTestAccess;

    /// What the review modal says, captured when the warning fires so a later tap from
    /// notification history still has it.
    struct TypeMismatchReview {
        std::string detected;
        int confidence = 0;
        std::string saved;
    };

    void launch_reconfig_wizard(std::vector<wizard::StepId> steps);
    void prompt_deferred_hardware_setup(std::vector<wizard::StepId> steps);
    void launch_deferred_hardware_setup();
    void settle_deferred_hardware_setup();
    void maybe_warn_type_mismatch(const PrinterDiscovery& hardware);
    /// @p retire_on_type_change watches the printer type and takes the toast down
    /// once the saved type is no longer the one warned about.
    void warn_type_mismatch(TypeMismatchReview review, bool retire_on_type_change = true);
    /// Build the review modal for the pending warning, unless one is already open.
    void open_type_mismatch_review();
    void launch_type_reidentify_wizard();
    void settle_type_mismatch_warning();
    /// A user-closed mismatch toast settles the warning: keep the saved type
    /// and stop asking, exactly like the review modal's "Keep current".
    void settle_type_mismatch_on_close(uint32_t generation);
    /// Whether warning @p generation is still the live, unanswered one for the
    /// saved type. A spent warning is dropped here; @p what names the caller in
    /// the log line that says why nothing happened.
    bool pending_review_current(uint32_t generation, const char* what);
    /// The printer type changed: a pending warning about the type it replaced is
    /// spent, so its sticky toast comes down. The history entry stays, and its
    /// Review then opens nothing.
    void retire_warning_if_type_changed();
    /// Run @p launch once the confirming modal's exit animation is over. One launch is
    /// pending at a time; the destructor cancels it.
    void launch_after_modal_exit(void (HardwareSetupPrompter::*launch)());
    void cancel_launch_timer();

    AsyncLifetimeGuard& lifetime_;
    std::function<lv_obj_t*()> screen_;
    std::function<IMoonrakerAPI*()> api_;

    // Steps the deferred offer runs if accepted, held for the confirm callback's timer.
    std::vector<wizard::StepId> pending_steps_;

    std::optional<TypeMismatchReview> pending_review_;
    /// Numbers each warning; bumped by every warning and by a printer switch, so a
    /// toast or history entry left from an earlier one cannot open a later review.
    uint32_t review_generation_ = 0;
    bool review_handler_registered_ = false;
    /// The review modal is up: set when it opens, cleared by either button and by any
    /// other dismissal.
    bool review_open_ = false;
    /// Watches the printer type while a warning is pending; re-armed by every warning.
    ObserverGuard type_change_observer_;

    lv_timer_t* launch_timer_ = nullptr;
    void (HardwareSetupPrompter::*pending_launch_)() = nullptr;
};

} // namespace helix
