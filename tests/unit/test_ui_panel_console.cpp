// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_panel_console.cpp
 * @brief Unit tests for ConsolePanel G-code history functionality
 *
 * Tests the static helper methods and logic for parsing G-code console entries.
 * These tests don't require LVGL initialization since they test pure C++ logic.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

// Tap-to-paste tests call the real ConsolePanel::find_entry_by_id, unlike the
// replicated helpers below. ui_panel_console.o is linked into the test binary
// (it is absent from the TEST_APP_OBJS filter-out list in mk/tests.mk).
#include "ui_panel_console.h"

// ============================================================================
// Test is_error_message() detection logic
// (Replicated from ui_panel_console.cpp since it's a private static method)
// ============================================================================

static bool is_error_message(const std::string& message) {
    if (message.size() >= 2 && message[0] == '!' && message[1] == '!') {
        return true;
    }

    if (message.size() >= 5) {
        auto ci_eq = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        };
        return std::equal(message.begin(), message.begin() + 5, "error", ci_eq);
    }

    return false;
}

// ============================================================================
// Error Message Detection Tests
// ============================================================================

TEST_CASE("Console: is_error_message() with empty string", "[ui][error_detection]") {
    REQUIRE(is_error_message("") == false);
}

TEST_CASE("Console: is_error_message() with !! prefix", "[ui][error_detection]") {
    REQUIRE(is_error_message("!! Error: Heater not responding") == true);
    REQUIRE(is_error_message("!!Thermistor disconnected") == true);
    REQUIRE(is_error_message("!! ") == true);
}

TEST_CASE("Console: is_error_message() with Error prefix", "[ui][error_detection]") {
    REQUIRE(is_error_message("Error: Command failed") == true);
    REQUIRE(is_error_message("ERROR: Unknown G-code") == true);
    REQUIRE(is_error_message("error: invalid parameter") == true);
    REQUIRE(is_error_message("ErRoR: mixed case") == true);
}

TEST_CASE("Console: is_error_message() with normal responses", "[ui][error_detection]") {
    // Normal OK responses
    REQUIRE(is_error_message("ok") == false);
    REQUIRE(is_error_message("// Klipper state: Ready") == false);
    REQUIRE(is_error_message("B:60.0 /60.0 T0:210.0 /210.0") == false);

    // Messages containing "error" but not at start
    REQUIRE(is_error_message("No error detected") == false);
    REQUIRE(is_error_message("G-code M112 for error stop") == false);
}

TEST_CASE("Console: is_error_message() with single character", "[ui][error_detection]") {
    REQUIRE(is_error_message("!") == false); // Only one !, not two
    REQUIRE(is_error_message("E") == false); // Not enough characters for "Error"
}

TEST_CASE("Console: is_error_message() with boundary cases", "[ui][error_detection]") {
    REQUIRE(is_error_message("Err") == false);   // Too short for "Error"
    REQUIRE(is_error_message("Erro") == false);  // Still too short
    REQUIRE(is_error_message("Error") == true);  // Exactly "Error"
    REQUIRE(is_error_message("Errorx") == true); // Starts with "Error"
}

// ============================================================================
// Real-World Error Message Tests
// ============================================================================

TEST_CASE("Console: typical Klipper error messages", "[ui][error_detection]") {
    // Real Klipper error message patterns
    REQUIRE(is_error_message("!! Move out of range: 0.000 250.000 0.500 [0.000]") == true);
    REQUIRE(is_error_message("!! Timer too close") == true);
    REQUIRE(is_error_message("!! MCU 'mcu' shutdown: Timer too close") == true);
    REQUIRE(is_error_message("Error: Bed heater not responding") == true);
}

TEST_CASE("Console: typical Klipper info messages", "[ui][error_detection]") {
    // Normal Klipper messages that should NOT be flagged as errors
    REQUIRE(is_error_message("// Klipper state: Ready") == false);
    REQUIRE(is_error_message("// probe at 150.000,150.000 is z=1.234567") == false);
    REQUIRE(is_error_message("echo: G28 homing completed") == false);
    REQUIRE(is_error_message("Recv: ok") == false);
}

