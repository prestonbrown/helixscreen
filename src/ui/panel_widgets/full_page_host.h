// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

class TileSizing;

/**
 * @brief The full-page form of a home widget (prestonbrown/helixscreen#1931)
 *
 * A widget that can show its overlay's controls in place owns one of these. When
 * the manager places the tile over a whole page other than the main page
 * (PanelWidget::fills_extra_page()), the host publishes `<instance>_full_page` = 1,
 * which the tile's XML uses to swap its compact face for `full_page_frame`, and
 * while that page is the one on screen it builds the overlay's own tree inside the
 * frame's `full_page_body` (OverlayBase::embed()). The tree is freed whenever the
 * page goes off screen or the home panel is covered, so a full page costs memory
 * only while it is looked at.
 *
 * Tiles that open the same overlay ask jump_to_full_page() first, so they move the
 * carousel to the page instead of opening the overlay over it.
 */
class FullPageHost {
  public:
    /// Builds the overlay's tree inside a container; false when it could not.
    using EmbedFn = std::function<bool(OverlayBase&, lv_obj_t*)>;

    /// @p function names what the page hosts ("motion"); @p overlay resolves the
    /// overlay to embed, on first use; @p embed builds it (an overlay with a mode
    /// sets it around OverlayBase::embed()), plain embed() when empty. Publishes
    /// the instance subject through @p sizing, so construct the host in the
    /// widget's constructor.
    FullPageHost(TileSizing& sizing, std::string function, std::function<OverlayBase&()> overlay,
                 EmbedFn embed = {});
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

    /// The tile's page is on screen (PanelWidget::on_activate()).
    void activate();
    /// The tile's page went off screen or the home panel was covered.
    void deactivate();

    bool is_full_page() const {
        return full_page_;
    }
    /// Whether this host's frame holds the overlay's tree right now.
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
    lv_obj_t* body() const;

    std::string function_;
    std::function<OverlayBase&()> overlay_;
    EmbedFn embed_;
    SubjectManager subjects_;
    lv_subject_t full_page_subject_{};
    /// Null once the tile is detached or its tree deleted.
    helix::ui::WidgetRef tile_root_;
    bool full_page_ = false;
    bool active_ = false;
    int page_index_ = 0;
};

/// Page index of an attached full page hosting @p function, or nullopt.
std::optional<int> full_page_for(std::string_view function);

/// Move the home carousel to @p function's full page. False, navigating nowhere,
/// when there is none (the caller opens the overlay instead) or the home panel
/// refuses (edit mode).
bool jump_to_full_page(std::string_view function);

} // namespace helix
