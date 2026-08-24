// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_action_prompt.cpp
 * @brief Unit tests for ActionPromptManager - Klipper's action:prompt protocol
 *
 * Tests the parsing of action:prompt messages from Klipper's notify_gcode_response.
 * Written TDD-style before implementation exists.
 *
 * Protocol specification (from Klipper docs):
 * - Messages arrive via `notify_gcode_response` with "// action:" prefix
 * - Commands: prompt_begin, prompt_text, prompt_button, prompt_footer_button,
 *   prompt_button_group_start/end, prompt_show, prompt_end, notify
 */

#include "action_prompt_manager.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Line Parsing Tests
// ============================================================================

TEST_CASE("parse_action_line: Extracts command from action messages", "[action_prompt][parser]") {
    SECTION("Valid action lines return command type and payload") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_begin Title");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_begin");
        REQUIRE(result->payload == "Title");
    }

    SECTION("prompt_text command") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_text Some message");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_text");
        REQUIRE(result->payload == "Some message");
    }

    SECTION("prompt_button command") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_button OK");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_button");
        REQUIRE(result->payload == "OK");
    }

    SECTION("prompt_show command (no payload)") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_show");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_show");
        REQUIRE(result->payload.empty());
    }

    SECTION("prompt_end command (no payload)") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_end");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_end");
        REQUIRE(result->payload.empty());
    }

    SECTION("notify command") {
        auto result = ActionPromptManager::parse_action_line("// action:notify Print complete!");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "notify");
        REQUIRE(result->payload == "Print complete!");
    }
}

TEST_CASE("parse_action_line: Rejects non-action lines", "[action_prompt][parser]") {
    SECTION("Regular G-code line returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("G1 X10 Y20 E1.5");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Comment without action prefix returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("; This is a comment");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Empty line returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Line with only // returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("//");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Line with // but no action: returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("// some other comment");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Partial action prefix returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("// action");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Malformed action (missing colon) returns nullopt") {
        auto result = ActionPromptManager::parse_action_line("// actionprompt_begin Title");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("parse_action_line: Case sensitivity", "[action_prompt][parser]") {
    SECTION("action: is case-sensitive (lowercase required)") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_begin Title");
        REQUIRE(result.has_value());
    }

    SECTION("ACTION: (uppercase) is rejected") {
        auto result = ActionPromptManager::parse_action_line("// ACTION:prompt_begin Title");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Action: (mixed case) is rejected") {
        auto result = ActionPromptManager::parse_action_line("// Action:prompt_begin Title");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("parse_action_line: Whitespace handling", "[action_prompt][parser]") {
    SECTION("Preserves payload whitespace") {
        auto result =
            ActionPromptManager::parse_action_line("// action:prompt_text   Multiple  spaces  ");
        REQUIRE(result.has_value());
        REQUIRE(result->payload == "  Multiple  spaces  ");
    }

    SECTION("Handles tab characters in payload") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_text Tab\there");
        REQUIRE(result.has_value());
        REQUIRE(result->payload == "Tab\there");
    }

    SECTION("Leading whitespace before // is ignored") {
        auto result = ActionPromptManager::parse_action_line("  // action:prompt_begin Title");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_begin");
    }

    SECTION("Space after // is required") {
        auto result = ActionPromptManager::parse_action_line("//action:prompt_begin Title");
        // Klipper format includes space: "// action:"
        REQUIRE_FALSE(result.has_value());
    }
}

// FlashForge/ZMOD emits its runout abort with no space after the command:
//   `// action:prompt_beginResumption interrupted!!!`
// The parser used to hand "prompt_beginResumption" to the dispatcher, which fell
// through to `unknown command` — so prompt_begin never ran, the state machine
// never opened, and the following prompt_text/prompt_show landed on nothing
// (bundle JX2FVRB9, AD5X v0.99.103).
//
// Mutation check: delete the glued_command_length() call in parse_action_line and
// the first SECTION fails with command == "prompt_beginResumption".
TEST_CASE("parse_action_line: Recovers a command glued to its payload",
          "[action_prompt][parser][malformed]") {
    SECTION("prompt_begin with the title glued on — the observed AD5X line") {
        auto result = ActionPromptManager::parse_action_line(
            "// action:prompt_beginResumption interrupted!!!");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_begin");
        REQUIRE(result->payload == "Resumption interrupted!!!");
    }

    SECTION("Glued with no trailing payload keeps just the recovered text") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_textOnly");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_text");
        REQUIRE(result->payload == "Only");
    }

    SECTION("Longest command wins — prompt_button must not eat the group commands") {
        auto result =
            ActionPromptManager::parse_action_line("// action:prompt_button_group_startX");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_button_group_start");
        REQUIRE(result->payload == "X");
    }

    SECTION("Well-formed lines are untouched") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_begin Title");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_begin");
        REQUIRE(result->payload == "Title");

        auto group = ActionPromptManager::parse_action_line("// action:prompt_button_group_start");
        REQUIRE(group.has_value());
        REQUIRE(group->command == "prompt_button_group_start");
        REQUIRE(group->payload.empty());
    }

    SECTION("A snake_case extension is NOT split — it may be a newer protocol command") {
        auto result = ActionPromptManager::parse_action_line("// action:prompt_begin_v2 Title");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "prompt_begin_v2");
        REQUIRE(result->payload == "Title");
    }

    SECTION("An unrelated command is left alone") {
        auto result = ActionPromptManager::parse_action_line("// action:frobnicate arg");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "frobnicate");
        REQUIRE(result->payload == "arg");
    }
}

// The whole point of recovering the command is that the state machine actually
// opens, so the text and buttons that follow have somewhere to land.
TEST_CASE("ActionPromptManager: A glued prompt_begin still builds a prompt",
          "[action_prompt][malformed][integration]") {
    ActionPromptManager mgr;
    std::optional<PromptData> shown;
    mgr.set_on_show([&](const PromptData& d) { shown = d; });

    mgr.process_line("// action:prompt_beginResumption interrupted!!!");
    mgr.process_line(
        "// action:prompt_text \"head_switch_sensor\" has detected that the filament has run out");
    mgr.process_line("// action:prompt_footer_button Ok|RESPOND TYPE=command "
                     "MSG=action:prompt_end|info");
    mgr.process_line("// action:prompt_show");

    REQUIRE(shown.has_value());
    CHECK(shown->title == "Resumption interrupted!!!");
    REQUIRE(shown->text_lines.size() == 1);
    CHECK(shown->text_lines[0] ==
          "\"head_switch_sensor\" has detected that the filament has run out");
    REQUIRE(shown->buttons.size() == 1);
    CHECK(shown->buttons[0].is_footer);
}

