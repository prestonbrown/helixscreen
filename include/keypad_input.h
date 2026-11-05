// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

namespace helix::ui {

/// Pure input buffer logic for the numeric keypad (no LVGL dependency).
struct KeypadInput {
    static constexpr size_t BUF_SIZE = 16;

    /// What the buffer holds. Number is a signed decimal; Ipv4 is a dotted quad
    /// typed octet by octet, which the digit caps and the sign below ignore.
    enum class Mode { Number, Ipv4 };
    Mode mode = Mode::Number;

    char buf[BUF_SIZE] = "";

    /// Digits accepted before a decimal point is entered. The default suits a
    /// temperature (nozzle tops out at 350C); Machine Limits widens it to 5
    /// for max accel and Retraction narrows it to 1 for a distance in mm.
    /// Derive it from the field's maximum with keypad_int_digits_for().
    int max_int_digits = 3;

    /// Total digits accepted once a decimal point is present, integer part
    /// included. "250.75" is five.
    int max_total_digits = 5;

    /// Digits accepted after the decimal point; by default only max_total_digits limits them.
    int max_frac_digits = static_cast<int>(BUF_SIZE);

    /// Whether the decimal point is offered at all. Integer-only fields set
    /// this false so a typed "25.5" cannot be silently truncated on send.
    bool allow_dot = true;

    /// Whether the sign key works. Only fields whose range reaches below zero set it.
    bool allow_sign = false;

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
    /// is present, and by max_total_digits and max_frac_digits after one.
    bool append_digit(int digit) {
        if (digit < 0 || digit > 9)
            return false;
        if (mode == Mode::Ipv4)
            return append_ipv4_digit(digit);

        size_t len = length();
        int digit_count = 0;
        int frac_count = 0;
        bool dot = false;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                digit_count++;
                if (dot)
                    frac_count++;
            } else if (buf[i] == '.') {
                dot = true;
            }
        }
        int max_digits = dot ? max_total_digits : max_int_digits;
        if (digit_count >= max_digits || (dot && frac_count >= max_frac_digits))
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
        if (mode == Mode::Ipv4)
            return append_ipv4_dot();
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

    bool negative() const {
        return buf[0] == '-';
    }

    /// Flip the draft's sign, typed digits or not. The sign is not a digit, so the
    /// digit caps are unaffected; it only needs room in the buffer.
    bool toggle_sign() {
        if (!allow_sign || mode == Mode::Ipv4)
            return false;
        size_t len = length();
        if (negative()) {
            memmove(buf, buf + 1, len); // includes the terminator
            return true;
        }
        if (len >= BUF_SIZE - 1)
            return false;
        memmove(buf + 1, buf, len + 1);
        buf[0] = '-';
        return true;
    }

    /// Remove the last character.
    bool backspace() {
        size_t len = length();
        if (len == 0)
            return false;
        buf[len - 1] = '\0';
        return true;
    }

    /// The buffer as typed.
    const char* text() const {
        return buf;
    }

    /// Parse buffer as float. Empty buffer returns 0, and so does a signed zero
    /// ("-", "-0.") rather than -0.
    float value() const {
        if (buf[0] == '\0')
            return 0.0f;
        const float v = static_cast<float>(atof(buf));
        return v == 0.0f ? 0.0f : v;
    }

  private:
    /// The octet being typed: the digits after the last dot.
    const char* current_octet() const {
        const char* dot = strrchr(buf, '.');
        return dot ? dot + 1 : buf;
    }

    bool append_char(char c) {
        size_t len = length();
        if (len >= BUF_SIZE - 1)
            return false;
        buf[len] = c;
        buf[len + 1] = '\0';
        return true;
    }

    /// At most three digits an octet, and never a digit that takes it past 255.
    bool append_ipv4_digit(int digit) {
        const char* octet = current_octet();
        if (strlen(octet) >= 3)
            return false;
        if (atoi(octet) * 10 + digit > 255)
            return false;
        return append_char(static_cast<char>('0' + digit));
    }

    /// A dot ends a non-empty octet; a quad has three.
    bool append_ipv4_dot() {
        size_t len = length();
        if (len == 0 || buf[len - 1] == '.')
            return false;
        int dots = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '.')
                dots++;
        }
        if (dots >= 3)
            return false;
        return append_char('.');
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
