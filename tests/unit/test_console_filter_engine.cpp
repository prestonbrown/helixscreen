// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console_filter_engine.h"
#include "printer_detector.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::ConsoleFilterEngine;

namespace {

// Real samples captured from a Creality K2 Plus running stock firmware (Apr 2026).
// These are the format strings emitted by motor_control_wrapper.cpython-39.so via
// gcode.respond_info() — Klipper prepends "// " when broadcasting on the GCode console.
constexpr const char* K2_NOISE_SAMPLES[] = {
    "// data_send:b'\\x83\\x05\\x00\\x06\\x02F' "
    "recv_result:b'\\xf7\\x83\\x07\\x00\\x06\\x00\\x00\\x96C\\t'",
    "// "
    "sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c-\xe8\xaf\xbb\xe5\x8f\x96:b'"
    "\\x00\\x00\\x96C' value:300.0",
    "// "
    "sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c-"
    "\xe5\x86\x99\xe5\x85\xa5\xe6\x88\x90\xe5\x8a\x9f:b'F' value:100.0",
    "// send float_bytes:b'\\x00\\x00\\xc8B'",
    "// recv_result:b'\\xf7\\x83\\x07\\x00\\x06B`\\xe5<\\x9c'",
};

// Lines that must NEVER be filtered (real GCode responses).
constexpr const char* GCODE_KEEP_SAMPLES[] = {
    "ok",
    "ok T:210.0 /210.0 B:60.0 /60.0",
    "// probe at 50.000,50.000 is z=0.123",
    "!! Move out of range",
    "Klipper state: Printer is ready",
    "// Done",
};

} // namespace

// ============================================================================
// Pattern parsing
// ============================================================================

