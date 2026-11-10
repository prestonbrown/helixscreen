// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_panel_console.cpp
 * @brief Unit tests for ConsolePanel G-code history functionality
 *
 * Tests the static helpers that parse G-code console entries as pure C++, and
 * the overlay's input and history list over the mock printer.
 */

#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_cache_registry.h"
#include "printer_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

// Tap-to-paste tests call the real ConsolePanel::find_entry_by_id.
// ui_panel_console.o is linked into the test binary
// (it is absent from the TEST_APP_OBJS filter-out list in mk/tests.mk).
#include "ui_panel_console.h"

// The pure line decisions under test, shared by the console overlay and the
// home console tile.
#include "console_line.h"

namespace {
const auto is_error_message = &helix::ui::is_console_error_message;
const auto is_temp_message = &helix::ui::is_console_temp_message;
const auto contains_html_spans = &helix::ui::contains_console_html_spans;
const auto parse_html_spans = &helix::ui::parse_console_html_spans;
} // namespace

// ============================================================================
// Error Message Detection Tests
// ============================================================================

TEST_CASE("Console: is_error_message() flags !! and a leading Error, any case",
          "[ui][error_detection]") {
    for (const char* line : {"!! Error: Heater not responding", "!!Thermistor disconnected", "!! ",
                             "Error: Command failed", "ERROR: Unknown G-code",
                             "error: invalid parameter", "ErRoR: mixed case", "Error", "Errorx",
                             "!! Move out of range: 0.000 250.000 0.500 [0.000]",
                             "!! MCU 'mcu' shutdown: Timer too close"}) {
        CAPTURE(line);
        CHECK(is_error_message(line));
    }
    for (const char* line : {"", "ok", "// Klipper state: Ready", "B:60.0 /60.0 T0:210.0 /210.0",
                             "No error detected", "G-code M112 for error stop", "!", "E", "Err",
                             "Erro", "echo: G28 homing completed", "Recv: ok"}) {
        CAPTURE(line);
        CHECK_FALSE(is_error_message(line));
    }
}

TEST_CASE("Console: is_temp_message() needs a digit right after T: or B: and a slash",
          "[ui][temp_filter]") {
    for (const char* line :
         {"T:210.0 /210.0 B:60.0 /60.0", "ok T:210.5 /210.0 B:60.2 /60.0",
          "B:60.0 /60.0 T0:210.0 /210.0", "T0:200.0 /200.0 T1:0.0 /0.0 B:55.0 /55.0", "T:25.0 /0.0",
          "B:22.0 /0.0", "T:0 /0", "B:0.0 /0.0", "ok T:25.3 /210.0"}) {
        CAPTURE(line);
        CHECK(is_temp_message(line));
    }
    for (const char* line :
         {"", "ok", "// Klipper state: Ready", "echo: G28 completed", "!! Error: Heater failed",
          "M104 S200", "G28 X Y", "T:", "B:60", "Setting T: value", "path/to/file",
          "50/50 complete", "T: see docs/here", "B: test/path", "T: abc/def",
          "Set T: to 200/250"}) {
        CAPTURE(line);
        CHECK_FALSE(is_temp_message(line));
    }
}

// ============================================================================
// Command History Buffer Tests
// ============================================================================

TEST_CASE("Console: Up and Down walk the history cursor and stop at its ends",
          "[ui][command_history]") {
    const auto step = &ConsolePanel::next_history_index;
    // From the typed input (-1), Up reaches the newest command, then older ones.
    CHECK(step(-1, 3, LV_KEY_UP) == 0);
    CHECK(step(1, 3, LV_KEY_UP) == 2);
    CHECK(step(2, 3, LV_KEY_UP) == std::nullopt);  // the oldest
    CHECK(step(-1, 0, LV_KEY_UP) == std::nullopt); // no history
    // Down returns toward the typed input and stops there.
    CHECK(step(2, 3, LV_KEY_DOWN) == 1);
    CHECK(step(0, 3, LV_KEY_DOWN) == -1);
    CHECK(step(-1, 3, LV_KEY_DOWN) == std::nullopt);
    CHECK(step(0, 3, LV_KEY_ENTER) == std::nullopt);
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
// function is static and takes the container, so no panel instance is built.
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
    constexpr size_t kCap = 4;

    std::vector<uint64_t> retired;
    for (int i = 0; i < 20; i++) {
        push(entries, next_id, "CMD_" + std::to_string(i));
        while (entries.size() > kCap) {
            retired.push_back(entries.front().id);
            entries.pop_front();
        }
    }

    REQUIRE(entries.size() == kCap);
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

// ============================================================================
// should_display() — the one filtering decision, shared by the history and live
// paths. Calls the real ConsolePanel static (not a replica) because the bug this
// replaced was precisely two copies of the rule disagreeing: the live path
// filtered and populate_entries() did not, so opening the console showed a
// screenful of the noise the filters existed to remove.
// ============================================================================

TEST_CASE("Console: should_display() suppresses firmware noise", "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// data_send:"));

    const std::string noise = "// data_send:b'\\x83\\x05' recv_result:None";
    const std::string real = "// Klipper state: Printer is ready";

    CHECK_FALSE(ConsolePanel::should_display(noise, false, true, true, eng));
    CHECK(ConsolePanel::should_display(real, false, true, true, eng));
}

