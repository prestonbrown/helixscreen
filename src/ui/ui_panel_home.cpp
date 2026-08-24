// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_home.h"

#include "ui_callback_helpers.h"
#include "ui_carousel.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "first_run_tour.h"
#include "input_settings_manager.h"
#include "lock_manager.h"
#include "observer_factory.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widgets/print_status_widget.h"
#include "panel_widgets/printer_image_widget.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>

using namespace helix;

/// Recursively set EVENT_BUBBLE on all descendants so touch events
/// (long_press, click, etc.) propagate up to the container.
static void set_event_bubble_recursive(lv_obj_t* obj) {
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        set_event_bubble_recursive(child);
    }
}

// disable_widget_clicks_recursive() and clear_pressed_state_recursive() are in ui_utils.h
using helix::ui::clear_pressed_state_recursive;
using helix::ui::disable_widget_clicks_recursive;

HomePanel::HomePanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Subscribe to printer image changes for immediate refresh
    image_changed_observer_ = helix::ui::observe_int_sync<HomePanel>(
        helix::PrinterImageManager::instance().get_image_changed_subject(), this,
        [](HomePanel* self, int /*ver*/) {
            // Clear cache so refresh_printer_image() actually applies the new image
            self->last_printer_image_path_.clear();
            self->refresh_printer_image();
        });
}

HomePanel::~HomePanel() {
    // Deinit subjects FIRST - disconnects observers before subject memory is freed
    deinit_subjects();

    // Gate observers watch external subjects (capabilities, klippy_state) that may
    // already be freed. Clear unconditionally.
    helix::PanelWidgetManager::instance().clear_gate_observers("home");
    helix::PanelWidgetManager::instance().unregister_rebuild_callback("home");

    // Detach all page widget instances
    for (auto& page : page_widgets_) {
        for (auto& w : page) {
            if (w)
                w->detach();
        }
    }
    page_widgets_.clear();
    page_containers_.clear();
    page_visible_ids_.clear();
    carousel_ = nullptr;
    carousel_host_ = nullptr;
    add_page_tile_ = nullptr;
    arrow_left_ = nullptr;
    arrow_right_ = nullptr;
}

void HomePanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Register panel-level event callbacks BEFORE loading XML.
    // Widget-specific callbacks (LED, power, temp, network, fan, macro, etc.)
    // are self-registered by each widget in their attach() method.
    register_xml_callbacks({
        {"ams_clicked_cb", ams_clicked_cb},
        {"on_home_grid_pressed", on_home_grid_pressed},
        {"on_home_grid_long_press", on_home_grid_long_press},
        {"on_home_grid_clicked", on_home_grid_clicked},
        {"on_home_grid_pressing", on_home_grid_pressing},
        {"on_home_grid_released", on_home_grid_released},
    });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "HomePanelSubjects", []() { get_global_home_panel().deinit_subjects(); });

    spdlog::debug("[{}] Registered subjects and event callbacks", get_name());
}

void HomePanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Release gate observers BEFORE subjects are freed
    helix::PanelWidgetManager::instance().clear_gate_observers("home");

    // Disconnect page observer before deiniting the subject
    page_observer_.reset();

    // Clear cached widget IDs so reconnects get a fresh rebuild
    page_visible_ids_.clear();

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Carousel construction and lifecycle
// ============================================================================