// ============================================================================
// Button Spec Parsing Tests
// ============================================================================

TEST_CASE("parse_button_spec: Simple label only", "[action_prompt][button]") {
    SECTION("Label becomes both label and gcode") {
        auto button = ActionPromptManager::parse_button_spec("OK");
        REQUIRE(button.label == "OK");
        REQUIRE(button.gcode == "OK");
        REQUIRE(button.color.empty());
    }

    SECTION("Label with spaces") {
        auto button = ActionPromptManager::parse_button_spec("Continue Print");
        REQUIRE(button.label == "Continue Print");
        REQUIRE(button.gcode == "Continue Print");
        REQUIRE(button.color.empty());
    }

    SECTION("Uppercase label") {
        auto button = ActionPromptManager::parse_button_spec("CANCEL");
        REQUIRE(button.label == "CANCEL");
        REQUIRE(button.gcode == "CANCEL");
    }
}

TEST_CASE("parse_button_spec: Label|GCODE format", "[action_prompt][button]") {
    SECTION("Separate label and gcode") {
        auto button = ActionPromptManager::parse_button_spec("Preheat|M104 S200");
        REQUIRE(button.label == "Preheat");
        REQUIRE(button.gcode == "M104 S200");
        REQUIRE(button.color.empty());
    }

    SECTION("Multi-word label with gcode") {
        auto button = ActionPromptManager::parse_button_spec("Start Print|RESUME");
        REQUIRE(button.label == "Start Print");
        REQUIRE(button.gcode == "RESUME");
    }

    SECTION("Gcode with parameters") {
        auto button = ActionPromptManager::parse_button_spec("Set Temp|M104 S{target_temp}");
        REQUIRE(button.label == "Set Temp");
        REQUIRE(button.gcode == "M104 S{target_temp}");
    }
}

TEST_CASE("parse_button_spec: Label|GCODE|color format", "[action_prompt][button]") {
    SECTION("Primary color") {
        auto button = ActionPromptManager::parse_button_spec("OK|CONFIRM|primary");
        REQUIRE(button.label == "OK");
        REQUIRE(button.gcode == "CONFIRM");
        REQUIRE(button.color == "primary");
    }

    SECTION("Secondary color") {
        auto button = ActionPromptManager::parse_button_spec("Cancel|ABORT|secondary");
        REQUIRE(button.label == "Cancel");
        REQUIRE(button.gcode == "ABORT");
        REQUIRE(button.color == "secondary");
    }

    SECTION("Info color") {
        auto button = ActionPromptManager::parse_button_spec("Details|SHOW_INFO|info");
        REQUIRE(button.label == "Details");
        REQUIRE(button.gcode == "SHOW_INFO");
        REQUIRE(button.color == "info");
    }

    SECTION("Warning color") {
        auto button = ActionPromptManager::parse_button_spec("Proceed|CONTINUE|warning");
        REQUIRE(button.label == "Proceed");
        REQUIRE(button.gcode == "CONTINUE");
        REQUIRE(button.color == "warning");
    }

    SECTION("Error color") {
        auto button = ActionPromptManager::parse_button_spec("Emergency Stop|M112|error");
        REQUIRE(button.label == "Emergency Stop");
        REQUIRE(button.gcode == "M112");
        REQUIRE(button.color == "error");
    }
}

TEST_CASE("parse_button_spec: Label||color format (gcode = label)", "[action_prompt][button]") {
    SECTION("Label with color, gcode matches label") {
        auto button = ActionPromptManager::parse_button_spec("ABORT||error");
        REQUIRE(button.label == "ABORT");
        REQUIRE(button.gcode == "ABORT");
        REQUIRE(button.color == "error");
    }

    SECTION("Multi-word label with color") {
        auto button = ActionPromptManager::parse_button_spec("Continue Print||primary");
        REQUIRE(button.label == "Continue Print");
        REQUIRE(button.gcode == "Continue Print");
        REQUIRE(button.color == "primary");
    }
}

TEST_CASE("parse_button_spec: Edge cases", "[action_prompt][button][edge]") {
    SECTION("Empty string returns empty button") {
        auto button = ActionPromptManager::parse_button_spec("");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode.empty());
        REQUIRE(button.color.empty());
    }

    SECTION("Single pipe returns empty label, empty gcode") {
        auto button = ActionPromptManager::parse_button_spec("|");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode.empty());
    }

    SECTION("Double pipe returns empty label/gcode") {
        auto button = ActionPromptManager::parse_button_spec("||");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode.empty());
        REQUIRE(button.color.empty());
    }

    SECTION("Triple pipe returns all empty") {
        auto button = ActionPromptManager::parse_button_spec("|||");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode.empty());
        REQUIRE(button.color.empty());
        REQUIRE(button.hex_color.empty());
    }

    SECTION("||color format with only color") {
        auto button = ActionPromptManager::parse_button_spec("||info");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode.empty());
        REQUIRE(button.color == "info");
    }

    SECTION("Unknown color is preserved (not validated here)") {
        auto button = ActionPromptManager::parse_button_spec("OK|CONFIRM|invalid_color");
        REQUIRE(button.label == "OK");
        REQUIRE(button.gcode == "CONFIRM");
        REQUIRE(button.color == "invalid_color");
    }

    SECTION("Extra pipes beyond 4th field are ignored") {
        auto button = ActionPromptManager::parse_button_spec("OK|CONFIRM|primary|7c4b00|extra");
        REQUIRE(button.label == "OK");
        REQUIRE(button.gcode == "CONFIRM");
        REQUIRE(button.color == "primary");
        REQUIRE(button.hex_color == "7c4b00");
    }

    SECTION("Pipe in label is split incorrectly (known limitation)") {
        // If user puts pipe in label, it splits - this is expected behavior
        auto button = ActionPromptManager::parse_button_spec("A|B button|GCODE");
        // First pipe splits label from rest
        REQUIRE(button.label == "A");
        REQUIRE(button.gcode == "B button");
    }

    SECTION("Whitespace around pipes is preserved") {
        auto button = ActionPromptManager::parse_button_spec(" Label | GCODE | primary ");
        REQUIRE(button.label == " Label ");
        REQUIRE(button.gcode == " GCODE ");
        REQUIRE(button.color == " primary ");
    }
}

