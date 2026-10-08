// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_usb_copy_name.cpp
 * @brief Naming a USB file's copy in Moonraker's usb_prints folder.
 *
 * Run with: ./build/bin/helix-tests "[usb][usb_copy_name]"
 */

#include "print_file_data.h"

#include "../catch_amalgamated.hpp"

using helix::choose_usb_copy_target;

TEST_CASE("choose_usb_copy_target never replaces a different file", "[usb][usb_copy_name]") {
    SECTION("absent: the file's own name, uploaded") {
        const auto t = choose_usb_copy_target("part.gcode", 100, {});
        CHECK(t.name == "part.gcode");
        CHECK_FALSE(t.reuse);
    }

    SECTION("present with the same size: reused") {
        const auto t = choose_usb_copy_target("part.gcode", 100, {{"part.gcode", 100}});
        CHECK(t.name == "part.gcode");
        CHECK(t.reuse);
    }

    SECTION("present with another size: the first suffix") {
        const auto t = choose_usb_copy_target("part.gcode", 100, {{"part.gcode", 99}});
        CHECK(t.name == "part (2).gcode");
        CHECK_FALSE(t.reuse);
    }

    SECTION("suffix (2) holding another file too: (3)") {
        const auto t =
            choose_usb_copy_target("part.gcode", 100, {{"part.gcode", 99}, {"part (2).gcode", 98}});
        CHECK(t.name == "part (3).gcode");
        CHECK_FALSE(t.reuse);
    }

    SECTION("an earlier suffixed copy of the same size is reused") {
        const auto t = choose_usb_copy_target("part.gcode", 100,
                                              {{"part.gcode", 99}, {"part (2).gcode", 100}});
        CHECK(t.name == "part (2).gcode");
        CHECK(t.reuse);
    }

    SECTION("the suffix goes before the last extension only") {
        const auto t = choose_usb_copy_target("cube.v2.gcode", 1, {{"cube.v2.gcode", 2}});
        CHECK(t.name == "cube.v2 (2).gcode");
    }

    SECTION("no extension") {
        const auto t = choose_usb_copy_target("part", 1, {{"part", 2}});
        CHECK(t.name == "part (2)");
    }
}