TEST_CASE("Console: should_display() hides a forwarder announcing itself", "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    CHECK_FALSE(
        ConsolePanel::should_display("// HELIX:ALIAS:HELIX_READY", false, false, false, eng));
    CHECK(ConsolePanel::should_display("// HELIX:READY", false, false, false, eng));
}

TEST_CASE("Console: should_display() honours the firmware-noise toggle", "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// data_send:"));
    const std::string noise = "// data_send:b'\\x83' recv_result:None";

    CHECK_FALSE(ConsolePanel::should_display(noise, false, true, true, eng));
    // Toggle off: the engine still matches, but nothing may be dropped.
    CHECK(ConsolePanel::should_display(noise, false, true, false, eng));
}

TEST_CASE("Console: should_display() honours the temperature toggle", "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng; // empty — temps are a separate switch
    const std::string temps = "ok T:210.0 /210.0 B:60.0 /60.0";

    CHECK_FALSE(ConsolePanel::should_display(temps, true, true, true, eng));
    CHECK(ConsolePanel::should_display(temps, true, false, true, eng));
}

TEST_CASE("Console: should_display() keeps everything when both filters are off",
          "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// data_send:"));

    CHECK(ConsolePanel::should_display("// data_send:b'\\x01'", true, false, false, eng));
    CHECK(ConsolePanel::should_display("ok T:210.0 /210.0", true, false, false, eng));
}

TEST_CASE("Console: should_display() never drops errors that match no pattern",
          "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// data_send:"));

    CHECK(ConsolePanel::should_display("!! Move out of range", false, true, true, eng));
    CHECK(ConsolePanel::should_display("!! Shutdown due to webhooks request", false, true, true,
                                       eng));
}

// ============================================================================
// The overlay: input and history list
// ============================================================================

using namespace helix;

namespace {

/// The mock printer as the app's globals, so the console fetches its stored history.
struct ConsoleOverlayFx : LVGLUITestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::unique_ptr<MoonrakerAPIMock> api;
    helix::IMoonrakerAPI* previous_api = nullptr;
    helix::IMoonrakerClient* previous_client = nullptr;

    ConsoleOverlayFx() {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        previous_api = get_moonraker_api();
        previous_client = get_moonraker_client();
        set_moonraker_api(api.get());
        set_moonraker_client(&client);
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }

    ~ConsoleOverlayFx() override {
        helix::PrinterCacheRegistry::instance().invalidate_all();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        set_moonraker_client(previous_client);
        set_moonraker_api(previous_api);
        client.stop_temperature_simulation();
        client.disconnect();
    }

    lv_obj_t* open() {
        REQUIRE(get_global_console_panel().show(lv_screen_active()));
        // The push, then the fetched history's own deferral.
        for (int i = 0; i < 3; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
        lv_obj_t* root = get_global_console_panel().get_root();
        REQUIRE(root != nullptr);
        return root;
    }
};

lv_obj_t* named(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return obj;
}

} // namespace

TEST_CASE_METHOD(
    ConsoleOverlayFx,
    "Console: Enter sends the typed command, Up recalls it, history hides the empty state",
    "[ui][console_overlay]") {
    lv_obj_t* root = open();
    lv_obj_t* input = named(root, "gcode_input");
    CHECK(lv_obj_is_hidden(named(root, "empty_state")));

    for (const char* cmd : {"M105", "M114"}) {
        lv_textarea_set_text(input, cmd);
        lv_obj_send_event(input, LV_EVENT_READY, nullptr);
        CHECK(std::string(lv_textarea_get_text(input)).empty());
    }
    // One Up press is one step back: the newest command first.
    uint32_t up = LV_KEY_UP;
    lv_obj_send_event(input, LV_EVENT_KEY, &up);
    CHECK(std::string(lv_textarea_get_text(input)) == "M114");
    lv_obj_send_event(input, LV_EVENT_KEY, &up);
    CHECK(std::string(lv_textarea_get_text(input)) == "M105");

    // Reopened over a full history, the rebuilt list does not show its empty state.
    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    root = open();
    CHECK(lv_obj_is_hidden(named(root, "empty_state")));
}
