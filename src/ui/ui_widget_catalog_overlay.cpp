// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_widget_catalog_overlay.h"

#include "ui_effects.h"
#include "ui_fonts.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "grid_layout.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

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

struct CatalogState {
    lv_obj_t* overlay_root = nullptr;
    lv_obj_t* backdrop = nullptr;      // Semi-transparent dark backdrop behind the catalog
    lv_obj_t* category_root = nullptr; // Pushed category sub-page, nullptr at the top level
    lv_obj_t* parent_screen = nullptr;
    const PanelWidgetConfig* config = nullptr; // Owned by the caller (GridEditMode)
    WidgetSelectedCallback on_select;
    CatalogClosedCallback on_close;
};

CatalogState g_catalog_state;

/// Drop every piece of catalog state and fire on_close exactly once.
///
/// Shared by all three teardown paths (close_catalog(), the LV_EVENT_DELETE
/// handler, the NavigationManager close callback) so none of them can fire the
/// callback twice or leave half the state behind.
void release_catalog_state() {
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
    NavigationManager::instance().unregister_overlay_instance(overlay);
    helix::ui::queue_update(tag, [overlay]() {
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

    auto& nav = NavigationManager::instance();
    // We drive this teardown ourselves — stop the page's own close callback from
    // racing us into retire_category_page().
    nav.unregister_overlay_close_callback(page);

    // #1221 guard. If something is stacked above the sub-page we must not pop it
    // — and must not reclaim it either, since the nav stack still points at it.
    // Leaking one widget beats deleting one the stack will unwind through.
    if (!nav.is_panel_on_top(page)) {
        spdlog::warn("[WidgetCatalog] Category page is not on top of the nav stack; "
                     "leaving it for the stack to unwind");
        return false;
    }
    nav.go_back();
    retire_category_page(page);
    return true;
}

void close_catalog() {
    if (!g_catalog_state.overlay_root) {
        return;
    }
    lv_obj_t* root = g_catalog_state.overlay_root;
    auto& nav = NavigationManager::instance();

    // Two-level teardown. go_back() queues its entire body, so two calls made
    // back to back would both evaluate is_panel_on_top() against the *pre-pop*
    // stack and the second would refuse to run (#1221). Queue the catalog's pop
    // instead: it lands after the sub-page pop body has executed, by which point
    // the catalog really is on top.
    bool popped_page = pop_category_page();

    // Unregister the close callback so the pop below cannot double-fire on_close.
    nav.unregister_overlay_close_callback(root);

    // Retire in the same scope as the pop that removed it, exactly as
    // pop_category_page() does. Queueing the reclaim separately would race
    // go_back()'s own queued body, and scrub_deleted_widget() erasing the widget
    // from panel_stack_ first would make that pop take the wrong overlay.
    if (popped_page) {
        helix::ui::queue_update("catalog_close_pop", [root]() {
            auto& n = NavigationManager::instance();
            if (n.is_panel_on_top(root)) {
                n.go_back();
                retire_overlay(root, "catalog_root_reclaim");
            }
        });
    } else if (nav.is_panel_on_top(root)) {
        nav.go_back();
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
                                           bool already_placed, bool hardware_gated) {
    // Row container: horizontal, fixed height
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    int32_t pad_xs = theme_manager_get_spacing("space_xs");
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_md"), 0);
    lv_obj_set_style_pad_gap(row, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_set_style_bg_color(row, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Icon
    if (icon && icon[0] != '\0') {
        const char* variant = (already_placed || hardware_gated) ? "muted" : "secondary";
        const char* icon_attrs[] = {"src", icon, "size", "sm", "variant", variant, nullptr};
        lv_xml_create(row, "icon", icon_attrs);
    }

    if (already_placed || hardware_gated) {
        lv_obj_set_style_opa(row, LV_OPA_40, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Pressed feedback
        lv_obj_set_style_bg_color(row, theme_get_accent_color(), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Left side: name + description column
    // Width comes from flex_grow, not from the content: the name can be much
    // wider than the row. A gated widget carries its reason in the name
    // ("Humidity (No humidity sensor detected)"), which is longer than any
    // widget name, and sizing to content ran that straight through the size
    // badge and the "Placed" label on a 480px panel.
    lv_obj_t* text_col = lv_obj_create(row);
    lv_obj_set_width(text_col, 0);
    lv_obj_set_height(text_col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(text_col, 0, 0);
    lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_col, 0, 0);
    lv_obj_set_layout(text_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

    // Wrap rather than ellipsize. The gate reason lives at the end of the name,
    // so clipping it is exactly the half that explains why the row is greyed out.
    lv_obj_t* name_label = lv_label_create(text_col);
    lv_label_set_text(name_label, name);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(name_label, &noto_sans_16, 0);
    lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);

    if (description && description[0] != '\0') {
        lv_obj_t* desc_label = lv_label_create(text_col);
        lv_label_set_text(desc_label, description);
        lv_obj_set_width(desc_label, LV_PCT(100));
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(desc_label, &noto_sans_12, 0);
        lv_obj_set_style_text_color(desc_label, theme_manager_get_color("text_muted"), 0);
    }

    // Right side: size badge + optional "Placed" label
    lv_obj_t* right_group = lv_obj_create(row);
    lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(right_group, 0, 0);
    lv_obj_set_style_pad_gap(right_group, pad_xs, 0);
    lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_group, 0, 0);
    lv_obj_set_layout(right_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(right_group, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_remove_flag(right_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(right_group, LV_OBJ_FLAG_SCROLLABLE);

    if (already_placed) {
        lv_obj_t* placed_label = lv_label_create(right_group);
        lv_label_set_text(placed_label, lv_tr("Placed"));
        lv_obj_set_style_text_font(placed_label, &noto_sans_12, 0);
        lv_obj_set_style_text_color(placed_label, theme_manager_get_color("text_muted"), 0);
    }

    // Size badge (e.g. "2x1")
    lv_obj_t* badge = lv_obj_create(right_group);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    int32_t badge_pad_ver = theme_manager_get_spacing("space_xxs");
    lv_obj_set_style_pad_left(badge, pad_xs, 0);
    lv_obj_set_style_pad_right(badge, pad_xs, 0);
    lv_obj_set_style_pad_top(badge, badge_pad_ver, 0);
    lv_obj_set_style_pad_bottom(badge, badge_pad_ver, 0);
    lv_obj_set_style_bg_color(badge, theme_manager_get_color("secondary"), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 4, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    char size_text[16];
    snprintf(size_text, sizeof(size_text), "%sx%s", format_track_span(colspan).c_str(),
             format_track_span(rowspan).c_str());
    lv_obj_t* badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, size_text);
    lv_obj_set_style_text_font(badge_label, &noto_sans_12, 0);
    lv_obj_set_style_text_color(badge_label, theme_manager_get_color("text_muted"), 0);

    return row;
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
        size_t count = widgets_in_category(cat.id).size();
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

void WidgetCatalogOverlay::populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                                         WidgetCategory category) {
    const auto defs = widgets_in_category(category);

    // Pre-pass: count placed instances per multi_instance base ID.
    // "Placed" means holding a grid cell, not merely enabled — an instance at
    // (-1,-1) is on no dashboard and counting it overstates what the user sees.
    std::unordered_map<std::string, int> multi_placed_count;
    for (const auto& entry : config.entries()) {
        auto colon_pos = entry.id.find(':');
        if (colon_pos != std::string::npos && entry.enabled && entry.has_grid_position()) {
            std::string base = entry.id.substr(0, colon_pos);
            multi_placed_count[base]++;
        }
    }

    for (const auto* def_ptr : defs) {
        const auto& def = *def_ptr;
        if (def.multi_instance) {
            // Multi-instance widget — show one row with placed count.
            // Clicking always mints a new instance.
            int placed = multi_placed_count[def.id];

            const char* display_name = def.display_name ? lv_tr(def.display_name) : def.id;

            // A multi-instance widget is never "all placed" — another instance
            // can always be minted — but it is still gated on its hardware. Two
            // widgets are both: power_device and thermistor. Skipping the gate
            // here let you add a Power tile on a printer with no Moonraker power
            // device, which then rendered as a dead control.
            bool hardware_gated = is_hardware_gated(def);

            std::string name_str =
                hardware_gated ? with_gate_hint(display_name, def) : std::string(display_name);
            if (!hardware_gated && placed > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), " (%d %s)", placed, lv_tr("Placed"));
                name_str += buf;
            }

            const char* desc = def.description ? lv_tr(def.description) : nullptr;
            lv_obj_t* row = create_row(scroll, name_str.c_str(), def.icon, desc, def.colspan,
                                       def.rowspan, /*already_placed=*/false, hardware_gated);

            // create_row() already stripped CLICKABLE when gated; binding the
            // mint handler anyway would leave a live callback on a dead row.
            if (hardware_gated) {
                continue;
            }

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
            // Single-instance widget.
            // is_placed(), not is_enabled(): a widget enabled at (-1,-1) is on
            // no grid, and the catalog is the only surface that can give it a
            // cell back. Dimming it there left it with no UI at all.
            bool already_placed = config.is_placed(def.id);

            const char* display_name = def.display_name ? lv_tr(def.display_name) : def.id;

            bool hardware_gated = is_hardware_gated(def);
            std::string name_str =
                hardware_gated ? with_gate_hint(display_name, def) : std::string(display_name);

            const char* desc = def.description ? lv_tr(def.description) : nullptr;
            lv_obj_t* row = create_row(scroll, name_str.c_str(), def.icon, desc, def.colspan,
                                       def.rowspan, already_placed, hardware_gated);

            if (!already_placed) {
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
        }
    }
}

// ============================================================================
// Show
// ============================================================================

void WidgetCatalogOverlay::show(lv_obj_t* parent_screen, const PanelWidgetConfig& config,
                                WidgetSelectedCallback on_select, CatalogClosedCallback on_close) {
    if (g_catalog_state.overlay_root) {
        spdlog::warn("[WidgetCatalog] Already open, ignoring duplicate show()");
        return;
    }
    lv_xml_register_event_cb(nullptr, "on_catalog_reset", on_catalog_reset);
    lv_xml_register_event_cb(nullptr, "on_catalog_category_clicked", on_category_row_clicked);

    // Create a semi-transparent dark backdrop so the home panel shows through.
    // Uses the same modal_backdrop_opacity constant as Modal dialogs (DRY).
    lv_opa_t backdrop_opa = 100; // fallback
    const char* opa_str = lv_xml_get_const(nullptr, "modal_backdrop_opacity");
    if (opa_str) {
        int val = atoi(opa_str);
        if (val >= 0 && val <= 255)
            backdrop_opa = static_cast<lv_opa_t>(val);
    }
    auto* backdrop = helix::ui::create_fullscreen_backdrop(parent_screen, backdrop_opa);
    if (backdrop) {
        // Don't block clicks — let taps on the backdrop close the catalog
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    }
    g_catalog_state.backdrop = backdrop;

    // Park the callbacks before anything can fail. GridEditMode has already set
    // catalog_open_ and hidden the dots overlay by the time it calls us, and it
    // only learns otherwise through on_close — so an early return that drops the
    // callback on the floor leaves edit mode permanently believing the catalog is
    // open, with the backdrop stranded over the panel. release_catalog_state()
    // below is only reachable once they are stored here.
    g_catalog_state.on_select = std::move(on_select);
    g_catalog_state.on_close = std::move(on_close);

    // Create overlay from XML
    auto* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen, "widget_catalog_overlay", nullptr));
    if (!overlay) {
        spdlog::error("[WidgetCatalog] Failed to create widget_catalog_overlay from XML");
        release_catalog_state();
        return;
    }

    // 70% by design — the home grid stays visible behind the list while you drag
    // a widget out of it. Neither navigation width class applies, so opt out of
    // push-time width management (#1178).
    NavigationManager::instance().set_overlay_width_unmanaged(overlay);

    // Initially hidden (NavigationManager will unhide during push)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_t* group = lv_obj_find_by_name(overlay, "category_group");
    if (!group) {
        spdlog::error("[WidgetCatalog] category_group not found in XML");
        // The delete re-enters the LV_EVENT_DELETE handler, which releases the
        // state and fires on_close while overlay_root still matches. Calling
        // release again is deliberate belt-and-braces: it is idempotent for
        // on_close (moved out and nulled), and leaving the fire to depend on a
        // re-entrant delete is too subtle to rely on.
        lv_obj_delete(overlay);
        release_catalog_state();
        return;
    }

    populate_category_rows(group);

    // Register with nullptr lifecycle — this overlay is function-based, not class-based
    NavigationManager::instance().register_overlay_instance(overlay, nullptr);

    // Push onto navigation stack — keep the home panel visible behind the catalog
    NavigationManager::instance().push_overlay(overlay, /*hide_previous=*/false);

    // Register close callback with NavigationManager so that go_back() (e.g., from
    // the header back button) properly cleans up catalog state. NavigationManager hides
    // overlays rather than deleting them, so LV_EVENT_DELETE alone is insufficient.
    NavigationManager::instance().register_overlay_close_callback(overlay, [overlay]() {
        if (g_catalog_state.overlay_root == overlay) {
            // A sub-page can only be popped before its parent, so nothing should
            // be dived in here. Retire one anyway rather than leak it if the nav
            // stack was unwound from underneath us.
            if (lv_obj_t* page = g_catalog_state.category_root) {
                g_catalog_state.category_root = nullptr;
                NavigationManager::instance().unregister_overlay_close_callback(page);
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

    spdlog::info("[WidgetCatalog] Overlay shown with {} categories over {} widget definitions",
                 get_widget_categories().size(), get_all_widget_defs().size());
}

// ============================================================================
// Category sub-page
// ============================================================================

void WidgetCatalogOverlay::show_category(WidgetCategory category) {
    if (!g_catalog_state.overlay_root || !g_catalog_state.config ||
        !g_catalog_state.parent_screen) {
        spdlog::warn("[WidgetCatalog] show_category() with no catalog open");
        return;
    }
    if (g_catalog_state.category_root) {
        spdlog::warn("[WidgetCatalog] A category page is already open, ignoring dive");
        return;
    }
    const WidgetCategoryDef* def = find_widget_category(category);
    if (!def) {
        spdlog::warn("[WidgetCatalog] Unknown category requested");
        return;
    }

    // The title is baked in at parse time — overlay_panel forwards $title to its
    // header_bar, so each dive creates a page already carrying its own name.
    const char* attrs[] = {"title", lv_tr(def->display_name), "title_tag", def->translation_tag,
                           nullptr};
    auto* page = static_cast<lv_obj_t*>(
        lv_xml_create(g_catalog_state.parent_screen, "widget_catalog_category_overlay", attrs));
    if (!page) {
        spdlog::error("[WidgetCatalog] Failed to create widget_catalog_category_overlay from XML");
        return;
    }

    auto& nav = NavigationManager::instance();
    // Same 70% opt-out as the catalog beneath it: without this the push would
    // inherit the parent's destination class and go full width (#1178).
    nav.set_overlay_width_unmanaged(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scroll = lv_obj_find_by_name(page, "catalog_scroll");
    if (!scroll) {
        spdlog::error("[WidgetCatalog] catalog_scroll not found in category overlay XML");
        lv_obj_delete(page);
        return;
    }
    populate_rows(scroll, *g_catalog_state.config, category);

    g_catalog_state.category_root = page;

    nav.register_overlay_instance(page, nullptr);
    nav.push_overlay(page, /*hide_previous=*/false);

    // Back out of the sub-page: return to the category list with the catalog
    // still open. Deliberately does NOT touch on_close — only leaving the
    // catalog itself closes it.
    nav.register_overlay_close_callback(page, [page]() {
        if (g_catalog_state.category_root != page) {
            return;
        }
        g_catalog_state.category_root = nullptr;
        retire_category_page(page);
        spdlog::debug("[WidgetCatalog] Category page closed, back at the category list");
    });

    spdlog::info("[WidgetCatalog] Dived into category '{}' ({} widgets)", def->display_name,
                 widgets_in_category(category).size());
}

} // namespace helix
