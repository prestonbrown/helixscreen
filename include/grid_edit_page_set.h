// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file grid_edit_page_set.h
 * @brief Where the home carousel lands after edit mode changes its page set: a
 * drop that adds a page, a prune or the delete-page button that removes one,
 * an undone removal that puts its pruned page back.
 *
 * Free of live objects: the change is described as a PageSetChange, and
 * HomePanel builds the carousel on the page page_set_landing() returns and shows
 * its focus (prestonbrown/helixscreen#1638).
 */

namespace helix {

/// A change to the home page set, numbered as the carousel was before it: config
/// pages 0 to page_count - 1, then the next-page slot's tile at page_count, and
/// tile -1 past the first page's left border, where a drag that prepends sits.
struct PageSetChange {
    int page_count = 0;          ///< Config pages before the change
    bool page_added = false;     ///< A page was added past the last one, on the slot's tile
    bool page_prepended = false; ///< A page was added before the first one
    int removed_page = -1;       ///< The config page removed; -1 when none was
    /// A page inserted at this index, numbered after the change: it and every
    /// later page move one later. The change ends on it. -1 when none was.
    int inserted_page = -1;
    /// The page the change asks to end on: where a drop landed, the added page
    /// (the slot's tile for one added past the last, tile -1 for one prepended),
    /// or the page removed from under the edit session
    int focus_page = 0;
};

/// Where a page-set rebuild lands, numbered after the change.
struct PageSetLanding {
    int shown = 0; ///< The page the rebuilt carousel comes up on
    /// The page it ends on, one slide from shown when they differ; the edit
    /// session's page
    int focus = 0;
};

/**
 * @brief The page @p change ends on, numbered after it.
 *
 * An inserted page, when the change made one. Otherwise the focus page,
 * shifted back one by a page removed before it. A focus the
 * change removed lands on its own index, clamped to the last page: the page
 * after it, or the new last page.
 */
int page_set_focus(const PageSetChange& change);

/**
 * @brief Where a page-set rebuild lands, for a carousel showing @p shown when it
 * runs.
 *
 * The rebuilt carousel comes up on the page that was on screen, renumbered, and
 * one slide carries it to page_set_focus() when that is another page. It comes up
 * on the focus directly only when the page on screen no longer exists: the
 * change removed it, or it is the next-page slot and no page was added there.
 *
 * @param change The change, numbered before it.
 * @param shown The carousel's page when the rebuild runs, the slot's tile being
 *        page_count.
 */
PageSetLanding page_set_landing(const PageSetChange& change, int shown);

} // namespace helix
