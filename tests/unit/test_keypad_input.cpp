// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keypad_input.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::KeypadInput;

TEST_CASE("KeypadInput digit entry", "[keypad]") {
    KeypadInput kp;

    SECTION("appends digits") {
        REQUIRE(kp.append_digit(1));
        REQUIRE(kp.append_digit(2));
        REQUIRE(kp.append_digit(3));
        CHECK(std::string(kp.buf) == "123");
    }

    SECTION("limits to 3 digits without decimal") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        CHECK_FALSE(kp.append_digit(4));
        CHECK(std::string(kp.buf) == "123");
    }

    SECTION("allows 5 digits with decimal") {
        kp.append_digit(2);
        kp.append_digit(5);
        kp.append_digit(0);
        kp.append_dot();
        kp.append_digit(7);
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == "250.75");
        CHECK_FALSE(kp.append_digit(9));
    }

    SECTION("rejects invalid digit values") {
        CHECK_FALSE(kp.append_digit(-1));
        CHECK_FALSE(kp.append_digit(10));
        CHECK(std::string(kp.buf) == "");
    }

    SECTION("zero works normally") {
        REQUIRE(kp.append_digit(0));
        CHECK(std::string(kp.buf) == "0");
    }
}

TEST_CASE("KeypadInput decimal point", "[keypad]") {
    KeypadInput kp;

    SECTION("appends dot") {
        kp.append_digit(1);
        REQUIRE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("only allows one dot") {
        kp.append_digit(1);
        kp.append_dot();
        CHECK_FALSE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("dot as first character") {
        REQUIRE(kp.append_dot());
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == ".5");
        CHECK(kp.value() == Catch::Approx(0.5f));
    }

    SECTION("digits after dot count toward 5-digit limit") {
        // 12.345 = 5 digits + dot
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_dot();
        kp.append_digit(3);
        kp.append_digit(4);
        kp.append_digit(5);
        CHECK(std::string(kp.buf) == "12.345");
        CHECK_FALSE(kp.append_digit(6));
    }
}

TEST_CASE("KeypadInput backspace", "[keypad]") {
    KeypadInput kp;

    SECTION("removes last character") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        REQUIRE(kp.backspace());
        CHECK(std::string(kp.buf) == "12");
    }

    SECTION("removes dot") {
        kp.append_digit(1);
        kp.append_dot();
        kp.backspace();
        CHECK(std::string(kp.buf) == "1");
        CHECK_FALSE(kp.has_dot());
    }

    SECTION("backspace on empty returns false") {
        CHECK_FALSE(kp.backspace());
    }

    SECTION("can re-add dot after backspacing it") {
        kp.append_digit(1);
        kp.append_dot();
        kp.backspace();
        REQUIRE(kp.append_dot());
        CHECK(std::string(kp.buf) == "1.");
    }

    SECTION("backspace all then re-enter") {
        kp.append_digit(5);
        kp.backspace();
        CHECK(std::string(kp.buf) == "");
        REQUIRE(kp.append_digit(9));
        CHECK(std::string(kp.buf) == "9");
    }
}

TEST_CASE("KeypadInput value parsing", "[keypad]") {
    KeypadInput kp;

    SECTION("empty buffer is 0") {
        CHECK(kp.value() == 0.0f);
    }

    SECTION("integer value") {
        kp.append_digit(2);
        kp.append_digit(5);
        kp.append_digit(0);
        CHECK(kp.value() == Catch::Approx(250.0f));
    }

    SECTION("decimal value") {
        kp.append_digit(1);
        kp.append_dot();
        kp.append_digit(5);
        CHECK(kp.value() == Catch::Approx(1.5f));
    }

    SECTION("trailing dot parses as integer") {
        kp.append_digit(4);
        kp.append_digit(2);
        kp.append_dot();
        CHECK(kp.value() == Catch::Approx(42.0f));
    }
}

TEST_CASE("KeypadInput clear", "[keypad]") {
    KeypadInput kp;
    kp.append_digit(1);
    kp.append_dot();
    kp.append_digit(5);

    kp.clear();
    CHECK(std::string(kp.buf) == "");
    CHECK(kp.value() == 0.0f);
    CHECK_FALSE(kp.has_dot());
}

TEST_CASE("KeypadInput digit limit transitions", "[keypad]") {
    KeypadInput kp;

    SECTION("adding dot after 3 digits allows more digits") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        CHECK_FALSE(kp.append_digit(4)); // blocked at 3
        REQUIRE(kp.append_dot());
        REQUIRE(kp.append_digit(4)); // now allowed (5 digit limit)
        REQUIRE(kp.append_digit(5));
        CHECK(std::string(kp.buf) == "123.45");
        CHECK_FALSE(kp.append_digit(6)); // blocked at 5
    }

    SECTION("backspace below limit allows re-entry") {
        kp.append_digit(1);
        kp.append_digit(2);
        kp.append_digit(3);
        kp.backspace();
        REQUIRE(kp.append_digit(9));
        CHECK(std::string(kp.buf) == "129");
    }
}

