// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_widget_catalog_overlay.h"

#include "ui_effects.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_row_text.h"
#include "ui_selector_model.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "grid_layout.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "ui/ui_widget_helpers.h"

#include <lvgl/lvgl.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

// ============================================================================
// State shared between the overlay and row click callbacks.
// Only one catalog overlay can be open at a time.
// ============================================================================

namespace {

/// widget_catalog_view subject values: which list the catalog body shows.
constexpr int kCatalogViewBrowse = 0; // category rows
constexpr int kCatalogViewSearch = 1; // flat search results

/// PanelWidgetManager gate-observer registration, held while the catalog is open.
constexpr const char* kGateObserverKey = "widget_catalog";

/// One registry def's identity as a row set was built from it. Runtime
/// definitions can change the set under an open catalog (a plugin loading or
/// faulting), and the def count and gate flags alone cannot see a same-count
/// swap or a rename, so the id and display name ride along.
struct DefRowIdentity {
    std::string id;
    std::string display_name;
    std::string icon;
    std::string description;
    int colspan = 0;
    int rowspan = 0;
    bool gated = false;

    bool operator==(const DefRowIdentity& other) const {
        return gated == other.gated && colspan == other.colspan && rowspan == other.rowspan &&
               id == other.id && display_name == other.display_name && icon == other.icon &&
               description == other.description;
    }
};

// Bumped by every show(): work queued by one opening must not land on a later
// one, and a close-then-reopen can reuse the overlay's address.
uint32_t g_catalog_generation = 0;

struct CatalogState {
    lv_obj_t* overlay_root = nullptr;
    lv_obj_t* backdrop = nullptr;      // Semi-transparent dark backdrop behind the catalog
    lv_obj_t* category_root = nullptr; // Pushed category sub-page, nullptr at the top level
    lv_obj_t* parent_screen = nullptr;
    const PanelWidgetConfig* config = nullptr; // Owned by the caller (GridEditMode)
    WidgetSelectedCallback on_select;
    CatalogClosedCallback on_close;
    WidgetFitCallback fits; // Null: every def fits the page the catalog opened from

    // Search state. Entries are parallel to the search_results children (one row
    // per registry def, both in registry order) and to nothing else — the
    // category pages build their own rows per dive.
    lv_subject_t view = {};        // kCatalogView*; bound by the XML view flip
    lv_subject_t match_count = {}; // visible search rows; drives the empty message
    std::vector<ui::SelectorEntry> entries;
    // The result rows exist. They are built on the first query, not at open:
    // a row per registry def is most of what the catalog costs to show, and
    // most opens never search.
    bool search_rows_built = false;

