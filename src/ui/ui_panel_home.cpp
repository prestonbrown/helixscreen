// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_home.h"

#include "ui_callback_helpers.h"
#include "ui_carousel.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_next_tick.h"
#include "ui_panel_ams.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "display_manager.h"
#include "first_run_tour.h"
#include "input_settings_manager.h"
#include "lock_manager.h"
#include "observer_factory.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widgets/print_status_widget.h"
#include "panel_widgets/printer_image_widget.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

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

/// A home carousel page container (ui_xml/components/home_page_container.xml) in @p parent.
static lv_obj_t* create_page_container(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "home_page_container", nullptr));
}

// disable_widget_clicks_recursive() and clear_pressed_state_recursive() are in ui_utils.h
using helix::ui::clear_pressed_state_recursive;
using helix::ui::disable_widget_clicks_recursive;

HomePanel::HomePanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Subscribe to printer image changes for immediate refresh
    image_changed_observer_ = helix::ui::observe<int>(
        helix::PrinterImageManager::instance().get_image_changed_subject(), this,
        [](HomePanel* self, int /*ver*/) {
            // Clear cache so refresh_printer_image() actually applies the new image
            self->last_printer_image_path_.clear();
            self->refresh_printer_image();
        },
        helix::PrinterImageManager::instance().get_subjects_lifetime());

    // Wired at construction: edit mode's gesture transitions can fire before
    // finalize_setup() runs, and every one of them must reach the swipe policy.
    grid_edit_mode_.set_gesture_ownership_callback([this]() { apply_edit_swipe_policy(); });
    grid_edit_mode_.set_show_page_callback([this](int page) { show_edit_page(page); });
    // Read at call time: a carousel rebuilt at the page cap has no slot.
    grid_edit_mode_.set_next_page_slot_callback(
        [this]() { return next_page_container_ != nullptr; });
    // Every page sits in the carousel viewport once the carousel is at rest.
    // Read at call time, so a rebuilt carousel is the one measured.
    grid_edit_mode_.set_page_frame_callback([this](lv_area_t& frame) {
        CarouselState* cstate = carousel_ ? ui_carousel_get_state(carousel_) : nullptr;
        if (!cstate || !cstate->scroll_container) {
            return false;
        }
        lv_obj_get_content_coords(cstate->scroll_container, &frame);
        return true;
    });
    grid_edit_mode_.set_pages_changed_callback(
        [this](const helix::PageSetChange& change) { on_edit_pages_changed(change); });

    init_panel_subjects();
}

HomePanel::~HomePanel() {
    // Deinit subjects FIRST - disconnects observers before subject memory is freed
    deinit_subjects();

    // Gate observers watch external subjects (capabilities, klippy_state) that may
    // already be freed. Clear unconditionally.
    helix::PanelWidgetManager::clear_gate_observers("home");
    helix::PanelWidgetManager::unregister_rebuild_callback("home");

    // Detach all page widget instances
    for (auto& page : pages_) {
        for (auto& w : page.widgets) {
            if (w)
                w->detach_tile();
        }
    }
    pages_.clear();
    carousel_ = nullptr;
    carousel_host_ = nullptr;
    next_page_container_ = nullptr;
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
        {"on_home_grid_pressed", on_home_grid_pressed},
        {"on_home_grid_long_press", on_home_grid_long_press},
        {"on_home_grid_clicked", on_home_grid_clicked},
        {"on_home_grid_pressing", on_home_grid_pressing},
        {"on_home_grid_released", on_home_grid_released},
        {"on_home_grid_press_cancelled", on_home_grid_press_cancelled},
        {"on_add_page_clicked", [](lv_event_t*) { get_global_home_panel().add_page_from_slot(); }},
    });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "HomePanelSubjects", []() { get_global_home_panel().deinit_subjects(); });

    spdlog::debug("[{}] Registered subjects and event callbacks", get_name());
}