TEST_CASE("ConsoleFilter: parse accepts well-formed specs", "[console_filter]") {
    ConsoleFilterEngine::Type type;
    std::string text;

    REQUIRE(ConsoleFilterEngine::parse("prefix:// foo", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Prefix);
    CHECK(text == "// foo");

    REQUIRE(ConsoleFilterEngine::parse("substring:LOAD_AI_DEAL", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Substring);
    CHECK(text == "LOAD_AI_DEAL");

    REQUIRE(ConsoleFilterEngine::parse("regex:^// config_ioRemap", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Regex);
    CHECK(text == "^// config_ioRemap");
}

TEST_CASE("ConsoleFilter: parse rejects malformed specs", "[console_filter]") {
    ConsoleFilterEngine::Type type;
    std::string text;

    CHECK_FALSE(ConsoleFilterEngine::parse("", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("nocolon", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse(":missing-kind", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("prefix:", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("unknown_kind:foo", type, text));
}

// ============================================================================
// Add / remove
// ============================================================================

TEST_CASE("ConsoleFilter: add accepts valid, rejects invalid", "[console_filter]") {
    ConsoleFilterEngine eng;
    CHECK(eng.add("prefix:// data_send:"));
    CHECK(eng.add("substring:LOAD_AI_DEAL"));
    CHECK(eng.add("regex:^// config_ioRemap"));
    CHECK(eng.size() == 3);

    CHECK_FALSE(eng.add("not-a-spec"));
    CHECK_FALSE(eng.add("regex:[invalid("));
    CHECK(eng.size() == 3);
}

TEST_CASE("ConsoleFilter: remove drops only exact (type, text) match", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("substring:// data_send:"); // same text, different type
    eng.add("prefix:// recv_result:");
    REQUIRE(eng.size() == 3);

    eng.remove("prefix:// data_send:");
    CHECK(eng.size() == 2);

    // The substring entry with the same text remains.
    CHECK(eng.should_filter("xx // data_send:foo"));
}

TEST_CASE("ConsoleFilter: empty engine never filters", "[console_filter]") {
    ConsoleFilterEngine eng;
    for (const char* line : K2_NOISE_SAMPLES) {
        CHECK_FALSE(eng.should_filter(line));
    }
}

// ============================================================================
// Matching semantics
// ============================================================================

TEST_CASE("ConsoleFilter: prefix match anchored at start only", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// foo");
    CHECK(eng.should_filter("// foo bar"));
    CHECK_FALSE(eng.should_filter("xx // foo bar"));
    CHECK_FALSE(eng.should_filter("// fo"));
}

TEST_CASE("ConsoleFilter: substring match anywhere", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("substring:LOAD_AI_DEAL");
    CHECK(eng.should_filter("// LOAD_AI_DEAL: ..."));
    CHECK(eng.should_filter("xxx LOAD_AI_DEAL xxx"));
    CHECK_FALSE(eng.should_filter("// load_ai_deal"));
}

TEST_CASE("ConsoleFilter: regex match", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("regex:^// config_ioRemap oid=[0-9]+");
    CHECK(eng.should_filter("// config_ioRemap oid=42 src_pin=PB1"));
    CHECK_FALSE(eng.should_filter("// config_ioRemap"));
    CHECK_FALSE(eng.should_filter("xx // config_ioRemap oid=1"));
}

// ============================================================================
// K2 noise preset (the original motivating use case)
// ============================================================================

TEST_CASE("ConsoleFilter: K2 firmware-noise preset suppresses all known samples",
          "[console_filter][k2]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("prefix:// recv_result:");
    eng.add("prefix:// send float_bytes:");
    // U+0073 prefixes plus the Chinese 系/参/数/操/作 prefix bytes — anything starting
    // with "// sys" + Chinese characters that the firmware uses for these messages.
    eng.add("prefix:// sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c");

    for (const char* sample : K2_NOISE_SAMPLES) {
        INFO("sample: " << sample);
        CHECK(eng.should_filter(sample));
    }
}

TEST_CASE("ConsoleFilter: K2 preset never suppresses real GCode responses",
          "[console_filter][k2]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("prefix:// recv_result:");
    eng.add("prefix:// send float_bytes:");
    eng.add("prefix:// sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c");

    for (const char* line : GCODE_KEEP_SAMPLES) {
        INFO("line: " << line);
        CHECK_FALSE(eng.should_filter(line));
    }
}

// ============================================================================
// Shipped preset — resolved from assets/config/printer_database.json through the
// same accessor the app uses. This used to be a hand-copied array carrying a
// "must stay in sync with printer_database.json" comment; it had already drifted
// (it still listed "prefix:// recv_result:", which can never match because
// recv_result: only ever appears mid-line). Reading the real database means the
// test fails when the shipped patterns regress, instead of quietly agreeing with
// a stale copy.
// ============================================================================

namespace {
/// Build an engine from a printer's shipped patterns, failing loudly if the
/// database no longer has any for that model.
ConsoleFilterEngine shipped_engine_for(const char* printer_name) {
    ConsoleFilterEngine eng;
    const std::vector<std::string> specs =
        PrinterDetector::get_console_filter_patterns(printer_name);
    INFO("printer: " << printer_name);
    REQUIRE_FALSE(specs.empty());
    for (const auto& spec : specs) {
        INFO("spec: " << spec);
        REQUIRE(eng.add(spec));
    }
    return eng;
}
} // namespace

TEST_CASE("ConsoleFilter: shipped K2 preset filters all known noise samples",
          "[console_filter][k2][preset]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K2 Plus");
    for (const char* sample : K2_NOISE_SAMPLES) {
        INFO("sample: " << sample);
        CHECK(eng.should_filter(sample));
    }
}

TEST_CASE("ConsoleFilter: shipped K2 preset preserves real GCode responses",
          "[console_filter][k2][preset]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K2 Plus");
    for (const char* line : GCODE_KEEP_SAMPLES) {
        INFO("line: " << line);
        CHECK_FALSE(eng.should_filter(line));
    }
}

TEST_CASE("ConsoleFilter: K2 preset filters belt_mdl / io_remap / load_ai chatter",
          "[console_filter][k2][preset]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K2 Plus");
    // Real samples emitted via gcode.respond_info() in Creality's extras modules.
    CHECK(eng.should_filter("// MDL_NAME: belt_x"));
    CHECK(eng.should_filter("// ACK_mdl_info"));
    CHECK(eng.should_filter("// ACK_mdl_pos"));
    CHECK(eng.should_filter("// halversion: 1.2.3"));
    CHECK(eng.should_filter("// softversion: 4.5.6"));
    CHECK(eng.should_filter("// reset:start error"));
    CHECK(eng.should_filter("// times:42"));
    CHECK(eng.should_filter("// config_ioRemap oid=1 src_pin=PB1"));
    CHECK(eng.should_filter("// operation_ioRemap oid=1 operation=2"));
    CHECK(eng.should_filter("// ai_switch = 1, ai_waste_switch = 0"));
    CHECK(eng.should_filter("// WILL LOAD_AI_DEAL"));
    CHECK(eng.should_filter("// LOAD_AI_DEAL: response"));
    // The tightened "times:" regex must NOT eat user macros.
    CHECK_FALSE(eng.should_filter("// times: completed"));
    CHECK_FALSE(eng.should_filter("// times: "));
    CHECK_FALSE(eng.should_filter("// times:"));
}

TEST_CASE("ConsoleFilter: K2 preset covers the RS-485 payload dumps by substring",
          "[console_filter][k2][preset]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K2 Plus");
    // 187 of 220 lines in a real K2 capture carry a Python bytes repr. Matching the
    // repr itself covers the whole bus family without a prefix per message tag.
    CHECK(eng.should_filter("// get motor addr:b'\\x01'"));
    CHECK(eng.should_filter("// get motor extruder addr:b'\\x02'"));
    CHECK(eng.should_filter("// read vals:b'\\xff\\xff'"));
    // ...and must not swallow ordinary output that merely mentions a byte count.
    CHECK_FALSE(eng.should_filter("// EEPROM read complete"));
}

TEST_CASE("ConsoleFilter: K1 family gets the Creality macro-chatter set",
          "[console_filter][k1][preset]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K1C");
    // Real samples from a 1000-entry K1C gcode_store capture.
    CHECK(eng.should_filter("// can_break_flag = 0"));
    CHECK(eng.should_filter("// x_axes: xyz"));
    CHECK(eng.should_filter("// x_axes is NULL"));
    CHECK(eng.should_filter("// wait temp start"));
    CHECK(eng.should_filter("// wait temp end"));
    CHECK(eng.should_filter("// x_park = -104.5"));
    CHECK(eng.should_filter("// y_park = 104.5"));
    CHECK(eng.should_filter("// move xy"));
    CHECK(eng.should_filter("// [CLEAR_NOZZLE_QUICK] src_pos[0]:10.0"));
    CHECK(eng.should_filter("// Run Current: 0.56A Hold Current: 0.56A"));
    CHECK(eng.should_filter("// pressure_advance: 0.02"));
}

TEST_CASE("ConsoleFilter: mesh results stay visible on every shipped printer",
          "[console_filter][preset]") {
    // klipper_verbose_probe exists as a set but is deliberately referenced by no
    // printer: per-point probe output is stock Klipper and is exactly what you
    // want to read while debugging a mesh.
    for (const char* printer : {"Creality K2 Plus", "Creality K1C"}) {
        ConsoleFilterEngine eng = shipped_engine_for(printer);
        INFO("printer: " << printer);
        CHECK_FALSE(eng.should_filter("// probe at 50.000,50.000 is z=0.123"));
        CHECK_FALSE(eng.should_filter("// Mesh Bed Leveling Complete"));
        CHECK_FALSE(eng.should_filter("// Bed Mesh state has been saved to profile [default]"));
    }
}

TEST_CASE("ConsoleFilter: unknown printer resolves to no patterns", "[console_filter][preset]") {
    CHECK(PrinterDetector::get_console_filter_patterns("No Such Printer 9000").empty());
    CHECK(PrinterDetector::get_console_filter_patterns("").empty());
}

TEST_CASE("ConsoleFilter: shared sets resolve identically for both K2 models",
          "[console_filter][preset]") {
    // Both reference the same two sets; that is the whole point of naming them.
    const auto plus = PrinterDetector::get_console_filter_patterns("Creality K2 Plus");
    const auto pro = PrinterDetector::get_console_filter_patterns("Creality K2 Pro");
    CHECK_FALSE(plus.empty());
    CHECK(plus == pro);
}

TEST_CASE("ConsoleFilter: resolved patterns contain no duplicates", "[console_filter][preset]") {
    for (const char* printer : {"Creality K2 Plus", "Creality K1C"}) {
        INFO("printer: " << printer);
        auto specs = PrinterDetector::get_console_filter_patterns(printer);
        const std::size_t before = specs.size();
        std::sort(specs.begin(), specs.end());
        specs.erase(std::unique(specs.begin(), specs.end()), specs.end());
        CHECK(specs.size() == before);
    }
}

TEST_CASE("ConsoleFilter: patterns are evaluated cheapest matcher first", "[console_filter]") {
    ConsoleFilterEngine eng;
    // Added worst-first on purpose; the engine must reorder.
    REQUIRE(eng.add("regex:^// zzz[0-9]"));
    REQUIRE(eng.add("substring:middle"));
    REQUIRE(eng.add("prefix:// aaa"));
    REQUIRE(eng.size() == 3);

    const auto types = eng.types_in_order();
    REQUIRE(types.size() == 3);
    CHECK(types[0] == ConsoleFilterEngine::Type::Prefix);
    CHECK(types[1] == ConsoleFilterEngine::Type::Substring);
    CHECK(types[2] == ConsoleFilterEngine::Type::Regex);

    // Reordering must not change what matches.
    CHECK(eng.should_filter("// aaa here"));
    CHECK(eng.should_filter("has middle in it"));
    CHECK(eng.should_filter("// zzz9"));
    CHECK_FALSE(eng.should_filter("// bbb"));
}

TEST_CASE("ConsoleFilter: shipped presets keep regex to a minimum", "[console_filter][preset]") {
    // std::regex costs ~200x a prefix compare per line and this runs on the LVGL
    // main thread. Presets should reach for it only when prefix/substring cannot
    // express the shape; if this trips, check whether a cheaper matcher would do.
    for (const char* printer : {"Creality K2 Plus", "Creality K1C"}) {
        ConsoleFilterEngine eng = shipped_engine_for(printer);
        const auto types = eng.types_in_order();
        const auto regexes = static_cast<std::size_t>(
            std::count(types.begin(), types.end(), ConsoleFilterEngine::Type::Regex));
        INFO("printer: " << printer << " regex patterns: " << regexes);
        CHECK(regexes <= 1);
    }
}

// ============================================================================
// Defensive edge cases
// ============================================================================

TEST_CASE("ConsoleFilter: empty line never matches a populated engine", "[console_filter]") {
    ConsoleFilterEngine eng = shipped_engine_for("Creality K2 Plus");
    CHECK_FALSE(eng.should_filter(""));
}

TEST_CASE("ConsoleFilter: trailing CRLF does not break prefix matching", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    CHECK(eng.should_filter("// data_send:foo\r\n"));
    CHECK(eng.should_filter("// data_send:bar\n"));
}

TEST_CASE("ConsoleFilter: substring match handles UTF-8 multi-byte correctly", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("substring:\xe5\x8f\x82\xe6\x95\xb0"); // 参数 (U+53C2 U+6570)
    CHECK(eng.should_filter("xx \xe5\x8f\x82\xe6\x95\xb0 xx"));
    // Partial codepoint at the boundary must NOT match. find() is byte-oriented
    // but since we're searching for a complete multi-byte sequence, byte-level
    // search aligns with codepoint boundaries provided no foreign bytes splice
    // into a complete codepoint mid-line.
    CHECK_FALSE(eng.should_filter("\xe5\x8f")); // truncated 参 — 2 of 3 bytes
}

TEST_CASE("ConsoleFilter: clear() then re-add() rebuilds correctly", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// foo");
    REQUIRE(eng.should_filter("// foo bar"));

    eng.clear();
    CHECK(eng.empty());
    CHECK_FALSE(eng.should_filter("// foo bar"));

    eng.add("prefix:// baz");
    CHECK(eng.should_filter("// baz qux"));
    CHECK_FALSE(eng.should_filter("// foo bar"));
}