    // What category_root lists: that category's available widgets, or the
    // unavailable ones when empty. A gate change rebuilds the page from it.
    std::optional<WidgetCategory> page_category;
    // Each registry def's identity, as the current rows were built. Gate
    // subjects also move between non-zero values (a second power device) without
    // moving a widget in or out of the catalog; only a change here alters any
    // row.
    std::vector<DefRowIdentity> row_defs;
};

CatalogState g_catalog_state;

/// Drop every piece of catalog state and fire on_close exactly once.
///
/// Shared by all three teardown paths (close_catalog(), the LV_EVENT_DELETE
/// handler, the NavigationManager close callback) so none of them can fire the
/// callback twice or leave half the state behind.
void release_catalog_state() {
    // First, so no queued gate rebuild can reach the rows of a closing catalog.
    PanelWidgetManager::clear_gate_observers(kGateObserverKey);
    // Defer backdrop deletion — every path into here can run from inside
    // LV_EVENT_CLICKED / LV_EVENT_DELETE processing, and a synchronous delete
    // there corrupts LVGL's event linked list.
    helix::ui::safe_delete_deferred(g_catalog_state.backdrop);
    auto on_close = std::move(g_catalog_state.on_close);
    g_catalog_state.overlay_root = nullptr;
    g_catalog_state.category_root = nullptr;
    g_catalog_state.parent_screen = nullptr;
    g_catalog_state.config = nullptr;
    g_catalog_state.on_select = nullptr;
    g_catalog_state.on_close = nullptr;
    g_catalog_state.fits = nullptr;
    g_catalog_state.search_rows_built = false;
    if (on_close) {
        on_close();
    }
}

/// Retire a popped catalog overlay: unregister it and hand the widget to the
/// deferred deleter.
///
/// Must be called immediately after the go_back() that popped it, in the same
/// synchronous scope. The delete is queued rather than issued directly because
/// go_back() runs its whole body through UpdateQueue; queueing here lands the
/// reclaim in the batch *after* the pop body has finished reading the widget it
/// is unwinding.
///
/// go_back() only hides an overlay — NavigationManager never deletes one. Without
/// this the tree stays parented to the screen for the life of the process, and a
/// catalog is cheap to reopen, so an edit session leaks one whole overlay per
/// open: the panel, its setting_group, and a setting_action_row per category,
/// each carrying ui_breakpoint observers.
void retire_overlay(lv_obj_t* overlay, const char* tag) {
    if (!overlay) {
        return;
    }
    helix::nav::unregister_overlay(overlay);
    helix::ui::queue_update(tag, [overlay]() { // QUEUE_TAG_OK: callers pass literals
        lv_obj_t* condemned = overlay;
        helix::ui::safe_delete_deferred(condemned);
    });
}

inline void retire_category_page(lv_obj_t* page) {
    retire_overlay(page, "catalog_category_reclaim");
}

/// Pop the category sub-page if one is open, leaving the catalog itself intact.
/// Returns true when a pop was actually queued.
bool pop_category_page() {
    lv_obj_t* page = g_catalog_state.category_root;
    if (!page) {
        return false;
    }
    g_catalog_state.category_root = nullptr;

    // We drive this teardown ourselves — stop the page's own close callback from
    // racing us into retire_category_page().
    helix::nav::clear_on_close(page);

    // #1221 guard. If something is stacked above the sub-page we must not pop it
    // — and must not reclaim it either, since the nav stack still points at it.
    // Leaking one widget beats deleting one the stack will unwind through.
    if (!helix::nav::is_on_top(page)) {
        spdlog::warn("[WidgetCatalog] Category page is not on top of the nav stack; "
                     "leaving it for the stack to unwind");
        return false;
    }
    helix::nav::go_back();
    retire_category_page(page);
    return true;
}

void close_catalog() {
    if (!g_catalog_state.overlay_root) {
        return;
    }
    lv_obj_t* root = g_catalog_state.overlay_root;

    // Two-level teardown. go_back() queues its entire body, so two calls made
    // back to back would both evaluate is_panel_on_top() against the *pre-pop*
    // stack and the second would refuse to run (#1221). Queue the catalog's pop
    // instead: it lands after the sub-page pop body has executed, by which point
    // the catalog really is on top.
    bool popped_page = pop_category_page();

    // Unregister the close callback so the pop below cannot double-fire on_close.
    helix::nav::clear_on_close(root);

    // Retire in the same scope as the pop that removed it, exactly as
    // pop_category_page() does. Queueing the reclaim separately would race
    // go_back()'s own queued body, and scrub_deleted_widget() erasing the widget
    // from panel_stack_ first would make that pop take the wrong overlay.
    if (popped_page) {
        helix::ui::queue_update("catalog_close_pop", [root]() {
            if (helix::nav::is_on_top(root)) {
                helix::nav::go_back();
                retire_overlay(root, "catalog_root_reclaim");
            }
        });
    } else if (helix::nav::is_on_top(root)) {
        helix::nav::go_back();
        retire_overlay(root, "catalog_root_reclaim");
    }

    release_catalog_state();
}

void on_catalog_reset(lv_event_t* /*e*/) {
    spdlog::info("[WidgetCatalog] Reset to defaults requested");

    ui::modal_confirm(
        lv_tr("Reset Home Screen?"),
        lv_tr("This will remove all custom pages and restore the default widget layout."),
        ModalSeverity::Warning, lv_tr("Reset"), [] {
            spdlog::info("[WidgetCatalog] Reset confirmed - resetting to defaults");
            auto& config = PanelWidgetManager::instance().get_widget_config("home");
            config.reset_to_defaults();
            config.save();
            close_catalog();
            PanelWidgetManager::instance().notify_config_changed("home");
        });
}

/// Renders a registry span for the size badge.
///
/// The registry stores spans in grid tracks and a track is half a cell
/// (GridLayout::TRACKS_PER_CELL), but a cell is the unit the grid shows the
/// user and the unit the user guide's widget tables are written in. Printing
/// the track count raw badged every one-cell widget as "2x2". The few widgets
/// that may occupy half a cell can carry an odd span, so those render as a
/// half rather than truncating to the cell below.
std::string format_track_span(int tracks) {
    const int cells = tracks / GridLayout::TRACKS_PER_CELL;
    if (tracks % GridLayout::TRACKS_PER_CELL == 0) {
        return std::to_string(cells);
    }
    return std::to_string(cells) + ".5";
}

} // namespace

// ============================================================================
// Row creation
// ============================================================================

