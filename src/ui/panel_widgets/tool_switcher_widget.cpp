// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_slot_resolver.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

namespace helix {

// Static instance for event callback routing
ToolSwitcherWidget* ToolSwitcherWidget::s_active_instance = nullptr;

/// Resolve a responsive spacing token to pixels, with a fallback.
static int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_pill_cb", ToolSwitcherWidget::tool_pill_cb);
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

// Compact mode: too small on both axes for pills (was colspan==1 &&
// rowspan==1). W_NORMAL/H_TALL are the pixel floors below which the old
// predicate's colspan/rowspan==1 held.
bool ToolSwitcherWidget::is_compact_size() const {
    return current_width_px_ < widget_size::W_NORMAL && current_height_px_ < widget_size::H_TALL;
}

// Narrow but tall: single vertical column of pills (was colspan==1 &&
// rowspan>=2) — the legacy 1x2 layout.
bool ToolSwitcherWidget::is_narrow_tall_size() const {
    return current_width_px_ < widget_size::W_NORMAL && current_height_px_ >= widget_size::H_TALL;
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_active_instance = this;

    // SIZE_CHANGED is a layout event — cannot be registered via XML
    // <event_cb>. Hooked on tool_switcher_container itself (not widget_obj_):
    // LVGL's layout_update_core() fires SIZE_CHANGED for an object as soon as
    // ITS OWN refr_size() runs, before it applies its layout to reflow
    // percentage-sized children (lv_layout_apply() runs after). Hooking
    // widget_obj_ and then self-measuring the child container inside
    // rebuild_pills() would read the child's stale, not-yet-cascaded size —
    // and the lv_obj_update_layout() call rebuild_pills() already does for
    // its own reasons is a no-op here too, since lv_obj_update_layout() self-
    // guards against the reentrant call this handler is nested inside via
    // its own mutex. Watching the container directly — the same object
    // rebuild_pills() measures — matches UiClogMeter/UiBufferMeter, which
    // always measure the same object their handler is hooked on.
    // See rebuild_for_settled_grid_size() for why this is here.
    size_watch_container_ = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (size_watch_container_) {
        lv_obj_add_event_cb(size_watch_container_, on_widget_size_changed, LV_EVENT_SIZE_CHANGED,
                            this);
    }

    auto& tool_state = ToolState::instance();
    auto token = lifetime_.token();

    // Observe active tool changes
    active_tool_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_active_tool_subject(), this,
        [token](ToolSwitcherWidget* self, int tool) {
            if (token.expired())
                return;
            self->on_active_tool_changed(tool);
        },
        tool_state.get_subjects_lifetime());

    // Observe tool count changes to trigger rebuild
    tool_count_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_tool_count_subject(), this,
        [token](ToolSwitcherWidget* self, int /*count*/) {
            if (token.expired())
                return;
            if (self->is_compact_size()) {
                self->rebuild_compact();
            } else {
                self->rebuild_pills();
            }
        },
        tool_state.get_subjects_lifetime());

    // Re-grey on every print-state transition. PanelWidget instances are
    // RECYCLED across home-panel rebuilds, so registering here (rather than
    // only reacting to on_size_changed) is what keeps a reused instance from
    // carrying the previous screen's gating. print_lifecycle rather than
    // print_state_enum: the gate now refuses during Preparing, and the raw enum
    // does not move on the Idle -> Preparing edge, so the pills would stay lit
    // through a host-side pre-print block even with the guard fixed.
    //
    // Takes the lifetime token. print_lifecycle is one of PrinterPrintState's
    // static subjects, torn down by deinit_subjects() between test cases, and an
    // ObserverGuard that outlives that cycle calls lv_observer_remove() on freed
    // memory (#705). The comment here used to claim none was needed; its two
    // sibling call sites (ui_panel_filament, ui_ams_sidebar) both pass it.
    print_state_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        printer_state_.get_print_lifecycle_subject(), this,
        [token](ToolSwitcherWidget* self, int /*state*/) {
            if (token.expired())
                return;
            self->refresh_print_gating();
        },
        printer_state_.get_static_print_subjects_lifetime());

    // Initial build deferred to on_size_changed() which fires after
    // the widget is fully attached to the screen tree.
    // Building here can crash (disp==NULL) if XML tree isn't mounted yet.
}

void ToolSwitcherWidget::detach() {
    lifetime_.invalidate();
    picker_.hide();
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    print_state_observer_.reset();
    pill_buttons_.clear();
    compact_label_ = nullptr;
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
    if (size_watch_container_) {
        lv_obj_remove_event_cb_with_user_data(size_watch_container_, on_widget_size_changed, this);
    }
    size_watch_container_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    grid_settled_w_px_ = -1;
    grid_settled_h_px_ = -1;
    in_grid_size_refresh_ = false;
}

void ToolSwitcherWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                         int height_px) {
    current_width_px_ = width_px;
    current_height_px_ = height_px;

    if (!widget_obj_)
        return;

    if (is_compact_size()) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}

void ToolSwitcherWidget::on_widget_size_changed(lv_event_t* e) {
    auto* self = static_cast<ToolSwitcherWidget*>(lv_event_get_user_data(e));
    if (self)
        self->rebuild_for_settled_grid_size();
}

void ToolSwitcherWidget::rebuild_for_settled_grid_size() {
    if (!widget_obj_ || !size_watch_container_)
        return;

    // Re-entrancy guard — see the member comment on in_grid_size_refresh_.
    if (in_grid_size_refresh_)
        return;

    int w = lv_obj_get_width(size_watch_container_);
    int h = lv_obj_get_height(size_watch_container_);

    // No-op when unchanged — see the member comment on grid_settled_w_px_.
    if (w == grid_settled_w_px_ && h == grid_settled_h_px_)
        return;

    in_grid_size_refresh_ = true;
    grid_settled_w_px_ = w;
    grid_settled_h_px_ = h;

    // current_width_px_/current_height_px_ (the granted cell size) were
    // already set correctly by on_size_changed() — PanelWidgetManager
    // computes those from grid_track_extent(), not from widget_obj_'s
    // on-screen size, so they are right from the start. Only
    // tool_switcher_container's OWN on-screen size lagged behind pre-grid;
    // re-running the same mode decision now lets rebuild_pills() self-measure
    // the now-settled container instead of the stale pre-grid box.
    if (is_compact_size()) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }

    in_grid_size_refresh_ = false;
}

// ============================================================================
// Pill buttons (inline mode for 1x2, 2x1, 2x2, etc.)
// ============================================================================

