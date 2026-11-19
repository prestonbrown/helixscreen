// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_op_heater.h
 * @brief The nozzle heat around a filament operation: heat before it, cool after it.
 *
 * Every Load / Unload / Purge / Extrude / Retract needs a hotend above Klipper's
 * min_extrude_temp, and the surface that asked for one can be closed or torn
 * down before the nozzle gets there: a dialog can be dismissed, a panel left.
 * So the wait belongs to an app-lifetime service, not to a panel. A dialog
 * that stays open (the runout guidance dialog offers Purge after Load, and
 * Resume beside both) is why a surface gets one op at a time and why a pause
 * ending drops a reload asked for during it.
 *
 * Three layers:
 *   - decide_preheat(): the pure rule. Hot enough, or heat to what and wait for what.
 *   - FilamentOpHeater: owns the one pending preheat, watches the nozzle, runs the
 *     op once it is hot, and schedules the post-op cooldown. Owned by
 *     SubjectInitializer, reached through get_filament_op_heater().
 *   - execute_filament_load/unload/purge() (filament_op_execute.h) go through it
 *     on every call, so a surface on the shared ladder cannot skip the preheat.
 *     Surfaces with a ladder of their own call run_after_preheat() directly.
 *
 * The heater decides; it draws nothing. A surface that shows its own heating UI
 * passes on_progress and announce = false; one with no UI of its own gets the
 * heater's toasts.
 */

#pragma once

#include "cleanup_hold.h"
#include "core/observer_guard.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "helix_shared_callback.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::ui {

/// What a filament op does with the melt zone. Picks the slot whose material sets
/// the temperature, and the copy in log lines and toasts.
enum class FilamentHeatOp { Load, Unload, Purge, Extrude, Retract };

/// Short lowercase name for @p op, for log lines. Never null.
[[nodiscard]] const char* filament_heat_op_name(FilamentHeatOp op);

/// A preheat is reached, and a hot nozzle counts as hot, within this many degrees.
inline constexpr int kPreheatMarginC = 5;

/// What the nozzle reads now and what the op needs, in whole degrees.
struct PreheatInputs {
    int current_c = 0;             ///< Live temperature
    int target_c = 0;              ///< Live target; 0 when the heater is off
    std::optional<int> material_c; ///< What the op's material wants, when anything names it
    int held_c = 0;                ///< Latched last non-zero target (the swap latch)
    int floor_c = 0;               ///< temperature::extrusion_floor_c()
    int ceiling_c = 0;             ///< temperature::nozzle_max_temp_c()
    /// A target someone set and the nozzle has reached counts as hot enough.
    /// True for ops that push what is already in the melt zone (purge, extrude,
    /// retract) and for any op while a job holds the machine: a paused print's
    /// reload runs at the print's own temperature.
    bool honor_live_target = false;
};

/// What to do about @p PreheatInputs.
struct PreheatDecision {
    bool heat = false; ///< false: the nozzle is hot enough, run the op now
    int send_c = 0;    ///< The target to command (the controller keeps a hotter one)
    int wait_c = 0;    ///< The temperature the op waits for, within kPreheatMarginC
};

/**
 * @brief The preheat rule, with no printer attached.
 *
 * Nothing runs below the extrusion floor. Above it, the nozzle is hot enough when
 *   - it is within kPreheatMarginC of the hotter of the material's temperature
 *     and the swap latch, so the filament already in the melt zone still flows
 *     (with no material named, the floor stands in for it), or
 *   - @p honor_live_target is set and it has reached a target someone set.
 *
 * Otherwise it heats to the material's temperature, or kDefaultLoadPreheatTemp
 * when nothing names one, held between the floor and the ceiling, and waits for
 * that raised to the latch: the controller's keep_previous_hot holds the nozzle
 * there, so a lower wait would fire before the nozzle settles.
 *
 * Skip reasons (preheat_skip_reason()) are the caller's to apply first.
 */
[[nodiscard]] PreheatDecision decide_preheat(const PreheatInputs& in);

/// Whether @p current_c has reached a preheat waiting for @p wait_c: within the
/// margin, and never below the extrusion floor.
[[nodiscard]] bool preheat_reached(int current_c, int wait_c, int floor_c);

