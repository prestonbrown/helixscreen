// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>

namespace helix::ui {

/**
 * @brief Maps an integer lv_slider position to the real value it represents.
 *
 * LVGL sliders are integer-only, but several settings need finer resolution
 * than 1: square corner velocity is tuned in 0.5 steps and retraction
 * distances in 0.01mm. Those sliders hold scaled integers -- tenths and
 * hundredths -- and this converts in both directions.
 *
 * Panels that need no scaling use divisor 1, so a panel can drive every one of
 * its rows through the same slider/keypad code path regardless of resolution.
 */
struct SliderScale {
    /// Slider positions per unit of the real value. 1 = integer slider,
    /// 10 = tenths, 100 = hundredths.
    int divisor = 1;

    /// A zero or negative divisor is a construction bug; treat it as 1 rather
    /// than dividing by zero and sending inf to the printer as a limit.
    int effective_divisor() const {
        return divisor > 0 ? divisor : 1;
    }

    /// Real value for a slider position.
    double to_value(int slider_pos) const {
        return static_cast<double>(slider_pos) / effective_divisor();
    }

    /// Nearest slider position for a real value. Rounds rather than truncating
    /// so a keypad entry of 5.5 lands on 55 and not 54.
    int to_slider(double value) const {
        return static_cast<int>(std::llround(value * effective_divisor()));
    }

    /// to_slider(), held inside the slider's own range. The keypad validates
    /// against the range in value space; this keeps the thumb legal in
    /// position space even when rounding pushes it a step past the end.
    int to_slider_clamped(double value, int min_pos, int max_pos) const {
        int pos = to_slider(value);
        if (pos < min_pos)
            return min_pos;
        if (pos > max_pos)
            return max_pos;
        return pos;
    }
};

} // namespace helix::ui
