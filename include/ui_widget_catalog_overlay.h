// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "panel_widget_registry.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

class PanelWidgetConfig;

/// Callback invoked when the user selects a widget from the catalog.
/// Receives the widget definition ID (e.g. "temperature", "network").
using WidgetSelectedCallback = std::function<void(const std::string& widget_id)>;

/// Callback invoked when the catalog overlay is closed (selection or back navigation).
using CatalogClosedCallback = std::function<void()>;

/// Callback the catalog asks whether a def still fits the page it was opened
/// from. Null (or absent) means everything fits.
using WidgetFitCallback = std::function<bool(const PanelWidgetDef&)>;

/// Shows a half-width overlay listing the widget categories available for grid
/// placement. Picking a category dives into a sub-page listing that category's
/// widgets, using the same header/back contract as the settings sub-pages.
/// A search box filters the whole registry (name, description, or category) into
/// a flat results list; clearing it returns to the category list. Widgets whose
/// hardware is missing on this printer are grouped under an "Unavailable on this
/// printer" row instead of cluttering their categories, and every row follows a
/// hardware gate that changes while the catalog is open.
/// Widgets already placed are shown dimmed with a "Placed" badge.
/// On selection, the callback fires and both levels close.
class WidgetCatalogOverlay {
  public:
    /// Open the catalog overlay.
    /// @param parent_screen  Screen to parent the overlay on
    /// @param config         Current widget config (to determine which are already placed)
    /// @param on_select      Called with the chosen widget ID when user taps a row
    /// @param on_close       Called when the overlay is closed for any reason
    /// @param fits           Asked per def whether it fits the page the catalog
    ///                       was opened from; a row it refuses is shown dimmed
    ///                       and unclickable, its name carrying the reason
    static void show(lv_obj_t* parent_screen, const PanelWidgetConfig& config,
                     WidgetSelectedCallback on_select, CatalogClosedCallback on_close = nullptr,
                     WidgetFitCallback fits = nullptr);

    /// Widget defs belonging to @p category, in registry order.
    ///
    /// Pure — reads the registry and nothing else — so the grouping can be
    /// asserted without standing up an overlay.
    static std::vector<const PanelWidgetDef*> widgets_in_category(WidgetCategory category);

    /// Close the open catalog, its category sub-page included, as a selection
    /// does: on_close fires once. Nothing when the catalog is closed.
    static void close();

    /// Root of the open catalog overlay, or nullptr when it is closed.
    static lv_obj_t* active_root();

    /// Root of the open category sub-page, or nullptr when no dive is active.
    static lv_obj_t* active_category_root();

  private:
    /// Push the sub-page listing one category's available widgets.
    static void show_category(WidgetCategory category);

    /// Push the sub-page listing the widgets unavailable on this printer.
    static void show_unavailable();

    /// Shared dive machinery: create the sub-page XML, populate it, and push it on
    /// top of the catalog. The page lists @p category's available widgets, or the
    /// unavailable ones when @p category is empty.
    static void show_widget_page(const char* title, const char* title_tag,
                                 std::optional<WidgetCategory> category);

    /// Rebuild every row that reads a hardware gate (the category list, the
    /// search results, an open sub-page) once the gated set differs from the one
    /// those rows were built with. PanelWidgetManager's gate observers drive it
    /// while the catalog is open.
    static void refresh_gated_rows();

    /// Flip the catalog between the category list and the flat search results,
    /// and show or hide result rows by match. Rows are built on the first query
    /// of an open; later keystrokes only toggle flags, the same treatment the
    /// wizard's ~105-row list gets.
    static void apply_search(const std::string& query);

    /// Build the search result rows into @p results, one per registry def in
    /// registry order, with the entries they are filtered by.
    static void build_search_rows(lv_obj_t* results);

    /// The search box's value_changed callback.
    static void on_search_changed(lv_event_t* e);

    /// Click dispatch for the top-level category rows.
    static void on_category_row_clicked(lv_event_t* e);

    /// Click dispatch for the "Unavailable on this printer" row.
    static void on_unavailable_row_clicked(lv_event_t* e);

    /// Click dispatch for a widget row: selects its def, or mints an instance
    /// of a multi-instance def. The row's name is the def id.
    static void on_widget_row_clicked(lv_event_t* e);

    /// Populate the category list with one row per category that has available
    /// widgets, plus the unavailable row when hardware is missing here.
    static void populate_category_rows(lv_obj_t* group);

    /// Populate a scroll container with one row per def in @p defs. Used by the
    /// category pages, the unavailable page, and the search results (which pass
    /// the whole registry).
    static void populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                              const std::vector<const PanelWidgetDef*>& defs);

    /// One row for @p def, named for its id: gate hint, placed badge and size
    /// badge. Returns the row.
    static lv_obj_t* create_widget_row(lv_obj_t* parent, const PanelWidgetDef& def,
                                       const PanelWidgetConfig& config,
                                       const std::unordered_map<std::string, int>& multi_placed);

    /// Create a single catalog row widget. @p unavailable dims the row and
    /// strips its click for a reason outside the row itself: the hardware is
    /// missing here, or the page has no room for the widget's minimum span.
    static lv_obj_t* create_row(lv_obj_t* parent, const char* name, const char* icon,
                                const char* description, int colspan, int rowspan,
                                bool already_placed, bool unavailable);
};

} // namespace helix