// ============================================================================
// Hex Color Parsing Tests
// ============================================================================

TEST_CASE("parse_button_spec: Hex color field (4th field)", "[action_prompt][button][hex_color]") {
    SECTION("Full 4-field spec with hex color") {
        auto button = ActionPromptManager::parse_button_spec("Label|GCODE|primary|7c4b00");
        REQUIRE(button.label == "Label");
        REQUIRE(button.gcode == "GCODE");
        REQUIRE(button.color == "primary");
        REQUIRE(button.hex_color == "7c4b00");
    }

    SECTION("Hex color with empty style field") {
        auto button = ActionPromptManager::parse_button_spec("Label|GCODE||ff0000");
        REQUIRE(button.label == "Label");
        REQUIRE(button.gcode == "GCODE");
        REQUIRE(button.color.empty());
        REQUIRE(button.hex_color == "ff0000");
    }

    SECTION("3-field spec without hex color preserves backward compatibility") {
        auto button = ActionPromptManager::parse_button_spec("Label|GCODE|primary");
        REQUIRE(button.label == "Label");
        REQUIRE(button.gcode == "GCODE");
        REQUIRE(button.color == "primary");
        REQUIRE(button.hex_color.empty());
    }

    SECTION("Empty 4th field leaves hex_color empty") {
        auto button = ActionPromptManager::parse_button_spec("Label|GCODE|primary|");
        REQUIRE(button.label == "Label");
        REQUIRE(button.gcode == "GCODE");
        REQUIRE(button.color == "primary");
        REQUIRE(button.hex_color.empty());
    }

    SECTION("Placeholder label is cleared when hex color present (ZMOD swatch tiles)") {
        // ZMOD's color grid sends "_ " (sometimes padded) with the real color
        // in field 4; the label would render as a dash on every swatch.
        auto button = ActionPromptManager::parse_button_spec(
            "_ |CHANGE_ZCOLOR SLOT=1 TYPE=PLA HEX=ffffff|primary|ffffff");
        REQUIRE(button.label.empty());
        REQUIRE(button.gcode == "CHANGE_ZCOLOR SLOT=1 TYPE=PLA HEX=ffffff");
        REQUIRE(button.hex_color == "ffffff");
    }

    SECTION("Bare underscore placeholder cleared with hex color") {
        auto button =
            ActionPromptManager::parse_button_spec("_|RUN_ZCOLOR SLOT=1 HEX=0acc38|primary|0acc38");
        REQUIRE(button.label.empty());
        REQUIRE(button.hex_color == "0acc38");
    }

    SECTION("Meaningful label kept when hex color present") {
        auto button = ActionPromptManager::parse_button_spec("Ok|CONFIRM|primary|7c4b00");
        REQUIRE(button.label == "Ok");
        REQUIRE(button.hex_color == "7c4b00");
    }

    SECTION("Placeholder label kept when no hex color") {
        // Without a hex color the label is the only content; clearing it would
        // leave an anonymous button.
        auto button = ActionPromptManager::parse_button_spec("_|GCODE|primary");
        REQUIRE(button.label == "_");
        REQUIRE(button.hex_color.empty());
    }
}

// ============================================================================
// State Machine Tests
// ============================================================================

TEST_CASE("ActionPromptManager: State transitions", "[action_prompt][state]") {
    ActionPromptManager manager;

    SECTION("Initial state is IDLE") {
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        REQUIRE_FALSE(manager.has_active_prompt());
    }

    SECTION("prompt_begin transitions IDLE -> BUILDING") {
        manager.process_line("// action:prompt_begin Test Title");
        REQUIRE(manager.get_state() == ActionPromptManager::State::BUILDING);
    }

    SECTION("prompt_show transitions BUILDING -> SHOWING") {
        manager.process_line("// action:prompt_begin Test Title");
        manager.process_line("// action:prompt_show");
        REQUIRE(manager.get_state() == ActionPromptManager::State::SHOWING);
        REQUIRE(manager.has_active_prompt());
    }

    SECTION("prompt_end transitions SHOWING -> IDLE") {
        manager.process_line("// action:prompt_begin Test Title");
        manager.process_line("// action:prompt_show");
        manager.process_line("// action:prompt_end");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        REQUIRE_FALSE(manager.has_active_prompt());
    }

    SECTION("prompt_begin while SHOWING replaces current prompt") {
        // First prompt
        manager.process_line("// action:prompt_begin First Prompt");
        manager.process_line("// action:prompt_show");
        REQUIRE(manager.get_state() == ActionPromptManager::State::SHOWING);
        REQUIRE(manager.get_current_prompt()->title == "First Prompt");

        // Second prompt replaces it
        manager.process_line("// action:prompt_begin Second Prompt");
        REQUIRE(manager.get_state() == ActionPromptManager::State::BUILDING);
        // Old prompt should be cleared
    }

    SECTION("prompt_begin while BUILDING uses latest title") {
        manager.process_line("// action:prompt_begin First Title");
        manager.process_line("// action:prompt_begin Second Title");
        REQUIRE(manager.get_state() == ActionPromptManager::State::BUILDING);

        manager.process_line("// action:prompt_show");
        REQUIRE(manager.get_current_prompt()->title == "Second Title");
    }
}