/// Where a preheat stands, as reported to the surface that asked for it.
enum class PreheatPhase {
    Heating,   ///< Target sent, waiting. Repeats as the temperature moves.
    Reached,   ///< Hot; the op runs immediately after this report
    Cancelled, ///< Withdrawn: the user re-tapped, or another op replaced it
    Abandoned, ///< It cannot complete: target taken away, timed out, a job started,
               ///< or the pause it was asked in ended
};

struct PreheatProgress {
    PreheatPhase phase = PreheatPhase::Heating;
    int current_c = 0;
    int target_c = 0; ///< The temperature being waited for
};

/// Names one request, so its completion can say whether the heater heated for it.
/// 0 is no request.
using FilamentOpTicket = uint64_t;

/// One filament op that needs a hot nozzle.
struct PreheatRequest {
    FilamentHeatOp op = FilamentHeatOp::Load;

    /// Prefixes log lines. Static storage duration, like FilamentOpSurface::log_tag.
    const char* log_tag = "[Filament]";

    /// preheat_skip_reason() for the plan about to run, or preheat_skip_reason_unplanned()
    /// for an op with no plan. Anything but None runs the op with no heat.
    PreheatSkip skip = PreheatSkip::None;

    /// Who asked: a surface's address (or a static tag). With @p op and @p slot it
    /// names the request, so the same request again is a re-tap.
    const void* owner = nullptr;

    /// The lane whose material sets the temperature, and whose mapped extruder
    /// heats: the lane a load or unload targets, the loaded lane for a purge /
    /// extrude / retract. Negative for none, which heats the active extruder.
    int slot = -1;

    /// The surface's own answer when neither the lane nor the external spool names
    /// a material (the Filament panel's material preset). Unset falls through to
    /// kDefaultLoadPreheatTemp.
    std::optional<PreheatTarget> fallback_material;

    /// Raise the heater's own "Heating to…" toast. A surface that renders its own
    /// heating state from on_progress turns it off; the toasts for a preheat that
    /// ends without running stay, because the op the user asked for did not happen.
    bool announce = true;

    /// The op. Runs on the main thread, now when no heat is needed, else once hot.
    /// It is handed the request's ticket, which its completion passes to
    /// op_succeeded() / op_failed(). Must outlive the surface: capture nothing a
    /// closing surface frees.
    helix::SharedCallback<void(FilamentOpTicket)> run;

    /// Phase reports, on the main thread. Wrap anything that touches the surface
    /// in its lifetime token: a report can land after the surface is gone.
    helix::SharedCallback<void(const PreheatProgress&)> on_progress;
};

/**
 * @brief The one pending preheat, and the cooldown after a filament op it heated for.
 *
 * Main thread only. One preheat at a time, because there is one nozzle:
 *   - the SAME request again (op, slot and owner) while it waits is the user
 *     changing their mind, so it cancels the pending one and runs nothing;
 *   - a different op from the same owner is refused with a toast and the
 *     pending one carries on: a dialog that stays open offers its next step
 *     (Purge after Load) while the first still heats;
 *   - a request from another owner replaces it: the pending one is reported
 *     Cancelled to its surface and the new one proceeds.
 *
 * A preheat ends without running its op when the target it sent never lands,
 * when that target is taken away, when it runs past kPreheatTimeout, when the
 * extruder it heats goes away, for a request made while no job held the
 * machine when a job takes it, and for a request made during a pause when the
 * pause ends. Heat it commanded for a preheat that ends unrun is put back the
 * way it found it, except while a print holds the machine, which then owns the
 * heater.
 */
class FilamentOpHeater {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// How long a sent target may take to appear on the nozzle.
    static constexpr std::chrono::seconds kTargetLandTimeout{20};
    /// How long a preheat may take altogether.
    static constexpr std::chrono::minutes kPreheatTimeout{10};
    /// How long a backend op may run before its completion stops being watched.
    static constexpr std::chrono::minutes kBackendWatchTimeout{15};

    /// @param clock Steady time; unset reads std::chrono::steady_clock.
    explicit FilamentOpHeater(PrinterState& state, Clock clock = {});
    ~FilamentOpHeater();

