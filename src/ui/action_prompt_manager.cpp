// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file action_prompt_manager.cpp
 * @brief Implementation of Klipper action:prompt protocol parser and state machine
 */

#include "action_prompt_manager.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <string_view>

namespace helix {

namespace {

// Every command the protocol defines, LONGEST FIRST so that a prefix match can
// never pick "prompt_button" out of "prompt_button_group_start".
constexpr std::string_view ACTION_COMMANDS[] = {
    "prompt_button_group_start",
    "prompt_button_group_end",
    "prompt_footer_button",
    "prompt_button",
    "prompt_begin",
    "prompt_text",
    "prompt_show",
    "prompt_end",
    "notify",
};

/// Recover the command from a firmware line that forgot the separating space,
/// e.g. FlashForge/ZMOD emits `// action:prompt_beginResumption interrupted!!!`
/// for its runout abort. Returns the matched command length, or 0 when `token`
/// is not a known command with something glued onto it.
///
/// A remainder starting with `_` is NOT split: real command names are lowercase
/// snake_case, so `prompt_begin_something` is far more likely to be a protocol
/// command this build does not know yet than a glued title, and mis-splitting it
/// would silently route it to the wrong handler.
size_t glued_command_length(std::string_view token) {
    for (std::string_view cmd : ACTION_COMMANDS) {
        if (token.size() <= cmd.size() || token.compare(0, cmd.size(), cmd) != 0) {
            continue;
        }
        if (token[cmd.size()] == '_') {
            return 0;
        }
        return cmd.size();
    }
    return 0;
}

} // namespace

// Static instance pointer for cross-TU access (atomic for thread-safe reads from websocket thread)
std::atomic<ActionPromptManager*> ActionPromptManager::s_instance{nullptr};

// ============================================================================
// Static Parsing Functions
// ============================================================================

std::optional<ActionLineResult> ActionPromptManager::parse_action_line(const std::string& line) {
    // Find the "// action:" prefix, allowing leading whitespace
    const std::string prefix = "// action:";

    // Skip leading whitespace
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }

    // Check if the line starts with "// action:" after whitespace
    if (line.size() < start + prefix.size()) {
        return std::nullopt;
    }

    if (line.substr(start, prefix.size()) != prefix) {
        return std::nullopt;
    }

    // Extract the rest after "// action:"
    size_t command_start = start + prefix.size();
    if (command_start >= line.size()) {
        return std::nullopt;
    }

    // Find the end of the command (first space or end of line)
    size_t command_end = command_start;
    while (command_end < line.size() && line[command_end] != ' ' && line[command_end] != '\t') {
        ++command_end;
    }

    std::string command = line.substr(command_start, command_end - command_start);
    if (command.empty()) {
        return std::nullopt;
    }

    // Extract payload (everything after the first space following the command)
    std::string payload;
    if (command_end < line.size()) {
        // Skip exactly one space after the command
        payload = line.substr(command_end + 1);
    }

    // Un-glue a missing separator before the payload is handed on. The glued
    // text belongs at the FRONT of the payload — it is the start of the title
    // the firmware meant to send.
    if (size_t cmd_len = glued_command_length(command); cmd_len > 0) {
        std::string glued = command.substr(cmd_len);
        command.resize(cmd_len);
        payload = payload.empty() ? glued : glued + " " + payload;
        spdlog::warn("[ActionPrompt] Malformed action line (missing space after '{}'): '{}' — "
                     "recovered payload '{}'",
                     command, line, payload);
    }

    return ActionLineResult{command, payload};
}

PromptButton ActionPromptManager::parse_button_spec(const std::string& spec) {
    PromptButton button;

    if (spec.empty()) {
        return button;
    }

    // Split by '|' character
    std::vector<std::string> parts;
    size_t start = 0;
    size_t pos = 0;

    while ((pos = spec.find('|', start)) != std::string::npos) {
        parts.push_back(spec.substr(start, pos - start));
        start = pos + 1;
    }
    parts.push_back(spec.substr(start));

    // Field 0 = label
    if (!parts.empty()) {
        button.label = parts[0];
    }

    // Field 1 = gcode (if empty, use label)
    if (parts.size() > 1 && !parts[1].empty()) {
        button.gcode = parts[1];
    } else {
        button.gcode = button.label;
    }

    // Field 2 = color
    if (parts.size() > 2) {
        button.color = parts[2];
    }

    // Field 3 = hex_color (raw hex override, e.g., "7c4b00")
    if (parts.size() > 3 && !parts[3].empty()) {
        button.hex_color = parts[3];
    }

    return button;
}

// ============================================================================
// State Machine
// ============================================================================

const PromptData* ActionPromptManager::get_current_prompt() const {
    if (m_state == State::SHOWING && m_current_prompt) {
        return m_current_prompt.get();
    }
    return nullptr;
}