// ============================================================================
// Temperature Message Filtering Tests
// (Replicated from ui_panel_console.cpp since it's a private static method)
// ============================================================================

/**
 * @brief Check if a message is a temperature status update
 *
 * Filters out periodic temperature reports like:
 * "ok T:210.0 /210.0 B:60.0 /60.0"
 */
static bool is_temp_message(const std::string& message) {
    if (message.empty()) {
        return false;
    }

    // Check for "T:" or "B:" followed immediately by a digit, with "/" somewhere after
    size_t t_pos = message.find("T:");
    size_t b_pos = message.find("B:");

    auto check_temp_pattern = [&](size_t pos) -> bool {
        if (pos == std::string::npos)
            return false;
        // Require digit immediately after the colon (e.g. "T:210" not "T: see docs")
        size_t val_start = pos + 2; // skip "T:" or "B:"
        if (val_start < message.size() &&
            std::isdigit(static_cast<unsigned char>(message[val_start]))) {
            // Also require "/" somewhere after the pattern (target temp separator)
            size_t slash_pos = message.find('/', val_start);
            return slash_pos != std::string::npos;
        }
        return false;
    };

    return check_temp_pattern(t_pos) || check_temp_pattern(b_pos);
}

// ============================================================================
// Temperature Message Detection Tests
// ============================================================================

TEST_CASE("Console: is_temp_message() with empty string", "[ui][temp_filter]") {
    REQUIRE(is_temp_message("") == false);
}

TEST_CASE("Console: is_temp_message() with standard temp reports", "[ui][temp_filter]") {
    // Standard Klipper temperature reports
    REQUIRE(is_temp_message("T:210.0 /210.0 B:60.0 /60.0") == true);
    REQUIRE(is_temp_message("ok T:210.5 /210.0 B:60.2 /60.0") == true);
    REQUIRE(is_temp_message("B:60.0 /60.0 T0:210.0 /210.0") == true);
    REQUIRE(is_temp_message("T0:200.0 /200.0 T1:0.0 /0.0 B:55.0 /55.0") == true);
}

TEST_CASE("Console: is_temp_message() with partial temp formats", "[ui][temp_filter]") {
    // Partial formats that should still be detected
    REQUIRE(is_temp_message("T:25.0 /0.0") == true); // Cold extruder
    REQUIRE(is_temp_message("B:22.0 /0.0") == true); // Cold bed
}

TEST_CASE("Console: is_temp_message() with non-temp messages", "[ui][temp_filter]") {
    // These should NOT be flagged as temperature messages
    REQUIRE(is_temp_message("ok") == false);
    REQUIRE(is_temp_message("// Klipper state: Ready") == false);
    REQUIRE(is_temp_message("echo: G28 completed") == false);
    REQUIRE(is_temp_message("!! Error: Heater failed") == false);
    REQUIRE(is_temp_message("M104 S200") == false); // Temp command, not status
    REQUIRE(is_temp_message("G28 X Y") == false);
}

TEST_CASE("Console: is_temp_message() edge cases", "[ui][temp_filter]") {
    // Edge cases that look like temps but aren't
    REQUIRE(is_temp_message("T:") == false);               // No value or slash
    REQUIRE(is_temp_message("B:60") == false);             // No slash
    REQUIRE(is_temp_message("Setting T: value") == false); // No digit after T:

    // Edge cases that might have slashes but no temp
    REQUIRE(is_temp_message("path/to/file") == false); // No T: or B:
    REQUIRE(is_temp_message("50/50 complete") == false);
}

TEST_CASE("Console: is_temp_message() tightened heuristic rejects non-digit after T:/B:",
          "[ui][temp_filter]") {
    // These have T: or B: with "/" but no digit after the colon
    // Old heuristic would match, new one correctly rejects
    REQUIRE(is_temp_message("T: see docs/here") == false);
    REQUIRE(is_temp_message("B: test/path") == false);
    REQUIRE(is_temp_message("T: abc/def") == false);
    REQUIRE(is_temp_message("Set T: to 200/250") == false); // space after T:

    // These should still match (digit immediately follows colon)
    REQUIRE(is_temp_message("T:0 /0") == true);
    REQUIRE(is_temp_message("B:0.0 /0.0") == true);
    REQUIRE(is_temp_message("ok T:25.3 /210.0") == true);
}

