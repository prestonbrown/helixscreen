// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

namespace helix {

/**
 * @brief A view a home full page can show in place (prestonbrown/helixscreen#1931)
 *
 * FullPageHost asks for the content through this interface, so one host serves
 * both kinds of view a tile can open: an overlay, whose tree is built inside the
 * page and freed when it goes (OverlayBase), and a main panel, whose one tree is
 * lent to the page and handed back to the navigation bar's panel container
 * (PanelBase, through NavigationManager::lend_panel()).
 */
class FullPageContent {
  public:
    virtual ~FullPageContent() = default;

    /// Show this view inside @p container and activate it. False, changing
    /// nothing, when it cannot be shown there now (open elsewhere).
    virtual bool embed(lv_obj_t* container) = 0;

    /// Deactivate the view and take its tree out of the page; no-op when none is
    /// in one.
    virtual void release_embed() = 0;

    /// The root embed() placed in a container, or nullptr when nothing is embedded.
    virtual lv_obj_t* embedded_root() const = 0;

    /**
     * @brief The page holding this view's tree is being deleted without a release
     *
     * Runs inside LVGL's delete event of the page's tile, before LVGL deletes the
     * tile's children, so the embedded tree is still whole. A view that owns its
     * tree elsewhere takes it back here and now; one whose tree dies with the page
     * may only schedule its cleanup.
     */
    virtual void host_deleting() = 0;
};

} // namespace helix
