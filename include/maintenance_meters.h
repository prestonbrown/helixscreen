// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Hour meters for consumable parts: what they hold, how print time accrues into them, and
// how they persist. Pure: no LVGL and no config access, so every rule here is testable
// without a display. maintenance_model.cpp owns the live state, the subjects and the
// persistence calls; the home tile and the detail overlay only read what it publishes.

#include "json_fwd.h"
#include "value_status.h"

#include <array>
#include <optional>
#include <vector>

namespace helix::maintenance {

/// The meters, in display order. The ints index State::meters and name the per-meter
/// subjects (maintenance_m<i>_*).
enum class MeterId : int { Filter = 0, Nozzle = 1, Rails = 2 };
inline constexpr int kMeterCount = 3;

/// The persisted key of each meter, shared with the plugin's store so a migration reads
/// the same names.
inline constexpr std::array<const char*, kMeterCount> kMeterKeys = {"filter", "nozzle", "rails"};

/// The intervals the detail overlay offers, in hours; 0 is Off and hides the meter. Any
/// stored interval is snapped to one of these, so the dropdown always shows what is used.
inline constexpr std::array<int, 14> kIntervalChoices = {0,   25,  50,  100, 150,  200,  250,
                                                         300, 400, 500, 750, 1000, 1500, 2000};

/// The interval each meter starts with.
inline constexpr std::array<int, kMeterCount> kDefaultIntervals = {200, 500, 300};

/// The upper bound of a stored hour count; a larger value is treated as corrupt.
inline constexpr double kMaxStoredHours = 1e7;

struct Meter {
    int interval_h = 0;               ///< 0 = Off
    double used_h = 0.0;              ///< print hours since the last reset
    std::optional<double> reset_at_h; ///< State::total_h when last reset
    bool notified = false;            ///< the due notice has been shown for this interval
};

struct State {
    double total_h = 0.0; ///< every print hour this screen has counted
    std::array<Meter, kMeterCount> meters{};
};

[[nodiscard]] State default_state();

[[nodiscard]] constexpr bool enabled(const Meter& m) {
    return m.interval_h > 0;
}

/// Used share of the interval, 0..1; 0 for a meter that is Off.
[[nodiscard]] double used_fraction(const Meter& m);

/// Whole hours left before the meter is due, never below 0.
[[nodiscard]] int hours_left(const Meter& m);

/// Whole hours used, rounded.
[[nodiscard]] int hours_used(const Meter& m);

/// Normal below three quarters of the interval, Warn from there, Danger once due.
[[nodiscard]] ValueStatus meter_status(const Meter& m);

/// The enabled meter with the largest used share, the first on a tie; nullopt when every
/// meter is Off.
[[nodiscard]] std::optional<int> most_due(const State& s);

/// The largest interval of any enabled meter, 0 when every meter is Off. No meter shows
/// more hours left than this, so a layout budgeted for it holds while the hours move.
[[nodiscard]] int longest_interval(const State& s);

/// Adds @p hours to the total and to every enabled meter. Returns the meters that reached
/// their interval for the first time since their last reset or interval change; each one
/// is latched so it is returned once.
std::vector<int> accrue(State& s, double hours);

/// Starts a new interval: the used hours go to 0 and the reset is stamped with the total.
void reset_meter(State& s, int meter);

/// Sets an interval (snapped to kIntervalChoices). A raised interval that puts the due
/// point ahead of the used hours re-arms the due notice.
void set_interval(State& s, int meter, int hours);

/// The kIntervalChoices entry nearest @p hours, the smaller on a tie. Zero or less is Off;
/// any positive request gets a positive interval.
[[nodiscard]] int snap_interval(int hours);

/// Index of @p hours in kIntervalChoices after snapping; the dropdown's selection.
[[nodiscard]] int interval_choice_index(int hours);

/// The persisted form.
[[nodiscard]] nlohmann::json to_json(const State& s);

/// Reads a persisted record. A field that is missing, of the wrong type or out of range
/// takes its default, because the file is user-editable bytes on disk.
[[nodiscard]] State from_json(const nlohmann::json& j);

/// The state the maintenance-meter example plugin left: @p storage is its store
/// (`plugin-data/maintenance-meter.json`, the object holding "state"), @p settings its
/// settings block (`/plugins/settings/maintenance-meter`). Either may be null. Returns
/// nullopt when neither holds anything to carry over.
[[nodiscard]] std::optional<State> from_plugin(const nlohmann::json& storage,
                                               const nlohmann::json& settings);

/// Turns Klipper's print_stats.print_duration, which counts the current job's printing
/// time and excludes pauses, into increments of print time.
///
/// The first value after construction or reset() only seeds the baseline: the time it
/// carries was printed before this screen was watching, or was already counted. After
/// that, a rise is printing time. A drop forgets the baseline and the reading after it
/// seeds a new one: a new job loses at most one reading, and the screen writing 0 locally
/// at a print start before Klipper's running value arrives counts nothing twice.
class DurationTracker {
  public:
    /// Seconds of printing to accrue for this reading.
    int observe(int duration_s);

    /// Forget the baseline, as when the connection changes and the next reading may
    /// belong to a different job or printer.
    void reset() {
        last_s_.reset();
    }

  private:
    std::optional<int> last_s_;
};

} // namespace helix::maintenance
