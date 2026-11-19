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
#include <cstdint>
#include <optional>
#include <vector>

namespace helix::maintenance {

/// The meters, in display order. The ints index State::meters and name the per-meter
/// subjects (maintenance_m<i>_*); persistence goes by key, so the order is free to change.
enum class MeterId : int {
    XyAxis = 0,
    ZAxis,
    Nozzle,
    Extruder,
    Filter,
    ElectronicsFans,
    BeltTension
};
inline constexpr int kMeterCount = 7;

/// The persisted key of each meter. "rails" is the XY meter's key: records in the field use it.
inline constexpr std::array<const char*, kMeterCount> kMeterKeys = {
    "rails", "z_axis", "nozzle", "extruder", "filter", "electronics_fans", "belt_tension"};

/// The calendar limits the intervals screen offers, in days; 0 is none.
inline constexpr std::array<int, 6> kDayChoices = {0, 30, 60, 90, 180, 365};

/// The intervals the detail overlay offers, in hours; 0 is Off and hides the meter. Any
/// stored interval is snapped to one of these, so the dropdown always shows what is used.
inline constexpr std::array<int, 14> kIntervalChoices = {0,   25,  50,  100, 150,  200,  250,
                                                         300, 400, 500, 750, 1000, 1500, 2000};

/// The interval each meter gets when it applies and the printer's database row names none.
/// Belt tension has none: only a database row turns it on.
inline constexpr std::array<int, kMeterCount> kDefaultIntervals = {300, 500, 500, 300, 200, 500, 0};

/// The ranked rows the home tile can show.
inline constexpr int kTileRows = 4;

/// The upper bound of a stored hour count; a larger value is treated as corrupt.
inline constexpr double kMaxStoredHours = 1e7;

struct Meter {
    int interval_h = 0;                ///< 0 = Off
    double used_h = 0.0;               ///< print hours since the last reset
    std::optional<double> reset_at_h;  ///< State::total_h when last reset
    bool notified = false;             ///< the due notice has been shown for this interval
    bool seeded = true;                ///< false: this printer's interval is not chosen yet
    int interval_d = 0;                ///< calendar limit in days; 0 = none
    std::optional<int64_t> reset_at_s; ///< trusted wall clock when the interval started
};

struct State {
    double total_h = 0.0; ///< every print hour this screen has counted
    std::array<Meter, kMeterCount> meters{};
};

/// What the printer reports, for deciding which meters apply.
struct Applicability {
    bool enclosed = false;
    bool chamber_filter_fan = false;
    bool firmware_filter = false;
    bool controller_fan = false;
};

/// One meter's printer database override; nullopt falls back to the generic default.
struct MeterDefault {
    std::optional<int> hours;
    std::optional<int> days;
};
using ModelDefaults = std::array<MeterDefault, kMeterCount>;

/// No hours, every meter unseeded and Off until seed_defaults() knows the printer.
[[nodiscard]] State default_state();

[[nodiscard]] constexpr bool enabled(const Meter& m) {
    return m.interval_h > 0;
}

/// Whether a meter applies with no database row saying otherwise.
[[nodiscard]] bool applies(MeterId m, const Applicability& a);

/// A meter's starting limits: the database row where it has them, else the generic default,
/// else Off when the meter does not apply.
[[nodiscard]] MeterDefault default_for(MeterId m, const Applicability& a,
                                       const ModelDefaults& model);

/// Gives every unseeded meter its default; returns whether anything changed. A seeded meter
/// keeps the user's choice.
bool seed_defaults(State& s, const Applicability& a, const ModelDefaults& model);

/// Starts the calendar limit of each enabled meter that has one but no start stamp, when
/// @p now_s is trusted. Returns whether anything changed.
bool stamp_calendar_starts(State& s, std::optional<int64_t> now_s);

/// Due share 0..1, the larger of hours and days; 0 for a meter that is Off. @p now_s is
/// nullopt when the clock is untrusted, which leaves the day limit idle.
[[nodiscard]] double used_fraction(const Meter& m, std::optional<int64_t> now_s = std::nullopt);

/// Whole hours left before the meter is due, never below 0.
[[nodiscard]] int hours_left(const Meter& m);

/// Whole hours used, rounded.
[[nodiscard]] int hours_used(const Meter& m);

/// Whole days left on the calendar limit, never below 0; nullopt when it is idle.
[[nodiscard]] std::optional<int> days_left(const Meter& m, std::optional<int64_t> now_s);

/// Whether the calendar limit, not the hours, is the closer one.
[[nodiscard]] bool days_are_closer(const Meter& m, std::optional<int64_t> now_s);

/// Normal below three quarters of the interval, Warn from there, Danger once due.
[[nodiscard]] ValueStatus meter_status(const Meter& m, std::optional<int64_t> now_s = std::nullopt);

/// The enabled meter with the largest due share, the first on a tie; nullopt when every
/// meter is Off.
[[nodiscard]] std::optional<int> most_due(const State& s,
                                          std::optional<int64_t> now_s = std::nullopt);

/// Enabled meters, most due first, ties in display order.
[[nodiscard]] std::vector<int> due_order(const State& s,
                                         std::optional<int64_t> now_s = std::nullopt);

/// Meters due and not yet announced for this interval, latching each one.
std::vector<int> take_pending_notices(State& s, std::optional<int64_t> now_s = std::nullopt);

/// The largest interval of any enabled meter, 0 when every meter is Off. No meter shows
/// more hours left than this, so a layout budgeted for it holds while the hours move.
[[nodiscard]] int longest_interval(const State& s);

/// Adds @p hours to the total and to every enabled meter.
///
/// @p skip_filter leaves the Filter meter out (its hours come from the firmware, see
/// apply_firmware_filter_hours); the total and the other meters still accrue.
void accrue(State& s, double hours, bool skip_filter = false);

/// Sets the Filter meter's used hours to what a firmware filter counter reports, which
/// counts the filter fan's own run time rather than print time. An Off meter takes nothing.
void apply_firmware_filter_hours(State& s, double hours);

/// Starts a new interval: the used hours go to 0 and the reset is stamped with the total
/// and, when @p now_s is trusted, the wall clock.
void reset_meter(State& s, int meter, std::optional<int64_t> now_s = std::nullopt);

/// Sets an interval (snapped to kIntervalChoices). One that leaves the meter not due re-arms
/// its notice.
void set_interval(State& s, int meter, int hours, std::optional<int64_t> now_s = std::nullopt);

/// Sets the calendar limit (snapped to kDayChoices), starting it now when it had no start.
void set_interval_days(State& s, int meter, int days, std::optional<int64_t> now_s);

/// The kIntervalChoices entry nearest @p hours, the smaller on a tie. Zero or less is Off;
/// any positive request gets a positive interval.
[[nodiscard]] int snap_interval(int hours);

/// Index of @p hours in kIntervalChoices after snapping; the dropdown's selection.
[[nodiscard]] int interval_choice_index(int hours);

/// The kDayChoices entry nearest @p days; zero or less is none.
[[nodiscard]] int snap_days(int days);

/// Index of @p days in kDayChoices after snapping.
[[nodiscard]] int day_choice_index(int days);

/// The persisted form.
[[nodiscard]] nlohmann::json to_json(const State& s);

/// Reads a persisted record. A field that is missing, of the wrong type or out of range
/// takes its default, because the file is user-editable bytes on disk. A meter with no
/// readable interval comes back unseeded. A record with no z_axis entry starts it from the
/// rails hours, the best evidence of the last Z service.
[[nodiscard]] State from_json(const nlohmann::json& j);

/// The state the maintenance-meter example plugin left: @p storage is its store
/// (`plugin-data/maintenance-meter.json`, the object holding "state"), @p settings its
/// settings block (`/plugins/settings/maintenance-meter`). Either may be null. Returns
/// nullopt when neither holds anything to carry over.
[[nodiscard]] std::optional<State> from_plugin(const nlohmann::json& storage,
                                               const nlohmann::json& settings);

/// A printer database row's `maintenance` field. Unknown keys and bad values are ignored
/// field by field.
[[nodiscard]] ModelDefaults model_defaults_from_json(const nlohmann::json& maintenance_field);

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