lv_obj_t* WidgetCatalogOverlay::create_row(lv_obj_t* parent, const char* name, const char* icon,
                                           const char* description, int colspan, int rowspan,
                                           bool already_placed, bool unavailable) {
    const bool dimmed = already_placed || unavailable;
    const bool has_icon = icon && icon[0] != '\0';
    const bool has_desc = description && description[0] != '\0';

    char size_text[16];
    snprintf(size_text, sizeof(size_text), "%sx%s", format_track_span(colspan).c_str(),
             format_track_span(rowspan).c_str());

    const char* attrs[] = {
        "hide_desc",     has_desc ? "false" : "true",
        "icon_src",      has_icon ? icon : "",
        "icon_variant",  dimmed ? "muted" : "secondary",
        "hide_icon",     has_icon ? "false" : "true",
        "size_text",     size_text,
        "placed_text",   already_placed ? lv_tr("Placed") : "",
        "hide_placed",   already_placed ? "false" : "true",
        "row_opa",       dimmed ? "40%" : "255",
        "row_clickable", dimmed ? "false" : "true",
        nullptr,
    };
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(parent, "widget_catalog_row", attrs));
    if (!row) {
        return nullptr;
    }
    // Widget names and descriptions can come from a Lua plugin.
    helix::ui::set_row_label_text(row, "row_name", name);
    helix::ui::set_row_label_text(row, "row_desc", has_desc ? description : "");
    return row;
}

/// True when this widget's hardware gate subject exists and reads 0.
///
/// A def with no gate subject is never gated. A gate subject that is not
/// registered yet also reads as available: the subjects come up with the panel,
/// and treating "not yet known" as missing hardware would grey out half the
/// catalog during startup.
static bool is_hardware_gated(const PanelWidgetDef& def) {
    if (!def.hardware_gate_subject) {
        return false;
    }
    lv_subject_t* gate = lv_xml_get_subject(nullptr, def.hardware_gate_subject);
    return gate && lv_subject_get_int(gate) == 0;
}

/// Appends " (<reason>)" to a gated widget's catalog name.
static std::string with_gate_hint(const char* display_name, const PanelWidgetDef& def) {
    std::string name(display_name);
    const char* hint =
        def.hardware_gate_hint ? lv_tr(def.hardware_gate_hint) : lv_tr("not detected");
    name += std::string(" (") + hint + ")";
    return name;
}

/// Appends the no-room reason to a widget's catalog name, sized to the minimum
/// the placement search shrinks to and rendered in the cell unit the size
/// badge uses.
static std::string with_fit_hint(const char* display_name, const PanelWidgetDef& def) {
    const std::string need =
        fmt::format(fmt::runtime(lv_tr("Needs {}x{} free: remove or shrink a widget first")),
                    format_track_span(def.effective_min_colspan()),
                    format_track_span(def.effective_min_rowspan()));
    return std::string(display_name) + " (" + need + ")";
}

/// Pointers to every registry def, in registry order — the def list behind the
/// search results.
static std::vector<const PanelWidgetDef*> all_widget_def_ptrs() {
    std::vector<const PanelWidgetDef*> defs;
    const auto& all = get_all_widget_defs();
    defs.reserve(all.size());
    for (const auto& def : all) {
        defs.push_back(&def);
    }
    return defs;
}

/// A category's defs that can actually be placed here, in registry order.
static std::vector<const PanelWidgetDef*> available_in_category(WidgetCategory category) {
    std::vector<const PanelWidgetDef*> out;
    for (const auto& def : get_all_widget_defs()) {
        if (def.category == category && !is_hardware_gated(def)) {
            out.push_back(&def);
        }
    }
    return out;
}

/// Every def whose hardware is missing here, in registry order — the contents of
/// the "Unavailable on this printer" dive.
static std::vector<const PanelWidgetDef*> gated_widget_defs() {
    std::vector<const PanelWidgetDef*> out;
    for (const auto& def : get_all_widget_defs()) {
        if (is_hardware_gated(def)) {
            out.push_back(&def);
        }
    }
    return out;
}

/// What a category sub-page lists: @p category's available widgets, or every
/// gated widget when there is no category.
static std::vector<const PanelWidgetDef*> page_defs(std::optional<WidgetCategory> category) {
    return category ? available_in_category(*category) : gated_widget_defs();
}

/// The registry's identity as the current rows were built: one entry per def,
/// in registry order, covering every field a row renders. refresh_gated_rows()
/// rebuilds when this differs, so a runtime definition arriving or leaving under
/// an open catalog rebuilds even when the def count and gate flags happen to be
/// unchanged, and a same-id re-registration that moved a def's spans, icon or
/// description rebuilds too.
static std::vector<DefRowIdentity> def_row_snapshot() {
    const auto& defs = get_all_widget_defs();
    std::vector<DefRowIdentity> out;
    out.reserve(defs.size());
    for (const auto& def : defs) {
        out.push_back({def.id, def.display_name ? def.display_name : "", def.icon ? def.icon : "",
                       def.description ? def.description : "", def.colspan, def.rowspan,
                       is_hardware_gated(def)});
    }
    return out;
}

