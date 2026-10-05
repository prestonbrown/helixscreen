// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file action_prompt_manager.h
 * @brief Klipper action:prompt protocol parser and state machine
 *
 * Handles parsing and processing of Klipper's action:prompt messages received
 * via notify_gcode_response. These messages allow Klipper macros to display
 * interactive prompts on the touchscreen.
 *
 * ## Protocol Overview
 *
 * Messages arrive with "// action:" prefix:
 * - `prompt_begin <title>` - Start building a new prompt
 * - `prompt_text <message>` - Add a text line
 * - `prompt_button <spec>` - Add a button (format: label|gcode|color)
 * - `prompt_footer_button <spec>` - Add a footer button
 * - `prompt_button_group_start` - Start a button group
 * - `prompt_button_group_end` - End a button group
 * - `prompt_show` - Display the prompt
 * - `prompt_end` - Close the prompt
 * - `notify <message>` - Show a standalone notification
 *
 * ## Threading
 *
 * Every state change runs on the main thread. Lines from the WebSocket thread
 * go through make_line_sink(), which hops them to the main thread, so
 * process_line() and the on_show/on_close callbacks are always main-thread
 * calls. is_showing() and current_prompt_name() may be called from any thread.
 *
 * ## Usage
 *
 * @code
 * ActionPromptManager manager;
 * manager.set_on_show([](const PromptData& data) {
 *     // Display the prompt UI
 * });
 * manager.set_on_close([]() {
 *     // Hide the prompt UI
 * });
 *
 * // Process each line from notify_gcode_response
 * manager.process_line("// action:prompt_begin Filament Change");
 * manager.process_line("// action:prompt_text Please load filament");
 * manager.process_line("// action:prompt_button Continue|RESUME|primary");
 * manager.process_line("// action:prompt_show");
 * @endcode
 *
 * @see https://www.klipper3d.org/G-Codes.html#action-commands
 */

namespace helix {

/**
 * @brief Represents a single button in an action prompt
 */
struct PromptButton {
    std::string label;      ///< Display text for the button
    std::string gcode;      ///< G-code to execute when clicked (empty = use label)
    std::string color;      ///< Color hint: primary/secondary/info/warning/error (empty = default)
    std::string hex_color;  ///< Raw hex color override (e.g., "7c4b00"), takes priority over color
    bool is_footer = false; ///< True if this is a footer button
    int group_id = -1;      ///< Group ID for button grouping (-1 = not grouped)
};

/**
 * @brief Data structure for a complete action prompt
 */
struct PromptData {
    std::string title;                   ///< Prompt title
    std::vector<std::string> text_lines; ///< Text content lines
    std::vector<PromptButton> buttons;   ///< All buttons (regular + footer)
    int current_group_id = -1;           ///< Current group being built (-1 = no active group)
    std::string severity; ///< "" (neutral, default) or "error" — drives the modal's error icon
};

/**
 * @brief How a prompt's dialog was closed on this screen
 */
enum class PromptCloseKind {
    ButtonWithGcode,    ///< A button tap that sent its own gcode
    ButtonWithoutGcode, ///< A button with no gcode: a plain dismiss
    UserDismiss,        ///< Backdrop tap or ESC
    HotReload,          ///< XML hot reload rebuilt the dialog (dev builds)
    External,           ///< A system sweep closed it (ctl reset, fault-modal dismissal)
};

/**
 * @brief Parsed result from an action line
 */
struct ActionLineResult {
    std::string command; ///< The action command (e.g., "prompt_begin", "prompt_text")
    std::string payload; ///< The payload after the command (may be empty)
};

/**
 * @brief State machine for processing Klipper action:prompt messages
 *
 * Manages the lifecycle of action prompts from begin to end, parsing
 * incoming messages and building up PromptData structures.
 */
class ActionPromptManager {
  public:
    /**
     * @brief State machine states
     */
    enum class State {
        IDLE,     ///< No prompt active, waiting for prompt_begin
        BUILDING, ///< Building a prompt, waiting for content or prompt_show
        SHOWING   ///< Prompt is being displayed, waiting for prompt_end
    };