// ============================================================================
// Timestamp Formatting Tests
// ============================================================================

#include <ctime>

static std::string format_timestamp(double timestamp) {
    time_t t;
    if (timestamp > 0.0) {
        t = static_cast<time_t>(timestamp);
    } else {
        t = std::time(nullptr);
    }
    struct tm tm_buf {};
    localtime_r(&t, &tm_buf);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d ", tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec);
    return std::string(buf);
}

TEST_CASE("Console: format_timestamp() with known timestamp", "[ui][timestamp]") {
    // Use a known UTC timestamp and convert to local time for comparison
    // 2024-01-01 00:00:00 UTC = 1704067200
    double ts = 1704067200.0;
    std::string result = format_timestamp(ts);

    // Should be in HH:MM:SS format with trailing space
    REQUIRE(result.size() == 9); // "HH:MM:SS "
    REQUIRE(result[2] == ':');
    REQUIRE(result[5] == ':');
    REQUIRE(result[8] == ' ');
    // All characters should be digits or colons or space
    for (size_t i = 0; i < 8; i++) {
        REQUIRE((std::isdigit(result[i]) || result[i] == ':'));
    }
}

TEST_CASE("Console: format_timestamp() with zero uses current time", "[ui][timestamp]") {
    std::string result = format_timestamp(0.0);

    // Should produce valid format
    REQUIRE(result.size() == 9);
    REQUIRE(result[2] == ':');
    REQUIRE(result[5] == ':');
    REQUIRE(result[8] == ' ');

    // Hour should be 00-23
    int hour = std::stoi(result.substr(0, 2));
    REQUIRE(hour >= 0);
    REQUIRE(hour <= 23);

    // Minute should be 00-59
    int minute = std::stoi(result.substr(3, 2));
    REQUIRE(minute >= 0);
    REQUIRE(minute <= 59);

    // Second should be 00-59
    int second = std::stoi(result.substr(6, 2));
    REQUIRE(second >= 0);
    REQUIRE(second <= 59);
}

// ============================================================================
// Command History Buffer Tests
// ============================================================================

TEST_CASE("Console: command history deque operations", "[ui][command_history]") {
    std::deque<std::string> history;
    constexpr size_t MAX_HISTORY = 20;

    SECTION("push and recall") {
        history.push_front("G28");
        history.push_front("M104 S200");
        history.push_front("G1 X10 Y10");

        REQUIRE(history.size() == 3);
        REQUIRE(history[0] == "G1 X10 Y10"); // Most recent
        REQUIRE(history[1] == "M104 S200");
        REQUIRE(history[2] == "G28"); // Oldest
    }

    SECTION("overflow at MAX_HISTORY") {
        for (int i = 0; i < 25; i++) {
            history.push_front("CMD_" + std::to_string(i));
            if (history.size() > MAX_HISTORY) {
                history.pop_back();
            }
        }

        REQUIRE(history.size() == MAX_HISTORY);
        REQUIRE(history[0] == "CMD_24"); // Most recent
        // Oldest should be CMD_5 (0-4 were popped)
        REQUIRE(history.back() == "CMD_5");
    }

    SECTION("up/down navigation simulation") {
        history.push_front("G28");
        history.push_front("M104 S200");
        history.push_front("G1 X10");

        int index = -1;
        std::string saved_input = "partial";

        // Up: save input, go to most recent
        index = 0;
        REQUIRE(history[static_cast<size_t>(index)] == "G1 X10");

        // Up again: go to older
        index = 1;
        REQUIRE(history[static_cast<size_t>(index)] == "M104 S200");

        // Up again: go to oldest
        index = 2;
        REQUIRE(history[static_cast<size_t>(index)] == "G28");

        // Up again: should stay at 2 (bounds check)
        int next = index + 1;
        REQUIRE(next >= static_cast<int>(history.size()));
        // Index stays at 2

        // Down: go to newer
        index = 1;
        REQUIRE(history[static_cast<size_t>(index)] == "M104 S200");

        // Down: go to most recent
        index = 0;
        REQUIRE(history[static_cast<size_t>(index)] == "G1 X10");

        // Down: restore saved input
        index = -1;
        REQUIRE(saved_input == "partial");
    }
}