/// Placed instances per multi_instance base ID.
///
/// "Placed" means holding a grid cell, not merely enabled — an instance at
/// (-1,-1) is on no dashboard and counting it overstates what the user sees.
static std::unordered_map<std::string, int> count_multi_placed(const PanelWidgetConfig& config) {
    std::unordered_map<std::string, int> multi_placed;
    for (const auto& entry : config.entries()) {
        auto colon_pos = entry.id.find(':');
        if (colon_pos != std::string::npos && entry.is_placed()) {
            multi_placed[entry.id.substr(0, colon_pos)]++;
        }
    }
    return multi_placed;
}

/// Searchable copy of the registry, translated: label = display name, group =
/// category name, description = the one-line blurb shown under each row. `id`
/// indexes get_all_widget_defs(), which is also the row order in search_results.
static std::vector<ui::SelectorEntry> build_catalog_entries() {
    std::vector<ui::SelectorEntry> entries;
    const auto& defs = get_all_widget_defs();
    entries.reserve(defs.size());
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto& def = defs[i];
        const WidgetCategoryDef* cat = find_widget_category(def.category);
        entries.push_back({
            def.display_name ? std::string(lv_tr(def.display_name)) : std::string(def.id),
            cat ? std::string(lv_tr(cat->display_name)) : std::string(),
            static_cast<int>(i),
            def.description ? std::string(lv_tr(def.description)) : std::string(),
        });
    }
    return entries;
}

void WidgetCatalogOverlay::build_search_rows(lv_obj_t* results) {
    g_catalog_state.entries = build_catalog_entries();
    populate_rows(results, *g_catalog_state.config, all_widget_def_ptrs());
    g_catalog_state.search_rows_built = true;
}

void WidgetCatalogOverlay::apply_search(const std::string& query) {
    lv_obj_t* results =
        helix::ui::find_required(g_catalog_state.overlay_root, "search_results", "WidgetCatalog");
    if (!results) {
        return;
    }
    if (ui::selector_query_is_blank(query)) {
        lv_subject_set_int(&g_catalog_state.view, kCatalogViewBrowse);
        return;
    }
    if (!g_catalog_state.search_rows_built && g_catalog_state.config) {
        build_search_rows(results);
    }

    int visible = 0;
    const uint32_t row_count = lv_obj_get_child_count(results);
    for (uint32_t i = 0; i < row_count && i < g_catalog_state.entries.size(); i++) {
        lv_obj_t* row = lv_obj_get_child(results, static_cast<int32_t>(i));
        if (!row) {
            continue;
        }
        if (ui::selector_entry_matches(g_catalog_state.entries[i], query)) {
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
            ++visible;
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_subject_set_int(&g_catalog_state.match_count, visible);
    lv_subject_set_int(&g_catalog_state.view, kCatalogViewSearch);
    lv_obj_scroll_to_y(results, 0, LV_ANIM_OFF);
    spdlog::debug("[WidgetCatalog] Search '{}' matched {} of {} widgets", query, visible,
                  g_catalog_state.entries.size());
}

/// Search box handler — the XML textarea's value_changed callback.
void WidgetCatalogOverlay::on_search_changed(lv_event_t* e) {
    if (!g_catalog_state.overlay_root) {
        return;
    }
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* text = lv_textarea_get_text(ta);
    apply_search(text ? text : "");
}

// ============================================================================
// Category grouping
// ============================================================================

std::vector<const PanelWidgetDef*>
WidgetCatalogOverlay::widgets_in_category(WidgetCategory category) {
    std::vector<const PanelWidgetDef*> out;
    for (const auto& def : get_all_widget_defs()) {
        if (def.category == category) {
            out.push_back(&def);
        }
    }
    return out;
}

lv_obj_t* WidgetCatalogOverlay::active_root() {
    return g_catalog_state.overlay_root;
}

lv_obj_t* WidgetCatalogOverlay::active_category_root() {
    return g_catalog_state.category_root;
}

// ============================================================================
// Populate rows
// ============================================================================

void WidgetCatalogOverlay::populate_category_rows(lv_obj_t* group) {
    const auto& categories = get_widget_categories();

    for (size_t i = 0; i < categories.size(); i++) {
        const auto& cat = categories[i];
        size_t count = available_in_category(cat.id).size();
        if (count == 0) {
            // Every widget this category holds is hardware-gated here; the
            // unavailable row below carries them. A category row that dives
            // into an empty page is a dead menu entry.
            continue;
        }
        std::string subtitle = fmt::format(fmt::runtime(lv_tr("{} widgets")), count);

        const char* attrs[] = {"label", lv_tr(cat.display_name), "label_tag", cat.translation_tag,
                               "icon", cat.icon, "description", subtitle.c_str(),
                               // Already translated and count-dependent — a tag
                               // would re-look-up the formatted string and miss.
                               "description_min_bp", "0", "callback", "on_catalog_category_clicked",
                               nullptr};

        auto* row = static_cast<lv_obj_t*>(lv_xml_create(group, "setting_action_row", attrs));
        if (!row) {
            spdlog::warn("[WidgetCatalog] Failed to create row for category '{}'",
                         cat.display_name);
            continue;
        }
        lv_obj_set_user_data(row, reinterpret_cast<void*>(i));
    }

    // Hardware-gated widgets get their own dive at the bottom rather than
    // greying out rows inside their categories: they stay discoverable (the
    // section is collapsed into one row) without padding out every category
    // with things this printer cannot place.
    size_t gated = gated_widget_defs().size();
    if (gated == 0) {
        return;
    }
    std::string subtitle = fmt::format(fmt::runtime(lv_tr("{} widgets")), gated);
    const char* attrs[] = {"label",
                           lv_tr("Unavailable on this printer"),
                           "label_tag",
                           "Unavailable on this printer",
                           "icon",
                           "block_helper",
                           "description",
                           subtitle.c_str(),
                           "description_min_bp",
                           "0",
                           "callback",
                           "on_catalog_unavailable_clicked",
                           nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(group, "setting_action_row", attrs));
    if (!row) {
        spdlog::warn("[WidgetCatalog] Failed to create the unavailable row");
    }
}

void WidgetCatalogOverlay::on_category_row_clicked(lv_event_t* e) {
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!row || !lv_obj_is_valid(row)) {
        spdlog::warn("[WidgetCatalog] Category row click on an invalid target");
        return;
    }
    auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
    const auto& categories = get_widget_categories();
    if (index >= categories.size()) {
        spdlog::warn("[WidgetCatalog] Category row index {} out of range", index);
        return;
    }
    show_category(categories[index].id);
}

void WidgetCatalogOverlay::on_unavailable_row_clicked(lv_event_t* /*e*/) {
    show_unavailable();
}

void WidgetCatalogOverlay::populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                                         const std::vector<const PanelWidgetDef*>& defs) {
    const auto multi_placed_count = count_multi_placed(config);
    for (const auto* def_ptr : defs) {
        create_widget_row(scroll, *def_ptr, config, multi_placed_count);
    }
}