    // Callback types
    using ShowCallback = std::function<void(const PromptData&)>;
    using CloseCallback = std::function<void()>;
    using NotifyCallback = std::function<void(const std::string&)>;

    ActionPromptManager() = default;
    /// Unregisters itself if it is the instance, so the accessors stop reporting its prompt.
    ~ActionPromptManager();

    /// Echoed back by Klipper as `// action:prompt_end`, which closes the prompt
    /// on every connected client. Sent when the user closes it here.
    static constexpr const char* PROMPT_END_GCODE =
        "RESPOND TYPE=command MSG=\"action:prompt_end\"";

    // ========================================================================
    // Static Instance Access
    // ========================================================================

    /**
     * @brief Set the global instance pointer for static accessors
     *
     * Called by Application when the ActionPromptManager is created/destroyed.
     * Enables other translation units (e.g., AmsBackendAfc) to query prompt state.
     * Main thread only; publishes the instance's current prompt for the accessors.
     *
     * @param instance Pointer to the active manager, or nullptr to clear
     */
    static void set_instance(ActionPromptManager* instance);

    /**
     * @brief Check if an action prompt is currently being displayed
     *
     * Safe from any thread: reads only the published title, never the prompt
     * the main thread is editing. A reader on another thread may see a value
     * one transition old.
     *
     * @return true if a prompt is currently visible
     */
    [[nodiscard]] static bool is_showing() {
        return std::atomic_load(&s_showing_title) != nullptr;
    }

    /**
     * @brief Get the title/name of the currently displayed prompt
     *
     * Returns the title from prompt_begin if a prompt is currently showing,
     * otherwise an empty string. Safe from any thread, with the same staleness
     * as is_showing().
     *
     * @return Current prompt title, or empty string
     */
    [[nodiscard]] static std::string current_prompt_name() {
        auto title = std::atomic_load(&s_showing_title);
        return title ? *title : std::string{};
    }

    /**
     * @brief Close the prompt on screen locally, as if prompt_end had arrived
     *
     * For a prompt that another dialog already covers. Main thread only.
     */
    static void dismiss_active() {
        if (auto* inst = s_instance.load(std::memory_order_acquire)) {
            inst->handle_prompt_end();
        }
    }

    // Non-copyable, non-movable: make_line_sink() hands out callables bound to `this`
    ActionPromptManager(const ActionPromptManager&) = delete;
    ActionPromptManager& operator=(const ActionPromptManager&) = delete;
    ActionPromptManager(ActionPromptManager&&) = delete;
    ActionPromptManager& operator=(ActionPromptManager&&) = delete;

    // ========================================================================
    // Static Parsing Functions (can be tested without instance)
    // ========================================================================

    /**
     * @brief Parse an action line and extract command + payload
     *
     * Parses lines in the format "// action:<command> <payload>".
     * Returns nullopt if the line is not a valid action line.
     *
     * @param line The raw line to parse
     * @return ActionLineResult if valid, nullopt otherwise
     */
    [[nodiscard]] static std::optional<ActionLineResult> parse_action_line(const std::string& line);

    /**
     * @brief Parse a button specification string
     *
     * Parses button specs in the format:
     * - "label" -> label=gcode=label, color empty
     * - "label|gcode" -> separate label and gcode
     * - "label|gcode|color" -> all three fields
     * - "label||color" -> label=gcode, with color
     *
     * @param spec The button specification string
     * @return PromptButton with parsed fields
     */
    [[nodiscard]] static PromptButton parse_button_spec(const std::string& spec);

    // ========================================================================
    // State Machine
    // ========================================================================

    /**
     * @brief Get the current state machine state
     * @return Current State
     */
    [[nodiscard]] State get_state() const {
        return m_state;
    }

    /**
     * @brief Check if a prompt is currently active (SHOWING state)
     * @return true if a prompt is being displayed
     */
    [[nodiscard]] bool has_active_prompt() const {
        return m_state == State::SHOWING;
    }

    /**
     * @brief Get the current prompt data
     * @return Pointer to PromptData if active, nullptr otherwise
     */
    [[nodiscard]] const PromptData* get_current_prompt() const;

