// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_fault_path.h"
#include "ui_modal.h"
#include "ui_widget_ref.h"

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
 * Buttons can be styled with different colors; each button group gets its own row.
 *
 * The action_prompt_modal and action_prompt_button_row components are
 * registered in src/xml_registration.cpp.
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

/// How one row of regular (non-footer) prompt buttons is laid out.
enum class ButtonRowKind {
    Wrap,     ///< content-sized buttons, row_wrap takes the lines they need
    EqualRow, ///< one non-wrapping line of equal-width cells
    Grid,     ///< equal-width cells, `cols` to a line
};

struct ButtonRowLayout {
    ButtonRowKind kind = ButtonRowKind::Wrap;
    int32_t cols = 0; ///< cells per line; 0 for Wrap
};

/// Measured inputs for choose_button_row_layout(), in pixels.
struct ButtonRowMeasure {
    int32_t count = 0;
    bool all_swatches = false; ///< every button is a label-less color tile
    int32_t widest_label = 0;
    int32_t available = 0; ///< the row's content width
    int32_t gap = 0;       ///< column gap between cells
    int32_t cell_pad = 0;  ///< horizontal padding on each side of a cell's label
};

/// Rows of at least this many labelled buttons become a grid.
inline constexpr int32_t kPromptGridMinButtons = 5;

/**
 * @brief Pick the layout for one row of prompt buttons.
 *
 * Four buttons whose labels fit an even share sit on one equal-width line
 * (AFC's "Lane 1".."Lane 4"). Five or more labelled buttons become a grid of
 * three columns, or two when the widest label does not fit a third: a label
 * cut to an ellipsis can lose the suffix that tells two units apart
 * ("HTLF_claymore_1" / "_2"), so the grid gives up a column before it
 * truncates. Swatch rows never become a grid; they keep one line of tiles.
 * Everything else wraps, which never clips. An unmeasured row (available <= 0)
 * wraps.
 */
ButtonRowLayout choose_button_row_layout(const ButtonRowMeasure& m);

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

    // Non-copyable, non-movable (Modal's move operations are deleted; nothing
    // ever moved an ActionPromptModal by value - its owner holds it by
    // unique_ptr)
    ActionPromptModal(const ActionPromptModal&) = delete;
    ActionPromptModal& operator=(const ActionPromptModal&) = delete;
    ActionPromptModal(ActionPromptModal&&) = delete;
    ActionPromptModal& operator=(ActionPromptModal&&) = delete;

    /**
     * @brief Show modal with prompt data
     * @param parent Parent screen for the modal
     * @param data Prompt data from ActionPromptManager
     * @return true if modal was created successfully
     */
    bool show_prompt(lv_obj_t* parent, const PromptData& data);

    /**
     * @brief Show a one-shot prompt whose instance the modal stack owns and frees on close
     * @return true if shown; on failure the instance is freed
     */
    static bool show_owned_prompt(lv_obj_t* parent, const PromptData& data);

    /**
     * @brief Set callback for when a button is clicked
     *
     * The callback receives the gcode string associated with the button.
     * After calling the callback, the modal closes automatically.
     *
     * @param callback Function to send gcode to printer
     */
    void set_gcode_callback(GcodeCallback callback);

    /**
     * @brief Callback for a close the owner did not ask for
     *
     * Fires from on_hide() for a button tap, backdrop tap, ESC or any other
     * close that is not the owner's own hide(), saying which kind it was.
     * It runs synchronously inside Modal::hide(), so it must not show a modal.
     */
    using DismissCallback = std::function<void(PromptCloseKind kind)>;
    void set_dismiss_callback(DismissCallback callback);

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
    /// Points the fault-path diagram at this prompt's own text, so a prompt that names no
    /// fault never shows one an earlier modal published.
    void publish_fault_path();
    FaultPathHold fault_path_;

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
    DismissCallback dismiss_callback_;
    bool button_sent_gcode_ = false;

    // === Dynamic button tracking ===
    std::vector<helix::ui::WeakWidget> created_buttons_;
    std::vector<lv_obj_t*> created_text_labels_;
    std::vector<std::unique_ptr<ButtonCallbackData>> button_callback_data_;

    // === Internal Methods ===
    void populate_content();
    void create_text_lines();
    void create_buttons();

    /// A row laid out as a Grid of `cols` equal-width cells.
    struct GridRow {
        lv_obj_t* row = nullptr;
        int32_t cols = 0;
    };

    /// The rows fit_rows() sizes once every row exists.
    struct RowFit {
        std::vector<GridRow> grid_rows;
        std::vector<lv_obj_t*> swatch_rows; ///< rows holding only swatch tiles
        int32_t swatch_cols = 0;            ///< most swatches in any of them
        std::vector<lv_obj_t*> wrap_rows;   ///< content-sized rows, capped at their width
    };

    /**
     * @brief Size the card, grid cells and swatch tiles, and cap the scroll area
     *
     * The card widens for swatch tiles, grid cells split their row, a wrap-row label
     * wider than its row ends in an ellipsis, swatch tiles
     * take the height left under the card's cap, and prompt_scroll is capped at
     * that height so text and buttons scroll while the footer stays on the card.
     */
    void fit_rows(const RowFit& rows);

    /// Fit each swatch tile's circle to the tile's shorter side.
    void size_swatch_discs();

    /**
     * @brief Create a single button inside @p container, laid out for @p kind
     *
     * Wrap buttons are content-sized; EqualRow and Grid buttons are equal-width
     * cells (a Grid cell's width is set later by fit_rows()). Footer buttons
     * ignore @p kind.
     */
    void create_button(const PromptButton& btn, lv_obj_t* container,
                       ButtonRowKind kind = ButtonRowKind::Wrap);
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
 * Main thread only: it shows a toast.
 *
 * @param error_message Klipper's message from the failed RPC (MoonrakerError::
 *                      localized_message()). Empty falls back to a generic string
 *                      so the toast is never blank.
 */
void report_action_prompt_gcode_failure(const std::string& error_message);

} // namespace helix::ui