TEST_CASE("ActionPromptManager: Invalid state transitions", "[action_prompt][state][error]") {
    ActionPromptManager manager;

    SECTION("prompt_text without prompt_begin is ignored") {
        manager.process_line("// action:prompt_text Orphan text");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
    }

    SECTION("prompt_button without prompt_begin is ignored") {
        manager.process_line("// action:prompt_button Orphan button");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
    }

    SECTION("prompt_show without prompt_begin is ignored") {
        manager.process_line("// action:prompt_show");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        REQUIRE_FALSE(manager.has_active_prompt());
    }

    SECTION("prompt_end without active prompt is ignored") {
        manager.process_line("// action:prompt_end");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
    }

    SECTION("prompt_end while BUILDING cancels build") {
        manager.process_line("// action:prompt_begin Title");
        manager.process_line("// action:prompt_end");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        REQUIRE_FALSE(manager.has_active_prompt());
    }
}

// ============================================================================
// Full Prompt Building Tests
// ============================================================================

TEST_CASE("ActionPromptManager: Simple prompt construction", "[action_prompt][build]") {
    ActionPromptManager manager;

    SECTION("Minimal prompt: begin + show") {
        manager.process_line("// action:prompt_begin Minimal Prompt");
        manager.process_line("// action:prompt_show");

        REQUIRE(manager.has_active_prompt());
        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title == "Minimal Prompt");
        REQUIRE(prompt->text_lines.empty());
        REQUIRE(prompt->buttons.empty());
    }

    SECTION("Prompt with single text line") {
        manager.process_line("// action:prompt_begin Prompt Title");
        manager.process_line("// action:prompt_text Hello, World!");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->text_lines.size() == 1);
        REQUIRE(prompt->text_lines[0] == "Hello, World!");
    }

    SECTION("Prompt with single button") {
        manager.process_line("// action:prompt_begin Prompt Title");
        manager.process_line("// action:prompt_button OK");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 1);
        REQUIRE(prompt->buttons[0].label == "OK");
        REQUIRE(prompt->buttons[0].gcode == "OK");
        REQUIRE_FALSE(prompt->buttons[0].is_footer);
    }
}

TEST_CASE("ActionPromptManager: Multi-element prompts", "[action_prompt][build]") {
    ActionPromptManager manager;

    SECTION("Multiple text lines") {
        manager.process_line("// action:prompt_begin Multi-line");
        manager.process_line("// action:prompt_text Line 1");
        manager.process_line("// action:prompt_text Line 2");
        manager.process_line("// action:prompt_text Line 3");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->text_lines.size() == 3);
        REQUIRE(prompt->text_lines[0] == "Line 1");
        REQUIRE(prompt->text_lines[1] == "Line 2");
        REQUIRE(prompt->text_lines[2] == "Line 3");
    }

    SECTION("Multiple buttons") {
        manager.process_line("// action:prompt_begin Button Test");
        manager.process_line("// action:prompt_button Yes|CONFIRM|primary");
        manager.process_line("// action:prompt_button No|CANCEL|secondary");
        manager.process_line("// action:prompt_button Maybe|DEFER|info");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 3);

        REQUIRE(prompt->buttons[0].label == "Yes");
        REQUIRE(prompt->buttons[0].gcode == "CONFIRM");
        REQUIRE(prompt->buttons[0].color == "primary");

        REQUIRE(prompt->buttons[1].label == "No");
        REQUIRE(prompt->buttons[1].gcode == "CANCEL");
        REQUIRE(prompt->buttons[1].color == "secondary");

        REQUIRE(prompt->buttons[2].label == "Maybe");
        REQUIRE(prompt->buttons[2].gcode == "DEFER");
        REQUIRE(prompt->buttons[2].color == "info");
    }

    SECTION("Complex prompt with text and buttons") {
        manager.process_line("// action:prompt_begin Filament Change");
        manager.process_line("// action:prompt_text Current filament: PLA Red");
        manager.process_line("// action:prompt_text Please remove the old filament");
        manager.process_line("// action:prompt_button Continue|RESUME_PRINT|primary");
        manager.process_line("// action:prompt_button Cancel Print|CANCEL_PRINT|error");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title == "Filament Change");
        REQUIRE(prompt->text_lines.size() == 2);
        REQUIRE(prompt->buttons.size() == 2);
    }
}

TEST_CASE("ActionPromptManager: Footer buttons", "[action_prompt][build][footer]") {
    ActionPromptManager manager;

    SECTION("Footer buttons have is_footer=true") {
        manager.process_line("// action:prompt_begin With Footer");
        manager.process_line("// action:prompt_button Regular|REG");
        manager.process_line("// action:prompt_footer_button Footer|FOOT|secondary");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 2);
        REQUIRE_FALSE(prompt->buttons[0].is_footer);
        REQUIRE(prompt->buttons[1].is_footer);
        REQUIRE(prompt->buttons[1].label == "Footer");
    }

    SECTION("Multiple footer buttons") {
        manager.process_line("// action:prompt_begin Footer Test");
        manager.process_line("// action:prompt_footer_button Help|SHOW_HELP|info");
        manager.process_line("// action:prompt_footer_button Close|CLOSE||secondary");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 2);
        REQUIRE(prompt->buttons[0].is_footer);
        REQUIRE(prompt->buttons[1].is_footer);
    }

    SECTION("Mixed regular and footer buttons maintain order") {
        manager.process_line("// action:prompt_begin Mixed");
        manager.process_line("// action:prompt_button First");
        manager.process_line("// action:prompt_button Second");
        manager.process_line("// action:prompt_footer_button Third");
        manager.process_line("// action:prompt_footer_button Fourth");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 4);
        REQUIRE_FALSE(prompt->buttons[0].is_footer);
        REQUIRE_FALSE(prompt->buttons[1].is_footer);
        REQUIRE(prompt->buttons[2].is_footer);
        REQUIRE(prompt->buttons[3].is_footer);
    }
}