lv_obj_t* WidgetCatalogOverlay::create_widget_row(
    lv_obj_t* parent, const PanelWidgetDef& def, const PanelWidgetConfig& config,
    const std::unordered_map<std::string, int>& multi_placed_count) {
    const char* display_name = def.display_name ? lv_tr(def.display_name) : def.id;

    // A multi-instance widget is never "all placed" — another instance can
    // always be minted — but it is still gated on its hardware. Two widgets
    // are both: power_device and thermistor. Skipping the gate check let you
    // add a Power tile on a printer with no Moonraker power device, which then
    // rendered as a dead control.
    bool hardware_gated = is_hardware_gated(def);

    // Single-instance widget: is_placed(), not is_enabled() — a widget enabled
    // at (-1,-1) is on no grid, and the catalog is the only surface that can
    // give it a cell back. Dimming it there left it with no UI at all.
    bool already_placed = def.multi_instance ? false : config.is_placed(def.id);

    // A placed or gated row is already dimmed for its own reason; the fit
    // question only decides anything for a row that is otherwise offerable.
    // The grid takes no input while the catalog is open, so the answer cannot
    // change under an open catalog.
    bool no_fit =
        !hardware_gated && !already_placed && g_catalog_state.fits && !g_catalog_state.fits(def);

    std::string name_str(display_name);
    if (hardware_gated) {
        name_str = with_gate_hint(display_name, def);
    } else if (no_fit) {
        name_str = with_fit_hint(display_name, def);
    } else if (def.multi_instance) {
        auto it = multi_placed_count.find(def.id);
        int placed = it != multi_placed_count.end() ? it->second : 0;
        if (placed > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), " (%d %s)", placed, lv_tr("Placed"));
            name_str += buf;
        }
    }

    const char* desc = def.description ? lv_tr(def.description) : nullptr;
    lv_obj_t* row = create_row(parent, name_str.c_str(), def.icon, desc, def.colspan, def.rowspan,
                               already_placed, hardware_gated || no_fit);
    if (!row) {
        return nullptr;
    }
    // Named for the def id so tests and `ctl` can address the row directly.
    lv_obj_set_name(row, def.id);

    // create_row() already stripped CLICKABLE when gated, placed or unfit;
    // binding a handler anyway would leave a live callback on a dead row.
    if (hardware_gated || already_placed || no_fit) {
        return row;
    }

    if (def.multi_instance) {
        // Multi-instance widget — clicking always mints a new instance.
        // The base ID pointer comes from the static def table (stable lifetime)
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* base_id = static_cast<const char*>(lv_event_get_user_data(ev));
                if (!base_id)
                    return;
                // Mint a new instance ID
                auto& mgr_config = PanelWidgetManager::instance().get_widget_config("home");
                std::string new_id = mgr_config.mint_instance_id(base_id);
                spdlog::info("[WidgetCatalog] Minted multi-instance widget: {}", new_id);
                auto cb = g_catalog_state.on_select;
                close_catalog();
                if (cb) {
                    cb(new_id);
                }
            },
            LV_EVENT_CLICKED, const_cast<char*>(def.id));
    } else {
        // Store widget ID in user data for the click handler.
        // The ID string comes from the static widget def table, so the pointer is
        // stable.
        lv_obj_set_user_data(row, const_cast<char*>(def.id));

        // Widget pool recycling exception: dynamic row click handler
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* widget_id = static_cast<const char*>(lv_event_get_user_data(ev));
                if (!widget_id) {
                    return;
                }
                spdlog::info("[WidgetCatalog] Selected widget: {}", widget_id);
                // Copy callback and ID before closing (close resets state)
                auto cb = g_catalog_state.on_select;
                std::string id_copy(widget_id);
                close_catalog();
                if (cb) {
                    cb(id_copy);
                }
            },
            LV_EVENT_CLICKED, const_cast<char*>(def.id));
    }
    return row;
}