void ToolSwitcherWidget::rebuild_pills() {
    if (!widget_obj_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for pill rebuild");
        return;
    }

    pill_buttons_.clear();
    compact_label_ = nullptr;
    helix::ui::safe_clean_children(container);

    // Neutralize any grid layout left active by a previous rebuild before we
    // measure or repopulate. safe_clean_children() defers child deletion, so the
    // old pills are briefly still attached; with the grid still active, the
    // measurement lv_obj_update_layout() below (or any interleaved refresh) would
    // run grid item_repos over them. The grid is re-activated only after every new
    // pill has its cell set (end of this function), so a layout pass can never
    // observe a grid child without a cell -> out-of-range track read / heap
    // walk-off (bundle P234RYCL, AD5X).
    lv_obj_set_layout(container, LV_LAYOUT_NONE);

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    if (tools.empty()) {
        spdlog::debug("[ToolSwitcher] No tools available for pill rebuild");
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);
    int btn_min_h = resolve_space_token("space_xl", 24);
    int btn_min_w = resolve_space_token("button_height_sm", 40);

    // Layout strategy:
    //  - colspan == 1 && rowspan >= 2: single tall column of pills (legacy 1x2 path).
    //  - otherwise: pick row count from available container height — if the widget
    //    is tall enough for two pill rows, split pills across 2 rows via a grid;
    //    otherwise keep the single flex row. Always horizontal-scroll for overflow.
    //  - Cap rows at 2: "split in two", not "stack like a virtual keyboard".
    int total = static_cast<int>(tools.size());
    int rows = 1;
    int cols = total;
    bool use_grid = false;

    if (is_narrow_tall_size()) {
        // Tall narrow widget — vertical pill column (legacy behavior).
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    } else {
        // Measure container height to decide whether 2 rows fit.
        // Force a layout pass first; rebuild can fire pre-layout (e.g. from
        // observers during attach) and lv_obj_get_height() would return 0.
        lv_obj_update_layout(container);
        int container_h = lv_obj_get_content_height(container);
        if (container_h <= 0) {
            container_h = lv_obj_get_height(container);
        }
        int pill_min_h = btn_min_w; // square-ish pill, use button_height_sm as min row height
        int row_gap = space_xs;
        int fit_rows =
            (container_h > 0) ? std::max(1, (container_h + row_gap) / (pill_min_h + row_gap)) : 1;
        int preferred_rows = std::min(2, fit_rows); // cap at 2 rows per spec

        if (preferred_rows >= 2 && total >= 2) {
            rows = preferred_rows;
            cols = (total + rows - 1) / rows; // ceil(total / rows)
            if (cols < 1)
                cols = 1;
            use_grid = true;

            // Build the grid descriptor now, but defer activating LV_LAYOUT_GRID
            // until after every pill is created and placed (end of this function)
            // so the first layout pass never reads a grid child whose cell is not
            // yet set.
            grid_col_dsc_.assign(static_cast<size_t>(cols), LV_GRID_CONTENT);
            grid_col_dsc_.push_back(LV_GRID_TEMPLATE_LAST);
            grid_row_dsc_.assign(static_cast<size_t>(rows), LV_GRID_FR(1));
            grid_row_dsc_.push_back(LV_GRID_TEMPLATE_LAST);
            lv_obj_set_scroll_dir(container, LV_DIR_HOR);
            lv_obj_set_style_pad_row(container, space_xs, 0);
            lv_obj_set_style_pad_column(container, space_xs, 0);
        } else {
            // Single-row flex (existing behavior, horizontal scroll for overflow).
            lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
        }
    }
    lv_obj_set_style_pad_gap(container, space_xs, 0);

    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Create pill button from XML ui_button widget — variant handles base styling
        const char* variant = is_active ? "primary" : "ghost";
        const char* attrs[] = {"variant", variant, "text", tools[i].name.c_str(), nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(container, "ui_button", attrs));
        if (!btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('ui_button') returned NULL for pill '{}'",
                          tools[i].name);
            continue;
        }

        if (use_grid) {
            // Row-major placement: T0..Tcols-1 on row 0, Tcols.. on row 1, etc.
            int row = static_cast<int>(i) / cols;
            int col = static_cast<int>(i) % cols;
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
            // Keep pill tappable even when the column shrinks to content.
            lv_obj_set_style_min_width(btn, btn_min_w, 0);
            lv_obj_set_height(btn, LV_SIZE_CONTENT);
        } else {
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_height(btn, LV_SIZE_CONTENT);
        }
        lv_obj_set_style_min_height(btn, btn_min_h, 0);
        lv_obj_set_style_radius(btn, btn_min_h / 2, 0);
        lv_obj_set_style_pad_ver(btn, resolve_space_token("space_xxs", 4), 0);
        lv_obj_set_style_pad_hor(btn, resolve_space_token("space_sm", 8), 0);

        // Pass tool index via event callback user_data (NOT obj user_data — L069)
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] pill_click");
                if (!s_active_instance)
                    return;
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                s_active_instance->handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        pill_buttons_.push_back(btn);
    }

    // Every pill now carries its grid cell (set in the loop above). Activate the
    // grid layout last so the first layout pass can never see an unplaced child.
    if (use_grid) {
        lv_obj_set_grid_dsc_array(container, grid_col_dsc_.data(), grid_row_dsc_.data());
        lv_obj_set_layout(container, LV_LAYOUT_GRID);
    }

    // Scroll the active pill into view when the container overflows.
    if (active >= 0 && active < static_cast<int>(pill_buttons_.size())) {
        lv_obj_scroll_to_view(pill_buttons_[active], LV_ANIM_OFF);
    }

    // Freshly created pills carry no state — apply the print gate to them here
    // as well as from the observer, or a rebuild silently re-enables them.
    refresh_print_gating();

    spdlog::debug("[ToolSwitcher] Built {} pill buttons, active={}, layout={} ({}x{})",
                  tools.size(), active, use_grid ? "grid" : "flex", rows, cols);
}

void ToolSwitcherWidget::on_active_tool_changed(int tool_index) {
    if (is_compact_size()) {
        // Compact mode — rebuild to update the label
        if (widget_obj_) {
            rebuild_compact();
        }
        return;
    }

    // Pill mode — rebuild to apply correct variant styling per button
    if (widget_obj_) {
        rebuild_pills();
    }

    spdlog::debug("[ToolSwitcher] Active tool changed to T{}", tool_index);
}

// ============================================================================
// Compact mode (1x1 — single label + picker popup)
// ============================================================================

