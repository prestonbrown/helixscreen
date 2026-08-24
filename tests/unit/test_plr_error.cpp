// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/snapmaker_resume.h"

#include "../catch_amalgamated.hpp"

using helix::snapmaker_extract_coded_msg;

TEST_CASE("snapmaker_extract_coded_msg: well-formed coded JSON extracts msg", "[plr][error]") {
    std::string raw = R"({"coded":"0001-0531-0000-0005","msg":"Printer is not idle, cannot )"
                      R"(restore power loss print","action":"none"})";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") ==
            "Printer is not idle, cannot restore power loss print");
}

TEST_CASE("snapmaker_extract_coded_msg: 0006 clear-env variant extracts msg", "[plr][error]") {
    std::string raw = R"({"coded":"0001-0531-0000-0006","msg":"Printer is printing, cannot )"
                      R"(clear power loss env","action":"none"})";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") ==
            "Printer is printing, cannot clear power loss env");
}

TEST_CASE("snapmaker_extract_coded_msg: JSON embedded in a longer gcode error string",
          "[plr][error]") {
    std::string raw = R"(!! Error executing script SDCARD_PRINT_PL_RESTORE: )"
                      R"({"coded":"0001-0531-0000-0005","msg":"Printer is not idle, cannot )"
                      R"(restore power loss print","action":"none"} (gcode line 42))";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") ==
            "Printer is not idle, cannot restore power loss print");
}

TEST_CASE("snapmaker_extract_coded_msg: plain non-JSON error falls back", "[plr][error]") {
    std::string raw = "Invalid z_info or stepper position data";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") == "fallback");
}

TEST_CASE("snapmaker_extract_coded_msg: empty string falls back", "[plr][error]") {
    REQUIRE(snapmaker_extract_coded_msg("", "fallback") == "fallback");
}

TEST_CASE("snapmaker_extract_coded_msg: malformed JSON falls back", "[plr][error]") {
    std::string raw = R"({"coded":"0001-0531-0000-0005","msg":"unterminated string)";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") == "fallback");
}

TEST_CASE("snapmaker_extract_coded_msg: JSON object without msg falls back", "[plr][error]") {
    std::string raw = R"({"coded":"0001-0531-0000-0005","action":"none"})";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") == "fallback");
}

TEST_CASE("snapmaker_extract_coded_msg: msg field present but non-string falls back",
          "[plr][error]") {
    std::string raw = R"({"coded":"0001-0531-0000-0005","msg":42,"action":"none"})";
    REQUIRE(snapmaker_extract_coded_msg(raw, "fallback") == "fallback");
}
