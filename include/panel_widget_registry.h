// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

class PanelWidget;

using WidgetFactory = std::function<std::unique_ptr<PanelWidget>(const std::string& instance_id)>;
using SubjectInitFn = std::function<void()>;

/// Grouping used to organize the Add Widget catalog.
///
/// Enumerator order is the display order in the catalog, ordered by how often
/// people reach for a group rather than alphabetically: the print you are
/// watching first, the machine housekeeping you rarely touch last.
///
/// These are deliberately coarser than the section list in
/// docs/user/guide/home-panel.md § "Available Widgets". That list is a
/// reference index you read top to bottom; this is a menu you navigate on a
/// 480px panel, where a category holding two widgets costs two taps to reach
/// two things and earns nothing.
enum class WidgetCategory {
    PrintStatus,
    Temperature,
    Filament,
    Controls,
    System,
    /// Runtime-added definitions (plugins). Last: reached for least.
    Plugins,
};

struct WidgetCategoryDef {
    WidgetCategory id;
    const char* display_name;
    const char* translation_tag; // For i18n
    const char* icon;            // Icon name from ui_icon_codepoints.h
};

struct PanelWidgetDef {
    const char* id;                    // Stable string for JSON config
    const char* display_name;          // For settings overlay UI
    const char* icon;                  // Icon name
    const char* description;           // Short description for settings overlay
    const char* hardware_gate_subject; // nullptr = always available
    const char* hardware_gate_hint; // Human-readable reason, e.g., "Requires AMS or MMU hardware"

    /// Catalog grouping. Deliberately has no default: it sits before the
    /// defaulted fields so every table entry must name one positionally, and a
    /// new widget cannot silently land in whichever category happens to be
    /// first.
    WidgetCategory category;

    bool default_enabled = true; // Whether enabled in fresh/default config
    int colspan = 1;             // Default grid columns spanned
    int rowspan = 1;             // Default grid rows spanned
    int min_colspan = 0;         // Minimum columns (0 = use colspan)
    int min_rowspan = 0;         // Minimum rows (0 = use rowspan)
    int max_colspan = 0;         // Maximum columns (0 = use colspan, i.e. not scalable)
    int max_rowspan = 0;         // Maximum rows (0 = use rowspan, i.e. not scalable)
    bool multi_instance = false; // Allows dynamic instance creation with base_id:N IDs
    /// Whether this widget resolves to half a cell on the column / row axis.
    ///
    /// An axis gets half-cell resolution when half a cell of extra room shows
    /// MORE: continuous content (a chart, an aspect-fit frame, wrapping text, a
    /// scrolling strip, stacked readout rows) and a centred glyph, which scales
    /// to whatever box it is given (prestonbrown/helixscreen#1559). Leave it off
    /// for a widget authored around a fixed number of cells, where the finer
    /// drag snap costs real precision at a 34px track and buys nothing.
    ///
    /// A minimum below a whole cell on an axis additionally needs this flag on
    /// that axis AND a widget that can refuse a box it cannot draw
    /// (PanelWidget::fits_at), because a one-track span clips any layout
    /// authored around whole cells. The rule is the same for columns and rows.
    bool supports_half_col = false;
    bool supports_half_row = false;

    /// True when this widget wants the home panel's shared card background
    /// drawn behind it, fused with its neighbours'.
    ///
    /// Most home widgets paint nothing — `extends="lv_obj"` inherits
    /// StyleRole::ObjBase, which is fully transparent — and would render on the
    /// bare panel background without this. The exceptions are the few that
    /// bring their own surface: print_status and camera extend ui_card,
    /// nozzle_temps hand-rolls one, and ams nests a card in its spool view.
    /// Fusing behind those would stack two cards and show a box inside a box.
    ///
    /// printer_image and tips opt out for looks rather than duplication: the
    /// printer render is meant to float on the panel background and tips is a
    /// left accent rule, not a tile.
    ///
    /// This used to be inferred from the span — a widget exactly one cell
    /// square merged, anything else was assumed to paint its own background.
    /// That conflated two unrelated questions and was wrong in both directions:
    /// a 2x1 fan_stack got no background at all, and a one-cell ams got two.
    bool merges_into_card = true;

    WidgetFactory factory = nullptr;       // nullptr = pure XML or externally managed
    SubjectInitFn init_subjects = nullptr; // Called before XML creation; must be idempotent

    // Resolved accessors (0 = "use default colspan/rowspan")
    int effective_min_colspan() const {
        return min_colspan > 0 ? min_colspan : colspan;
    }
    int effective_min_rowspan() const {
        return min_rowspan > 0 ? min_rowspan : rowspan;
    }
    int effective_max_colspan() const {
        return max_colspan > 0 ? max_colspan : colspan;
    }
    int effective_max_rowspan() const {
        return max_rowspan > 0 ? max_rowspan : rowspan;
    }
    bool is_scalable() const {
        return effective_max_colspan() > effective_min_colspan() ||
               effective_max_rowspan() > effective_min_rowspan();
    }
};

/// Categories in catalog display order.
const std::vector<WidgetCategoryDef>& get_widget_categories();
/// nullptr when `id` is not one of the enumerators.
const WidgetCategoryDef* find_widget_category(WidgetCategory id);

const std::vector<PanelWidgetDef>& get_all_widget_defs();
const PanelWidgetDef* find_widget_def(std::string_view id);
size_t widget_def_count();
void register_widget_factory(std::string_view id, WidgetFactory factory);
void register_widget_subjects(std::string_view id, SubjectInitFn init_fn);

/// A widget definition added while the app runs. Spans are in grid tracks.
struct RuntimeWidgetDef {
    std::string id;
    std::string display_name;
    std::string icon;
    std::string description;
    int colspan = 2, rowspan = 2, max_colspan = 0, max_rowspan = 0;
    WidgetFactory factory;
};

/// Adds, or replaces, a Plugins-category definition whose strings the registry
/// owns. False when `def.id` is a built-in widget. A definition pointer from
/// find_widget_def() or get_all_widget_defs() is valid until the next register
/// or unregister call.
bool register_runtime_widget_def(RuntimeWidgetDef def);
/// Deactivates a runtime definition. The id's storage is kept so pointers
/// handed to LVGL user_data stay readable until the rows using them rebuild.
void unregister_runtime_widget_def(std::string_view id);
/// Bumped by every register/unregister of a runtime definition. A reload that
/// re-registers the same ids leaves every id list unchanged while the
/// factories now build widgets bound to a different owner, so caches keyed on
/// the id list alone (home's page cache) must also compare this.
uint64_t runtime_widget_generation();
// Internal — called once from PanelWidgetManager::init_widget_subjects().
// Do not call directly; widget factories require runtime context (singletons, shared resources).
void init_widget_registrations();

} // namespace helix
