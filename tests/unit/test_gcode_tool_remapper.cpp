// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/text_io.h"
#include "gcode_tool_remapper.h"
#include "test_helpers/unique_temp_dir.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#include "../catch_amalgamated.hpp"

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Split into physical lines (drop the trailing empty element from a final newline).
static std::vector<std::string> to_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
    return lines;
}

TEST_CASE("U1 remap rewrites all three command families", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}}; // logical tool 1 -> physical head 2
    std::string in = slurp("assets/test_gcodes/u1_4color_ring.gcode");
    REQUIRE(!in.empty()); // fixture found
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);

    // body Tn: no bare "T1" line remains; T0 lines untouched
    CHECK(out.find("\nT1\n") == std::string::npos);
    CHECK(out.find("\nT0\n") != std::string::npos);

    // prestart family: every EXECUTABLE SM_PRINT_* command line has been rewritten
    // away from EXTRUDER=1. As with the temp family, a global substring scan is NOT
    // a valid contract -- the fixture's "; machine_start_gcode = ..." comment line
    // embeds the literal "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1" inside a comment,
    // and a conservative remapper must not touch comments. So we assert per command
    // line (lines that actually START with SM_PRINT_*, not comment text).
    bool saw_auto_feed_0 = false;
    for (const auto& line : to_lines(out)) {
        if (line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_FLOW_CALIBRATE EXTRUDER=", 0) == 0) {
            CHECK(line.rfind("SM_PRINT_FLOW_CALIBRATE EXTRUDER=1", 0) != 0);
        }
        if (line.rfind("SM_PRINT_AUTO_FEED EXTRUDER=0", 0) == 0) {
            saw_auto_feed_0 = true; // head 0 untouched
        }
    }
    CHECK(saw_auto_feed_0);

    // temp family: no M104/M109 COMMAND line retains a "T1" tool token.
    // NOTE: a global out.find(" T1 ") scan is NOT a valid contract here -- the
    // captured fixture embeds tool tokens inside comments (e.g. the
    // "; machine_start_gcode = ... S0 T1 A0 ..." escaped block on one physical
    // line, and "M104 S220 T1 ; preheat T1 time: 30s" where the comment text
    // also contains "T1"). A conservative remapper must NOT rewrite comments,
    // so we assert the precise behavioral truth: the executable portion of every
    // M104/M109 line carries the remapped head, not the original.
    for (const auto& line : to_lines(out)) {
        if (line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0) {
            continue;
        }
        std::string code = line.substr(0, line.find(';')); // strip trailing comment
        // strip trailing whitespace so " T1" at EOL is caught regardless of \r/spaces
        while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) {
            code.pop_back();
        }
        CHECK(code.find(" T1 ") == std::string::npos);
        if (code.size() >= 3) {
            CHECK(code.substr(code.size() - 3) != " T1"); // no tool token at end of command
        }
    }
    // and the remap target landed on at least one temp line
    CHECK(out.find("M109 S220 T2") != std::string::npos);
}

TEST_CASE("remap is collision-safe for a swap", "[remap][gcode]") {
    // each line mapped from its ORIGINAL index in a single pass: 1<->2 swap must not chain
    std::string in = "T1\nT2\nSM_PRINT_AUTO_FEED EXTRUDER=1\nSM_PRINT_AUTO_FEED EXTRUDER=2\n";
    std::map<int, int> remap = {{1, 2}, {2, 1}};
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);
    CHECK(out == "T2\nT1\nSM_PRINT_AUTO_FEED EXTRUDER=2\nSM_PRINT_AUTO_FEED EXTRUDER=1\n");
}

TEST_CASE("unmapped indices and unrelated lines are untouched", "[remap][gcode]") {
    std::string in = "T0\nT3\nG1 X10 Y10\nM104 S200 T0\n";
    std::map<int, int> remap = {{1, 2}}; // nothing matches
    CHECK(helix::GcodeToolRemapper::apply_to_string(in, remap) == in);
}