void HomePanel::deinit_subjects() {
    // Panel subjects are construction-registered in their own manager, so
    // they must be withdrawn here even when init_subjects() never ran. RAII
    // withdraws the XML names and deinits the subjects; idempotent.
    panel_subjects_.deinit_all();

    if (!subjects_initialized_) {
        return;
    }
    // Release gate observers BEFORE subjects are freed
    helix::PanelWidgetManager::clear_gate_observers("home");

    // Disconnect page observer before deiniting the subject
    page_observer_.reset();

    // Clear cached widget IDs so reconnects get a fresh rebuild
    for (auto& page : pages_) {
        page.visible_ids.reset();
    }

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Carousel construction and lifecycle
// ============================================================================

void HomePanel::init_panel_subjects() {
    UI_MANAGED_SUBJECT_STRING(page_badge_subject_, page_badge_buf_, "", "home_page_badge",
                              panel_subjects_);
    UI_MANAGED_SUBJECT_INT(populated_pages_subject_, 0, "home_populated_pages", panel_subjects_);
}

void HomePanel::update_page_badge() {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    // Format into a local first: lv_subject_copy_string copies into the
    // subject's own buffer, and copying that buffer onto itself is not defined.
    char text[sizeof(page_badge_buf_)];
    snprintf(text, sizeof(text), "%d / %d", active_page_index_ + 1,
             static_cast<int>(config.page_count()));
    lv_subject_copy_string(&page_badge_subject_, text);
}

void HomePanel::update_populated_pages_subject() {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int populated = 0;
    for (size_t page = 0; page < config.page_count(); ++page) {
        if (config.page_is_populated(page)) {
            ++populated;
        }
    }
    lv_subject_set_int(&populated_pages_subject_, populated);
}

lv_obj_t* HomePanel::edit_container(int page) const {
    if (page >= 0 && page < static_cast<int>(pages_.size())) {
        return pages_[static_cast<size_t>(page)].container;
    }
    if (page == static_cast<int>(pages_.size())) {
        return next_page_container_;
    }
    return nullptr;
}

void HomePanel::show_edit_page(int page) {
    lv_obj_t* container = carousel_ ? edit_container(page) : nullptr;
    if (!container) {
        spdlog::debug("[{}] Show edit page {}: no page there", get_name(), page);
        return;
    }
    const bool slot = container == next_page_container_;
    // Animated like a swipe: the edit session measures its cross-page rules
    // against the page frame, not the sliding container. The slot's tile sits
    // past the pages ui_carousel_goto_page() clamps to.
    if (slot) {
        helix::ui::carousel_goto_tile(carousel_, page, /*animate=*/true);
    } else {
        ui_carousel_goto_page(carousel_, page, /*animate=*/true);
    }
    // The goto set the page subject, whose observer re-scopes only on a change
    // to a config page other than the active one: the slot, and a return to
    // the active page, are re-scoped here.
    rescope_edit_page(page);
    spdlog::trace("[{}] Show edit page {} (slot={})", get_name(), page, slot);
}

void HomePanel::on_edit_pages_changed(const helix::PageSetChange& change) {
    // The carousel has not been rebuilt, so its page is numbered as the change
    // is, the next-page slot's tile being page_count.
    const helix::PageSetLanding landing =
        helix::page_set_landing(change, ui_carousel_get_current_page(carousel_));
    spdlog::debug("[{}] Edit pages changed; rebuilding carousel on page {}, focusing page {}",
                  get_name(), landing.shown, landing.focus);
    rebuild_carousel(landing.shown);
    if (pages_.empty()) {
        // No carousel host means no rebuild happened (config is still
        // correct); there is no page to show.
        return;
    }
    // Animated, so a focus on another page is the one slide from the page that
    // was on screen; a carousel already on the focus does not move.
    show_edit_page(landing.focus);
}

void HomePanel::rescope_edit_page(int page) {
    if (!grid_edit_mode_.is_active()) {
        return;
    }
    lv_obj_t* container = edit_container(page);
    if (!container) {
        return;
    }
    grid_edit_mode_.switch_page(container, page);
}

void HomePanel::apply_edit_swipe_policy() {
    if (!carousel_) {
        return;
    }
    using helix::ui::CarouselSwipe;
    // A gesture or the open widget catalog holds the page; otherwise the pages
    // swipe by their count, in edit mode as out of it.
    const bool edit_holds_page =
        grid_edit_mode_.owns_gesture() || grid_edit_mode_.is_catalog_open();
    helix::ui::carousel_set_swipe(carousel_,
                                  edit_holds_page ? CarouselSwipe::Disabled : CarouselSwipe::Auto);
    // The next-page slot carries the + that adds a page and stays a drag's drop
    // target, so its tile is within reach whenever the slot exists. A drop that
    // creates a page leaves the session scoped to it, and the carousel resting
    // on it, until the page-set rebuild on the next tick.
    helix::ui::carousel_set_trailing_tiles_reachable(carousel_, next_page_container_ != nullptr);
}

void HomePanel::build_carousel(int initial_page) {
    carousel_host_ = helix::ui::find_required(panel_, "carousel_host", get_name());
    if (!carousel_host_) {
        return;
    }

    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());
    const int shown = std::clamp(initial_page, 0, std::max(num_pages - 1, 0));

    spdlog::debug("[{}] Building carousel: {} pages, showing page {}", get_name(), num_pages,
                  shown);

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

    pages_.resize(static_cast<size_t>(num_pages));

    // One tile per config page
    for (int i = 0; i < num_pages; ++i) {
        lv_obj_t* container = create_page_container(carousel_host_);
        if (!container) {
            spdlog::error("[{}] Failed to create the container for page {}", get_name(), i);
            continue;
        }
        ui_carousel_add_item(carousel_, container);
        pages_[static_cast<size_t>(i)].container = container;
    }

    // The next-page slot past the last page, below the page cap
    if (config.can_add_page()) {
        lv_obj_t* slot =
            static_cast<lv_obj_t*>(lv_xml_create(carousel_host_, "home_next_page_slot", nullptr));
        if (slot) {
            ui_carousel_add_item(carousel_, slot);
            next_page_container_ =
                helix::ui::find_required(slot, "next_page_container", get_name());
        } else {
            spdlog::error("[{}] Failed to create the next-page slot", get_name());
        }
    }

    // The indicator dots count config pages, not the slot
    ui_carousel_set_real_page_count(carousel_, num_pages);
    // The swipe and the slot's reach for the edit state this carousel is built in
    apply_edit_swipe_policy();

    // Events from widgets inside page containers, which bubble by their
    // component, propagate up through tile -> scroll -> carousel ->
    // carousel_host_, where the edit mode handlers are registered via XML. The
    // carousel keeps this across its own page-count changes.
    helix::ui::carousel_set_bubble_events(carousel_, true);

    // Create arrow buttons for page navigation. Shown where a swipe is
    // unreliable: a mouse-driven SDL window (test mode) and resistive panels,
    // which are exactly the input devices the display backend classifies as
    // needing affine calibration.
    auto* display = DisplayManager::instance();
    bool show_arrows = get_runtime_config()->test_mode ||
                       (display != nullptr && display->needs_touch_calibration());
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
            lv_label_set_text(label, helix::ui::icon::lookup_codepoint(icon_name));
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

    // The initial page, before anything observes the page subject
    if (shown > 0) {
        ui_carousel_goto_page(carousel_, shown, false);
    }
    active_page_index_ = shown;

    // Populate all pages, activate only the shown page's widgets
    for (int i = 0; i < num_pages; ++i) {
        populate_page(i, true);
    }

    update_arrow_visibility(active_page_index_);
    update_page_badge();
    update_populated_pages_subject();

    // Observed last. LVGL notifies an observer as it is added, and that
    // notification finds active_page_index_ already naming the page on screen,
    // so the build re-scopes no edit session into a container the population
    // above has cleared. Immediate (non-deferred) because the subject is set
    // from carousel_scroll_end_cb on the UI thread, and the deferred path via
    // observe<int> drops the callback (weak_alive expires before the
    // queued lambda executes, causing active_page_index_ desync).
    page_observer_ = helix::ui::observe<int>(
        &page_subject_, this, [](HomePanel* self, int page) { self->on_page_changed(page); },
        get_subjects_lifetime(), helix::ui::Dispatch::Immediate);

    spdlog::debug("[{}] Carousel built with {} pages", get_name(), num_pages);
}

