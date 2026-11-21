// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

namespace helix::ui {

/**
 * @brief The theme editor's more menu: Reset and Save As New
 *
 * Save is the header's own action; the two rarer ones fold behind the header's
 * more button. The action id is kActionReset, kActionSaveAs or kActionCancelled.
 */
class ThemeEditorMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(ThemeEditorMenu)

  public:
    static constexpr int kActionReset = 0;
    static constexpr int kActionSaveAs = 1;

    /// Register the menu's row callbacks. Idempotent.
    static void register_callbacks();

  protected:
    const char* xml_component_name() const override {
        return "theme_editor_menu";
    }

  private:
    static void pick(int action);
};

} // namespace helix::ui