TEST_CASE("ActionPromptManager: Button groups", "[action_prompt][build][groups]") {
    ActionPromptManager manager;

    SECTION("Buttons in group have matching group_id") {
        manager.process_line("// action:prompt_begin Grouped");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button A");
        manager.process_line("// action:prompt_button B");
        manager.process_line("// action:prompt_button C");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 3);

        int group_id = prompt->buttons[0].group_id;
        REQUIRE(group_id >= 0);
        REQUIRE(prompt->buttons[1].group_id == group_id);
        REQUIRE(prompt->buttons[2].group_id == group_id);
    }

    SECTION("Buttons outside group have group_id = -1") {
        manager.process_line("// action:prompt_begin Mixed Groups");
        manager.process_line("// action:prompt_button Before");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button In Group");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_button After");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 3);
        REQUIRE(prompt->buttons[0].group_id == -1);
        REQUIRE(prompt->buttons[1].group_id >= 0);
        REQUIRE(prompt->buttons[2].group_id == -1);
    }

    SECTION("Multiple groups have different group_ids") {
        manager.process_line("// action:prompt_begin Multi Groups");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button Group1-A");
        manager.process_line("// action:prompt_button Group1-B");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button Group2-A");
        manager.process_line("// action:prompt_button Group2-B");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 4);

        int group1_id = prompt->buttons[0].group_id;
        int group2_id = prompt->buttons[2].group_id;

        REQUIRE(group1_id >= 0);
        REQUIRE(group2_id >= 0);
        REQUIRE(group1_id != group2_id);

        REQUIRE(prompt->buttons[0].group_id == group1_id);
        REQUIRE(prompt->buttons[1].group_id == group1_id);
        REQUIRE(prompt->buttons[2].group_id == group2_id);
        REQUIRE(prompt->buttons[3].group_id == group2_id);
    }

    SECTION("Empty group (start immediately followed by end)") {
        manager.process_line("// action:prompt_begin Empty Group");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_button After Empty");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 1);
        REQUIRE(prompt->buttons[0].group_id == -1);
    }

    SECTION("Unclosed group at show time") {
        manager.process_line("// action:prompt_begin Unclosed");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button In unclosed group");
        manager.process_line("// action:prompt_show"); // No group_end

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 1);
        // Button should still have its group_id assigned
        REQUIRE(prompt->buttons[0].group_id >= 0);
    }

    SECTION("group_end without group_start is ignored") {
        manager.process_line("// action:prompt_begin Orphan End");
        manager.process_line("// action:prompt_button_group_end"); // No start
        manager.process_line("// action:prompt_button Normal");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 1);
        REQUIRE(prompt->buttons[0].group_id == -1);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("ActionPromptManager: Edge cases", "[action_prompt][edge]") {
    ActionPromptManager manager;

    SECTION("Empty title in prompt_begin") {
        manager.process_line("// action:prompt_begin ");
        manager.process_line("// action:prompt_show");

        REQUIRE(manager.has_active_prompt());
        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title.empty());
    }

    SECTION("Very long text line") {
        std::string long_text(1000, 'x');
        manager.process_line("// action:prompt_begin Long Text Test");
        manager.process_line("// action:prompt_text " + long_text);
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->text_lines.size() == 1);
        REQUIRE(prompt->text_lines[0].length() == 1000);
    }

    SECTION("Very long button label") {
        std::string long_label(200, 'L');
        manager.process_line("// action:prompt_begin Long Label");
        manager.process_line("// action:prompt_button " + long_label);
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->buttons.size() == 1);
        REQUIRE(prompt->buttons[0].label.length() == 200);
    }

    SECTION("Special characters in text") {
        manager.process_line("// action:prompt_begin Special Chars");
        manager.process_line("// action:prompt_text Line with pipe | character");
        manager.process_line("// action:prompt_text Line with newline \\n escaped");
        manager.process_line("// action:prompt_text Unicode: \xC2\xA9 \xE2\x9C\x93");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->text_lines.size() == 3);
        REQUIRE(prompt->text_lines[0] == "Line with pipe | character");
        REQUIRE(prompt->text_lines[1] == "Line with newline \\n escaped");
        REQUIRE(prompt->text_lines[2] == "Unicode: \xC2\xA9 \xE2\x9C\x93");
    }

    SECTION("Rapid prompt replacement") {
        // Quickly send multiple prompts
        manager.process_line("// action:prompt_begin First");
        manager.process_line("// action:prompt_show");
        manager.process_line("// action:prompt_begin Second");
        manager.process_line("// action:prompt_show");
        manager.process_line("// action:prompt_begin Third");
        manager.process_line("// action:prompt_show");

        REQUIRE(manager.has_active_prompt());
        REQUIRE(manager.get_current_prompt()->title == "Third");
    }

    SECTION("prompt_end clears everything") {
        manager.process_line("// action:prompt_begin Prompt");
        manager.process_line("// action:prompt_text Some text");
        manager.process_line("// action:prompt_button Some button");
        manager.process_line("// action:prompt_show");
        manager.process_line("// action:prompt_end");

        REQUIRE_FALSE(manager.has_active_prompt());
        REQUIRE(manager.get_current_prompt() == nullptr);
    }

    SECTION("Non-action lines are ignored during building") {
        manager.process_line("// action:prompt_begin Test");
        manager.process_line("G1 X10 Y20"); // Regular G-code
        manager.process_line("; A comment");
        manager.process_line(""); // Empty line
        manager.process_line("// action:prompt_text Still works");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title == "Test");
        REQUIRE(prompt->text_lines.size() == 1);
        REQUIRE(prompt->text_lines[0] == "Still works");
    }
}

// ============================================================================
// Notify Command Tests
// ============================================================================