// ============================================================================
// Show
// ============================================================================

void WidgetCatalogOverlay::close() {
    close_catalog();
}

void WidgetCatalogOverlay::show(lv_obj_t* parent_screen, const PanelWidgetConfig& config,
                                WidgetSelectedCallback on_select, CatalogClosedCallback on_close,
                                WidgetFitCallback fits) {
    if (g_catalog_state.overlay_root) {
        spdlog::warn("[WidgetCatalog] Already open, ignoring duplicate show()");
        return;
    }
    lv_xml_register_event_cb(nullptr, "on_catalog_reset", on_catalog_reset);
    lv_xml_register_event_cb(nullptr, "on_catalog_category_clicked", on_category_row_clicked);
    lv_xml_register_event_cb(nullptr, "on_catalog_unavailable_clicked", on_unavailable_row_clicked);
    lv_xml_register_event_cb(nullptr, "on_catalog_search_changed", on_search_changed);

    // Search subjects. Registered before the overlay XML is parsed so its
    // bind_flag_* elements resolve them, and re-initialized per open: entries
    // from the previous open's widgets removed themselves when those widgets
    // were deleted, and the next open must not see their values.
    UI_SUBJECT_INIT_AND_REGISTER_INT(g_catalog_state.view, kCatalogViewBrowse,
                                     "widget_catalog_view");
    UI_SUBJECT_INIT_AND_REGISTER_INT(g_catalog_state.match_count, 0, "widget_catalog_match_count");

    // Create a semi-transparent dark backdrop so the home panel shows through.
    // Uses the same modal_backdrop_opacity constant as Modal dialogs (DRY).
    lv_opa_t backdrop_opa = 100; // fallback
    const char* opa_str = lv_xml_get_const(nullptr, "modal_backdrop_opacity");
    if (opa_str) {
        int val = atoi(opa_str);
        if (val >= 0 && val <= 255)
            backdrop_opa = static_cast<lv_opa_t>(val);
    }

    // Park the callbacks before anything can fail. GridEditMode has already set
    // catalog_open_ by the time it calls us, and it
    // only learns otherwise through on_close — so an early return that drops the
    // callback on the floor leaves edit mode permanently believing the catalog is
    // open, with the backdrop stranded over the panel. release_catalog_state()
    // below is only reachable once they are stored here.
    g_catalog_state.on_select = std::move(on_select);
    g_catalog_state.on_close = std::move(on_close);
    g_catalog_state.fits = std::move(fits);

    // Create overlay from XML
    auto* overlay = helix::ui::create_xml_hidden(parent_screen, "widget_catalog_overlay");
    if (!overlay) {
        spdlog::error("[WidgetCatalog] Failed to create widget_catalog_overlay from XML");
        release_catalog_state();
        return;
    }

    // 70% by design — the home grid stays visible behind the list while you drag
    // a widget out of it. Neither navigation width class applies, so opt out of
    // push-time width management (#1178).
    helix::nav::set_overlay_width_unmanaged(overlay);

    // Store state. on_select/on_close were parked above, before the first thing
    // that can fail — re-moving them here would assign the moved-from empties.
    g_catalog_state.overlay_root = overlay;
    g_catalog_state.parent_screen = parent_screen;
    g_catalog_state.config = &config;

    // DELETE cleanup exception: detect when NavigationManager pops the overlay
    // without going through close_catalog() (e.g., system back navigation)
    lv_obj_add_event_cb(
        overlay,
        [](lv_event_t* e) {
            // Only the overlay that owns the current state may clear it — a
            // stale root being reclaimed must not tear down a newer catalog.
            if (g_catalog_state.overlay_root != lv_event_get_target_obj(e)) {
                return;
            }
            release_catalog_state();
        },
        LV_EVENT_DELETE, nullptr);

    // Find the category list container and populate
    lv_obj_t* group = helix::ui::find_required(overlay, "category_group", "WidgetCatalog");
    if (!group) {
        // The delete re-enters the LV_EVENT_DELETE handler, which releases the
        // state and fires on_close while overlay_root still matches. Calling
        // release again is deliberate belt-and-braces: it is idempotent for
        // on_close (moved out and nulled), and leaving the fire to depend on a
        // re-entrant delete is too subtle to rely on.
        lv_obj_delete(overlay);
        release_catalog_state();
        return;
    }

    g_catalog_state.row_defs = def_row_snapshot();
    populate_category_rows(group);

    // Search results are built on the first query (apply_search).
    if (!helix::ui::find_required(overlay, "search_results", "WidgetCatalog")) {
        lv_obj_delete(overlay);
        release_catalog_state();
        return;
    }

    // Register with nullptr lifecycle — this overlay is function-based, not class-based
    helix::nav::register_overlay(overlay, nullptr);

    // The backdrop is made by the same queue drain that runs the push below, just
    // ahead of it, so the screen changes once: made here, it would be drawn in a
    // frame of its own and the whole screen drawn again when the push lands.
    const uint32_t generation = ++g_catalog_generation;
    helix::ui::queue_update("WidgetCatalog::backdrop", [parent_screen, generation, backdrop_opa]() {
        if (generation != g_catalog_generation || !g_catalog_state.overlay_root ||
            !lv_obj_is_valid(parent_screen))
            return;
        auto* backdrop = helix::ui::create_fullscreen_backdrop(parent_screen, backdrop_opa);
        if (backdrop) {
            // Don't block clicks — let taps on the backdrop close the catalog
            lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
            helix::ui::bring_to_front(backdrop);
        }
        g_catalog_state.backdrop = backdrop;
    });

    // Push onto navigation stack — keep the home panel visible behind the catalog
    helix::nav::push_overlay(overlay, /*hide_previous=*/false);

    // Register close callback with NavigationManager so that go_back() (e.g., from
    // the header back button) properly cleans up catalog state. NavigationManager hides
    // overlays rather than deleting them, so LV_EVENT_DELETE alone is insufficient.
    helix::nav::on_close(overlay, [overlay]() {
        if (g_catalog_state.overlay_root == overlay) {
            // A sub-page can only be popped before its parent, so nothing should
            // be dived in here. Retire one anyway rather than leak it if the nav
            // stack was unwound from underneath us.
            if (lv_obj_t* page = g_catalog_state.category_root) {
                g_catalog_state.category_root = nullptr;
                helix::nav::clear_on_close(page);
                retire_category_page(page);
            }
            release_catalog_state();
            // The pop that triggered this callback has already run, so the
            // widget is off the stack and safe to reclaim. Without this the
            // header back button leaks the whole tree the same way close_catalog
            // used to.
            retire_overlay(overlay, "catalog_root_reclaim");
            spdlog::debug("[WidgetCatalog] Closed via navigation go_back");
        }
    });

    // Every row above read the gates once, and capability subjects keep landing
    // for seconds into discovery. release_catalog_state() drops the registration.
    PanelWidgetManager::instance().setup_gate_observers(kGateObserverKey,
                                                        [] { refresh_gated_rows(); });

    spdlog::info("[WidgetCatalog] Overlay shown with {} categories over {} widget definitions",
                 get_widget_categories().size(), get_all_widget_defs().size());
}

