// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"

#include "src/ui/panel_widgets/tool_switcher_layout.h"

#include <functional>

namespace helix::ui {

/**
 * @brief Every head as a grid of tool_switcher_segment cells
 *        (ui_xml/tool_switcher_picker.xml), sized by fit_tool_picker_grid().
 *
 * Two modes. Mount, raised by the Home tool switcher: the mounted head is
 * highlighted, Park and the feeder sit in a footer, the cells disable while a
 * print refuses a change, and a pick asks for the change. Target, raised by a
 * surface's tool chip: the target tool's head is highlighted, the footer is
 * hidden, nothing disables, and a pick only aims the target tool. A tap
 * outside chooses nothing.
 */
class ToolPicker : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(ToolPicker)

  public:
    enum class Mode { Mount, Target };

    /// Register the picker's subjects; a no-op once they exist.
    static void init_subjects();

    /// Raise the picker under @p anchor. @p on_pick gets the chosen head
    /// (an index into ToolState::physical_heads()).
    bool open(lv_obj_t* screen, lv_obj_t* anchor, AnchorAlign align, Mode mode,
              std::function<void(int)> on_pick);

    /// A cell's tap: close, then hand the head to the pick callback.
    void choose(int head);

    [[nodiscard]] Mode mode() const {
        return mode_;
    }

  protected:
    const char* xml_component_name() const override {
        return "tool_switcher_picker";
    }
    void on_created(lv_obj_t* backdrop) override;

  private:
    Mode mode_ = Mode::Mount;
    std::function<void(int)> on_pick_;
};

/// The app's one tool picker.
ToolPicker& tool_picker();

/// The action cells the connected backend offers: Park where it can dock the
/// mounted tool, the feeder where it has one the user can open.
[[nodiscard]] ToolSwitcherActions tool_switcher_actions_offered();

/// The feeder label form whose text fits a feeder cell @p cell_w px wide.
[[nodiscard]] FeederLabelForm feeder_label_form_for(int cell_w);

/// Raise the picker in Target mode under @p anchor: a pick aims the shared
/// target tool and never changes tools. Works with no Home widget placed.
bool open_target_tool_picker(lv_obj_t* anchor);

} // namespace helix::ui
