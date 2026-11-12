// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/temperature_units.h"

#include <atomic>
#include <cmath>
#include <iterator>

namespace helix::units {

namespace {
// Background threads build status text with it too, so atomic.
std::atomic<uint8_t> g_display_unit{static_cast<uint8_t>(TempUnit::Celsius)};

constexpr double kFahrenheitPerCelsius = 9.0 / 5.0;
constexpr double kFahrenheitAtZeroCelsius = 32.0;

int round_whole(double value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int>(std::lround(value));
}
} // namespace

double celsius_to_unit(double celsius, TempUnit unit) {
    if (unit == TempUnit::Fahrenheit) {
        return celsius * kFahrenheitPerCelsius + kFahrenheitAtZeroCelsius;
    }
    return celsius;
}

int celsius_to_unit_whole(double celsius, TempUnit unit) {
    return round_whole(celsius_to_unit(celsius, unit));
}

double celsius_delta_to_unit(double delta_celsius, TempUnit unit) {
    if (unit == TempUnit::Fahrenheit) {
        return delta_celsius * kFahrenheitPerCelsius;
    }
    return delta_celsius;
}

int unit_to_celsius_whole(double value, TempUnit unit) {
    if (unit == TempUnit::Fahrenheit) {
        return round_whole((value - kFahrenheitAtZeroCelsius) / kFahrenheitPerCelsius);
    }
    return round_whole(value);
}

const char* temp_unit_symbol(TempUnit unit) {
    return unit == TempUnit::Fahrenheit ? "°F" : "°C";
}

TempUnit display_temp_unit() {
    return static_cast<TempUnit>(g_display_unit.load(std::memory_order_relaxed));
}

void set_display_temp_unit(TempUnit unit) {
    g_display_unit.store(static_cast<uint8_t>(unit), std::memory_order_relaxed);
}

std::vector<TempAxisTick> temp_axis_ticks(double min_c, double max_c, double step_c,
                                          TempUnit unit) {
    std::vector<TempAxisTick> ticks;
    if (!(step_c > 0.0) || !std::isfinite(min_c) || !std::isfinite(max_c) || max_c < min_c) {
        return ticks;
    }
    if (unit == TempUnit::Celsius) {
        for (double c = min_c; c <= max_c + 1e-6; c += step_c) {
            ticks.push_back({c, round_whole(c)});
        }
        return ticks;
    }
    // The smallest round °F step at least as wide as the Celsius one.
    constexpr double kRoundSteps[] = {1, 2, 5, 10, 20, 25, 40, 50, 100, 200, 500};
    const double wanted = celsius_delta_to_unit(step_c, unit);
    double step_f = kRoundSteps[std::size(kRoundSteps) - 1];
    for (double candidate : kRoundSteps) {
        if (candidate >= wanted - 1e-6) {
            step_f = candidate;
            break;
        }
    }
    const double lo = celsius_to_unit(min_c, unit);
    const double hi = celsius_to_unit(max_c, unit);
    for (double f = std::ceil(lo / step_f - 1e-9) * step_f; f <= hi + 1e-6; f += step_f) {
        ticks.push_back({(f - kFahrenheitAtZeroCelsius) / kFahrenheitPerCelsius, round_whole(f)});
    }
    return ticks;
}

std::string format_display_temp(double celsius) {
    return std::to_string(to_display_temp_whole(celsius)) + display_temp_symbol();
}

std::string format_display_temp_delta(double delta_celsius) {
    return std::to_string(round_whole(to_display_temp_delta(delta_celsius))) +
           display_temp_symbol();
}

} // namespace helix::units
