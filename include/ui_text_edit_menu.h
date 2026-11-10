// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"
#include "ui_widget_ref.h"

#include "text_clipboard.h"

#include <lvgl.h>

namespace helix::ui {

/// One edit a text field's menu can make.
enum class TextEditAction { Cut, Copy, Paste };

/// The edits @p textarea offers right now (helix::clipboard::actions_for_field
/// over the field's live password mode and text, and the clipboard).
[[nodiscard]] clipboard::EditActions text_edit_actions(lv_obj_t* textarea);

/**
 * @brief Make @p action on @p textarea
 *
 * Copy puts the field's whole text on the clipboard; Cut does that and empties
 * the field; Paste inserts the clipboard at the cursor, sanitized for the field
 * (one-line or not), through the textarea's own insert path so max_length and
 * accepted characters still apply. Each edit fires VALUE_CHANGED, so a bound
 * subject follows.
 *
 * @return false, changing nothing, when the field does not offer @p action
 */
bool apply_text_edit_action(lv_obj_t* textarea, TextEditAction action);

/**
 * @brief Cut / Copy / Paste menu for a held text field
 *
 * KeyboardManager raises it from a long press on any textarea it manages, so
 * every <text_input> has it. One menu exists at a time, held by this module.
 */
class TextEditMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(TextEditMenu)

  public:
    /**
     * @brief Raise the menu around @p textarea on its screen
     * @return false, showing nothing, when the field offers no edit at all
     */
    bool show_for(lv_obj_t* textarea);

    /// Register the row callbacks and the subjects the XML binds. Idempotent.
    static void register_callbacks();

  protected:
    const char* xml_component_name() const override {
        return "text_edit_menu";
    }

  private:
    static void pick(TextEditAction action);

    WidgetRef target_;
};

/**
 * @brief LV_EVENT_LONG_PRESSED handler for a managed textarea
 *
 * Raises the shared TextEditMenu and ends the press when the field offers an
 * edit; otherwise leaves the press alone.
 */
void text_edit_long_press_cb(lv_event_t* e);

/// Close and drop the shared menu, for a UI teardown.
void text_edit_menu_reset();

} // namespace helix::ui