void ToolSwitcherWidget::rebuild_compact() {
    if (!widget_obj_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for compact rebuild");
        return;
    }

    pill_buttons_.clear();
    helix::ui::safe_clean_children(container);

    auto& tool_state = ToolState::instance();
    int active = tool_state.active_tool_index();
    const auto& tools = tool_state.tools();

    // Set container clickable for compact mode
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(container, resolve_space_token("space_xxs", 2), 0);

    // Swap icon above tool label
    const char* icon_attrs[] = {"src",     "arrow_left_right", "size", "sm",
                                "variant", "secondary",        nullptr};
    auto* icon = static_cast<lv_obj_t*>(lv_xml_create(container, "icon", icon_attrs));
    if (icon) {
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Current tool label centered with larger font
    lv_obj_t* label = lv_label_create(container);
    std::string tool_name =
        (active >= 0 && active < static_cast<int>(tools.size())) ? tools[active].name : "T?";
    lv_label_set_text(label, tool_name.c_str());
    compact_label_ = label;
    const lv_font_t* body_font = theme_manager_get_font("font_body");
    if (body_font)
        lv_obj_set_style_text_font(label, body_font, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Click opens picker
    lv_obj_add_event_cb(
        container,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] compact_click");
            if (s_active_instance) {
                s_active_instance->show_tool_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    // Sets the label colour (muted while a print blocks the change, normal
    // otherwise). Must run on every rebuild, not just on a state change.
    refresh_print_gating();

    spdlog::debug("[ToolSwitcher] Built compact mode, active=T{}", active);
}

// ============================================================================
// Tool picker popup (for compact mode)
// ============================================================================

void ToolSwitcherWidget::show_tool_picker() {
    if (picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    // Compact mode's only affordance is this picker, so refuse before opening a
    // list in which every entry is a guaranteed-failure dead end.
    const AmsError refusal = tool_change_refusal();
    if (!refusal.success()) {
        spdlog::info("[ToolSwitcher] Picker refused: {}", refusal.technical_msg);
        helix::ui::notify_ams_warning(refusal);
        return;
    }

    if (ToolState::instance().tools().empty()) {
        return;
    }

    // The card hangs off the widget tile's left edge, so the tool names line up
    // with the compact readout they replace.
    picker_.show_below_widget(parent_screen_, widget_obj_,
                              helix::ui::ContextMenu::AnchorAlign::Left);
}

void ToolSwitcherWidget::ToolPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* tool_list = lv_obj_find_by_name(backdrop, "tool_list");
    if (!tool_list) {
        spdlog::error("[ToolSwitcher] tool_list not found in picker XML");
        return;
    }

    // The card is as wide as the widget tile it hangs off, so the buttons inside
    // it line up with the compact readout. Set before the rows are built: they are
    // width="100%" and cannot resolve against a width="content" card.
    if (lv_obj_t* menu_card = card()) {
        lv_obj_set_width(menu_card, lv_obj_get_width(owner_.widget_obj_));
    }

    // Cap the list at a share of the screen so a 15-lane AFC scrolls the list
    // instead of growing the card past the panel.
    lv_obj_set_style_max_height(tool_list, screen_height_pct(60), 0);

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    lv_obj_t* active_btn_in_picker = nullptr;
    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Create picker button from XML template
        const char* btn_attrs[] = {"tool_text", tools[i].name.c_str(), nullptr};
        lv_obj_t* picker_btn =
            static_cast<lv_obj_t*>(lv_xml_create(tool_list, "tool_picker_button", btn_attrs));
        if (!picker_btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('tool_picker_button') returned NULL");
            continue;
        }

        // Find the actual ui_button — context menu buttons are full width
        lv_obj_t* btn = lv_obj_find_by_name(picker_btn, "tool_btn");
        if (!btn) {
            continue;
        }
        lv_obj_set_width(picker_btn, LV_PCT(100));
        lv_obj_set_width(btn, LV_PCT(100));

        // Active tool: use primary variant styling (let ui_button handle colors)
        if (is_active) {
            active_btn_in_picker = picker_btn;
            // ui_button "ghost" doesn't have a bg — set primary bg directly
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_t* label = lv_obj_find_by_name(picker_btn, "tool_btn_label");
            if (label) {
                lv_obj_set_style_text_color(label, theme_manager_get_color("screen_bg"), 0);
            }
        }

        // Pass tool index via event callback user_data (NOT obj user_data — L069:
        // ui_button already owns obj user_data for its internal button_data_t)
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] picker_tool_click");
                auto* picker = helix::ui::ContextMenu::active_as<ToolPicker>();
                if (!picker)
                    return;
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                ToolSwitcherWidget& owner = picker->owner_;
                picker->hide();
                owner.handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }

    // Scroll the active tool into view inside the capped list.
    if (active_btn_in_picker) {
        lv_obj_update_layout(tool_list);
        lv_obj_scroll_to_view(active_btn_in_picker, LV_ANIM_OFF);
    }

    spdlog::debug("[ToolSwitcher] Picker built with {} tools", tools.size());
}

// ============================================================================
// Tool selection with safety gate
// ============================================================================

AmsError ToolSwitcherWidget::tool_change_refusal() const {
    const auto lifecycle = printer_state_.get_print_lifecycle();
    const bool paused = lifecycle == PrintState::Paused;

    // No backend means a plain Tn / macro path with no firmware macro that could
    // hide a home — the documented argument for passing false here.
    AmsBackend* backend = AmsState::instance().get_backend();
    const bool self_homes = backend && backend->filament_ops_self_home();

    if (!helix::ui::print_blocks_filament_op(lifecycle, self_homes)) {
        return AmsErrorHelper::success();
    }
    // Same copy the backend would have produced had the request reached it, so
    // the pre-guard and the backend refusal never say two different things.
    return AmsErrorHelper::print_active(paused, /*pause_allows_ops=*/!self_homes);
}

void ToolSwitcherWidget::refresh_print_gating() {
    const bool blocked = !tool_change_refusal().success();

    for (lv_obj_t* pill : pill_buttons_) {
        if (!pill)
            continue;
        if (blocked) {
            lv_obj_add_state(pill, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(pill, LV_STATE_DISABLED);
        }
    }

    if (compact_label_) {
        lv_obj_set_style_text_color(compact_label_,
                                    theme_manager_get_color(blocked ? "text_muted" : "text"), 0);
    }
}

void ToolSwitcherWidget::dispatch_tool_change(int tool_index) {
    spdlog::info("[ToolSwitcher] Requesting tool change to T{}", tool_index);

    // A null api is NOT a reason to skip the call: the AMS backend performs the
    // change without one, and request_tool_change() reports "No API connection"
    // through on_error when there is no backend either. The previous
    // `if (api)` guard turned that case into silence too.
    ToolState::instance().request_tool_change(
        tool_index, get_moonraker_api(),
        /*on_success=*/nullptr, [](const std::string& error) {
            NOTIFY_ERROR(lv_tr("Tool change failed: {}"), error);
            // The pills and the compact label are rebuilt from ToolState's
            // active-tool subject, so a refused change never moved the
            // highlight in the first place. Resync anyway: a backend that got
            // partway before failing leaves the subject as the only truth, and
            // this costs one rebuild on an error path.
            helix::ui::async_call(
                [](void*) {
                    if (s_active_instance) {
                        s_active_instance->on_active_tool_changed(
                            ToolState::instance().active_tool_index());
                    }
                },
                nullptr);
        });
}

void ToolSwitcherWidget::handle_tool_selected(int tool_index) {
    auto& tool_state = ToolState::instance();

    // Already on this tool
    if (tool_index == tool_state.active_tool_index()) {
        spdlog::debug("[ToolSwitcher] Tool T{} already active, ignoring", tool_index);
        return;
    }

    // The buttons are greyed by refresh_print_gating(), but a tap can still land
    // in the window between a print starting and the observer firing — and the
    // backend refuses PRINTING unconditionally, so offering the change behind a
    // confirmation modal was offering a dead end. Refuse here with copy the user
    // can act on, exactly as AmsOperationSidebar::handle_unload() does.
    const AmsError refusal = tool_change_refusal();
    if (!refusal.success()) {
        spdlog::info("[ToolSwitcher] Tool change to T{} refused: {}", tool_index,
                     refusal.technical_msg);
        helix::ui::notify_ams_warning(refusal);
        return;
    }

    // Reaching here while PAUSED means the backend permits filament ops on a
    // paused job (everything except AD5X IFS) — pause-then-swap is the runout
    // and colour-change recovery workflow, so the change is offered, with a
    // confirmation because it moves the toolhead into a part still on the bed.
    const auto lifecycle = printer_state_.get_print_lifecycle();
    if (lifecycle == PrintState::Paused) {
        spdlog::info("[ToolSwitcher] Print paused, showing confirmation for T{}", tool_index);

        helix::ui::modal_show_confirmation(
            lv_tr("Change Tool While Paused"),
            lv_tr("The print is paused. Changing tools now moves the toolhead and swaps the "
                  "filament at the nozzle. Resume the print once the change finishes."),
            ::ModalSeverity::Warning, lv_tr("Change Tool"),
            // on_confirm
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] confirm_tool_change");
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                dispatch_tool_change(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            // on_cancel (nullptr = just dismiss)
            nullptr,
            // user_data = tool_index
            reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
        return;
    }

    dispatch_tool_change(tool_index);
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_pill_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_pill_cb");
    if (!s_active_instance)
        return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    s_active_instance->handle_tool_selected(idx);
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_compact_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
