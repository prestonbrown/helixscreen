// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file temperature_units.h
 * @brief The one place a temperature crosses between Celsius and the unit the user reads.
 *
 * Every temperature in state, settings and G-code stays in Celsius. Only what is
 * shown and what is typed goes through here: a reading leaves in the display unit,
 * and a typed target comes back as a whole Celsius target. Both directions round with
 * std::lround (half away from zero), so 0.5 always rounds the same way everywhere.
 */

namespace helix::units {

/// The unit temperatures are shown and entered in. Stored as this int in settings.
enum class TempUnit : uint8_t { Celsius = 0, Fahrenheit = 1 };

// ---------------------------------------------------------------------------
// Pure conversions: no state, the unit is an argument.
// ---------------------------------------------------------------------------

/// A Celsius temperature in @p unit.
double celsius_to_unit(double celsius, TempUnit unit);

/// A Celsius temperature in @p unit, rounded to a whole degree.
int celsius_to_unit_whole(double celsius, TempUnit unit);

/// A temperature difference (a band, a rate, a deficit): it scales, it has no offset.
double celsius_delta_to_unit(double delta_celsius, TempUnit unit);

/// A value typed in @p unit as the whole Celsius target it commands.
int unit_to_celsius_whole(double value, TempUnit unit);

/// The unit's symbol, "°C" or "°F".
const char* temp_unit_symbol(TempUnit unit);

// ---------------------------------------------------------------------------
// The user's display unit. DisplaySettingsManager sets it from the setting;
// it defaults to Celsius until then.
// ---------------------------------------------------------------------------

TempUnit display_temp_unit();
void set_display_temp_unit(TempUnit unit);

/// celsius_to_unit() and friends in the display unit.
inline double to_display_temp(double celsius) {
    return celsius_to_unit(celsius, display_temp_unit());
}
inline int to_display_temp_whole(double celsius) {
    return celsius_to_unit_whole(celsius, display_temp_unit());
}
inline double to_display_temp_delta(double delta_celsius) {
    return celsius_delta_to_unit(delta_celsius, display_temp_unit());
}
inline int display_temp_to_celsius(double value) {
    return unit_to_celsius_whole(value, display_temp_unit());
}
inline const char* display_temp_symbol() {
    return temp_unit_symbol(display_temp_unit());
}

/// One labelled tick on a temperature axis drawn in Celsius.
struct TempAxisTick {
    double celsius; ///< where the tick sits on the Celsius scale
    int label;      ///< the whole number it reads, in the axis unit
};

/// Ticks for a Celsius axis from @p min_c to @p max_c labelled every @p step_c.
/// In Celsius they sit on the step; in Fahrenheit they sit on round °F values
/// with a step near the same spacing (50°C becomes 100°F), so the labels never
/// read 122 or 212.
std::vector<TempAxisTick> temp_axis_ticks(double min_c, double max_c, double step_c, TempUnit unit);

/// "210°C" / "410°F": a whole-degree Celsius temperature as the user reads it.
std::string format_display_temp(double celsius);

/// "5°C" / "9°F": a whole-degree temperature difference as the user reads it.
std::string format_display_temp_delta(double delta_celsius);

} // namespace helix::units
