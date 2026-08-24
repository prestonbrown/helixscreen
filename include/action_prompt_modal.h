// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "action_prompt_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file action_prompt_modal.h
 * @brief Modal dialog for displaying Klipper action:prompt messages
 *
 * Displays interactive prompts from Klipper macros with dynamic buttons.
 * Buttons can be styled with different colors and grouped for layout.
 *
 * ## Integration Note
 * The component must be registered in main.cpp before use:
 * @code
 * lv_xml_register_component_from_file("action_prompt_modal",
 *     "ui_xml/action_prompt_modal.xml");
 * @endcode
 *
 * ## Usage:
 * @code
 * helix::ui::ActionPromptModal modal;
 * modal.set_gcode_callback([&api](const std::string& gcode) {
 *     api.send_gcode(gcode);
 * });
 * modal.show_prompt(parent, prompt_data);
 * @endcode
 */

// Forward declaration so the modal can grant white-box layout-test access.
struct ActionPromptModalTestAccess;

namespace helix::ui {

/**
 * @brief Modal dialog for Klipper action:prompt messages
 *
 * Dynamically creates buttons based on PromptData from the ActionPromptManager.
 * Supports button colors, grouping, and footer buttons.
 */
class ActionPromptModal : public Modal {
  public:
    /**
     * @brief Callback type for button clicks that send gcode
     */
    using GcodeCallback = std::function<void(const std::string& gcode)>;

    ActionPromptModal();
    ~ActionPromptModal() override;

    // Non-copyable
    ActionPromptModal(const ActionPromptModal&) = delete;
    ActionPromptModal& operator=(const ActionPromptModal&) = delete;

    // Movable
    ActionPromptModal(ActionPromptModal&& other) noexcept;
    ActionPromptModal& operator=(ActionPromptModal&& other) noexcept;

    /**
     * @brief Show modal with prompt data
     * @param parent Parent screen for the modal
     * @param data Prompt data from ActionPromptManager
     * @return true if modal was created successfully
     */
    bool show_prompt(lv_obj_t* parent, const PromptData& data);

    /**
     * @brief Set callback for when a button is clicked
     *
     * The callback receives the gcode string associated with the button.
     * After calling the callback, the modal closes automatically.
     *
     * @param callback Function to send gcode to printer
     */
    void set_gcode_callback(GcodeCallback callback);

    // Modal interface
    [[nodiscard]] const char* get_name() const override {
        return "Action Prompt Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "action_prompt_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    // Grants white-box access to button widgets / containers for layout tests.
    friend struct ::ActionPromptModalTestAccess;

    /**
     * @brief Data passed as user_data to button event callbacks
     *
     * Owns a copy of the gcode string (not a pointer into prompt_data_.buttons)
     * and holds a LifetimeToken to detect if the modal has been destroyed
     * before the queued click event fires (crash #437).
     */
    struct ButtonCallbackData {
        ActionPromptModal* modal;
        std::optional<helix::LifetimeToken> token;
        std::string gcode; // Owned copy, safe from vector reallocation
    };

    // === State ===
    PromptData prompt_data_;
    GcodeCallback gcode_callback_;

    // === Dynamic button tracking ===
    std::vector<lv_obj_t*> created_buttons_;
    std::vector<lv_obj_t*> created_text_labels_;
    std::vector<std::unique_ptr<ButtonCallbackData>> button_callback_data_;

    // === Internal Methods ===
    void populate_content();
    void create_text_lines();
    void create_buttons();
    /**
     * @brief Create a single button inside @p container.
     *
     * @param equal_width When true (>= 4 regular buttons whose labels all fit an
     *        even share of the row) the button is laid out as an equal-width flex
     *        cell (grow=1, width 0) so several short labels share one
     *        non-wrapping row. When false the legacy content-sized layout is
     *        used, which row_wrap then spreads over as many lines as the labels
     *        need.
     */
    void create_button(const PromptButton& btn, lv_obj_t* container, bool equal_width = false);
    lv_color_t get_button_color(const std::string& color_name);
    void clear_dynamic_content();

    // === Event Handler ===
    void handle_button_click(const std::string& gcode);

    // === Static Callbacks ===
    static void on_button_cb(lv_event_t* e);
};

/**
 * @brief Show the user why a prompt button's gcode did not run.
 *
 * The modal closes on every button press by design and waits for the firmware
 * to push a replacement prompt, so a macro that aborts leaves the screen empty.
 * This toast fills that gap. It is also what makes the send's
 * `caller_surfaces_errors=true` claim honest: a caller that declares it reports
 * the failure records the message in rpc_error_correlation, which suppresses
 * the independent `!!` GcodeError toast for the same rejection — so dropping
 * this toast would leave the failure with no surface at all. See
 * include/rpc_error_policy.h.
 *
 * Safe to call from the WebSocket background thread - the notification layer
 * marshals to the main thread itself.
 *
 * @param error_message Klipper's message from the failed RPC (MoonrakerError::
 *                      user_message()). Empty falls back to a generic string so
 *                      the toast is never blank.
 */
void report_action_prompt_gcode_failure(const std::string& error_message);

} // namespace helix::ui