    FilamentOpHeater(const FilamentOpHeater&) = delete;
    FilamentOpHeater& operator=(const FilamentOpHeater&) = delete;

    enum class Outcome {
        Ran,       ///< No heat needed (or skipped); run() has returned
        Heating,   ///< run() waits for the nozzle
        Cancelled, ///< Not run: a re-tap withdrew the pending request, this
                   ///< surface's other op is still heating, or this one could not
                   ///< take the heater. run() never runs.
    };

    /// Run @p req.run now, or heat first and run it once hot.
    Outcome run_after_preheat(PreheatRequest req);

    /// Whether a preheat is waiting.
    [[nodiscard]] bool pending() const {
        return pending_.has_value();
    }

    /// Withdraw the pending preheat: the heater goes back off if it was off when
    /// the request arrived, and the op never runs. False when nothing was pending.
    bool cancel();

    /// cancel(), but only a preheat @p owner asked for. False otherwise.
    bool cancel_owned(const void* owner);

    /// Whether a request for @p op from @p owner would be refused because a
    /// different op of the same owner is still heating; toasts the refusal when
    /// it would. run_after_preheat() applies it, and a surface that arms its own
    /// state before asking checks it first, so the refusal leaves that state to
    /// the op that is heating.
    bool refuse_while_heating(FilamentHeatOp op, const void* owner, const char* log_tag) const;

    /// The op @p ticket names finished. Schedules the post-op cooldown of the
    /// extruder it heated when this heater heated for it, no job holds the
    /// machine and no preheat waits; anything else (a hand preheat, a backend
    /// that heats for itself) is not ours to cool.
    void op_succeeded(FilamentOpTicket ticket);

    /// The op @p ticket names failed: the heater stays where the user can see
    /// what happened, and the ticket is forgotten.
    void op_failed(FilamentOpTicket ticket);

    /// The op @p ticket names stopped before it was done (the user backed out, an
    /// extrusion failed, the printer went away) with the nozzle hot for it. A heater
    /// this heater heated goes off now, unless a job holds the machine. With the
    /// printer unreachable the send cannot land, so the post-op cooldown is
    /// scheduled in its place. The ticket is forgotten.
    void op_cancelled(FilamentOpTicket ticket);

    /// The op @p ticket names waits on the user with the nozzle hot (a manual
    /// load's hand push), for as long as the user takes. Until op_succeeded(),
    /// op_failed() or op_cancelled() names the ticket, SIGTERM takes the graceful
    /// path and before_disconnect() turns that heater off when this heater heated
    /// it, then reports @p on_lost: the op is over, and must send nothing more.
    void hold_heat(FilamentOpTicket ticket, helix::SharedCallback<void()> on_lost);

    /// Whether an op holds the nozzle hot through hold_heat().
    [[nodiscard]] bool holding() const {
        return !holds_.empty();
    }

    /// A backend op for @p ticket is about to be dispatched and completes on the
    /// AMS action feed: op_succeeded() when the action moves to busy after this
    /// call and then returns to IDLE, op_failed() on ERROR, forgotten after
    /// kBackendWatchTimeout. Arm it BEFORE the dispatch so a fast backend cannot
    /// finish unseen; call op_failed() if the dispatch is refused. Does nothing
    /// for a ticket the heater did not heat for.
    void watch_backend_completion(FilamentOpTicket ticket);

    /// End a waiting preheat while the printer can still hear it: a heater this
    /// wait turned on goes back off, and the op it was for does not run. Heat an
    /// op holds (hold_heat()) goes off too, and that op is told it was lost.
    void before_disconnect();

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Pending {
        PreheatRequest req;
        FilamentOpTicket ticket = 0;
        std::string heater;   ///< The extruder heated, always by name
        std::string extruder; ///< Empty when that is the active one (whose subjects differ)
        std::weak_ptr<bool> extruder_life; ///< The mapped extruder's subjects
        int wait_c = 0;
        int floor_c = 0;
        int prior_target_c = 0; ///< That extruder's target before we sent ours
        bool target_seen = false;
        bool job_free_at_request = true;
        bool paused_at_request = false; ///< Asked for while the print was paused
        TimePoint started{};
    };