// ============================================================================
// Category sub-page
// ============================================================================

void WidgetCatalogOverlay::show_category(WidgetCategory category) {
    const WidgetCategoryDef* def = find_widget_category(category);
    if (!def) {
        spdlog::warn("[WidgetCatalog] Unknown category requested");
        return;
    }
    show_widget_page(lv_tr(def->display_name), def->translation_tag, category);
}

void WidgetCatalogOverlay::show_unavailable() {
    if (gated_widget_defs().empty()) {
        spdlog::debug("[WidgetCatalog] No gated widgets; ignoring unavailable dive");
        return;
    }
    show_widget_page(lv_tr("Unavailable on this printer"), "Unavailable on this printer",
                     std::nullopt);
}

void WidgetCatalogOverlay::show_widget_page(const char* title, const char* title_tag,
                                            std::optional<WidgetCategory> category) {
    if (!g_catalog_state.overlay_root || !g_catalog_state.config ||
        !g_catalog_state.parent_screen) {
        spdlog::warn("[WidgetCatalog] show_widget_page() with no catalog open");
        return;
    }
    if (g_catalog_state.category_root) {
        spdlog::warn("[WidgetCatalog] A category page is already open, ignoring dive");
        return;
    }

    // The title is baked in at parse time — overlay_panel forwards $title to its
    // header_bar, so each dive creates a page already carrying its own name.
    const char* attrs[] = {"title", title, "title_tag", title_tag, nullptr};
    auto* page = helix::ui::create_xml_hidden(g_catalog_state.parent_screen,
                                              "widget_catalog_category_overlay", attrs);
    if (!page) {
        spdlog::error("[WidgetCatalog] Failed to create widget_catalog_category_overlay from XML");
        return;
    }

    // Same 70% opt-out as the catalog beneath it: without this the push would
    // inherit the parent's destination class and go full width (#1178).
    helix::nav::set_overlay_width_unmanaged(page);

    lv_obj_t* scroll = helix::ui::find_required(page, "catalog_scroll", "WidgetCatalog");
    if (!scroll) {
        lv_obj_delete(page);
        return;
    }
    const auto defs = page_defs(category);
    populate_rows(scroll, *g_catalog_state.config, defs);

    g_catalog_state.category_root = page;
    g_catalog_state.page_category = category;

    helix::nav::register_overlay(page, nullptr);
    helix::nav::push_overlay(page, /*hide_previous=*/false);

    // Back out of the sub-page: return to the category list with the catalog
    // still open. Deliberately does NOT touch on_close — only leaving the
    // catalog itself closes it.
    helix::nav::on_close(page, [page]() {
        if (g_catalog_state.category_root != page) {
            return;
        }
        g_catalog_state.category_root = nullptr;
        retire_category_page(page);
        spdlog::debug("[WidgetCatalog] Category page closed, back at the category list");
    });

    spdlog::info("[WidgetCatalog] Dived into '{}' ({} widgets)", title_tag, defs.size());
}