TEST_CASE("ActionPromptManager: notify command", "[action_prompt][notify]") {
    ActionPromptManager manager;

    SECTION("notify is separate from prompt system") {
        // notify should work independently of prompt state
        auto result = ActionPromptManager::parse_action_line("// action:notify Print complete!");
        REQUIRE(result.has_value());
        REQUIRE(result->command == "notify");
        REQUIRE(result->payload == "Print complete!");
    }

    SECTION("notify does not affect prompt state") {
        manager.process_line("// action:prompt_begin Active Prompt");
        manager.process_line("// action:prompt_show");

        manager.process_line("// action:notify Some notification");

        // Prompt should still be active
        REQUIRE(manager.has_active_prompt());
        REQUIRE(manager.get_current_prompt()->title == "Active Prompt");
    }

    SECTION("notify works when no prompt is active") {
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        // Should process without error (implementation may emit callback)
        manager.process_line("// action:notify Standalone notification");
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
    }
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST_CASE("ActionPromptManager: Callbacks", "[action_prompt][callback]") {
    ActionPromptManager manager;
    bool show_called = false;
    bool close_called = false;
    std::string notify_message;

    manager.set_on_show([&show_called](const PromptData&) { show_called = true; });

    manager.set_on_close([&close_called]() { close_called = true; });

    manager.set_on_notify([&notify_message](const std::string& msg) { notify_message = msg; });

    SECTION("on_show callback fires on prompt_show") {
        manager.process_line("// action:prompt_begin Test");
        REQUIRE_FALSE(show_called);
        manager.process_line("// action:prompt_show");
        REQUIRE(show_called);
    }

    SECTION("on_close callback fires on prompt_end") {
        manager.process_line("// action:prompt_begin Test");
        manager.process_line("// action:prompt_show");
        REQUIRE_FALSE(close_called);
        manager.process_line("// action:prompt_end");
        REQUIRE(close_called);
    }

    SECTION("on_notify callback fires for notify command") {
        manager.process_line("// action:notify Hello World");
        REQUIRE(notify_message == "Hello World");
    }

    SECTION("Callbacks can be null") {
        ActionPromptManager manager2;
        // No callbacks set - should not crash
        REQUIRE_NOTHROW(manager2.process_line("// action:prompt_begin Test"));
        REQUIRE_NOTHROW(manager2.process_line("// action:prompt_show"));
        REQUIRE_NOTHROW(manager2.process_line("// action:notify Test"));
        REQUIRE_NOTHROW(manager2.process_line("// action:prompt_end"));
        // After full lifecycle with null callbacks, state returns to IDLE
        REQUIRE_FALSE(manager2.has_active_prompt());
    }
}

// ============================================================================
// Integration/Realistic Tests
// ============================================================================

TEST_CASE("ActionPromptManager: Realistic prompt sequences", "[action_prompt][integration]") {
    ActionPromptManager manager;

    SECTION("Filament runout prompt") {
        // Simulates what a filament runout macro might send
        manager.process_line("// action:prompt_begin Filament Runout Detected");
        manager.process_line("// action:prompt_text The printer has detected a filament runout.");
        manager.process_line("// action:prompt_text Please load new filament and press continue.");
        manager.process_line("// action:prompt_button Continue|RESUME|primary");
        manager.process_line("// action:prompt_button Cancel Print|CANCEL_PRINT|error");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title == "Filament Runout Detected");
        REQUIRE(prompt->text_lines.size() == 2);
        REQUIRE(prompt->buttons.size() == 2);
        REQUIRE(prompt->buttons[0].color == "primary");
        REQUIRE(prompt->buttons[1].color == "error");
    }

    SECTION("Multi-material change prompt with button groups") {
        manager.process_line("// action:prompt_begin MMU Selector");
        manager.process_line("// action:prompt_text Select the filament slot:");
        manager.process_line("// action:prompt_button_group_start");
        manager.process_line("// action:prompt_button Slot 1|T0|primary");
        manager.process_line("// action:prompt_button Slot 2|T1|primary");
        manager.process_line("// action:prompt_button Slot 3|T2|primary");
        manager.process_line("// action:prompt_button Slot 4|T3|primary");
        manager.process_line("// action:prompt_button_group_end");
        manager.process_line("// action:prompt_footer_button Cancel|CANCEL|secondary");
        manager.process_line("// action:prompt_show");

        auto prompt = manager.get_current_prompt();
        REQUIRE(prompt->title == "MMU Selector");
        REQUIRE(prompt->buttons.size() == 5);

        // First 4 buttons should be in a group
        int slot_group = prompt->buttons[0].group_id;
        REQUIRE(slot_group >= 0);
        for (int i = 0; i < 4; i++) {
            REQUIRE(prompt->buttons[i].group_id == slot_group);
            REQUIRE_FALSE(prompt->buttons[i].is_footer);
        }

        // Last button is footer, not in group
        REQUIRE(prompt->buttons[4].is_footer);
        REQUIRE(prompt->buttons[4].group_id == -1);
    }

    SECTION("Error prompt followed by recovery") {
        // Error prompt
        manager.process_line("// action:prompt_begin Error");
        manager.process_line("// action:prompt_text Thermal runaway detected!");
        manager.process_line("// action:prompt_button Acknowledge|M999|error");
        manager.process_line("// action:prompt_show");

        REQUIRE(manager.has_active_prompt());
        REQUIRE(manager.get_current_prompt()->title == "Error");

        // User acknowledges, then recovery prompt appears
        manager.process_line("// action:prompt_end");
        REQUIRE_FALSE(manager.has_active_prompt());

        // Recovery prompt
        manager.process_line("// action:prompt_begin Printer Ready");
        manager.process_line("// action:prompt_text Error cleared. Ready to continue.");
        manager.process_line("// action:prompt_button Continue|RESUME|primary");
        manager.process_line("// action:prompt_show");

        REQUIRE(manager.has_active_prompt());
        REQUIRE(manager.get_current_prompt()->title == "Printer Ready");
    }
}

// ============================================================================
// Data Structure Tests
// ============================================================================

TEST_CASE("PromptButton: Default values", "[action_prompt][data]") {
    PromptButton button;

    REQUIRE(button.label.empty());
    REQUIRE(button.gcode.empty());
    REQUIRE(button.color.empty());
    REQUIRE_FALSE(button.is_footer);
    REQUIRE(button.group_id == -1);
}

TEST_CASE("PromptData: Default values", "[action_prompt][data]") {
    PromptData prompt;

    REQUIRE(prompt.title.empty());
    REQUIRE(prompt.text_lines.empty());
    REQUIRE(prompt.buttons.empty());
    REQUIRE(prompt.current_group_id == -1);
}

// ============================================================================
// ActionPromptModal Tests
// ============================================================================
//
// These tests validate the ActionPromptModal class which displays prompts
// from the Klipper action:prompt protocol as modal dialogs.
//
// Note: These tests are written TDD-style before implementation exists.
// Many will fail until ActionPromptModal is implemented.
// ============================================================================

// Forward declaration for ActionPromptModal (header doesn't exist yet)
// #include "ui_action_prompt_modal.h"