// ============================================================================
// HTML Span Parsing
// (Replicated from ui_panel_console.cpp since it's in anonymous namespace)
// ============================================================================

static constexpr const char SPAN_OPEN[] = "<span class=";
static constexpr size_t SPAN_OPEN_LEN = sizeof(SPAN_OPEN) - 1;
static constexpr const char SPAN_CLOSE[] = "</span>";
static constexpr size_t SPAN_CLOSE_LEN = sizeof(SPAN_CLOSE) - 1;

struct TextSegment {
    std::string text;
    std::string color_class; // empty = default, "success", "info", "warning", "error"
};

static std::string extract_color_class(const std::string& class_attr) {
    static constexpr std::pair<const char*, const char*> mappings[] = {
        {"success--text", "success"},
        {"info--text", "info"},
        {"warning--text", "warning"},
        {"error--text", "error"},
    };
    for (const auto& [pattern, name] : mappings) {
        if (class_attr.find(pattern) != std::string::npos) {
            return name;
        }
    }
    return {};
}

static bool contains_html_spans(const std::string& message) {
    return message.find(SPAN_OPEN) != std::string::npos &&
           (message.find("success--text") != std::string::npos ||
            message.find("info--text") != std::string::npos ||
            message.find("warning--text") != std::string::npos ||
            message.find("error--text") != std::string::npos);
}

static std::vector<TextSegment> parse_html_spans(const std::string& message) {
    std::vector<TextSegment> segments;

    size_t pos = 0;
    const size_t len = message.size();

    while (pos < len) {
        size_t span_start = message.find(SPAN_OPEN, pos);

        if (span_start == std::string::npos) {
            std::string remaining = message.substr(pos);
            if (!remaining.empty()) {
                segments.push_back({std::move(remaining), {}});
            }
            break;
        }

        if (span_start > pos) {
            segments.push_back({message.substr(pos, span_start - pos), {}});
        }

        size_t class_start = span_start + SPAN_OPEN_LEN;
        size_t class_end = message.find('>', class_start);

        if (class_end == std::string::npos) {
            segments.push_back({message.substr(span_start), {}});
            break;
        }

        std::string color_class =
            extract_color_class(message.substr(class_start, class_end - class_start));

        size_t content_start = class_end + 1;
        size_t span_close = message.find(SPAN_CLOSE, content_start);

        if (span_close == std::string::npos) {
            segments.push_back({message.substr(content_start), color_class});
            break;
        }

        std::string content = message.substr(content_start, span_close - content_start);
        if (!content.empty()) {
            segments.push_back({std::move(content), std::move(color_class)});
        }

        pos = span_close + SPAN_CLOSE_LEN;
    }

    return segments;
}

// ============================================================================
// HTML Span Detection Tests
// ============================================================================

TEST_CASE("Console: contains_html_spans() with no HTML", "[ui][html_parse]") {
    REQUIRE(contains_html_spans("") == false);
    REQUIRE(contains_html_spans("ok") == false);
    REQUIRE(contains_html_spans("Normal text message") == false);
    REQUIRE(contains_html_spans("!! Error message") == false);
}

TEST_CASE("Console: contains_html_spans() with HTML spans", "[ui][html_parse]") {
    REQUIRE(contains_html_spans("<span class=success--text>LOADED</span>") == true);
    REQUIRE(contains_html_spans("Text <span class=error--text>ERROR</span> more") == true);
    REQUIRE(contains_html_spans("lane1: <span class=info--text>ready</span>") == true);
}

TEST_CASE("Console: contains_html_spans() with partial/invalid HTML", "[ui][html_parse]") {
    REQUIRE(contains_html_spans("<span>no class</span>") == false);
    REQUIRE(contains_html_spans("<span class=other>unknown</span>") == false);
    REQUIRE(contains_html_spans("<div>not a span</div>") == false);
}

// ============================================================================
// HTML Span Parsing Tests
// ============================================================================

TEST_CASE("Console: parse_html_spans() plain text only", "[ui][html_parse]") {
    auto segments = parse_html_spans("Hello world");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "Hello world");
    REQUIRE(segments[0].color_class.empty());
}

