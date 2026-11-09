// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "bed_drying.h"
#include "core/observer_guard.h"
#include "helix_shared_callback.h"
#include "i_moonraker_api.h"
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>

namespace helix {

class PrinterState;
class TemperatureController;

/**
 * @brief Runs filament drying on the heated bed (prestonbrown/helixscreen#1730)
 *
 * The sequence is prepare() (home while the bed is empty, move the plate as far
 * from the nozzle as Z allows, park the toolhead at the back), then
 * confirm_placed() once the spools are on the plate: that sets the
 * spools-on-the-bed latch, persists the run, heats the bed (and a chamber
 * appliance dryer when asked) and holds Klipper's idle timeout to the planned
 * end plus a dead-man margin. HelixScreen owns the timer. At the end the
 * heaters go off and the latch stays until confirm_removed(), which the UI
 * offers once the bed reads below the cool-down threshold.
 *
 * The run lives in settings.json, so restore() picks it up after an app
 * restart or a power loss: the latch comes back before anything can move.
 *
 * Main thread only.
 */
class BedDryingController {
  public:
    /// Unloading and Preparing come before the latch: the toolhead is being
    /// cleared, then the plate homed and moved, and no spools are on it yet.
    enum class State {
        Idle = 0,
        Running = 1,
        Cooling = 2,
        ReadyToRemove = 3,
        Placing = 4,
        Unloading = 5,
        Preparing = 6
    };

    using Clock = std::function<long long()>;

    BedDryingController(PrinterState& state, IMoonrakerAPI* api, TemperatureController* tc,
                        Clock clock = {});
    ~BedDryingController();
    BedDryingController(const BedDryingController&) = delete;
    BedDryingController& operator=(const BedDryingController&) = delete;

    void init_subjects();

    /// Pick up a persisted run: the latch, and the timer or the cool-down.
    void restore();

    /// What a sensor says about filament at the toolhead; nullopt when no
    /// sensor can say for certain.
    [[nodiscard]] std::optional<bool> toolhead_loaded() const;

    /// Home if needed, then the clearance move and park. @p with_appliance asks
    /// for the chamber too: its dryer, or a plain chamber heater without one. Once the plate is in
    /// place the latch is set and persisted before @p on_ready fires (the place
    /// prompt): spools can land on the plate from then on. @p on_error with a
    /// message when a move was refused or failed, or the latch could not be
    /// saved.
    void prepare(const bed_drying::Material& material, bool with_appliance,
                 helix::SharedCallback<void()> on_ready,
                 helix::SharedCallback<void(const std::string&)> on_error);

    /// The spools are on the plate: persist the run, heat. False, with nothing
    /// heated, when the run could not be saved.
    bool confirm_placed();

    /// The user says no spools went on the plate: the only way out of
    /// Placing without the removal confirmation.
    void cancel_placement();

    /// End the run early. The latch stays.
    void stop();

    /// The spools are off the plate: clear the latch and the persisted run.
    void confirm_removed();

    /// How long a filament-system unload may take to show as busy before the
    /// flow gives up on it.
    static constexpr uint32_t kUnloadStartWindowMs = 30000;

    /// Wait for the filament system's unload before the plate moves: @p on_done
    /// once its action has gone busy and back to idle, @p on_failed on ERROR
    /// (started = true) or when it never goes busy within kUnloadStartWindowMs
    /// (started = false). A second call replaces the first wait.
    void await_unload(helix::SharedCallback<void()> on_done,
                      helix::SharedCallback<void(bool started)> on_failed);

    /// Drop the wait without running either callback.
    void cancel_unload_wait();

    /// Stop the flow before the spools go on: the unload wait is dropped and a
    /// plate move in flight no longer leads to the place prompt. A move or an
    /// unload the printer is already running finishes on its own.
    void cancel_preparation();

    /// Advance the run to @p now_s (wall clock seconds). Driven by a 1 s timer.
    void tick(long long now_s);

    [[nodiscard]] State state() const;
    [[nodiscard]] const bed_drying::RunRecord& record() const {
        return record_;
    }

