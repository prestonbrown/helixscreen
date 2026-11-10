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
// HTML spans
// ============================================================================

namespace {

/// parse_html_spans() as one string: segments joined by '|', a coloured one as "class:text".
std::string spans(const std::string& line) {
    std::string out;
    for (const auto& seg : parse_html_spans(line)) {
        if (!out.empty()) {
            out += '|';
        }
        out += seg.color_class.empty() ? seg.text : seg.color_class + ":" + seg.text;
    }
    return out;
}

} // namespace

TEST_CASE("Console: contains_html_spans() needs a span with a known colour class",
          "[ui][html_parse]") {
    for (const char* plain : {"", "ok", "!! Error message", "<span>no class</span>",
                              "<span class=other>unknown</span>", "<div>not a span</div>"}) {
        CAPTURE(plain);
        CHECK_FALSE(contains_html_spans(plain));
    }
    for (const char* html : {"<span class=success--text>LOADED</span>",
                             "Text <span class=error--text>ERROR</span> more",
                             "lane1: <span class=info--text>ready</span>"}) {
        CAPTURE(html);
        CHECK(contains_html_spans(html));
    }
}

TEST_CASE("Console: parse_html_spans() splits a line into coloured segments", "[ui][html_parse]") {
    struct Case {
        const char* line;
        const char* segments;
    };
    for (const Case& c : {
             Case{"Hello world", "Hello world"},
             Case{"lane1: <span class=success--text>LOCKED</span> done",
                  "lane1: |success:LOCKED| done"},
             Case{"<span class=success--text>OK</span><span class=error--text>FAIL</span>",
                  "success:OK|error:FAIL"},
             Case{"<span class=info--text>b</span>", "info:b"},
             Case{"<span class=warning--text>c</span>", "warning:c"},
             Case{"<span class=success--text>line1\nline2</span>", "success:line1\nline2"},
             // Real AFC output.
             Case{"lane1 tool cmd: T0  <span class=success--text>LOCKED</span>"
                  "<span class=success--text> AND LOADED</span>",
                  "lane1 tool cmd: T0  |success:LOCKED|success: AND LOADED"},
             // An empty span is dropped; no '>' leaves the rest plain; no </span>
             // still colours the content; an unknown class parses plain; quotes match.
             Case{"<span class=success--text></span>", ""},
             Case{"<span class=success--text", "<span class=success--text"},
             Case{"<span class=success--text>content", "success:content"},
             Case{"<span class=unknown--text>text</span>", "text"},
             Case{"<span class=\"success--text\">OK</span>", "success:OK"},
         }) {
        CAPTURE(c.line);
        CHECK(spans(c.line) == c.segments);
    }
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

TEST_CASE("Console: find_entry_by_id resolves live entries only, never 0 or a pruned id",
          "[ui][tap_to_paste]") {
    std::deque<GcodeEntry> entries;
    uint64_t next_id = 1;
    // The lookup is type-agnostic: a response resolves as a command does.
    const uint64_t resp = push(entries, next_id, "ok", GcodeEntry::Type::RESPONSE).id;
    // An entry that never entered entries_ keeps id 0, and 0 never resolves.
    GcodeEntry unassigned;
    unassigned.message = "NEVER_TAPPABLE";
    entries.push_back(unassigned);
    CHECK(ConsolePanel::find_entry_by_id(entries, 0) == nullptr);
    CHECK(ConsolePanel::find_entry_by_id(entries, 9999) == nullptr);
    const GcodeEntry* found = ConsolePanel::find_entry_by_id(entries, resp);
    REQUIRE(found != nullptr);
    CHECK(found->type == GcodeEntry::Type::RESPONSE);
    entries.clear();

    // add_entry() prunes from the front. A widget can outlive its entry, so a
    // retired id fails closed rather than resolving to whatever took its place.
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
    for (uint64_t gone : retired) {
        CHECK(ConsolePanel::find_entry_by_id(entries, gone) == nullptr);
    }
    for (const auto& entry : entries) {
        const GcodeEntry* live = ConsolePanel::find_entry_by_id(entries, entry.id);
        REQUIRE(live != nullptr);
        CHECK(live->message == entry.message);
    }
}

// ============================================================================
// should_display() — the one filtering decision, shared by the history and live
// paths. Calls the real ConsolePanel static (not a replica) because the history
// and live paths must agree on what the filters drop.
// ============================================================================

TEST_CASE("Console: should_display() drops noise and temperatures only under their toggles",
          "[ui][console_filter]") {
    helix::ui::ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// data_send:"));
    const std::string noise = "// data_send:b'\\x83\\x05' recv_result:None";
    const std::string temps = "ok T:210.0 /210.0 B:60.0 /60.0";
    const auto shown = [&eng](const std::string& m, bool is_temp, bool temps_off, bool noise_off) {
        return ConsolePanel::should_display(m, is_temp, temps_off, noise_off, eng);
    };

    CHECK_FALSE(shown(noise, false, true, true));
    CHECK(shown(noise, false, true, false)); // the engine matches, the toggle is off
    CHECK(shown("// Klipper state: Printer is ready", false, true, true));
    CHECK_FALSE(shown(temps, true, true, true));
    CHECK(shown(temps, true, false, true));
    CHECK(shown(noise, true, false, false));
    // Errors that match no pattern always show.
    CHECK(shown("!! Move out of range", false, true, true));
    // A forwarder's announcement is bookkeeping, whatever the toggles.
    CHECK_FALSE(shown("// HELIX:ALIAS:HELIX_READY", false, false, false));
    CHECK(shown("// HELIX:READY", false, false, false));
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