void HomePanel::rebuild_carousel(int shown) {
    spdlog::debug("[{}] Rebuilding carousel", get_name());

    // The teardown deletes every page container, so a live edit session lets go
    // of its page first; show_edit_page() scopes it into the rebuilt carousel.
    grid_edit_mode_.forget_scope();
    teardown_carousel();
    build_carousel(shown);
}

void HomePanel::teardown_carousel() {
    // Deactivate current page widgets
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(pages_.size())) {
        for (auto& w : pages_[static_cast<size_t>(active_page_index_)].widgets) {
            if (w)
                w->on_deactivate();
        }
    }

    // Detach all widget instances across all pages
    for (auto& page : pages_) {
        for (auto& w : page.widgets) {
            if (w)
                w->detach_tile();
        }
    }
    pages_.clear();

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
    next_page_container_ = nullptr;
    arrow_left_ = nullptr;
    arrow_right_ = nullptr;

    // The page subject is re-inited by build_carousel(); subjects_initialized_
    // stays true, since init_subjects() registered the callbacks.
    subjects_.deinit_all();
}

void HomePanel::populate_page(int page_index, bool force) {
    if (populating_widgets_) {
        spdlog::debug("[{}] populate_page: already in progress, skipping", get_name());
        return;
    }

    if (page_index < 0 || page_index >= static_cast<int>(pages_.size())) {
        spdlog::error("[{}] populate_page: page_index {} out of range", get_name(), page_index);
        return;
    }

    populating_widgets_ = true;
    auto idx = static_cast<size_t>(page_index);

    lv_obj_t* container = pages_[idx].container;
    if (!container) {
        spdlog::error("[{}] populate_page: null container for page {}", get_name(), page_index);
        populating_widgets_ = false;
        return;
    }

    // Compute the widget ID list once per populate_page call; the cache below
    // stores this snapshot. A read taken after placement can include a
    // capability that arrived meanwhile (printer_has_led flipping 0 to 1), and
    // cached, it makes the next gate-observer rebuild short-circuit as
    // "unchanged" and leaves the widget in its ~gated placeholder.
    auto snapshot_ids =
        helix::PanelWidgetManager::instance().compute_visible_widget_ids("home", page_index);

    // Skip rebuild if the resulting widget list would be identical
    if (!force) {
        uint64_t gen = helix::runtime_widget_generation();
        if (pages_[idx].visible_ids && snapshot_ids == *pages_[idx].visible_ids &&
            pages_[idx].widget_gen == gen) {
            spdlog::debug("[{}] Page {} widget list unchanged, skipping rebuild", get_name(),
                          page_index);
            populating_widgets_ = false;
            return;
        }

        // A change that only flips hardware gates re-creates just those tiles,
        // not the page: every page build is seconds of UI thread on slow boards.
        // A generation change is not a gate flip: the factories behind unchanged
        // ids now build widgets for a different runtime, so it must not take
        // this partial path.
        if (pages_[idx].visible_ids && pages_[idx].widget_gen == gen) {
            if (auto flips = helix::PanelWidgetManager::gate_flips_only(*pages_[idx].visible_ids,
                                                                        snapshot_ids)) {
                {
                    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
                    helix::ui::UpdateQueue::instance().drain();
                }
                auto fresh = helix::PanelWidgetManager::instance().swap_gated_tiles(
                    "home", container, page_index, snapshot_ids, *flips, pages_[idx].widgets);
                if (fresh) {
                    set_event_bubble_recursive(container);
                    if (grid_edit_mode_.is_active()) {
                        disable_widget_clicks_recursive(container);
                    }
                    if (panel_active_ && page_index == active_page_index_) {
                        for (auto* w : *fresh) {
                            w->on_activate();
                        }
                    }
                    pages_[idx].visible_ids = std::move(snapshot_ids);
                    pages_[idx].widget_gen = gen;
                    populating_widgets_ = false;
                    return;
                }
            }
        }
    }

    // Extract reusable widget instances
    helix::WidgetReuseMap reuse;
    {
        auto& widgets = pages_[idx].widgets;
        for (auto& w : widgets) {
            if (w) {
                w->detach_tile();
                if (w->supports_reuse()) {
                    reuse[w->id()] = std::move(w);
                }
            }
        }
        // Remove null entries
        widgets.erase(
            std::remove_if(widgets.begin(), widgets.end(), [](const auto& w) { return !w; }),
            widgets.end());
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

    pages_[idx].widgets.clear();

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

    // Store widgets, and cache visible widget IDs — use the snapshot computed at
    // populate_page entry so the cache matches the gate values that drove
    // placement, not a fresh read that could include late-arriving capability flips.
    pages_[idx].widgets = std::move(widgets);
    pages_[idx].visible_ids = std::move(snapshot_ids);
    pages_[idx].widget_gen = helix::runtime_widget_generation();

    populating_widgets_ = false;
}

bool HomePanel::relayout_edit_page(const std::vector<std::string>& changed_ids,
                                   const std::string& resized_id) {
    const int page = grid_edit_mode_.page_index();
    if (page < 0 || page >= static_cast<int>(pages_.size()) ||
        !grid_edit_mode_.is_scoped_to(pages_[static_cast<size_t>(page)].container)) {
        return false;
    }
    auto& entry = pages_[static_cast<size_t>(page)];
    if (!helix::PanelWidgetManager::instance().relayout_tiles(
            "home", entry.container, page, changed_ids, resized_id, entry.widgets)) {
        return false;
    }
    // A resize can make a widget build children of its own. As populate_page()
    // treats a built page: bubbling for edit mode's handlers, and disarmed,
    // since a session is live.
    if (lv_obj_t* tile = resized_id.empty()
                             ? nullptr
                             : lv_obj_get_child_by_name(entry.container, resized_id.c_str())) {
        set_event_bubble_recursive(tile);
        disable_widget_clicks_recursive(tile);
    }
    return true;
}

void HomePanel::on_page_changed(int new_page) {
    if (new_page == active_page_index_) {
        return;
    }

    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    int num_pages = static_cast<int>(config.page_count());

    // The next-page slot is not a page: a drag's flip onto it re-scopes through
    // show_edit_page()
    if (new_page >= num_pages) {
        return;
    }

    spdlog::debug("[{}] Page changed: {} -> {}", get_name(), active_page_index_, new_page);

    // Deactivate old page widgets
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(pages_.size())) {
        for (auto& w : pages_[static_cast<size_t>(active_page_index_)].widgets) {
            if (w)
                w->on_deactivate();
        }
    }

    active_page_index_ = new_page;

    // Activate new page widgets if panel is active
    if (panel_active_ && new_page >= 0 && new_page < static_cast<int>(pages_.size())) {
        for (auto& w : pages_[static_cast<size_t>(new_page)].widgets) {
            if (w)
                w->on_activate();
        }
    }

    rescope_edit_page(new_page);

    update_page_badge();
    update_arrow_visibility(new_page);
}

