// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file cold_pull_runner.h
 * @brief Runs one guided cold pull against the printer (prestonbrown/helixscreen#2011).
 *
 * App-lifetime, owned by SubjectInitializer like FilamentOpHeater: the heater is
 * watched here, never by a widget tree, so closing or rebuilding the overlay cannot
 * leave a hot nozzle unwatched. ColdPullFlow decides; this class feeds it the
 * printer's events and a 1 s tick, applies what it returns through
 * TemperatureController and IMoonrakerAPI, and publishes the stage for the overlay.
 */

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "cold_pull_flow.h"
#include "display_awake_hold.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#include <chrono>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::ui {

/// Why a start is refused, or Ok. Published on cold_pull_refusal.
enum class ColdPullRefusal : int {
    Ok = 0,
    JobHoldsMachine,
    PreheatPending,
    NotReady,
    NoHeater,
    Running
};

/// Pure: the refusal for these inputs, checked in the order of the enum.
[[nodiscard]] ColdPullRefusal cold_pull_refusal(PrintState lifecycle, bool preheat_pending,
                                                bool klippy_ready, bool head_has_heater,
                                                bool running);

class ColdPullRunner {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// @param clock Steady time; unset reads std::chrono::steady_clock.
    explicit ColdPullRunner(PrinterState& state, Clock clock = {});
    /// Applies the flow's exit_actions() when a pull is still live.
    ~ColdPullRunner();

    ColdPullRunner(const ColdPullRunner&) = delete;
    ColdPullRunner& operator=(const ColdPullRunner&) = delete;

    /// Registers the cold_pull_* subjects the overlay binds. Once.
    void init_subjects();

    [[nodiscard]] ColdPullRefusal can_start() const;
    /// Shows Setup again when no pull is live, with the refusal Start would meet.
    void prepare_setup();
    /// Starts a pull on the active head with @p temps; the refusal when it cannot.
    ColdPullRefusal start(ColdPullMaterial material, ColdPullTemps temps, bool hold_pressure);

    // User events, each forwarded to the flow with what it returns applied.
    void home_and_park();
    void unload_lane();
    void toolhead_clear();
    void extrude();
    void fed();
    void pulled();
    void repeat();
    void done();
    void cancel();

    [[nodiscard]] bool running() const;
    /// The live or last flow; null before the first start.
    [[nodiscard]] const ColdPullFlow* flow() const {
        return flow_ ? &*flow_ : nullptr;
    }

    /// Setup's choices, published for the overlay and read by start_from_setup().
    void select_material(ColdPullMaterial material);
    void adjust_soak(int delta_c);
    void adjust_pull(int delta_c);
    /// start() with what Setup shows; the edit is saved for the material.
    ColdPullRefusal start_from_setup();

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    /// A cleanup the printer could not take when the connection went away.
    struct Owed {
        std::string heater;
        int target_c = 0;
        ColdPullActions actions;
    };

    void apply(const ColdPullActions& a);
    void after_event();
    void publish();
    void publish_setup();
    void watch(bool on);
    void on_temperature();
    void on_lifecycle();
    void on_connection();
    void on_sensor(int value);
    void on_ams_action(int action);
    void send_gcode(const std::string& gcode, bool report);
    void dispatch_unload();
    void settle_owed();
    void cancel_tick_timer();
    [[nodiscard]] bool printer_ready() const;
    [[nodiscard]] TimePoint now() const;

    PrinterState& state_;
    Clock clock_;
    std::optional<ColdPullFlow> flow_;
    std::optional<Owed> owed_;
    ColdPullMaterial material_ = ColdPullMaterial::Pla;
    ColdPullTemps setup_temps_{};
    int last_sensor_ = -1;
    int unload_last_action_ = -1;
    bool unload_seen_busy_ = false;
    bool unload_watching_ = false;

    DisplayAwakeToken awake_;
    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard klippy_observer_;
    ObserverGuard sensor_observer_;
    ObserverGuard action_observer_;
    lv_timer_t* tick_timer_ = nullptr;
    AsyncLifetimeGuard lifetime_;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    lv_subject_t stage_{};
    lv_subject_t feed_stage_{};
    lv_subject_t pass_{};
    lv_subject_t end_reason_{};
    lv_subject_t has_fan_{};
    lv_subject_t can_home_park_{};
    lv_subject_t busy_{};
    lv_subject_t unload_failed_{};
    lv_subject_t material_subject_{};
    lv_subject_t hold_pressure_{};
    lv_subject_t refusal_{};
    lv_subject_t progress_{};
    lv_subject_t soak_text_{};
    char soak_text_buf_[16] = {};
    lv_subject_t pull_text_{};
    char pull_text_buf_[16] = {};
    lv_subject_t temp_text_{};
    char temp_text_buf_[32] = {};
    lv_subject_t pass_text_{};
    char pass_text_buf_[32] = {};
    int ring_from_c_ = 0; ///< Where the ring's stage started, for its fill
};

/// The app's ColdPullRunner, or nullptr before it exists.
[[nodiscard]] ColdPullRunner* get_cold_pull_runner();

/// Opens the cold pull overlay. Registered as guided device flow "cold_pull" so
/// generic code (a clog alert's recovery action) opens it by id.
void open_cold_pull_overlay();

/// Registers the "cold_pull" guided device flow.
void register_cold_pull_flow();

} // namespace helix::ui