// ============================================================================
// Hardware gate changes
// ============================================================================

void WidgetCatalogOverlay::refresh_gated_rows() {
    if (!g_catalog_state.overlay_root || !g_catalog_state.config) {
        return;
    }
    std::vector<DefRowIdentity> row_defs = def_row_snapshot();
    if (row_defs == g_catalog_state.row_defs) {
        return;
    }
    g_catalog_state.row_defs = std::move(row_defs);
    const PanelWidgetConfig& config = *g_catalog_state.config;
    lv_obj_t* root = g_catalog_state.overlay_root;

    // Each container empties at once and refills in place; the old rows go
    // through LVGL's async delete.
    if (lv_obj_t* group = helix::ui::find_required(root, "category_group", "WidgetCatalog")) {
        helix::ui::safe_clean_children(group);
        populate_category_rows(group);
    }

    // Rebuilt result rows stay parallel to entries (one per def, registry order)
    // and start out visible, so the query still in the box re-filters them.
    // Entries are index-parallel to these rows: rebuilding one without the other
    // leaves the query filtering rows by the wrong def's name. Rows no query has
    // asked for yet are built by the first one.
    lv_obj_t* results = helix::ui::find_required(root, "search_results", "WidgetCatalog");
    if (results && g_catalog_state.search_rows_built) {
        helix::ui::safe_clean_children(results);
        build_search_rows(results);
        lv_obj_t* input = helix::ui::find_required(root, "catalog_search_input", "WidgetCatalog");
        const char* query = input ? lv_textarea_get_text(input) : nullptr;
        apply_search(query ? query : "");
    }

    if (lv_obj_t* page = g_catalog_state.category_root) {
        if (lv_obj_t* scroll = helix::ui::find_required(page, "catalog_scroll", "WidgetCatalog")) {
            helix::ui::safe_clean_children(scroll);
            populate_rows(scroll, config, page_defs(g_catalog_state.page_category));
        }
    }
    spdlog::debug(
        "[WidgetCatalog] Hardware gates or widget definitions changed; rebuilt the catalog rows");
}

} // namespace helix
