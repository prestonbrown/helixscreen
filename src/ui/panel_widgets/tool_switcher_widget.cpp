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
#include "helix/ui/text_metrics.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

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
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : TiledPanelWidget("tool_switcher", TileSizing::Content{"", "", "", true}),
      printer_state_(printer_state) {
    // Registered before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    UI_MANAGED_SUBJECT_INT(compact_subject_, 0, "tool_switcher_compact", subjects_);
    UI_MANAGED_SUBJECT_STRING(active_label_subject_, active_label_buf_, "",
                              "tool_switcher_active_label", subjects_);
    refresh_label_budget();
}

void ToolSwitcherWidget::refresh_label_budget() {
    std::string widest;
    for (const auto& tool : ToolState::instance().tools()) {
        if (tool.display_label.size() > widest.size()) {
            widest = tool.display_label;
        }
    }
    sizing_.set_content({widest, widest, "", true});
}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

bool ToolSwitcherWidget::is_compact_size() const {
    return is_compact_at(current_width_px_, current_height_px_);
}

// Compact when the box is small on both axes; the pills need room on one.
ToolSwitcherWidget::PillGrid ToolSwitcherWidget::pill_grid_at(int width_px, int height_px) {
    const auto& tools = ToolState::instance().tools();
    const int count = static_cast<int>(tools.size());
    if (count == 0) {
        return {};
    }
    // A pill is legible when its label fits on one line inside the button's
    // padding, in the face a pill label inherits: the screen's base font.
    const lv_font_t* face = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    int label_w = 0;
    for (const auto& tool : tools) {
        label_w = std::max<int>(label_w, ui::text_width(tool.display_label.c_str(), face));
    }
    // The 2px on each axis is the button's border.
    const int pill_min_w = label_w + 2 * resolve_space_token("space_sm", 8) + 2;
    const int pill_min_h = std::max(resolve_space_token("space_xl", 24),
                                    static_cast<int>(face ? lv_font_get_line_height(face) : 0) +
                                        2 * resolve_space_token("space_xxs", 4) + 2);
    const int gap = resolve_space_token("space_xs", 4);
    const int inset = resolve_space_token("space_md", 10); // tool_switcher_container's padding
    const int avail_w = width_px - 2 * inset;
    const int avail_h = height_px - 2 * inset;

    // Of the arrangements whose equal cells each hold a legible pill, the one
    // with the squarest cells: a row in a wide box, a column in a tall one, a
    // balanced grid in a square one.
    PillGrid best;
    int best_short_side = 0;
    for (int rows = 1; rows <= count; ++rows) {
        const int cols = (count + rows - 1) / rows;
        const int cell_w = (avail_w - (cols - 1) * gap) / cols;
        const int cell_h = (avail_h - (rows - 1) * gap) / rows;
        if (cell_w >= pill_min_w && cell_h >= pill_min_h &&
            std::min(cell_w, cell_h) > best_short_side) {
            best = {cols, rows};
            best_short_side = std::min(cell_w, cell_h);
        }
    }
    return best;
}