TEST_CASE("Console: parse_html_spans() single span", "[ui][html_parse]") {
    auto segments = parse_html_spans("<span class=success--text>LOADED</span>");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "LOADED");
    REQUIRE(segments[0].color_class == "success");
}

TEST_CASE("Console: parse_html_spans() mixed content", "[ui][html_parse]") {
    auto segments = parse_html_spans("lane1: <span class=success--text>LOCKED</span> done");
    REQUIRE(segments.size() == 3);
    REQUIRE(segments[0].text == "lane1: ");
    REQUIRE(segments[0].color_class.empty());
    REQUIRE(segments[1].text == "LOCKED");
    REQUIRE(segments[1].color_class == "success");
    REQUIRE(segments[2].text == " done");
    REQUIRE(segments[2].color_class.empty());
}

TEST_CASE("Console: parse_html_spans() multiple spans", "[ui][html_parse]") {
    auto segments =
        parse_html_spans("<span class=success--text>OK</span><span class=error--text>FAIL</span>");
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].text == "OK");
    REQUIRE(segments[0].color_class == "success");
    REQUIRE(segments[1].text == "FAIL");
    REQUIRE(segments[1].color_class == "error");
}

TEST_CASE("Console: parse_html_spans() all color classes", "[ui][html_parse]") {
    auto seg1 = parse_html_spans("<span class=success--text>a</span>");
    REQUIRE(seg1[0].color_class == "success");

    auto seg2 = parse_html_spans("<span class=info--text>b</span>");
    REQUIRE(seg2[0].color_class == "info");

    auto seg3 = parse_html_spans("<span class=warning--text>c</span>");
    REQUIRE(seg3[0].color_class == "warning");

    auto seg4 = parse_html_spans("<span class=error--text>d</span>");
    REQUIRE(seg4[0].color_class == "error");
}

TEST_CASE("Console: parse_html_spans() preserves newlines", "[ui][html_parse]") {
    auto segments = parse_html_spans("<span class=success--text>line1\nline2</span>");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "line1\nline2");
}

TEST_CASE("Console: parse_html_spans() real AFC output", "[ui][html_parse]") {
    // Real example from printer
    auto segments = parse_html_spans("lane1 tool cmd: T0  <span class=success--text>LOCKED</span>"
                                     "<span class=success--text> AND LOADED</span>");
    REQUIRE(segments.size() == 3);
    REQUIRE(segments[0].text == "lane1 tool cmd: T0  ");
    REQUIRE(segments[1].text == "LOCKED");
    REQUIRE(segments[1].color_class == "success");
    REQUIRE(segments[2].text == " AND LOADED");
    REQUIRE(segments[2].color_class == "success");
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_CASE("Console: parse_html_spans() empty span content", "[ui][html_parse]") {
    // Span with empty content should be skipped
    auto segments = parse_html_spans("<span class=success--text></span>");
    REQUIRE(segments.empty());
}

TEST_CASE("Console: parse_html_spans() malformed no closing bracket", "[ui][html_parse]") {
    // Missing > should return rest as plain text
    auto segments = parse_html_spans("<span class=success--text");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "<span class=success--text");
    REQUIRE(segments[0].color_class.empty());
}

TEST_CASE("Console: parse_html_spans() malformed no closing tag", "[ui][html_parse]") {
    // Missing </span> should still extract content with color
    auto segments = parse_html_spans("<span class=success--text>content");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "content");
    REQUIRE(segments[0].color_class == "success");
}

TEST_CASE("Console: parse_html_spans() unknown class extracts text plain", "[ui][html_parse]") {
    // Unknown class should still parse, just with empty color_class
    auto segments = parse_html_spans("<span class=unknown--text>text</span>");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "text");
    REQUIRE(segments[0].color_class.empty());
}

TEST_CASE("Console: parse_html_spans() quoted class attribute", "[ui][html_parse]") {
    // Quoted class attribute - class name includes quotes but still matches
    auto segments = parse_html_spans("<span class=\"success--text\">OK</span>");
    REQUIRE(segments.size() == 1);
    REQUIRE(segments[0].text == "OK");
    REQUIRE(segments[0].color_class == "success");
}

