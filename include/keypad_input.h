// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

namespace helix::ui {

/// Pure input buffer logic for the numeric keypad (no LVGL dependency).
struct KeypadInput {
    static constexpr size_t BUF_SIZE = 16;

    char buf[BUF_SIZE] = "";

    /// Digits accepted before a decimal point is entered. The default suits a
    /// temperature (nozzle tops out at 350C); Machine Limits widens it to 5
    /// for max accel and Retraction narrows it to 1 for a distance in mm.
    /// Derive it from the field's maximum with keypad_int_digits_for().
    int max_int_digits = 3;

    /// Total digits accepted once a decimal point is present, integer part
    /// included. "250.75" is five.
    int max_total_digits = 5;

    /// Whether the decimal point is offered at all. Integer-only fields set
    /// this false so a typed "25.5" cannot be silently truncated on send.
    bool allow_dot = true;

    void clear() {
        buf[0] = '\0';
    }

    size_t length() const {
        return strlen(buf);
    }

    bool has_dot() const {
        return strchr(buf, '.') != nullptr;
    }

    /// Append a digit 0-9. Limited by max_int_digits before a decimal point
    /// is present and by max_total_digits after one.
    bool append_digit(int digit) {
        if (digit < 0 || digit > 9)
            return false;

        size_t len = length();
        int digit_count = 0;
        bool dot = false;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9')
                digit_count++;
            else if (buf[i] == '.')
                dot = true;
        }
        int max_digits = dot ? max_total_digits : max_int_digits;
        if (digit_count >= max_digits)
            return false;

        if (len < BUF_SIZE - 1) {
            buf[len] = '0' + digit;
            buf[len + 1] = '\0';
            return true;
        }
        return false;
    }

    /// Append a decimal point. Only one allowed, and only when allow_dot.
    bool append_dot() {
        if (!allow_dot)
            return false;
        if (has_dot())
            return false;
        size_t len = length();
        if (len < BUF_SIZE - 1) {
            buf[len] = '.';
            buf[len + 1] = '\0';
            return true;
        }
        return false;
    }

    /// Remove the last character.
    bool backspace() {
        size_t len = length();
        if (len == 0)
            return false;
        buf[len - 1] = '\0';
        return true;
    }

    /// Parse buffer as float. Empty buffer returns 0.
    float value() const {
        if (buf[0] == '\0')
            return 0.0f;
        return static_cast<float>(atof(buf));
    }
};

/**
 * @brief Digits in the integer part of @p max_value, for KeypadInput::max_int_digits.
 *
 * Derived from the field's own maximum so a new keypad caller cannot forget to
 * widen the cap and then find it cannot type its largest legal value. Floors at
 * 1 (a maximum below 10 still needs one digit) and never exceeds what the
 * buffer can hold.
 */
inline int keypad_int_digits_for(double max_value) {
    double magnitude = max_value < 0 ? -max_value : max_value;
    constexpr int LIMIT = static_cast<int>(KeypadInput::BUF_SIZE) - 1;

    int digits = 1;
    for (double remaining = magnitude; remaining >= 10.0 && digits < LIMIT; remaining /= 10.0) {
        digits++;
    }
    return digits;
}

} // namespace helix::ui