void HomePanel::delete_edit_page() {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    helix::PageSetChange change;
    change.page_count = static_cast<int>(config.page_count());
    change.focus_page = grid_edit_mode_.page_index();
    if (config.remove_page(static_cast<size_t>(change.focus_page))) {
        change.removed_page = change.focus_page;
    }
    config.save();
    exit_grid_edit_mode();
    // On the next tick, outside the confirmation button's click, landing as
    // every page-set change from edit mode does: on the deleted page's index,
    // clamped to the last page.
    helix::ui::run_next_tick(lifetime_.token(),
                             [this, change]() { on_edit_pages_changed(change); });
}

void HomePanel::add_page_from_slot() {
    auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
    helix::PageSetChange change;
    change.page_count = static_cast<int>(config.page_count());
    change.page_added = true;
    // The slot's tile names the page the + creates, as a drag's landing does.
    change.focus_page = change.page_count;
    if (config.add_page(config.generate_page_id()) < 0) {
        return;
    }
    config.save();
    // On the next tick, outside the + button's click, landing as every page-set
    // change does: on the added page.
    helix::ui::run_next_tick(lifetime_.token(),
                             [this, change]() { on_edit_pages_changed(change); });
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
    for (int i = 0; i < num_pages && i < static_cast<int>(pages_.size()); ++i) {
        populate_page(i, force);
    }
    update_populated_pages_subject();
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
        for (const auto& page : pages_) {
            for (const auto& w : page.widgets) {
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

    // Build carousel with pages from config, showing the main page
    build_carousel(static_cast<int>(
        helix::PanelWidgetManager::instance().get_widget_config("home").main_page_index()));

    // Observe hardware gate subjects so widgets appear/disappear when
    // capabilities change (e.g. power devices discovered after startup).
    setup_widget_gate_observers();

    register_config_rebuild_callback();
    wire_grid_edit_page_callbacks();

    spdlog::debug("[{}] Finalize complete", get_name());
}

void HomePanel::register_config_rebuild_callback() {
    helix::PanelWidgetManager::instance().register_rebuild_callback("home", [this]() {
        if (grid_edit_mode_.is_active()) {
            spdlog::debug("[{}] Deferring settings rebuild until edit mode ends", get_name());
            config_rebuild_deferred_ = true;
            return;
        }
        populate_widgets();
    });
}

void HomePanel::wire_grid_edit_page_callbacks() {
    // The rebuild edit mode schedules after it rearranges widgets
    grid_edit_mode_.set_rebuild_callback([this]() { populate_widgets(); });
    grid_edit_mode_.set_relayout_callback(
        [this](const std::vector<std::string>& changed_ids, const std::string& resized_id) {
            return relayout_edit_page(changed_ids, resized_id);
        });

    grid_edit_mode_.set_delete_page_callback([]() {
        helix::ui::modal_confirm("Delete Page", "Remove this page and all its widgets?",
                                 ModalSeverity::Warning, "Delete",
                                 [] { get_global_home_panel().delete_edit_page(); });
    });
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
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(pages_.size())) {
        for (auto& w : pages_[static_cast<size_t>(active_page_index_)].widgets) {
            if (w)
                w->on_activate();
        }
    }

    // Start Spoolman polling for AMS mini status updates
    SpoolmanManager::instance().start_spoolman_polling();

    // First-run tour — respects gate (wizard complete + tour not yet completed at version).
    helix::tour::FirstRunTour::instance().maybe_start();
}

void HomePanel::on_deactivating(DeactivateReason reason) {
    panel_active_ = false;
    // Leave edit mode. Pushing the widget catalog deactivates this panel, and
    // the session outlives that push, since the catalog places into it. A
    // hot-reload rebuild replaces every object the session points into, so it
    // ends the session whether the catalog is open or not.
    if (reason == DeactivateReason::Rebuild || !grid_edit_mode_.is_catalog_open()) {
        exit_grid_edit_mode();
    }

    // Notify only the active page's widgets that the panel is going offscreen
    if (active_page_index_ >= 0 && active_page_index_ < static_cast<int>(pages_.size())) {
        for (auto& w : pages_[static_cast<size_t>(active_page_index_)].widgets) {
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
    for (auto& page : pages_) {
        for (auto& w : page.widgets) {
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
    for (auto& page : pages_) {
        for (auto& w : page.widgets) {
            if (w && std::strcmp(w->id(), helix::PrintStatusWidget::WIDGET_ID) == 0) {
                static_cast<helix::PrintStatusWidget*>(w.get())->trigger_idle_runout_check();
                return;
            }
        }
    }
    spdlog::debug("[{}] PrintStatusWidget not active - skipping runout check", get_name());
}

// ============================================================================
// Static callback trampolines
// ============================================================================

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
    if (panel.grid_edit_mode_.is_active()) {
        panel.grid_edit_mode_.begin_press();
    }
    LVGL_SAFE_EVENT_CB_END();
}

bool HomePanel::carousel_on_scoped_page() const {
    // The carousel's page is the one a goto chose, set as the goto starts, or
    // the one a swipe last settled on, set as it settles, so a slide in either
    // direction reads as the page it began from or is going to, never as a
    // mismatch. What remains is the carousel at rest on a page the session does
    // not follow: one the arrow buttons paged to while the widget catalog holds
    // the session, where the scoped page's widgets are off screen and a hold or
    // a tap must not act on them.
    if (!carousel_) {
        return true;
    }
    const int scoped =
        grid_edit_mode_.is_active() ? grid_edit_mode_.page_index() : active_page_index_;
    return ui_carousel_get_current_page(carousel_) == scoped;
}

void HomePanel::on_home_grid_long_press(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_long_press");
    auto& panel = get_global_home_panel();
    if (!should_suppress_edit_mode(e) && panel.carousel_on_scoped_page()) {
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
                panel.active_page_index_ < static_cast<int>(panel.pages_.size())) {
                container = panel.pages_[static_cast<size_t>(panel.active_page_index_)].container;
            }
            if (container) {
                clear_pressed_state_recursive(container);
            }

            // Enter edit mode on the active page container
            auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
            if (container) {
                panel.grid_edit_mode_.enter(container, &config, panel.active_page_index_);
                // Enter() disarmed clicks on the entry page only; a swipe can
                // settle on any other page mid-session, and its widgets must
                // not fire their real handlers while edit mode is live.
                for (const CarouselPage& page : panel.pages_) {
                    if (page.container && page.container != container) {
                        helix::ui::disable_widget_clicks_recursive(page.container);
                    }
                }
                // State the entry's policy directly, whatever an exit that
                // ended no gesture left: no gesture holds the page, so the
                // pages swipe by their count and the next-page slot is out of
                // reach. Flags only, so this dispatch creates and deletes
                // nothing.
                panel.apply_edit_swipe_policy();
                // Select the widget under the finger (if any). The entry hold
                // selects and never grabs: enter() latched this gesture as a
                // non-grab, so moving the widget takes a fresh press on it or
                // a hold within the session.
                panel.grid_edit_mode_.handle_click(e);
            }
        } else {
            // Already in edit mode: the hold grabs, or opens the catalog on
            // empty grid
            panel.grid_edit_mode_.handle_long_press(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_clicked");
    auto& panel = get_global_home_panel();
    // LVGL clicks after any release no scroll took, a drag's included. In edit
    // mode a click is a release that lands where its press did: a drag or a
    // resize settles the selection at its own end, and a press that moved
    // made its selection where it landed.
    if (panel.grid_edit_mode_.is_active() && panel.carousel_on_scoped_page() &&
        !panel.finger_drifted_since_press()) {
        panel.grid_edit_mode_.handle_click(e);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_pressing(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_pressing");
    auto& panel = get_global_home_panel();
    if (!should_suppress_edit_mode(e) && panel.carousel_on_scoped_page()) {
        if (panel.grid_edit_mode_.is_active()) {
            panel.grid_edit_mode_.handle_pressing(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_released(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_released");
    auto& panel = get_global_home_panel();
    if (!should_suppress_edit_mode(e)) {
        if (panel.grid_edit_mode_.is_active()) {
            panel.grid_edit_mode_.handle_released(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::on_home_grid_press_cancelled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_press_cancelled");
    // Ungated, unlike the handlers that classify a press: a gesture whose press
    // LVGL took away ends wherever the carousel sits and whatever suppresses
    // new input.
    get_global_home_panel().grid_edit_mode_.handle_press_cancelled(e);
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
    if (!grid_edit_mode_.is_active()) {
        return;
    }
    // A live gesture ends uncommitted first: a drag over the next-page slot is
    // carried back to its page before the session is gone.
    grid_edit_mode_.end_gesture_uncommitted();
    grid_edit_mode_.exit();
    // The widgets are left as edit mode arranged them; every page was disarmed
    // at entry, so every page is armed again.
    for (const CarouselPage& page : pages_) {
        helix::ui::enable_widget_clicks_recursive(page.container);
        for (const auto& w : page.widgets) {
            if (w) {
                w->on_edit_mode_exited();
            }
        }
    }
    // Gate and config rebuilds wait out a session. On the next tick, outside
    // the input dispatch that ended it, catch up: a config change rebuilds
    // every page, a gate change only the pages whose widget list it changed.
    const bool force = std::exchange(config_rebuild_deferred_, false);
    helix::ui::run_next_tick(lifetime_.token(), [this, force]() {
        if (!grid_edit_mode_.is_active()) {
            populate_widgets(force);
        }
    });
    // Hand the carousel swipe back to its page count, and take the next-page
    // slot out of reach
    apply_edit_swipe_policy();
}

helix::GridDimensions HomePanel::page_grid(int page) const {
    if (page < 0 || page >= static_cast<int>(pages_.size()) || !pages_[page].container) {
        return {0, 0};
    }
    lv_obj_t* c = pages_[page].container;
    return {helix::grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(c, LV_PART_MAIN)),
            helix::grid_count_tracks(lv_obj_get_style_grid_row_dsc_array(c, LV_PART_MAIN))};
}

void HomePanel::open_widget_catalog() {
    if (grid_edit_mode_.is_active() && parent_screen_) {
        grid_edit_mode_.open_widget_catalog(parent_screen_);
    }
}

// ============================================================================
// Global instance
// ============================================================================

HomePanel& get_global_home_panel() {
    return helix::lazy_global<HomePanel>("HomePanel", get_printer_state(), nullptr);
}