TEST_CASE("temp token remapped in all positions", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}};
    // token mid-line, token at EOL, token before comment
    std::string in = "M104 T1 S140\n"
                     "M109 S220 T1\n"
                     "M104 S70 T1 ; set nozzle temperature ;cooldown\n";
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, remap);
    CHECK(out == "M104 T2 S140\n"
                 "M109 S220 T2\n"
                 "M104 S70 T2 ; set nozzle temperature ;cooldown\n");
}

TEST_CASE("comment lines containing tool tokens are not rewritten", "[remap][gcode]") {
    std::map<int, int> remap = {{1, 2}};
    std::string in = "; Change Tool1 -> Tool0\n"
                     "; machine_start_gcode = ...M104 S0 T1 A0...\n"
                     "G28\n";
    // none of these are bare-Tn / SM_PRINT_ / M10x command lines -> unchanged
    CHECK(helix::GcodeToolRemapper::apply_to_string(in, remap) == in);
}

// ---------------------------------------------------------------------------
// File form: the production rewrite path. Peak memory is one line, so the
// oracle below is what guarantees a 400MB job gets the same bytes a 4KB one does.
// ---------------------------------------------------------------------------

namespace {
// Runs `in_text` through apply_to_file() via real files, and checks the
// whole-content form gives the same bytes.
std::string stream_remap(const std::string& in_text, const std::map<int, int>& remap,
                         size_t* changed_out = nullptr) {
    const std::string in_path = helix::test::unique_temp_file("helix_remap_in", "gcode");
    const std::string out_path = helix::test::unique_temp_file("helix_remap_out", "gcode");
    REQUIRE(helix::text_io::write_file(in_path, in_text));
    std::optional<size_t> changed =
        helix::GcodeToolRemapper::apply_to_file(in_path, out_path, remap);
    std::string out = helix::text_io::read_file(out_path).value_or("<unreadable>");
    std::error_code ec;
    std::filesystem::remove(in_path, ec);
    std::filesystem::remove(out_path, ec);
    REQUIRE(changed.has_value());
    if (changed_out != nullptr) {
        *changed_out = *changed;
    }
    CHECK(helix::GcodeToolRemapper::apply_to_string(in_text, remap) == out);
    return out;
}
} // namespace

TEST_CASE("streaming a real file on disk rewrites every changed line and nothing else",
          "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    const std::string src = "assets/test_gcodes/u1_4color_ring.gcode";
    std::string original = slurp(src);
    REQUIRE(!original.empty());

    const std::string out_path = helix::test::unique_temp_file("helix_stream_remap", "gcode");
    std::optional<size_t> result = helix::GcodeToolRemapper::apply_to_file(src, out_path, remap);
    REQUIRE(result.has_value());
    const size_t changed = *result;
    std::string rewritten = slurp(out_path);
    std::error_code ec;
    std::filesystem::remove(out_path, ec);
    CHECK(rewritten == helix::GcodeToolRemapper::apply_to_string(original, remap));

    // The rewrite is line-for-line, so the file keeps its shape whatever else
    // changed. A count that drifts from the line count means a line was
    // dropped or split.
    CHECK(to_lines(rewritten).size() == to_lines(original).size());
    CHECK(changed > 0);

    // Every executable line that named tool 1 now names tool 2, and every line
    // that named neither is byte-identical to where it started.
    auto orig_lines = to_lines(original);
    auto new_lines = to_lines(rewritten);
    REQUIRE(orig_lines.size() == new_lines.size());
    size_t differing = 0;
    for (size_t i = 0; i < orig_lines.size(); ++i) {
        if (orig_lines[i] != new_lines[i]) {
            ++differing;
            INFO("line " << (i + 1) << ": '" << orig_lines[i] << "' -> '" << new_lines[i] << "'");
            // A changed line only ever moves 1 -> 2; nothing else may move.
            CHECK(orig_lines[i].find('1') != std::string::npos);
            CHECK(new_lines[i].find('2') != std::string::npos);
        }
    }
    CHECK(differing == changed);
    CHECK(new_lines[0] == orig_lines[0]); // the header comment is untouched
}