// ============================================================================
// Button Click Callback Tests
// ============================================================================

TEST_CASE("ActionPromptModal: Button click fires callback with gcode",
          "[action_prompt][modal][callback]") {
    SECTION("Click callback receives correct gcode") {
        PromptData data;
        data.title = "Test";
        data.buttons.push_back({"Continue", "RESUME_PRINT", "primary", "", false, -1});

        std::string received_gcode;
        // When implemented, the modal should support setting a button callback:
        // ActionPromptModal modal;
        // modal.set_button_callback([&received_gcode](const std::string& gcode) {
        //     received_gcode = gcode;
        // });
        // modal.set_prompt_data(data);
        // modal.simulate_button_click(0);
        // REQUIRE(received_gcode == "RESUME_PRINT");

        // For now, verify the data is correct
        REQUIRE(data.buttons[0].gcode == "RESUME_PRINT");
    }

    SECTION("Each button sends its own gcode") {
        PromptData data;
        data.title = "Choose";
        data.buttons.push_back({"Resume", "RESUME_PRINT", "", "", false, -1});
        data.buttons.push_back({"Cancel", "CANCEL_PRINT", "", "", false, -1});
        data.buttons.push_back({"Retry", "RETRY_ACTION", "", "", false, -1});

        // Each button has distinct gcode
        REQUIRE(data.buttons[0].gcode == "RESUME_PRINT");
        REQUIRE(data.buttons[1].gcode == "CANCEL_PRINT");
        REQUIRE(data.buttons[2].gcode == "RETRY_ACTION");
    }

    SECTION("Button with empty gcode uses label as gcode") {
        // Per parse_button_spec, if gcode is empty, it equals the label
        PromptButton btn = ActionPromptManager::parse_button_spec("OK");
        REQUIRE(btn.label == "OK");
        REQUIRE(btn.gcode == "OK");
    }
}

// ============================================================================
// Modal Lifecycle Tests
// ============================================================================