    /// The bed temperature a run of @p material would use on this printer.
    [[nodiscard]] int bed_temp_for(const bed_drying::Material& material) const;

    /// What can heat the chamber alongside the bed on this printer.
    [[nodiscard]] bed_drying::ChamberAssist chamber_assist_available() const;

    /// The target a plain chamber heater would hold for @p material.
    [[nodiscard]] int chamber_temp_for(const bed_drying::Material& material) const;

    /// Publish the start modal's chamber row for @p material: which form it
    /// takes (a ChamberAssist) and the heater form's label.
    void describe_chamber(const bed_drying::Material& material);

    /// Called once each time the bed has cooled enough to take the spools off.
    void set_on_ready_to_remove(std::function<void()> cb) {
        on_ready_to_remove_ = std::move(cb);
    }

    /// Called once each time a print takes hold of the machine while spools
    /// are latched on the bed: a start from another client or a macro, which
    /// the latch cannot refuse.
    void set_on_print_while_latched(std::function<void()> cb) {
        on_print_while_latched_ = std::move(cb);
    }

    lv_subject_t* get_state_subject() {
        return &bed_drying_state_;
    }
    /// Death signal for every subject above, for observers outside the controller.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }
    lv_subject_t* get_text_subject() {
        return &bed_drying_text_;
    }
    /// Whole minutes left in a running cycle, rounded up; -1 outside one.
    lv_subject_t* get_remaining_min_subject() {
        return &bed_drying_remaining_min_;
    }
    /// 0..100 through a running cycle; -1 outside one.
    lv_subject_t* get_progress_pct_subject() {
        return &bed_drying_progress_pct_;
    }
    /// The start modal's "I understand" checkbox; Start enables on 1.
    lv_subject_t* get_ack_subject() {
        return &bed_drying_ack_;
    }
    lv_subject_t* get_chamber_assist_subject() {
        return &bed_drying_chamber_assist_;
    }
    lv_subject_t* get_chamber_text_subject() {
        return &bed_drying_chamber_text_;
    }

  private:
    friend struct BedDryingControllerTestAccess;

    long long now() const;
    void end_run(const char* why);
    bool begin_placement();
    void hold_idle(int seconds);
    [[nodiscard]] bool klipper_ready() const;
    void publish();
    void check_print_alarm();
    void set_latch(bool on);
    void cancel_timer();
    void finish_unload_wait(bool done);
    void drop_unload_wait();

    enum class PreRun { None, Unloading, Preparing };
    void set_pre_run(PreRun p);

    PrinterState& state_;
    IMoonrakerAPI* api_;
    TemperatureController* tc_;
    Clock clock_;

    bed_drying::RunRecord record_;
    bed_drying::Material pending_material_{};
    bool pending_appliance_ = false;
    bool bed_target_seen_ = false;
    bool removal_prompted_ = false;
    PreRun pre_run_ = PreRun::None;
    unsigned prep_gen_ = 0; ///< bumped when a preparation is dropped

    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    lv_subject_t bed_drying_state_{};
    lv_subject_t bed_drying_text_{};
    lv_subject_t bed_drying_remaining_min_{};
    lv_subject_t bed_drying_progress_pct_{};
    lv_subject_t bed_drying_ack_{};
    lv_subject_t bed_drying_chamber_assist_{};
    lv_subject_t bed_drying_chamber_text_{};
    char text_buf_[96]{};
    char chamber_text_buf_[96]{};

    lv_timer_t* timer_ = nullptr;
    std::function<void()> on_ready_to_remove_;
    std::function<void()> on_print_while_latched_;
    ObserverGuard print_watch_;
    bool print_alarm_raised_ = false;

    ObserverGuard unload_watch_;
    lv_timer_t* unload_timer_ = nullptr;
    bool unload_seen_busy_ = false;
    helix::SharedCallback<void()> unload_done_;
    helix::SharedCallback<void(bool)> unload_failed_;

    AsyncLifetimeGuard lifetime_;
};

/// The app's controller; nullptr before SubjectInitializer creates it.
BedDryingController* get_bed_drying_controller();

} // namespace helix