bool ToolSwitcherWidget::is_compact_at(int width_px, int height_px) {
    return !pill_grid_at(width_px, height_px).fits();
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    install_delete_hook(widget_obj);
    s_active_instance = this;

    pill_container_ = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");

    auto& tool_state = ToolState::instance();
    auto token = lifetime_.token();

    // Observe active tool changes
    active_tool_observer_ = helix::ui::observe<int>(
        tool_state.get_active_tool_subject(), this,
        [token](ToolSwitcherWidget* self, int tool) {
            if (token.expired())
                return;
            self->on_active_tool_changed(tool);
        },
        tool_state.get_subjects_lifetime());

    // Observe tool count changes to trigger rebuild
    tool_count_observer_ = helix::ui::observe<int>(
        tool_state.get_tool_count_subject(), this,
        [token](ToolSwitcherWidget* self, int /*count*/) {
            if (token.expired())
                return;
            // New tools change the widest label the tile is budgeted for, so
            // re-measure at the size it holds before rebuilding either form.
            self->on_size_changed(0, 0, self->current_width_px_, self->current_height_px_);
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
    print_state_observer_ = helix::ui::observe<int>(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [token](ToolSwitcherWidget* self, int /*state*/) {
            if (token.expired())
                return;
            self->refresh_print_gating();
        },
        printer_state_.print_state().get_static_subjects_lifetime());

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
    uninstall_delete_hook();
    forget_tile_widgets();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the pending deferred observer
    // callbacks and drop the cached pointers only. The observers themselves
    // stay registered on their (still live) subjects until detach() or the
    // destructor resets them — every callback checks its token first, so a
    // drained refresh_print_gating() or rebuild no-ops instead of running
    // lv_obj_add_state()/lv_obj_find_by_name() over the freed tree.
    lifetime_.invalidate();
    forget_tile_widgets();
}

void ToolSwitcherWidget::forget_tile_widgets() {
    pill_buttons_.clear();
    compact_label_ = nullptr;
    if (pill_container_ && lv_is_initialized()) {
        // #983 shape: lv_obj_set_grid_dsc_array() stores the descriptor pointers
        // without copying, so a condemned container still in LV_LAYOUT_GRID keeps
        // reading grid_col_dsc_/grid_row_dsc_ after a recycled instance's next
        // rebuild_pills() .assign() frees the old buffer (safe_clean_children
        // reparents the tile to lv_layer_top and deletes it async, leaving exactly
        // that cross-attach window). Stripping the layout as the pointer is
        // dropped makes a condemned container structurally unable to read the
        // descriptors again — the same mitigation PanelWidgetManager applies to
        // the page container. It belongs HERE rather than in detach(): the
        // raw-delete path (on_hooked_root_deleted) reaches this function without
        // a detach() of its own, and PanelWidgetManager::populate_page()'s
        // safe_clean_children() has no detach either — it relies entirely on its
        // caller. rebuild_pills() re-establishes the grid when it rebuilds one.
        lv_obj_set_layout(pill_container_, LV_LAYOUT_NONE);
    }
    pill_container_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolSwitcherWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                         int height_px) {
    current_width_px_ = width_px;
    current_height_px_ = height_px;

    refresh_label_budget();
    sizing_.measure_and_publish(width_px, height_px);

    if (!widget_obj_)
        return;

    if (is_compact_size()) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}

// ============================================================================
// Pill buttons (inline mode for 1x2, 2x1, 2x2, etc.)
// ============================================================================

void ToolSwitcherWidget::rebuild_pills() {
    // Drop the cached pills before anything below can early-return. If the
    // container lookup fails while widget_obj_ is still set, the list would
    // keep pointers to widgets a previous rebuild already condemned, and
    // refresh_print_gating() would run unchecked lv_obj_add_state()/
    // lv_obj_remove_state() over them.
    pill_buttons_.clear();
    compact_label_ = nullptr;

    if (!widget_obj_)
        return;
    lv_subject_set_int(&compact_subject_, 0);

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for pill rebuild");
        return;
    }

    helix::ui::safe_clean_children(container);

    // Neutralize any grid layout left active by a previous rebuild before we
    // repopulate. safe_clean_children() defers child deletion, so the old pills
    // are briefly still attached; with the grid still active, any interleaved
    // refresh would run grid item_repos over them. The grid is re-activated only after every new
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

    const int space_xs = resolve_space_token("space_xs", 4);
    const int btn_min_h = resolve_space_token("space_xl", 24);

    // An even grid of equal pills from the granted size: every pill fills its
    // cell's width and as much of its height as a large button takes, and the
    // grid sits centred in the tile.
    const PillGrid grid = pill_grid_at(current_width_px_, current_height_px_);
    const int cols = std::max(grid.cols, 1);
    const int rows = std::max(grid.rows, 1);
    const int inset = resolve_space_token("space_md", 10);
    const int cell_h = (current_height_px_ - 2 * inset - (rows - 1) * space_xs) / rows;
    const int pill_h = std::clamp(cell_h, btn_min_h,
                                  std::max(btn_min_h, resolve_space_token("button_height_lg", 56)));
    grid_col_dsc_.assign(static_cast<size_t>(cols), LV_GRID_FR(1));
    grid_col_dsc_.push_back(LV_GRID_TEMPLATE_LAST);
    grid_row_dsc_.assign(static_cast<size_t>(rows), LV_GRID_CONTENT);
    grid_row_dsc_.push_back(LV_GRID_TEMPLATE_LAST);
    lv_obj_set_style_grid_row_align(container, LV_GRID_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(container, space_xs, 0);

    for (size_t i = 0; i < tools.size(); ++i) {
        bool is_active = (static_cast<int>(i) == active);

        // Equal buttons, the active one highlighted; the variant carries the look
        const char* variant = is_active ? "primary" : "secondary";
        const std::string name = "tool_pill_" + std::to_string(i);
        const char* attrs[] = {"name",  name.c_str(), "variant",
                               variant, "text",       tools[i].display_label.c_str(),
                               nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(container, "ui_button", attrs));
        if (!btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('ui_button') returned NULL for pill '{}'",
                          tools[i].name);
            continue;
        }

        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, static_cast<int>(i) % cols, 1,
                             LV_GRID_ALIGN_CENTER, static_cast<int>(i) / cols, 1);
        lv_obj_set_height(btn, pill_h);
        lv_obj_set_style_radius(btn, pill_h / 2, 0);
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
    lv_obj_set_grid_dsc_array(container, grid_col_dsc_.data(), grid_row_dsc_.data());
    lv_obj_set_layout(container, LV_LAYOUT_GRID);

    // Scroll the active pill into view when the container overflows.
    if (active >= 0 && active < static_cast<int>(pill_buttons_.size())) {
        lv_obj_scroll_to_view(pill_buttons_[active], LV_ANIM_OFF);
    }

    // Freshly created pills carry no state — apply the print gate to them here
    // as well as from the observer, or a rebuild silently re-enables them.
    refresh_print_gating();

    spdlog::debug("[ToolSwitcher] Built {} pill buttons, active={}, {} cols x {} rows",
                  tools.size(), active, cols, rows);
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
    // Drop the cached pills/label before anything below can early-return, for
    // the same reason as rebuild_pills(): a failed container lookup must not
    // leave compact_label_ pointing at the previous build's (condemned) label,
    // which refresh_print_gating() would then restyle.
    pill_buttons_.clear();
    compact_label_ = nullptr;

    if (!widget_obj_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for compact rebuild");
        return;
    }

    helix::ui::safe_clean_children(container);
    lv_subject_set_int(&compact_subject_, 1);

    auto& tool_state = ToolState::instance();
    int active = tool_state.active_tool_index();
    const auto& tools = tool_state.tools();

    // An out-of-range active index has no position to name. "?" keeps the one
    // line this mode carries visibly occupied, where an empty label reads as a
    // widget that failed to draw.
    std::string tool_name = (active >= 0 && active < static_cast<int>(tools.size()))
                                ? tools[active].display_label
                                : "?";
    lv_subject_copy_string(&active_label_subject_, tool_name.c_str());
    compact_label_ = lv_obj_find_by_name(widget_obj_, "tool_switcher_compact_label");

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
        const char* btn_attrs[] = {"tool_text", tools[i].display_label.c_str(), nullptr};
        lv_obj_t* picker_btn =
            static_cast<lv_obj_t*>(lv_xml_create(tool_list, "tool_picker_button", btn_attrs));
        if (!picker_btn) {
            spdlog::error("[ToolSwitcher] lv_xml_create('tool_picker_button') returned NULL");
            continue;
        }

        // Find the actual ui_button — context menu buttons are full width
        lv_obj_t* btn = helix::ui::find_required(picker_btn, "tool_btn", "ToolSwitcher");
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
            lv_obj_t* label =
                helix::ui::find_required(picker_btn, "tool_btn_label", "ToolSwitcher");
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
    const auto lifecycle = printer_state_.print_state().get_print_lifecycle();
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
            helix::ui::queue_update("ToolSwitcherWidget::resync_after_error", []() {
                if (s_active_instance) {
                    s_active_instance->on_active_tool_changed(
                        ToolState::instance().active_tool_index());
                }
            });
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
    const auto lifecycle = printer_state_.print_state().get_print_lifecycle();
    if (lifecycle == PrintState::Paused) {
        spdlog::info("[ToolSwitcher] Print paused, showing confirmation for T{}", tool_index);

        helix::ui::modal_confirm(
            lv_tr("Change Tool While Paused"),
            lv_tr("The print is paused. Changing tools now moves the toolhead and swaps the "
                  "filament at the nozzle. Resume the print once the change finishes."),
            ::ModalSeverity::Warning, lv_tr("Change Tool"),
            // dispatch_tool_change() is static, so the capture is the tool index
            // by value - nothing here touches the widget instance.
            [tool_index] { dispatch_tool_change(tool_index); });
        return;
    }

    dispatch_tool_change(tool_index);
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_compact_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