// ============================================================================
// Configurable digit caps
//
// The 3-digit default is temperature-shaped (nozzle tops out at 350C). Machine
// Limits needs five (max accel 50000) and Retraction needs one plus decimals
// (retract length 6.0mm), so the cap is derived from the configured maximum
// rather than hard-coded. The defaults above must keep behaving exactly as
// they did -- every existing case in this file exercises them.
// ============================================================================

TEST_CASE("keypad_int_digits_for derives the cap from the range maximum", "[keypad]") {
    using helix::ui::keypad_int_digits_for;

    SECTION("Machine limit magnitudes") {
        CHECK(keypad_int_digits_for(50000.0) == 5); // max accel
        CHECK(keypad_int_digits_for(1000.0) == 4);  // max velocity
        CHECK(keypad_int_digits_for(20.0) == 2);    // square corner velocity
        CHECK(keypad_int_digits_for(6.0) == 1);     // retract length (mm)
    }

    SECTION("Temperature range keeps the historical 3") {
        CHECK(keypad_int_digits_for(350.0) == 3);
        CHECK(keypad_int_digits_for(999.0) == 3);
    }

    SECTION("Boundaries roll over at powers of ten") {
        CHECK(keypad_int_digits_for(9.0) == 1);
        CHECK(keypad_int_digits_for(10.0) == 2);
        CHECK(keypad_int_digits_for(99.0) == 2);
        CHECK(keypad_int_digits_for(100.0) == 3);
    }

    SECTION("A fractional maximum counts only its integer part") {
        CHECK(keypad_int_digits_for(6.5) == 1);
        CHECK(keypad_int_digits_for(99.9) == 2);
    }

    SECTION("Degenerate maxima still allow one digit") {
        CHECK(keypad_int_digits_for(0.0) == 1);
        CHECK(keypad_int_digits_for(0.5) == 1);
        CHECK(keypad_int_digits_for(-50.0) == 2); // magnitude, not sign
    }

    SECTION("Absurd maxima stay inside the buffer") {
        // BUF_SIZE is 16; a cap that exceeds it would let append_digit walk
        // off the end rather than refusing input.
        CHECK(keypad_int_digits_for(1e30) <= static_cast<int>(KeypadInput::BUF_SIZE) - 1);
    }
}

TEST_CASE("KeypadInput honours a widened integer cap", "[keypad]") {
    KeypadInput kp;
    kp.max_int_digits = 5;
    kp.max_total_digits = 7;

    SECTION("Five digits are accepted for max accel") {
        for (int d : {5, 0, 0, 0, 0})
            REQUIRE(kp.append_digit(d));
        CHECK(std::string(kp.buf) == "50000");
        CHECK(kp.value() == Catch::Approx(50000.0f));
    }

    SECTION("The sixth integer digit is still refused") {
        for (int d : {5, 0, 0, 0, 0})
            REQUIRE(kp.append_digit(d));
        CHECK_FALSE(kp.append_digit(1));
        CHECK(std::string(kp.buf) == "50000");
    }

    SECTION("A four-digit entry that the old cap blocked now works") {
        for (int d : {3, 0, 0, 0})
            REQUIRE(kp.append_digit(d));
        CHECK(kp.value() == Catch::Approx(3000.0f));
    }
}

TEST_CASE("KeypadInput honours a narrowed integer cap", "[keypad]") {
    // Retract length: 0.0-6.0mm. One integer digit, two decimals.
    KeypadInput kp;
    kp.max_int_digits = 1;
    kp.max_total_digits = 3;

    SECTION("One integer digit then blocked") {
        REQUIRE(kp.append_digit(6));
        CHECK_FALSE(kp.append_digit(0));
        CHECK(std::string(kp.buf) == "6");
    }

    SECTION("Decimals still reachable past the integer cap") {
        REQUIRE(kp.append_digit(0));
        REQUIRE(kp.append_dot());
        REQUIRE(kp.append_digit(8));
        REQUIRE(kp.append_digit(5));
        CHECK(std::string(kp.buf) == "0.85");
        CHECK_FALSE(kp.append_digit(1)); // 3 total digits reached
        CHECK(kp.value() == Catch::Approx(0.85f));
    }
}

TEST_CASE("KeypadInput can refuse the decimal point entirely", "[keypad]") {
    // allow_decimal=false in ui_keypad_config_t was stored but never enforced,
    // so an integer-only field accepted "25.5" and then truncated it.
    KeypadInput kp;
    kp.allow_dot = false;

    SECTION("Dot is rejected") {
        kp.append_digit(2);
        CHECK_FALSE(kp.append_dot());
        CHECK(std::string(kp.buf) == "2");
    }

    SECTION("Digits are unaffected") {
        REQUIRE(kp.append_digit(2));
        REQUIRE(kp.append_digit(5));
        CHECK(kp.value() == Catch::Approx(25.0f));
    }

    SECTION("Dot is allowed by default") {
        KeypadInput permissive;
        permissive.append_digit(2);
        CHECK(permissive.append_dot());
    }
}
