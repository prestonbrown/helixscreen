// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include "tap_lockout.h"

#include <functional>
#include <string>

namespace helix::ui {

/// What the home Edit Mode tile callout can do to its tile.
enum class TileCalloutAction : int { Configure = 0, Remove = 1 };

/// What the callout offers for one tile.
struct TileCalloutRows {
    bool configure = false;
    bool remove = true;
};

/// The rows a tile gets: Configure only for a widget with an edit-mode
/// configure action, Remove always.
[[nodiscard]] TileCalloutRows tile_callout_rows(bool has_edit_configure);

/// Home Edit Mode's menu for the selected tile: its name and one row per
/// action, placed around the tile (ContextMenu::AnchorMode::AroundAnchor) over
/// the shared backdrop, with a ring drawn at the tile on the backdrop. A
/// backdrop tap closes it and sends nothing. Rows ignore a press within
/// TapLockout::kDefaultMs of the card's first frame.
class TileEditCallout : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(TileEditCallout)

  public:
    using ActionHandler = std::function<void(TileCalloutAction)>;

    /// Raise the callout for @p tile, titled @p title, around it on @p screen.
    /// @p on_action gets the chosen action after the callout has closed.
    bool show_for(lv_obj_t* screen, lv_obj_t* tile, const std::string& title, TileCalloutRows rows,
                  ActionHandler on_action);

    /// Register the row callbacks and the subjects the XML binds. Idempotent.
    static void register_callbacks();

  protected:
    const char* xml_component_name() const override {
        return "tile_edit_callout";
    }
    void on_created(lv_obj_t* backdrop) override;
    CardWidth card_width() const override;
    int32_t content_width() const override;

  private:
    static void pick(lv_event_t* e, TileCalloutAction action);
    static void row_pressed(lv_event_t* e);
    static void row_press_lost(lv_event_t* e);

    lv_area_t tile_area_{};
    TapLockout lockout_;
    int32_t widest_row_ = 0;
};

} // namespace helix::ui