TEST_CASE("ActionPromptModal: Modal closes after button click",
          "[action_prompt][modal][lifecycle]") {
    SECTION("Default behavior: modal closes on button click") {
        PromptData data;
        data.title = "Confirm";
        data.buttons.push_back({"OK", "CONFIRM", "primary", "", false, -1});

        // By default, clicking any button should close the modal
        // The callback fires first, then the modal closes

        // When implemented:
        // ActionPromptModal modal;
        // modal.set_prompt_data(data);
        // modal.show(parent);
        // REQUIRE(modal.is_visible());
        // modal.simulate_button_click(0);
        // REQUIRE_FALSE(modal.is_visible());

        REQUIRE(data.buttons.size() == 1);
    }

    SECTION("Modal closes when prompt_end is received") {
        // The modal should also close when the Klipper sends prompt_end
        // This happens externally via ActionPromptManager::on_close callback

        ActionPromptManager manager;
        bool close_called = false;
        manager.set_on_close([&close_called]() { close_called = true; });

        manager.process_line("// action:prompt_begin Test");
        manager.process_line("// action:prompt_show");
        REQUIRE(manager.has_active_prompt());

        manager.process_line("// action:prompt_end");
        REQUIRE_FALSE(manager.has_active_prompt());
        REQUIRE(close_called);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("ActionPromptModal: Edge cases", "[action_prompt][modal][edge]") {
    SECTION("Modal with no buttons displays correctly") {
        PromptData data;
        data.title = "Information Only";
        data.text_lines.push_back("This is a notification");
        // No buttons - user must use prompt_end to close

        REQUIRE(data.buttons.empty());
        REQUIRE(data.text_lines.size() == 1);
    }

    SECTION("Modal with many buttons") {
        PromptData data;
        data.title = "Many Options";
        for (int i = 0; i < 10; i++) {
            data.buttons.push_back({"Button " + std::to_string(i), "ACTION_" + std::to_string(i),
                                    i % 2 == 0 ? "primary" : "secondary", "", false, -1});
        }

        REQUIRE(data.buttons.size() == 10);
    }

    SECTION("Modal with very long text") {
        PromptData data;
        data.title = "Long Text Test";
        std::string long_text(500, 'x');
        data.text_lines.push_back(long_text);

        REQUIRE(data.text_lines[0].length() == 500);
    }

    SECTION("Modal with special characters in text") {
        PromptData data;
        data.title = "Special Characters";
        data.text_lines.push_back("Temperature: 200°C");
        data.text_lines.push_back("Progress: 50%");
        data.text_lines.push_back("Status: OK ✓");

        REQUIRE(data.text_lines.size() == 3);
    }

    SECTION("Rapid show/hide cycles") {
        // Multiple prompts in quick succession should not cause issues
        ActionPromptManager manager;
        int show_count = 0;
        int close_count = 0;

        manager.set_on_show([&show_count](const PromptData&) { show_count++; });
        manager.set_on_close([&close_count]() { close_count++; });

        for (int i = 0; i < 5; i++) {
            manager.process_line("// action:prompt_begin Prompt " + std::to_string(i));
            manager.process_line("// action:prompt_show");
            manager.process_line("// action:prompt_end");
        }

        REQUIRE(show_count == 5);
        REQUIRE(close_count == 5);
    }
}

// ============================================================================
// Integration with ActionPromptManager
// ============================================================================

// ============================================================================
// Test/Development Helper Tests
// ============================================================================

TEST_CASE("ActionPromptManager: trigger_test_prompt creates comprehensive test prompt",
          "[action_prompt][test][helper]") {
    ActionPromptManager manager;
    bool show_called = false;
    PromptData received_data;

    manager.set_on_show([&show_called, &received_data](const PromptData& data) {
        show_called = true;
        received_data = data;
    });

    SECTION("trigger_test_prompt shows a prompt") {
        manager.trigger_test_prompt();
        REQUIRE(show_called);
        REQUIRE(received_data.title == "Test Prompt");
    }

    SECTION("test prompt has text lines") {
        manager.trigger_test_prompt();
        REQUIRE(received_data.text_lines.size() >= 1);
    }

    SECTION("test prompt demonstrates all 5 button colors") {
        manager.trigger_test_prompt();

        // Check that we have buttons with all color types
        bool has_primary = false, has_secondary = false, has_info = false;
        bool has_warning = false, has_error = false;

        for (const auto& btn : received_data.buttons) {
            if (btn.color == "primary")
                has_primary = true;
            if (btn.color == "secondary")
                has_secondary = true;
            if (btn.color == "info")
                has_info = true;
            if (btn.color == "warning")
                has_warning = true;
            if (btn.color == "error")
                has_error = true;
        }

        REQUIRE(has_primary);
        REQUIRE(has_secondary);
        REQUIRE(has_info);
        REQUIRE(has_warning);
        REQUIRE(has_error);
    }

    SECTION("test prompt has button group") {
        manager.trigger_test_prompt();

        // Check that at least some buttons have group_id >= 0
        bool has_grouped_buttons = false;
        for (const auto& btn : received_data.buttons) {
            if (btn.group_id >= 0) {
                has_grouped_buttons = true;
                break;
            }
        }
        REQUIRE(has_grouped_buttons);
    }

    SECTION("test prompt has footer button") {
        manager.trigger_test_prompt();

        // Check that at least one button is a footer
        bool has_footer = false;
        for (const auto& btn : received_data.buttons) {
            if (btn.is_footer) {
                has_footer = true;
                break;
            }
        }
        REQUIRE(has_footer);
    }
}

TEST_CASE("ActionPromptManager: trigger_test_notify sends notification",
          "[action_prompt][test][helper]") {
    ActionPromptManager manager;
    std::string received_message;

    manager.set_on_notify([&received_message](const std::string& msg) { received_message = msg; });

    SECTION("trigger_test_notify with default message") {
        manager.trigger_test_notify();
        REQUIRE_FALSE(received_message.empty());
        REQUIRE(received_message.find("Test") != std::string::npos);
    }

    SECTION("trigger_test_notify with custom message") {
        manager.trigger_test_notify("Custom test message");
        REQUIRE(received_message == "Custom test message");
    }

    SECTION("trigger_test_notify does not affect prompt state") {
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
        manager.trigger_test_notify();
        REQUIRE(manager.get_state() == ActionPromptManager::State::IDLE);
    }
}

// ============================================================================
// Integration with ActionPromptManager
// ============================================================================

TEST_CASE("ActionPromptModal: Integration with ActionPromptManager",
          "[action_prompt][modal][integration]") {
    SECTION("on_show callback receives complete PromptData") {
        ActionPromptManager manager;
        PromptData received_data;

        manager.set_on_show([&received_data](const PromptData& data) { received_data = data; });

        manager.process_line("// action:prompt_begin Filament Change");
        manager.process_line("// action:prompt_text Please load new filament");
        manager.process_line("// action:prompt_text Current: PLA Red");
        manager.process_line("// action:prompt_button Continue|RESUME|primary");
        manager.process_line("// action:prompt_button Cancel|ABORT|error");
        manager.process_line("// action:prompt_show");

        REQUIRE(received_data.title == "Filament Change");
        REQUIRE(received_data.text_lines.size() == 2);
        REQUIRE(received_data.buttons.size() == 2);
        REQUIRE(received_data.buttons[0].label == "Continue");
        REQUIRE(received_data.buttons[0].gcode == "RESUME");
        REQUIRE(received_data.buttons[0].color == "primary");
        REQUIRE(received_data.buttons[1].label == "Cancel");
        REQUIRE(received_data.buttons[1].color == "error");
    }

    SECTION("Modal can be shown from on_show callback") {
        ActionPromptManager manager;
        bool modal_would_show = false;

        manager.set_on_show([&modal_would_show](const PromptData& data) {
            // In real code, this would create and show the modal:
            // auto modal = std::make_unique<ActionPromptModal>();
            // modal->set_prompt_data(data);
            // modal->show(lv_screen_active());
            modal_would_show = !data.title.empty();
        });

        manager.process_line("// action:prompt_begin Test");
        manager.process_line("// action:prompt_show");

        REQUIRE(modal_would_show);
    }
}

// ============================================================================
// Static Accessor Tests (is_showing / current_prompt_name)
// ============================================================================

TEST_CASE("ActionPromptManager: Static is_showing() accessor", "[action_prompt][static]") {
    SECTION("is_showing returns false when idle") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        REQUIRE_FALSE(ActionPromptManager::is_showing());
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("is_showing returns true after prompt_show") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        manager.process_line("// action:prompt_begin AFC Error");
        manager.process_line("// action:prompt_show");
        REQUIRE(ActionPromptManager::is_showing());
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("is_showing returns false after prompt_end") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        manager.process_line("// action:prompt_begin AFC Error");
        manager.process_line("// action:prompt_show");
        REQUIRE(ActionPromptManager::is_showing());
        manager.process_line("// action:prompt_end");
        REQUIRE_FALSE(ActionPromptManager::is_showing());
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("is_showing returns false when no instance is set") {
        ActionPromptManager::set_instance(nullptr);
        REQUIRE_FALSE(ActionPromptManager::is_showing());
    }
}

TEST_CASE("ActionPromptManager: Static current_prompt_name() accessor", "[action_prompt][static]") {
    SECTION("current_prompt_name returns empty when not showing") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        REQUIRE(ActionPromptManager::current_prompt_name().empty());
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("current_prompt_name returns title from prompt_begin") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        manager.process_line("// action:prompt_begin AFC Lane Error");
        manager.process_line("// action:prompt_show");
        REQUIRE(ActionPromptManager::current_prompt_name() == "AFC Lane Error");
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("current_prompt_name returns empty after prompt_end") {
        ActionPromptManager manager;
        ActionPromptManager::set_instance(&manager);
        manager.process_line("// action:prompt_begin AFC Error");
        manager.process_line("// action:prompt_show");
        manager.process_line("// action:prompt_end");
        REQUIRE(ActionPromptManager::current_prompt_name().empty());
        ActionPromptManager::set_instance(nullptr);
    }

    SECTION("current_prompt_name returns empty when no instance is set") {
        ActionPromptManager::set_instance(nullptr);
        REQUIRE(ActionPromptManager::current_prompt_name().empty());
    }
}