// ============================================================================
// Tap-to-Paste: entry id resolution
//
// These exercise the REAL ConsolePanel::find_entry_by_id, not a replica. The
// function is static and takes the container, so no panel instance is built —
// see docs/devel/specs/2026-07-20-print-status-panel-test-isolation.md for why
// constructing a panel in a test is unsafe.
// ============================================================================

namespace {

using GcodeEntry = ConsolePanel::GcodeEntry;

/// Append an entry the way ConsolePanel::add_entry does, assigning the next id.
GcodeEntry& push(std::deque<GcodeEntry>& entries, uint64_t& next_id, const std::string& message,
                 GcodeEntry::Type type = GcodeEntry::Type::COMMAND) {
    GcodeEntry entry;
    entry.message = message;
    entry.type = type;
    entries.push_back(entry);
    entries.back().id = next_id++;
    return entries.back();
}

} // namespace

TEST_CASE("Console: find_entry_by_id resolves a live entry", "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;

    push(entries, next_id, "G28");
    uint64_t wanted = push(entries, next_id, "BED_MESH_CALIBRATE").id;
    push(entries, next_id, "M104 S210");

    const GcodeEntry* found = ConsolePanel::find_entry_by_id(entries, wanted);
    REQUIRE(found != nullptr);
    REQUIRE(found->message == "BED_MESH_CALIBRATE");
}

TEST_CASE("Console: find_entry_by_id rejects id 0", "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;
    push(entries, next_id, "G28");

    // An entry that never entered entries_ keeps id 0. A widget carrying 0 must
    // never resolve, even though a default-constructed entry sits in the buffer.
    GcodeEntry unassigned;
    unassigned.message = "NEVER_TAPPABLE";
    entries.push_back(unassigned);

    REQUIRE(ConsolePanel::find_entry_by_id(entries, 0) == nullptr);
}

TEST_CASE("Console: find_entry_by_id returns null for an unknown id", "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;
    push(entries, next_id, "G28");

    REQUIRE(ConsolePanel::find_entry_by_id(entries, 9999) == nullptr);
}

TEST_CASE("Console: find_entry_by_id returns null once the entry is pruned", "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;

    uint64_t oldest = push(entries, next_id, "G28").id;
    push(entries, next_id, "M104 S210");

    // add_entry() prunes from the front when over MAX_ENTRIES. A widget can
    // outlive its entry, so the lookup must fail closed rather than resolve to
    // whatever slid into that position.
    entries.pop_front();

    REQUIRE(ConsolePanel::find_entry_by_id(entries, oldest) == nullptr);
}

TEST_CASE("Console: pruned ids are never reused by later entries", "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;
    constexpr size_t CAP = 4;

    std::vector<uint64_t> retired;
    for (int i = 0; i < 20; i++) {
        push(entries, next_id, "CMD_" + std::to_string(i));
        while (entries.size() > CAP) {
            retired.push_back(entries.front().id);
            entries.pop_front();
        }
    }

    REQUIRE(entries.size() == CAP);
    REQUIRE_FALSE(retired.empty());

    // This is the safety property that makes an integer id safe where a raw
    // pointer would not be: no retired id ever resolves again.
    for (uint64_t gone : retired) {
        REQUIRE(ConsolePanel::find_entry_by_id(entries, gone) == nullptr);
    }

    // ...and every surviving entry still resolves to its own text.
    for (const auto& entry : entries) {
        const GcodeEntry* found = ConsolePanel::find_entry_by_id(entries, entry.id);
        REQUIRE(found != nullptr);
        REQUIRE(found->message == entry.message);
    }
}

TEST_CASE("Console: find_entry_by_id resolves responses too", "[ui][tap_to_paste]") {
    // The lookup itself is type-agnostic; only create_entry_widget() decides what
    // gets a click handler. Keeping the lookup general means a future "tap a
    // response" feature does not need it changed.
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;

    uint64_t resp = push(entries, next_id, "ok", GcodeEntry::Type::RESPONSE).id;

    const GcodeEntry* found = ConsolePanel::find_entry_by_id(entries, resp);
    REQUIRE(found != nullptr);
    REQUIRE(found->type == GcodeEntry::Type::RESPONSE);
}
