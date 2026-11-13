// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "full_page_content.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

class TileSizing;

/// The values of a tile's `<instance>_full_page` subject.
enum class FullPageState : int {
    Compact = 0, ///< A tile: the compact face
    Empty = 1, ///< A full page showing its placeholder (off screen, or the view is open elsewhere)
    Hosting = 2, ///< A full page holding the view's tree
};

/**
 * @brief The full-page form of a home widget (prestonbrown/helixscreen#1931)
 *
 * A widget that can show the view it opens in place owns one of these. When
 * the manager places the tile over a whole page other than the main page
 * (PanelWidget::fills_extra_page()), the host publishes `<instance>_full_page`
 * (FullPageState), which the tile's XML uses to swap its compact face for
 * `full_page_frame` and the frame uses to drop its placeholder, and
 * while that page is the one on screen it builds the overlay's own tree inside the
 * frame's `full_page_body` (FullPageContent::embed()): an overlay builds its tree
 * there, a main panel lends its one tree. The tree leaves whenever the page goes
 * off screen or the home panel is covered, so an overlay's full page costs memory
 * only while it is looked at.
 *
 * Tiles that open the same view ask jump_to_full_page() first, so they move the
 * carousel to the page instead of opening the view over it.
 */
class FullPageHost {
  public:
    /// Shows the content inside a container; false when it could not.
    using EmbedFn = std::function<bool(FullPageContent&, lv_obj_t*)>;

    /// @p function names what the page hosts ("motion"); @p content resolves the
    /// view to embed, on first use; @p embed shows it (an overlay with a mode
    /// sets it around embed()), plain embed() when empty. Publishes
    /// the instance subject through @p sizing, so construct the host in the
    /// widget's constructor.
    FullPageHost(TileSizing& sizing, std::string function,
                 std::function<FullPageContent&()> content, EmbedFn embed = {});
    ~FullPageHost();

    FullPageHost(const FullPageHost&) = delete;
    FullPageHost& operator=(const FullPageHost&) = delete;

    /// The tile's placement, from the widget's on_size_changed(). Builds or frees
    /// the tree when the change happens while the page is on screen.
    void set_placement(bool fills_extra_page, int page_index);

    /// The tile was bound to @p tile_root (attach()).
    void attach(lv_obj_t* tile_root);
    /// The tile is going away (detach()): free the tree and stop being a jump target.
    void detach();

    /// The tile's tree was deleted without detach() (PanelWidget's delete hook).
    /// Runs inside LVGL's delete event, before the tile's children go: the content
    /// takes what it must out of the dying tree (FullPageContent::host_deleting()).
    void on_tile_deleted();

    /// The tile's page is on screen (PanelWidget::on_activate()).
    void activate();
    /// The tile's page went off screen or the home panel was covered.
    void deactivate();

    bool is_full_page() const {
        return full_page_;
    }
    /// Whether this host's frame holds the content's tree right now.
    bool hosts_tree() const;

    const std::string& function() const {
        return function_;
    }
    int page_index() const {
        return page_index_;
    }
    /// Attached and placed as a full page: a jump target.
    bool is_jump_target() const {
        return full_page_ && tile_root_ != nullptr;
    }

  private:
    void embed_if_shown();
    void release_tree();
    void publish_state();
    lv_obj_t* body() const;

    std::string function_;
    std::function<FullPageContent&()> content_;
    EmbedFn embed_;
    SubjectManager subjects_;
    lv_subject_t full_page_subject_{};
    /// Null once the tile is detached or its tree deleted.
    helix::ui::WidgetRef tile_root_;
    bool full_page_ = false;
    bool active_ = false;
    /// This host's embed() succeeded and nothing has released it since.
    bool hosting_ = false;
    int page_index_ = 0;
};

/// Page index of an attached full page hosting @p function, or nullopt.
std::optional<int> full_page_for(std::string_view function);

/// Move the home carousel to @p function's full page. False, navigating nowhere,
/// when there is none (the caller opens the overlay instead) or the home panel
/// refuses (edit mode).
bool jump_to_full_page(std::string_view function);

} // namespace helix