TEST_CASE("streaming rewrite preserves a missing final newline", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // A last line carrying no newline must not gain one: the slicer footer this
    // file ends in is parsed by byte offset, and a stray byte moves all of it.
    CHECK(stream_remap("G28\nT1", remap) == "G28\nT2");
    CHECK(stream_remap("G28\nT1\n", remap) == "G28\nT2\n");
    CHECK(stream_remap("T1", remap) == "T2");
    CHECK(stream_remap("", remap).empty());
    CHECK(stream_remap("\n", remap) == "\n");
}

TEST_CASE("file rewrite reports failure instead of a partial file", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    const std::string missing_dir = helix::test::unique_temp_dir("helix_remap_nodir");
    CHECK_FALSE(helix::GcodeToolRemapper::apply_to_file(missing_dir + "/in.gcode",
                                                        missing_dir + "/out.gcode", remap)
                    .has_value());

    const std::string in_path = helix::test::unique_temp_file("helix_remap_in", "gcode");
    REQUIRE(helix::text_io::write_file(in_path, "T1\n"));
    CHECK_FALSE(helix::GcodeToolRemapper::apply_to_file(in_path, missing_dir + "/out.gcode", remap)
                    .has_value());
    std::error_code ec;
    std::filesystem::remove(in_path, ec);
}

TEST_CASE("streaming rewrite preserves CRLF line endings", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // The \r belongs to the line, not to the separator. A rewritten line that
    // dropped it would silently change every byte offset after it.
    CHECK(stream_remap("G28\r\nT1\r\nG1 X1\r\n", remap) == "G28\r\nT2\r\nG1 X1\r\n");
}

TEST_CASE("multi-digit tool numbers are remapped, not truncated", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{10, 3}, {2, 11}};
    CHECK(stream_remap("T10\nT2\nT1\n", remap) == "T3\nT11\nT1\n");
    // T1 is a different tool from T10 and must not be caught by a prefix match.
    CHECK(stream_remap("M109 S220 T10\n", remap) == "M109 S220 T3\n");
}

TEST_CASE("streaming rewrite counts only the lines it changed", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    size_t changed = 99;
    stream_remap("T0\nT1\nG1 X1\nM109 S220 T1\n", remap, &changed);
    CHECK(changed == 2);

    // A count of zero is the signal the print path uses to skip the temp copy
    // and print the original, so an identity remap has to reach it.
    changed = 99;
    stream_remap("T0\nT1\nG1 X1\n", {{1, 1}}, &changed);
    CHECK(changed == 0);

    // So does a remap naming a tool the file never uses.
    changed = 99;
    stream_remap("T0\nG1 X1\n", {{4, 5}}, &changed);
    CHECK(changed == 0);
}

TEST_CASE("a tool change the preview counts is a tool change the remapper rewrites",
          "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    CHECK(stream_remap("T1 ; tool change\n", remap) == "T2 ; tool change\n");
    CHECK(stream_remap("  T1\n", remap) == "  T2\n");
    CHECK(stream_remap("\tT1;next\r\n", remap) == "\tT2;next\r\n");
}

TEST_CASE("a tool change's macro parameters survive the rewrite byte for byte",
          "[remap][gcode][stream][1848]") {
    std::map<int, int> remap = {{0, 2}};
    CHECK(stream_remap("T0 PURGE_LENGTH=121.399 NEW_EXTRUDER_TEMP=222\n", remap) ==
          "T2 PURGE_LENGTH=121.399 NEW_EXTRUDER_TEMP=222\n");
    CHECK(stream_remap("  T0\tPURGE_LENGTH=0 ; change\r\n", remap) ==
          "  T2\tPURGE_LENGTH=0 ; change\r\n");
    // A G-code word among the parameters keeps the line out of the rewrite.
    CHECK(stream_remap("T0 PURGE_LENGTH=1 X1\n", remap) == "T0 PURGE_LENGTH=1 X1\n");
}

TEST_CASE("near-miss tokens are left alone", "[remap][gcode][stream]") {
    std::map<int, int> remap = {{1, 2}};
    // Each of these is a corrupted print if the match is ever loosened: a
    // parameterised toolchange, a macro whose name starts with T, a temperature
    // line with no tool token, and a filename that contains one.
    CHECK(stream_remap("T1X\n", remap) == "T1X\n");
    CHECK(stream_remap("TOOL\n", remap) == "TOOL\n");
    CHECK(stream_remap("M104 S200\n", remap) == "M104 S200\n");
    CHECK(stream_remap("; printing T1_bracket.gcode\n", remap) == "; printing T1_bracket.gcode\n");
}