void HomePanel::build_carousel() {
    carousel_host_ = lv_obj_find_by_name(panel_, "carousel_host");
    if (!carousel_host_) {
        spdlog::error("[{}] carousel_host not found in XML", get_name());
        return;
    }

    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());
    int main_page = static_cast<int>(config.main_page_index());

    spdlog::debug("[{}] Building carousel: {} pages, main={}", get_name(), num_pages, main_page);

    // Create carousel programmatically inside the host
    carousel_ = ui_carousel_create_obj(carousel_host_);
    if (!carousel_) {
        spdlog::error("[{}] Failed to create carousel", get_name());
        return;
    }

    // Init page subject and connect to carousel
    lv_subject_init_int(&page_subject_, 0);
    subjects_.register_subject(&page_subject_);

    CarouselState* cstate = ui_carousel_get_state(carousel_);
    if (cstate) {
        cstate->page_subject = &page_subject_;
        cstate->wrap = false;
    }

    // Resize vectors for page tracking
    page_widgets_.resize(static_cast<size_t>(num_pages));
    page_containers_.resize(static_cast<size_t>(num_pages), nullptr);
    page_visible_ids_.resize(static_cast<size_t>(num_pages));

    // Add one tile per config page
    for (int i = 0; i < num_pages; ++i) {
        lv_obj_t* container = lv_obj_create(carousel_host_);
        lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(container, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);

        ui_carousel_add_item(carousel_, container);
        page_containers_[static_cast<size_t>(i)] = container;
    }

    // Add "+" tile for adding new pages
    if (num_pages < MAX_PAGES) {
        add_page_tile_ = lv_obj_create(carousel_host_);
        lv_obj_set_size(add_page_tile_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(add_page_tile_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(add_page_tile_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(add_page_tile_, LV_OBJ_FLAG_SCROLLABLE);

        // Plus icon centered in the tile
        lv_obj_t* plus_btn = lv_obj_create(add_page_tile_);
        lv_obj_set_size(plus_btn, 64, 64);
        lv_obj_set_style_radius(plus_btn, 32, LV_PART_MAIN);
        lv_obj_set_style_bg_color(plus_btn, theme_manager_get_color("card_bg"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(plus_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(plus_btn, 0, LV_PART_MAIN);
        lv_obj_align(plus_btn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(plus_btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* plus_label = lv_label_create(plus_btn);
        lv_label_set_text(plus_label, ui_icon::lookup_codepoint("plus"));
        lv_obj_set_style_text_font(plus_label, &mdi_icons_32, LV_PART_MAIN);
        lv_obj_set_style_text_color(plus_label, theme_manager_get_color("secondary"), LV_PART_MAIN);
        lv_obj_align(plus_label, LV_ALIGN_CENTER, 0, 0);

        ui_carousel_add_item(carousel_, add_page_tile_);

        // Click handler on the "+" button (acceptable exception for programmatic creation)
        lv_obj_add_event_cb(
            plus_btn,
            [](lv_event_t* /*e*/) {
                LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_add_page_clicked");
                get_global_home_panel().on_add_page_clicked();
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // Exclude "+" tile from indicator dots
    ui_carousel_set_real_page_count(carousel_, num_pages);

    // Enable event bubbling through the carousel LVGL tree so that
    // long_press/click/pressing/released events from widgets inside page
    // containers propagate up through tile -> scroll -> carousel -> carousel_host_
    // where the edit mode handlers are registered via XML.
    if (cstate) {
        lv_obj_add_flag(carousel_, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (cstate->scroll_container) {
            lv_obj_add_flag(cstate->scroll_container, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        for (auto* tile : cstate->real_tiles) {
            lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    // Also add event bubbling on the page containers themselves
    for (auto* pc : page_containers_) {
        if (pc) {
            lv_obj_add_flag(pc, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    // Create arrow buttons for page navigation
    // Only shown on SDL (test mode) or resistive touchscreens where swipe is unreliable
    // TODO: Add proper resistive touch detection; for now use test_mode as proxy for SDL
    bool show_arrows = get_runtime_config()->test_mode;
    if (show_arrows) {
        auto arrow_size = theme_manager_get_spacing("button_height");
        auto create_arrow = [&](const char* icon_name, lv_align_t align) -> lv_obj_t* {
            lv_obj_t* arrow = lv_obj_create(carousel_host_);
            lv_obj_set_size(arrow, arrow_size, arrow_size);
            lv_obj_set_style_radius(arrow, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(arrow, theme_manager_get_color("card_bg"), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(arrow, LV_OPA_40, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(arrow, LV_OPA_80, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_border_width(arrow, 0, LV_PART_MAIN);
            lv_obj_align(arrow, align, 0, 0);
            lv_obj_add_flag(arrow, LV_OBJ_FLAG_FLOATING);
            lv_obj_add_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(arrow, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(arrow);
            lv_label_set_text(label, ui_icon::lookup_codepoint(icon_name));
            lv_obj_set_style_text_font(label, &mdi_icons_24, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
            return arrow;
        };

        arrow_left_ = create_arrow("chevron_left", LV_ALIGN_LEFT_MID);
        arrow_right_ = create_arrow("chevron_right", LV_ALIGN_RIGHT_MID);
    }

    // Arrow click handlers (only when arrows were created)
    if (arrow_left_) {
        lv_obj_add_event_cb(
            arrow_left_,
            [](lv_event_t* /*e*/) {
                LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] arrow_left_clicked");
                auto& panel = get_global_home_panel();
                if (panel.carousel_) {
                    int cur = ui_carousel_get_current_page(panel.carousel_);
                    if (cur > 0) {
                        ui_carousel_goto_page(panel.carousel_, cur - 1, true);
                    }
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    if (arrow_right_) {
        lv_obj_add_event_cb(
            arrow_right_,
            [](lv_event_t* /*e*/) {
                LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] arrow_right_clicked");
                auto& panel = get_global_home_panel();
                if (panel.carousel_) {
                    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
                    int cur = ui_carousel_get_current_page(panel.carousel_);
                    int max_page = static_cast<int>(config.page_count()) - 1;
                    if (cur < max_page) {
                        ui_carousel_goto_page(panel.carousel_, cur + 1, true);
                    }
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // Observe page subject for page change callbacks.
    // Use immediate (non-deferred) observer because the subject is set from
    // carousel_scroll_end_cb on the UI thread, and the deferred path via
    // observe_int_sync drops the callback (weak_alive expires before the
    // queued lambda executes, causing active_page_index_ desync).
    page_observer_ = helix::ui::observe_int_immediate<HomePanel>(
        &page_subject_, this, [](HomePanel* self, int page) { self->on_page_changed(page); });

    // Navigate to main page
    if (main_page > 0) {
        ui_carousel_goto_page(carousel_, main_page, false);
    }
    active_page_index_ = main_page;

    // Populate all pages, activate only the main page widgets
    for (int i = 0; i < num_pages; ++i) {
        populate_page(i, true);
    }

    update_arrow_visibility(active_page_index_);

    spdlog::debug("[{}] Carousel built with {} pages", get_name(), num_pages);
}

void HomePanel::rebuild_carousel() {
    spdlog::debug("[{}] Rebuilding carousel", get_name());

    int prev_page = active_page_index_;

    // Deactivate current page widgets
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[static_cast<size_t>(active_page_index_)]) {
            if (w)
                w->on_deactivate();
        }
    }

    // Detach all widget instances across all pages
    for (auto& page : page_widgets_) {
        for (auto& w : page) {
            if (w)
                w->detach();
        }
    }
    page_widgets_.clear();
    page_containers_.clear();
    page_visible_ids_.clear();

    // Disconnect page observer before deiniting subject
    page_observer_.reset();

    // Freeze queue, drain deferred callbacks, then async-clean the carousel.
    // safe_clean_children schedules child deletion via lv_obj_delete_async
    // (reparented to lv_layer_top) instead of deleting synchronously inside
    // the outer UpdateQueue batch — which would corrupt LVGL's event list
    // when populate_page runs via the gate observer path (#776 / #834).
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        if (carousel_host_) {
            helix::ui::safe_clean_children(carousel_host_);
        }
    }

    // Null all pointers
    carousel_ = nullptr;
    add_page_tile_ = nullptr;
    arrow_left_ = nullptr;
    arrow_right_ = nullptr;

    // Unregister page subject from SubjectManager before deiniting
    // (SubjectManager tracks it, but we need to re-init with a fresh one)
    subjects_.deinit_all();

    // Re-register subjects (page_subject_ will be re-inited in build_carousel)
    // Note: subjects_initialized_ stays true since init_subjects() registered callbacks

    // Rebuild
    build_carousel();

    // Restore previous page, clamped to valid range
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int max_page = static_cast<int>(config.page_count()) - 1;
    int restored = std::min(prev_page, max_page);
    if (restored > 0 && carousel_) {
        ui_carousel_goto_page(carousel_, restored, false);
    }
}

void HomePanel::populate_page(int page_index, bool force) {
    if (populating_widgets_) {
        spdlog::debug("[{}] populate_page: already in progress, skipping", get_name());
        return;
    }

    if (page_index < 0 || page_index >= static_cast<int>(page_containers_.size())) {
        spdlog::error("[{}] populate_page: page_index {} out of range", get_name(), page_index);
        return;
    }

    populating_widgets_ = true;
    auto idx = static_cast<size_t>(page_index);

    lv_obj_t* container = page_containers_[idx];
    if (!container) {
        spdlog::error("[{}] populate_page: null container for page {}", get_name(), page_index);
        populating_widgets_ = false;
        return;
    }

    // Compute the widget ID list ONCE per populate_page call.  Re-reading gate
    // subjects after populate_widgets (e.g. when caching at line ~510) caused a
    // race: late-arriving capabilities (printer_has_led flipping 0→1 shortly
    // after placement) would be baked into the cache, making the subsequent
    // gate-observer rebuild short-circuit as "unchanged" and leaving the widget
    // permanently in its initial ~gated placeholder.  Snapshotting here ties
    // the cache to the snapshot driving placement.
    auto snapshot_ids =
        helix::PanelWidgetManager::instance().compute_visible_widget_ids("home", page_index);

    // Skip rebuild if the resulting widget list would be identical
    if (!force) {
        if (idx < page_visible_ids_.size() && snapshot_ids == page_visible_ids_[idx]) {
            spdlog::debug("[{}] Page {} widget list unchanged, skipping rebuild", get_name(),
                          page_index);
            populating_widgets_ = false;
            return;
        }
    }

    // Extract reusable widget instances
    helix::WidgetReuseMap reuse;
    if (idx < page_widgets_.size()) {
        for (auto& w : page_widgets_[idx]) {
            if (w) {
                w->detach();
                if (w->supports_reuse()) {
                    reuse[w->id()] = std::move(w);
                }
            }
        }
        // Remove null entries
        page_widgets_[idx].erase(std::remove_if(page_widgets_[idx].begin(),
                                                page_widgets_[idx].end(),
                                                [](const auto& w) { return !w; }),
                                 page_widgets_[idx].end());
    }

    // Flush deferred callbacks, then async-clean the LVGL tree. Gate observers
    // call this via queue_update, so sync lv_obj_clean here would corrupt
    // LVGL's event list when it's batched with sibling deletes (#776 / #834).
    // safe_clean_children reparents each child to lv_layer_top and schedules
    // lv_obj_delete_async, escaping the UpdateQueue batch.
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(container);
        helix::ui::safe_clean_children(container);
    }

    if (idx < page_widgets_.size()) {
        page_widgets_[idx].clear();
    }

    // Populate widgets for this page
    auto widgets = helix::PanelWidgetManager::instance().populate_widgets(
        "home", container, page_index, std::move(reuse));

    // Enable event bubbling for edit mode detection
    set_event_bubble_recursive(container);

    // If edit mode is active, disable clickability
    if (grid_edit_mode_.is_active()) {
        disable_widget_clicks_recursive(container);
    }

    // Activate widgets if this is the active page and panel is active
    if (panel_active_ && page_index == active_page_index_) {
        for (auto& w : widgets) {
            if (w)
                w->on_activate();
        }
    }

    // Store widgets
    if (idx >= page_widgets_.size()) {
        page_widgets_.resize(idx + 1);
    }
    page_widgets_[idx] = std::move(widgets);

    // Cache visible widget IDs — use the snapshot computed at populate_page
    // entry so the cache matches the gate values that drove placement, not a
    // fresh read that could include late-arriving capability flips.
    if (idx >= page_visible_ids_.size()) {
        page_visible_ids_.resize(idx + 1);
    }
    page_visible_ids_[idx] = std::move(snapshot_ids);

    populating_widgets_ = false;
}

void HomePanel::on_page_changed(int new_page) {
    if (new_page == active_page_index_) {
        return;
    }

    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());

    // Ignore page changes to the "+" tile
    if (new_page >= num_pages) {
        return;
    }

    spdlog::debug("[{}] Page changed: {} -> {}", get_name(), active_page_index_, new_page);

    // Deactivate old page widgets
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[static_cast<size_t>(active_page_index_)]) {
            if (w)
                w->on_deactivate();
        }
    }

    active_page_index_ = new_page;

    // Activate new page widgets if panel is active
    if (panel_active_ && new_page >= 0 && new_page < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[static_cast<size_t>(new_page)]) {
            if (w)
                w->on_activate();
        }
    }

    update_arrow_visibility(new_page);
}

void HomePanel::on_add_page_clicked() {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    if (static_cast<int>(config.page_count()) >= MAX_PAGES) {
        spdlog::info("[{}] Max page count reached ({})", get_name(), MAX_PAGES);
        return;
    }

    std::string page_id = config.generate_page_id();
    int new_idx = config.add_page(page_id);
    if (new_idx < 0) {
        spdlog::error("[{}] Failed to add page", get_name());
        return;
    }

    config.save();
    spdlog::info("[{}] Added new page '{}' at index {}", get_name(), page_id, new_idx);

    rebuild_carousel();

    // Animate to the new page
    if (carousel_) {
        ui_carousel_goto_page(carousel_, new_idx, true);
    }
}

void HomePanel::update_arrow_visibility(int page) {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());

    // Hide arrows entirely when there's only one page
    if (num_pages <= 1) {
        if (arrow_left_)
            lv_obj_add_flag(arrow_left_, LV_OBJ_FLAG_HIDDEN);
        if (arrow_right_)
            lv_obj_add_flag(arrow_right_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Left hidden on page 0
    if (arrow_left_) {
        if (page <= 0) {
            lv_obj_add_flag(arrow_left_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(arrow_left_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Right hidden on last real page
    if (arrow_right_) {
        if (page >= num_pages - 1) {
            lv_obj_add_flag(arrow_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(arrow_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void HomePanel::populate_widgets(bool force) {
    // Multi-page path: populate all pages
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());
    for (int i = 0; i < num_pages && i < static_cast<int>(page_containers_.size()); ++i) {
        populate_page(i, force);
    }
}

void HomePanel::setup_widget_gate_observers() {
    auto& mgr = helix::PanelWidgetManager::instance();
    // Gate observer rebuilds are the only path that benefits from the
    // skip-if-unchanged optimization. Config changes and grid edit mode
    // always need a real rebuild (positions/config may change without
    // changing the widget ID list).
    mgr.setup_gate_observers("home", [this]() {
        // Skip gate-triggered rebuilds during edit mode — lv_obj_clean would
        // destroy overlay objects whose pointers GridEditMode still holds.
        if (grid_edit_mode_.is_active()) {
            spdlog::debug("[{}] Skipping gate rebuild during edit mode", get_name());
            return;
        }
        // Skip if any widget on any page has a fullscreen overlay open
        for (const auto& page : page_widgets_) {
            for (const auto& w : page) {
                if (w && w->has_overlay_open()) {
                    spdlog::debug("[{}] Skipping gate rebuild while widget '{}' has overlay open",
                                  get_name(), w->id());
                    return;
                }
            }
        }
        populate_widgets(/*force=*/false);
    });
}

void HomePanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Deliberately minimal: the carousel and widget-config-dependent wiring
    // are deferred to finalize_setup() so they run after the first-run wizard
    // completes. On a fresh install the user configures Moonraker inside the
    // wizard, so AMS (and other hardware) hasn't been detected yet when
    // panels are first set up — building the default layout now would persist
    // a non-AMS grid even on AMS-equipped printers.
    spdlog::debug("[{}] Setup (awaiting finalize)", get_name());
}

void HomePanel::finalize_setup() {
    if (finalized_) {
        return;
    }
    if (!panel_) {
        spdlog::warn("[{}] finalize_setup called before setup", get_name());
        return;
    }
    finalized_ = true;

    spdlog::debug("[{}] Finalizing setup (carousel + widgets)", get_name());

    // Build carousel with pages from config
    build_carousel();

    // Observe hardware gate subjects so widgets appear/disappear when
    // capabilities change (e.g. power devices discovered after startup).
    setup_widget_gate_observers();

    // Register rebuild callback so settings overlay toggle changes take effect immediately
    helix::PanelWidgetManager::instance().register_rebuild_callback("home", [this]() {
        if (grid_edit_mode_.is_active()) {
            spdlog::debug("[{}] Skipping settings rebuild during edit mode", get_name());
            return;
        }
        populate_widgets();
    });

    // Set grid edit mode rebuild callback once (used when edit mode rearranges widgets)
    grid_edit_mode_.set_rebuild_callback([this]() { populate_widgets(); });

    // Set delete page callback for edit mode
    grid_edit_mode_.set_delete_page_callback([this]() {
        helix::ui::modal_show_confirmation(
            "Delete Page", "Remove this page and all its widgets?", ModalSeverity::Warning,
            "Delete",
            [](lv_event_t* ev) {
                LV_UNUSED(ev);
                auto& panel = get_global_home_panel();
                int page_to_delete = panel.grid_edit_mode_.page_index();
                auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
                config.remove_page(static_cast<size_t>(page_to_delete));
                config.save();
                panel.grid_edit_mode_.exit();
                panel.rebuild_carousel();
            },
            nullptr, nullptr);
    });

    spdlog::debug("[{}] Finalize complete", get_name());
}

void HomePanel::repopulate() {
    // setup() only stores the new panel pointer — the carousel, the widget grid
    // and every PanelWidget attachment are built by finalize_setup(). Its
    // one-shot guard already fired during startup, so clear it and re-run the
    // pass against the new tree. Without this the rebuilt panel renders empty
    // and the recycled PanelWidget instances keep raw lv_obj_t* pointers into
    // the old tree, which crashes as soon as an observer fires on them.
    finalized_ = false;
    finalize_setup();
}

void HomePanel::on_activate() {
    panel_active_ = true;

    // Notify only the active page's widgets that the panel is visible
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[static_cast<size_t>(active_page_index_)]) {
            if (w)
                w->on_activate();
        }
    }

    // Start Spoolman polling for AMS mini status updates
    SpoolmanManager::instance().start_spoolman_polling();

    // First-run tour — respects gate (wizard complete + tour not yet completed at version).
    helix::tour::FirstRunTour::instance().maybe_start();
}

void HomePanel::on_deactivate() {
    panel_active_ = false;
    // Exit grid edit mode if active, UNLESS the widget catalog overlay is open
    // (push_overlay triggers on_deactivate, but edit mode must survive)
    if (grid_edit_mode_.is_active() && !grid_edit_mode_.is_catalog_open()) {
        grid_edit_mode_.exit();
    }

    // Notify only the active page's widgets that the panel is going offscreen
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[static_cast<size_t>(active_page_index_)]) {
            if (w)
                w->on_deactivate();
        }
    }

    SpoolmanManager::instance().stop_spoolman_polling();
}

void HomePanel::apply_printer_config() {
    // Widgets use version observers for auto-binding (LED, power, etc.)
    // Just refresh the printer image (delegated to PrinterImageWidget)
    refresh_printer_image();
}

void HomePanel::refresh_printer_image() {
    // Search all pages for the PrinterImageWidget.
    //
    // id() is the widget's factory-registration key, and the registry is a
    // one-to-one map: an id names exactly one concrete PanelWidget subclass.
    // Matching on it and then static_cast'ing is therefore equivalent to the
    // dynamic_cast this replaces, and works under -fno-rtti (firmware).
    for (auto& page : page_widgets_) {
        for (auto& w : page) {
            if (w && std::strcmp(w->id(), helix::PrinterImageWidget::WIDGET_ID) == 0) {
                static_cast<helix::PrinterImageWidget*>(w.get())->refresh_printer_image();
                return;
            }
        }
    }
}

void HomePanel::trigger_idle_runout_check() {
    // Search all pages for the PrintStatusWidget (see refresh_printer_image()
    // for why the id() match stands in for a dynamic_cast).
    for (auto& page : page_widgets_) {
        for (auto& w : page) {
            if (w && std::strcmp(w->id(), helix::PrintStatusWidget::WIDGET_ID) == 0) {
                static_cast<helix::PrintStatusWidget*>(w.get())->trigger_idle_runout_check();
                return;
            }
        }
    }
    spdlog::debug("[{}] PrintStatusWidget not active - skipping runout check", get_name());
}

// ============================================================================
// Panel-level click handlers
// ============================================================================

void HomePanel::handle_ams_clicked() {
    spdlog::info("[{}] AMS indicator clicked - opening AMS panel overlay", get_name());

    auto& ams_panel = get_global_ams_panel();
    if (!ams_panel.are_subjects_initialized()) {
        ams_panel.init_subjects();
    }
    lv_obj_t* panel_obj = ams_panel.get_panel();
    if (panel_obj) {
        // Re-register before push: get_global_ams_panel() only registers the
        // overlay instance inside its lazy-creation block, and
        // switch_to_panel_impl() clears overlay_instances_ on navbar switches
        // (keeping only the persistent map). A cached panel re-opened after a
        // navbar tap therefore loses its registration, so on_deactivate() never
        // fires on dismiss and the filament-path animation keeps drawing into a
        // torn-down panel. Idempotent (keyed by widget). Mirrors
        // AmsOverviewPanel and lazy_create_and_push_overlay.
        NavigationManager::instance().register_overlay_instance(panel_obj, &ams_panel);
        NavigationManager::instance().push_overlay(panel_obj);
    }
}

// ============================================================================
// Static callback trampolines
// ============================================================================

void HomePanel::ams_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] ams_clicked_cb");
    (void)e;
    get_global_home_panel().handle_ams_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

/// Returns true if the active input device is interacting with a widget that
/// consumes drag gestures — either scrolling (e.g., swiping a carousel) or
/// dragging an arc/slider knob (e.g., adjusting fan speed) — or if the screen
/// is locked. LVGL fires LONG_PRESSED based purely on hold duration, regardless
/// of finger movement, so we must check for these interactions to prevent false
/// edit mode entry.
static bool should_suppress_edit_mode(lv_event_t* e) {
    // Global kill-switch: when the user has disabled home-screen edit mode
    // (Touch & Input settings), no long-press enters it (#1245).
    if (!helix::InputSettingsManager::instance().get_home_edit_mode_enabled())
        return true;

    // A hold that reaches the grid while the lock screen is up was never a
    // request to rearrange widgets — the panel is not even the thing the user
    // is looking at. Waking an Android device with a resting finger used to
    // deliver exactly that, so edit mode activated underneath the PIN pad and
    // was found once the PIN cleared (#1245).
    if (helix::LockManager::instance().is_locked())
        return true;

    lv_indev_t* indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev))
        return true;

    // Check if the original press target (before event bubbling) is an arc or
    // slider — these widgets consume drag gestures for value adjustment, so a
    // long hold on them should never trigger edit mode.
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (!target)
        return false;
    lv_obj_t* current = lv_event_get_current_target_obj(e);
    while (target) {
        if (lv_obj_has_class(target, &lv_arc_class) || lv_obj_has_class(target, &lv_slider_class))
            return true;
        // Stop at the container that owns the event handler
        if (target == current)
            break;
        target = lv_obj_get_parent(target);
    }

    return false;
}

bool HomePanel::finger_drifted_since_press() const {
    if (!press_point_valid_)
        return false;
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return false;
    lv_point_t now;
    lv_indev_get_point(indev, &now);
    const int dx = now.x - press_start_point_.x;
    const int dy = now.y - press_start_point_.y;
    const int limit = lv_dpx(AppConstants::Input::EDIT_MODE_MOVE_CANCEL_DPX);
    return (dx * dx + dy * dy) > (limit * limit);
}

void HomePanel::on_home_grid_pressed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_pressed");
    (void)e;
    auto& panel = get_global_home_panel();
    lv_indev_t* indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &panel.press_start_point_);
        panel.press_point_valid_ = true;
    } else {
        panel.press_point_valid_ = false;
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_long_press(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_long_press");
    if (!should_suppress_edit_mode(e)) {
        auto& panel = get_global_home_panel();
        if (!panel.grid_edit_mode_.is_active()) {
            // Entering edit mode requires a deliberate, stationary hold. LVGL
            // fires LONG_PRESSED on hold duration alone, so a press that drifted
            // from its landing point is an accidental rest, not a hold — ignore it.
            if (panel.finger_drifted_since_press()) {
                return; // inside the SAFE_EVENT_CB try block; END closes it below
            }
            // Cancel the in-progress press to prevent the widget's click
            // action from firing on release.
            lv_indev_t* indev = lv_indev_active();
            if (indev)
                lv_indev_reset(indev, nullptr);

            // Clear PRESSED state from active page container
            lv_obj_t* container = nullptr;
            if (panel.active_page_index_ >= 0 &&
                panel.active_page_index_ < static_cast<int>(panel.page_containers_.size())) {
                container = panel.page_containers_[static_cast<size_t>(panel.active_page_index_)];
            }
            if (container) {
                clear_pressed_state_recursive(container);
            }

            // Enter edit mode on the active page container
            auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
            if (container) {
                panel.grid_edit_mode_.enter(container, &config, panel.active_page_index_);
                // Disable carousel swiping during edit mode
                if (panel.carousel_) {
                    ui_carousel_set_scroll_enabled(panel.carousel_, false);
                }
                // Select the widget under the finger and start dragging immediately.
                panel.grid_edit_mode_.handle_click(e);
                if (panel.grid_edit_mode_.selected_widget()) {
                    panel.grid_edit_mode_.handle_drag_start(e);
                }
            }
        } else {
            // Already in edit mode — start drag if a widget is selected
            panel.grid_edit_mode_.handle_long_press(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_clicked");
    auto& panel = get_global_home_panel();
    if (panel.grid_edit_mode_.is_active()) {
        panel.grid_edit_mode_.handle_click(e);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_pressing(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_pressing");
    if (!should_suppress_edit_mode(e)) {
        auto& panel = get_global_home_panel();
        if (panel.grid_edit_mode_.is_active()) {
            panel.grid_edit_mode_.handle_pressing(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_released(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_released");
    auto& panel = get_global_home_panel();
    panel.press_point_valid_ = false; // press cycle ended — stop drift tracking
    if (!should_suppress_edit_mode(e)) {
        if (panel.grid_edit_mode_.is_active()) {
            panel.grid_edit_mode_.handle_released(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::go_to_main_page() {
    if (!carousel_ || grid_edit_mode_.is_active())
        return;
    int current = ui_carousel_get_current_page(carousel_);
    if (current != 0) {
        spdlog::debug("[HomePanel] Navigating carousel to main page (page 0)");
        ui_carousel_goto_page(carousel_, 0, true);
    }
}

void HomePanel::exit_grid_edit_mode() {
    if (grid_edit_mode_.is_active()) {
        grid_edit_mode_.exit();
        // Re-enable carousel swiping after edit mode
        if (carousel_) {
            ui_carousel_set_scroll_enabled(carousel_, true);
        }
    }
}

void HomePanel::open_widget_catalog() {
    if (grid_edit_mode_.is_active() && parent_screen_) {
        grid_edit_mode_.open_widget_catalog(parent_screen_);
    }
}

// ============================================================================
// Global instance
// ============================================================================

static std::unique_ptr<HomePanel> g_home_panel;

HomePanel& get_global_home_panel() {
    if (!g_home_panel) {
        g_home_panel = std::make_unique<HomePanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("HomePanel",
                                                         []() { g_home_panel.reset(); });
    }
    return *g_home_panel;
}