void ActionPromptManager::process_line(const std::string& line) {
    auto result = parse_action_line(line);
    if (!result.has_value()) {
        return; // Not an action line, ignore
    }

    const std::string& command = result->command;
    const std::string& payload = result->payload;

    spdlog::debug("ActionPromptManager: command='{}' payload='{}'", command, payload);

    if (command == "prompt_begin") {
        handle_prompt_begin(payload);
    } else if (command == "prompt_text") {
        handle_prompt_text(payload);
    } else if (command == "prompt_button") {
        handle_prompt_button(payload, false);
    } else if (command == "prompt_footer_button") {
        handle_prompt_button(payload, true);
    } else if (command == "prompt_button_group_start") {
        handle_prompt_button_group_start();
    } else if (command == "prompt_button_group_end") {
        handle_prompt_button_group_end();
    } else if (command == "prompt_show") {
        handle_prompt_show();
    } else if (command == "prompt_end") {
        handle_prompt_end();
    } else if (command == "notify") {
        handle_notify(payload);
    } else {
        spdlog::debug("ActionPromptManager: unknown command '{}'", command);
    }
}

// ============================================================================
// Command Handlers
// ============================================================================

void ActionPromptManager::handle_prompt_begin(const std::string& payload) {
    // If currently showing, close the current prompt first
    if (m_state == State::SHOWING && m_on_close) {
        m_on_close();
    }

    // Create a new prompt
    m_current_prompt = std::make_unique<PromptData>();
    m_current_prompt->title = payload;

    // Reset group tracking for this new prompt
    m_in_group = false;
    // Note: m_next_group_id is NOT reset - it keeps incrementing across prompts

    m_state = State::BUILDING;
}

void ActionPromptManager::handle_prompt_text(const std::string& payload) {
    if (m_state != State::BUILDING || !m_current_prompt) {
        return; // Ignore text outside of BUILDING state
    }

    m_current_prompt->text_lines.push_back(payload);
}

void ActionPromptManager::handle_prompt_button(const std::string& payload, bool is_footer) {
    if (m_state != State::BUILDING || !m_current_prompt) {
        return; // Ignore buttons outside of BUILDING state
    }

    PromptButton button = parse_button_spec(payload);
    button.is_footer = is_footer;

    // Assign group_id if currently in a group
    if (m_in_group) {
        button.group_id = m_current_prompt->current_group_id;
    } else {
        button.group_id = -1;
    }

    m_current_prompt->buttons.push_back(button);
}

void ActionPromptManager::handle_prompt_button_group_start() {
    if (m_state != State::BUILDING || !m_current_prompt) {
        return;
    }

    // Assign a new group ID
    m_current_prompt->current_group_id = m_next_group_id++;
    m_in_group = true;
}

void ActionPromptManager::handle_prompt_button_group_end() {
    if (m_state != State::BUILDING || !m_current_prompt) {
        return;
    }

    // End the current group
    m_current_prompt->current_group_id = -1;
    m_in_group = false;
}

void ActionPromptManager::handle_prompt_show() {
    if (m_state != State::BUILDING || !m_current_prompt) {
        return; // Ignore show without a prompt being built
    }

    m_state = State::SHOWING;

    if (m_on_show) {
        m_on_show(*m_current_prompt);
    }
}

void ActionPromptManager::handle_prompt_end() {
    if (m_state == State::IDLE) {
        return; // Nothing to end
    }

    // Fire close callback only if we were showing
    if (m_state == State::SHOWING && m_on_close) {
        m_on_close();
    }

    // Clear the prompt and return to IDLE
    m_current_prompt.reset();
    m_in_group = false;
    m_state = State::IDLE;
}

void ActionPromptManager::handle_notify(const std::string& payload) {
    // Notify is independent of prompt state
    if (m_on_notify) {
        m_on_notify(payload);
    }
}

// ============================================================================
// Test/Development Helpers
// ============================================================================

void ActionPromptManager::trigger_test_prompt() {
    spdlog::info("[ActionPromptManager] Triggering test prompt");

    // Build a comprehensive test prompt showing all features:
    // - Title and text
    // - All 5 button colors
    // - Footer button
    // - Button group

    process_line("// action:prompt_begin Test Prompt");
    process_line("// action:prompt_text This is a test prompt for development.");
    process_line("// action:prompt_text Press any button to dismiss.");

    // All 5 button colors (regular buttons)
    process_line("// action:prompt_button Primary|RESPOND msg=\"primary\"|primary");
    process_line("// action:prompt_button Secondary|RESPOND msg=\"secondary\"|secondary");
    process_line("// action:prompt_button Info|RESPOND msg=\"info\"|info");
    process_line("// action:prompt_button Warning|RESPOND msg=\"warning\"|warning");
    process_line("// action:prompt_button Error|RESPOND msg=\"error\"|error");

    // Button group example
    process_line("// action:prompt_button_group_start");
    process_line("// action:prompt_button Yes|RESPOND msg=\"yes\"|primary");
    process_line("// action:prompt_button No|RESPOND msg=\"no\"|secondary");
    process_line("// action:prompt_button_group_end");

    // Footer button
    process_line("// action:prompt_footer_button Cancel|RESPOND msg=\"cancel\"|error");

    process_line("// action:prompt_show");
}

void ActionPromptManager::trigger_test_notify(const std::string& message) {
    std::string msg = message.empty() ? "Test notification from action:notify" : message;
    spdlog::info("[ActionPromptManager] Triggering test notification: {}", msg);

    process_line("// action:notify " + msg);
}

} // namespace helix