// ---------------------------------------------------------------------------
// INITIAL_TOOL: the one start-macro parameter whose meaning is fixed by the
// slicer's own placeholder name, so it is rewritten like a T line.
// ---------------------------------------------------------------------------

TEST_CASE("INITIAL_TOOL is remapped on a start macro line", "[remap][gcode]") {
    std::map<int, int> remap = {{0, 1}};
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START INITIAL_TOOL=0 BED_TEMP=60\n",
                                                    remap) ==
          "PRINT_START INITIAL_TOOL=1 BED_TEMP=60\n");
    // Klipper params are case-insensitive, so the key is too; its spelling survives.
    CHECK(helix::GcodeToolRemapper::apply_to_string("START_PRINT initial_tool=0\n", remap) ==
          "START_PRINT initial_tool=1\n");
    // Any macro carrying it, and multi-digit values.
    CHECK(
        helix::GcodeToolRemapper::apply_to_string("MY_START BED=60 INITIAL_TOOL=10\n", {{10, 3}}) ==
        "MY_START BED=60 INITIAL_TOOL=3\n");
}

TEST_CASE("INITIAL_TOOL and T lines swap without chaining", "[remap][gcode]") {
    std::map<int, int> remap = {{0, 1}, {1, 0}};
    std::string in = "PRINT_START INITIAL_TOOL=0\nT0\nT1\nPRINT_START INITIAL_TOOL=1\n";
    CHECK(helix::GcodeToolRemapper::apply_to_string(in, remap) ==
          "PRINT_START INITIAL_TOOL=1\nT1\nT0\nPRINT_START INITIAL_TOOL=0\n");
}

TEST_CASE("INITIAL_TOOL near-misses and comments are left alone", "[remap][gcode]") {
    std::map<int, int> remap = {{0, 1}};
    for (const char* line : {
             "PRINT_START XINITIAL_TOOL=0\n",         // longer key that ends the same way
             "PRINT_START INITIAL_TOOL=abc\n",        // not a number
             "PRINT_START INITIAL_TOOL=\n",           // no value
             "; PRINT_START INITIAL_TOOL=0\n",        // comment line
             "PRINT_START BED=60 ; INITIAL_TOOL=0\n", // comment tail
             "PRINT_START INITIAL_TOOL=2\n",          // unmapped value, byte-identical
         }) {
        CAPTURE(line);
        CHECK(helix::GcodeToolRemapper::apply_to_string(line, remap) == line);
    }
    // The comment tail survives a rewrite of the parameter before it.
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START INITIAL_TOOL=0 ; INITIAL_TOOL=0\n",
                                                    remap) ==
          "PRINT_START INITIAL_TOOL=1 ; INITIAL_TOOL=0\n");
}

// The line and the body a MedusaHC user sliced: tool 0 only, remapped to tool 1.
static const char* const kOrcaToolchangerStart =
    "PRINT_START INITIAL_TOOL=0 INITIAL_TEMP=230 EXTRUDER_TEMP=150 EXTRUDER1_TEMP=0 "
    "EXTRUDER2_TEMP=0 EXTRUDER3_TEMP=0 EXTRUDER4_TEMP=0 EXTRUDER5_TEMP=0 BED_TEMP=60";

TEST_CASE("a single-tool Orca job remaps both its start line and its tool change",
          "[remap][gcode]") {
    const std::string start = kOrcaToolchangerStart;
    std::string in = start + "\nG90\nT0\nG1 X1\n";
    std::string out = helix::GcodeToolRemapper::apply_to_string(in, {{0, 1}});
    std::string expected_start = start;
    expected_start.replace(expected_start.find("INITIAL_TOOL=0"), 14, "INITIAL_TOOL=1");
    CHECK(out == expected_start + "\nG90\nT1\nG1 X1\n");
}

