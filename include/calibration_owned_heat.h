// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "heater_hold.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/// Whether a heater a calibration turned on is still the calibration's to clear.
/// @param expected_c The target it set; 0 when firmware chose the target
/// @param live_c The target the printer reports; 0 can be this run's own send, unreported yet
constexpr bool owned_heat_may_clear(int expected_c, int live_c, bool job_holds) noexcept {
    return !job_holds && (expected_c == 0 || live_c == 0 || live_c == expected_c);
}

/**
 * @brief The heaters a calibration turned on and alone will turn off
 *
 * Every off respects a target someone else has set since and a job that has
 * started since. An off that falls due with the link down is sent after the
 * reconnect. Only ever sends heater offs, so it is safe mid-probe: Klipper runs
 * gcode one at a time and an off waits behind the running command.
 */
class CalibrationOwnedHeat {
  public:
    using SendOff = std::function<void(const std::string& heater)>;

    /// Long enough for the first status after a reconnect to arrive.
    static constexpr uint32_t kOwedOffSettleMs = 5000;

    CalibrationOwnedHeat(const char* log_tag, SendOff send_off);
    ~CalibrationOwnedHeat();
    CalibrationOwnedHeat(const CalibrationOwnedHeat&) = delete;
    CalibrationOwnedHeat& operator=(const CalibrationOwnedHeat&) = delete;

    /// Record a heater this flow turned on. Taking one already recorded adopts it
    /// for the new run and cancels any off still owed on it.
    /// @param expected_c The target being set; 0 when firmware chooses
    /// @param hold False for a heater that already had someone's target, which
    ///             the flow clears on its normal endings but not at shutdown
    void own(const std::string& heater, int expected_c, bool hold = true);

    [[nodiscard]] bool owns(const std::string& heater) const;
    [[nodiscard]] bool any() const;
    [[nodiscard]] bool owes_off(const std::string& heater) const;
    [[nodiscard]] bool held() const {
        return heater_hold_.held();
    }

    /// The run is over: send every off, or owe them if the link is down.
    void turn_off();
    /// Last chance while the client can send. Afterwards nothing is owned.
    void before_disconnect();
    /// Drop everything unsent, for when the printer has already cleared its heaters.
    void forget();
    /// Send what is owed if the link is up; otherwise it stays owed.
    void flush_owed();

    [[nodiscard]] bool settle_timer_armed() const {
        return settle_timer_ != nullptr;
    }

  private:
    struct Entry {
        std::string heater;
        int expected_c = 0;
        bool hold = true;
        bool owed = false;
    };

    void send_unless_taken(const Entry& entry);
    void update_hold();
    void watch_connection();
    void stop_waiting();
    void cancel_settle_timer();
    static void on_settle_timer(lv_timer_t* timer);

    const char* log_tag_;
    SendOff send_off_;
    std::vector<Entry> entries_;
    ObserverGuard connection_observer_;
    lv_timer_t* settle_timer_ = nullptr;
    HeaterHold heater_hold_;
};

} // namespace helix