    struct Heated {
        FilamentOpTicket ticket = 0;
        std::string extruder; ///< The extruder heated, by name
        TimePoint at{};
    };

    /// A replaced preheat's heat, waiting for whoever takes the heater next.
    struct Handoff {
        std::string heater;
        int prior_target_c = 0;
    };

    /// A cooldown this heater scheduled, owed on @p heater until it fires.
    struct Owed {
        std::string heater;
        TimePoint at{};
    };

    /// An op keeping the nozzle hot while the user acts (hold_heat()).
    struct Hold {
        FilamentOpTicket ticket = 0;
        std::string heater; ///< Set when this heater heated it, so its to turn off
        helix::SharedCallback<void()> on_lost;
    };

    struct Watch {
        FilamentOpTicket ticket = 0;
        int last_action = -1; ///< The action as it stood when last seen
        bool seen_busy = false;
        TimePoint deadline{};
    };

    void start_heating(Pending p, int send_c, const std::string& material_name);
    void on_temperature();
    void on_target(int target_deci);
    void on_lifecycle();
    /// Whether the print lifecycle moved out from under the pending request: a
    /// print started after an idle request, or the pause a request was made in
    /// ended (resumed, cancelled, finished).
    [[nodiscard]] bool lifecycle_lost() const;
    void on_ams_action(int action);
    void check_deadlines();
    /// Take the pending preheat out, stop watching the nozzle, then report @p phase
    /// to its surface: the report runs with the heater already idle, so it may
    /// start another op.
    void end_pending(PreheatPhase phase);
    /// End the pending preheat unrun: toast, put the heater back as it was when
    /// @p restore, report Abandoned.
    void abandon(const char* why, const char* toast, bool restore);
    /// Turn @p heater back off when it was off before we heated it.
    void restore_prior(const std::string& heater, int prior_target_c);
    /// Held while a waiting preheat is on a heater that restore_prior() would turn off.
    void sync_heater_hold();
    /// Whatever earlier requests and ops left on @p heater passes to the request
    /// that works it now: a replaced preheat's heat and a cooldown owed on it.
    /// Heat left on another heater is put back. Returns the target @p heater had
    /// before this heater heated it, when this heater did.
    std::optional<int> take_over(const std::string& heater);
    /// How long a scheduled cooldown is owed before its timer has fired.
    [[nodiscard]] std::chrono::seconds owed_window() const;
    void report_heating();
    void cancel_deadline_timer();
    [[nodiscard]] TimePoint now() const;
    [[nodiscard]] lv_subject_t* temp_subject(const std::string& extruder) const;
    [[nodiscard]] lv_subject_t* target_subject(const std::string& extruder) const;
    [[nodiscard]] int read_c(lv_subject_t* subject) const;
    [[nodiscard]] const Heated* heated_for(FilamentOpTicket ticket) const;
    void forget(FilamentOpTicket ticket);

    PrinterState& state_;
    Clock clock_;
    std::optional<Pending> pending_;
    FilamentOpTicket next_ticket_ = 0;
    std::vector<Heated> heated_;
    std::optional<Handoff> handoff_;
    CleanupHold heater_hold_;
    std::vector<Owed> owed_;
    std::vector<Watch> watches_;
    std::vector<Hold> holds_;
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard action_observer_;
    lv_timer_t* deadline_timer_ = nullptr;
};

/// Connected, with Klippy ready: a command sent now reaches the printer. Main thread.
[[nodiscard]] bool printer_reachable(PrinterState& state);

/// The app's FilamentOpHeater (a shared resource SubjectInitializer registers),
/// or nullptr before it exists.
[[nodiscard]] FilamentOpHeater* get_filament_op_heater();

/// The material a filament op on @p slot heats for: that lane's, else the
/// external spool's (resolve_load_preheat_material()). nullopt when neither names
/// one, so a surface's own tail (a material preset, a default) applies.
[[nodiscard]] std::optional<PreheatTarget> preheat_material_for_slot(int slot);

/// preheat_skip_reason() for an op with no tier plan (purge, extrude, retract):
/// only the user's own claim applies, because no backend or macro has been chosen.
[[nodiscard]] PreheatSkip preheat_skip_reason_unplanned();

} // namespace helix::ui