    // ========================================================================
    // Processing
    // ========================================================================

    /**
     * @brief Process a single line from notify_gcode_response
     *
     * Main entry point for feeding lines to the state machine.
     * Non-action lines are silently ignored. Main thread only; other threads
     * feed lines through make_line_sink().
     *
     * @param line The raw line to process
     */
    void process_line(const std::string& line);

    /**
     * @brief A callable that feeds lines to process_line() from any thread
     *
     * Call on the main thread. The returned callable may run on any thread: it
     * drops non-action lines where it runs and defers the rest to the main
     * thread, where they are skipped once this manager is destroyed.
     */
    [[nodiscard]] std::function<void(const std::string&)> make_line_sink();

    /**
     * @brief The showing prompt's dialog was closed on this screen
     *
     * Brings the state in line with the screen without firing on_close: the
     * dialog is already going away. A user close and an External sweep end the
     * prompt; HotReload keeps it and re-shows it on a later tick. Does nothing
     * unless a prompt is SHOWING. Telling the firmware is the caller's job.
     * Main thread only.
     *
     * @return true when the caller should send PROMPT_END_GCODE to Klipper
     */
    bool closed_on_screen(PromptCloseKind kind);

    // ========================================================================
    // Callbacks
    // ========================================================================

    /**
     * @brief Set callback for when a prompt should be shown
     * @param callback Function to call with prompt data
     */
    void set_on_show(ShowCallback callback) {
        m_on_show = std::move(callback);
    }

    /**
     * @brief Set callback for when the prompt should be closed
     * @param callback Function to call on close
     */
    void set_on_close(CloseCallback callback) {
        m_on_close = std::move(callback);
    }

    /**
     * @brief Set callback for notify messages
     * @param callback Function to call with notification message
     */
    void set_on_notify(NotifyCallback callback) {
        m_on_notify = std::move(callback);
    }

    // ========================================================================
    // Test/Development Helpers
    // ========================================================================

    /**
     * @brief Trigger a test prompt for development/testing
     *
     * Shows a sample prompt with various button types for testing
     * the UI without a real Klipper connection. Demonstrates all
     * five button colors, footer buttons, and button groups.
     *
     * Only call this in test mode (RuntimeConfig::is_test_mode()).
     */
    void trigger_test_prompt();

    /**
     * @brief Trigger a test notification
     *
     * Shows a sample notification toast for testing the action:notify
     * functionality without a real Klipper connection.
     *
     * @param message Optional custom message (defaults to test message if empty)
     */
    void trigger_test_notify(const std::string& message = "");

  private:
    // Static instance for cross-TU access
    static std::atomic<ActionPromptManager*> s_instance;

    // The registered instance's showing prompt title, for readers on any thread;
    // null unless it is SHOWING. Readers never touch the instance itself, which
    // the main thread may be destroying. Accessed only through
    // std::atomic_load/std::atomic_store.
    static std::shared_ptr<const std::string> s_showing_title;

    // State machine. Change it through set_state(), which publishes s_showing_title.
    State m_state = State::IDLE;

    // Counts prompt_show transitions, so a deferred re-show can tell its prompt
    // is still the one on screen.
    uint64_t m_show_count = 0;

    // Expires the deferred lines of make_line_sink() and re-shows when this manager dies.
    AsyncLifetimeGuard m_lifetime;

    // Current prompt being built or shown
    std::unique_ptr<PromptData> m_current_prompt;

    // Group tracking
    int m_next_group_id = 0;
    bool m_in_group = false;

    // Callbacks
    ShowCallback m_on_show;
    CloseCallback m_on_close;
    NotifyCallback m_on_notify;

    // ========================================================================
    // Command Handlers
    // ========================================================================

    void set_state(State state);
    void publish_title() const;
    void end_showing();
    void handle_prompt_begin(const std::string& payload);
    void handle_prompt_text(const std::string& payload);
    void handle_prompt_button(const std::string& payload, bool is_footer);
    void handle_prompt_button_group_start();
    void handle_prompt_button_group_end();
    void handle_prompt_show();
    void handle_prompt_end();
    void handle_notify(const std::string& payload);
};

} // namespace helix