TEST_CASE("unremapped_tool_params names what the rewrite leaves on a line", "[remap][gcode]") {
    using helix::GcodeToolRemapper;
    using V = std::vector<std::string>;

    CHECK(GcodeToolRemapper::unremapped_tool_params(kOrcaToolchangerStart) ==
          V{"EXTRUDER_TEMP", "EXTRUDER1_TEMP", "EXTRUDER2_TEMP", "EXTRUDER3_TEMP", "EXTRUDER4_TEMP",
            "EXTRUDER5_TEMP"});

    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START BED_TEMP=60 CHAMBER=40").empty());
    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START INITIAL_TOOL=0").empty());
    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START T0_TEMP=220 T1=0 TX=4") ==
          V{"T0_TEMP", "T1"});
    // TOOL= is rewritten, so it is not reported; keys that merely contain TOOL are.
    CHECK(GcodeToolRemapper::unremapped_tool_params("START_PRINT tool=2 Extruder=210") ==
          V{"Extruder"});
    CHECK(GcodeToolRemapper::unremapped_tool_params("START_PRINT TOOL_TEMP=220 TOOLS=3 XTOOL=1") ==
          V{"TOOL_TEMP", "TOOLS", "XTOOL"});
    // A bare T= names a tool (MedusaHC's own SET/MHC_SET take it) and is not rewritten.
    CHECK(GcodeToolRemapper::unremapped_tool_params("MHC_SET T=1 BED=60") == V{"T"});
    // A rewritten key whose value is a tool NAME is left by the rewrite, so it is reported.
    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START TOOL=T0 INITIAL_TOOL=0") ==
          V{"TOOL"});
    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START initial_tool=T1") ==
          V{"initial_tool"});
    // Nothing after a comment, and a bare word is not a parameter.
    CHECK(GcodeToolRemapper::unremapped_tool_params("PRINT_START BED=60 ; EXTRUDER_TEMP=200")
              .empty());
    CHECK(GcodeToolRemapper::unremapped_tool_params("EXTRUDER_TEMP").empty());
}

TEST_CASE("TOOL is remapped like INITIAL_TOOL", "[remap][gcode]") {
    std::map<int, int> remap = {{0, 1}, {1, 0}};
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START TOOL=0 BED=60\n", remap) ==
          "PRINT_START TOOL=1 BED=60\n");
    CHECK(helix::GcodeToolRemapper::apply_to_string("SET_TOOL_TEMPERATURE tool=1 TARGET=200\n",
                                                    remap) ==
          "SET_TOOL_TEMPERATURE tool=0 TARGET=200\n");
    // Swap does not chain, across lines or families.
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START TOOL=0\nT1\nPRINT_START TOOL=1\n",
                                                    remap) ==
          "PRINT_START TOOL=1\nT0\nPRINT_START TOOL=0\n");
    // Both parameters on one line are rewritten, each from its own original value.
    CHECK(helix::GcodeToolRemapper::apply_to_string(
              "PRINT_START INITIAL_TOOL=0 BED=60 TOOL=1 ; TOOL=0\n", remap) ==
          "PRINT_START INITIAL_TOOL=1 BED=60 TOOL=0 ; TOOL=0\n");
    // Keys that only contain TOOL are someone else's convention.
    for (const char* line :
         {"PRINT_START XTOOL=0\n", "PRINT_START TOOLS=0\n", "PRINT_START TOOL_TEMP=0\n",
          "PRINT_START TOOL=T0\n", "PRINT_START TOOL=0x\n"}) {
        CAPTURE(line);
        CHECK(helix::GcodeToolRemapper::apply_to_string(line, remap) == line);
    }
}

TEST_CASE("tool parameters are rewritten across CRLF and tab separators", "[remap][gcode]") {
    std::map<int, int> remap = {{0, 1}};
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START TOOL=0\r\nT0\r\n", remap) ==
          "PRINT_START TOOL=1\r\nT1\r\n");
    CHECK(helix::GcodeToolRemapper::apply_to_string("PRINT_START\tINITIAL_TOOL=0\tBED=60\tTOOL=0\n",
                                                    remap) ==
          "PRINT_START\tINITIAL_TOOL=1\tBED=60\tTOOL=1\n");
}
