// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_home.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widgets/print_status_widget.h"
#include "panel_widgets/printer_image_widget.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

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

HomePanel::HomePanel(PrinterState& printer_state, MoonrakerAPI* api)
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

    // Detach active PanelWidget instances
    for (auto& w : active_widgets_) {
        w->detach();
    }
    active_widgets_.clear();
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
        {"printer_status_clicked_cb", printer_status_clicked_cb},
        {"ams_clicked_cb", ams_clicked_cb},
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

    // Clear cached widget IDs so reconnects get a fresh rebuild
    last_visible_widget_ids_.clear();

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
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
        // Skip if any widget has a fullscreen overlay open — detach() would
        // destroy the overlay LVGL objects mid-display (camera fullscreen).
        for (const auto& w : active_widgets_) {
            if (w->has_overlay_open()) {
                spdlog::debug("[{}] Skipping gate rebuild while widget '{}' has overlay open",
                              get_name(), w->id());
                return;
            }
        }
        populate_widgets(/*force=*/false);
    });
}

void HomePanel::populate_widgets(bool force) {
    if (populating_widgets_) {
        spdlog::debug("[{}] populate_widgets: already in progress, skipping", get_name());
        return;
    }
    populating_widgets_ = true;

    lv_obj_t* container = lv_obj_find_by_name(panel_, "widget_container");
    if (!container) {
        spdlog::error("[{}] widget_container not found", get_name());
        populating_widgets_ = false;
        return;
    }

    // Skip rebuild if the resulting widget list would be identical.
    // Only applies to gate observer rebuilds (force=false). Config changes
    // and grid edit mode pass force=true to always rebuild, since positions
    // and per-widget config can change without affecting the widget ID list.
    if (!force) {
        auto new_ids = helix::PanelWidgetManager::instance().compute_visible_widget_ids("home");
        if (new_ids == last_visible_widget_ids_) {
            spdlog::debug("[{}] Widget list unchanged, skipping rebuild", get_name());
            populating_widgets_ = false;
            return;
        }
    }

    // Extract reusable widget instances before destroying the rest.
    // Widgets that support reuse (e.g. CameraWidget) keep expensive C++ state
    // (camera stream) alive across LVGL tree rebuilds.
    helix::WidgetReuseMap reuse;
    for (auto& w : active_widgets_) {
        w->detach();
        if (w->supports_reuse()) {
            reuse[w->id()] = std::move(w);
        }
    }

    // Flush any deferred observer callbacks that captured raw widget pointers.
    // observe_int_sync / observe_string defer via ui_queue_update(), so lambdas
    // may already be queued with a `self` pointer to a widget we're about to
    // destroy.  Draining now ensures they run while the C++ objects still exist
    // (detach() cleared widget_obj_ so the guards will skip the work).
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // Destroy LVGL children BEFORE destroying C++ widget instances.
    // Must happen here (not in the manager) to ensure correct ordering:
    // drain → clean LVGL → clear C++. Manager's lv_obj_clean is a no-op.
    lv_obj_clean(container);
    active_widgets_.clear();

    // Delegate generic widget creation to the manager, passing reusable instances
    active_widgets_ =
        helix::PanelWidgetManager::instance().populate_widgets("home", container, std::move(reuse));

    // Enable event bubbling on the entire widget subtree so touch events
    // (long_press, click, etc.) propagate from deeply-nested clickable
    // elements up to the widget_container, where the grid edit mode
    // handlers are registered via XML.
    set_event_bubble_recursive(container);

    // If edit mode is active (e.g. rebuild triggered during editing),
    // disable clickability so widget click handlers don't fire.
    if (grid_edit_mode_.is_active()) {
        disable_widget_clicks_recursive(container);
    }

    // If the panel is already active (rebuild triggered by gate observer or
    // settings change), activate the new widgets so timers/observers start.
    if (panel_active_) {
        for (auto& w : active_widgets_) {
            w->on_activate();
        }
    }

    // Cache the visible widget IDs AFTER successful rebuild so that
    // gate observer rebuilds can skip if the list hasn't changed.
    last_visible_widget_ids_ =
        helix::PanelWidgetManager::instance().compute_visible_widget_ids("home");

    populating_widgets_ = false;
}

void HomePanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    spdlog::debug("[{}] Setting up...", get_name());

    // Dynamically populate grid widgets from PanelWidgetConfig
    populate_widgets();

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

    // Widgets handle their own initialization via version observers
    // (no explicit config reload needed)

    spdlog::debug("[{}] Setup complete!", get_name());
}

void HomePanel::on_activate() {
    panel_active_ = true;

    // Notify all widgets that the panel is visible
    for (auto& w : active_widgets_) {
        w->on_activate();
    }

    // Start Spoolman polling for AMS mini status updates
    AmsState::instance().start_spoolman_polling();
}

void HomePanel::on_deactivate() {
    panel_active_ = false;
    // Exit grid edit mode if active, UNLESS the widget catalog overlay is open
    // (push_overlay triggers on_deactivate, but edit mode must survive)
    if (grid_edit_mode_.is_active() && !grid_edit_mode_.is_catalog_open()) {
        grid_edit_mode_.exit();
    }

    // Notify all widgets that the panel is going offscreen
    for (auto& w : active_widgets_) {
        w->on_deactivate();
    }

    AmsState::instance().stop_spoolman_polling();
}

void HomePanel::apply_printer_config() {
    // Widgets use version observers for auto-binding (LED, power, etc.)
    // Just refresh the printer image (delegated to PrinterImageWidget)
    refresh_printer_image();
}

void HomePanel::refresh_printer_image() {
    for (auto& w : active_widgets_) {
        if (auto* piw = dynamic_cast<helix::PrinterImageWidget*>(w.get())) {
            piw->refresh_printer_image();
            return;
        }
    }
}

void HomePanel::trigger_idle_runout_check() {
    for (auto& w : active_widgets_) {
        if (auto* psw = dynamic_cast<helix::PrintStatusWidget*>(w.get())) {
            psw->trigger_idle_runout_check();
            return;
        }
    }
    spdlog::debug("[{}] PrintStatusWidget not active - skipping runout check", get_name());
}

// ============================================================================
// Panel-level click handlers
// ============================================================================

void HomePanel::handle_printer_status_clicked() {
    spdlog::info("[{}] Printer status icon clicked - navigating to advanced settings", get_name());
    NavigationManager::instance().set_active(PanelId::Advanced);
}

void HomePanel::handle_ams_clicked() {
    spdlog::info("[{}] AMS indicator clicked - opening AMS panel overlay", get_name());

    auto& ams_panel = get_global_ams_panel();
    if (!ams_panel.are_subjects_initialized()) {
        ams_panel.init_subjects();
    }
    lv_obj_t* panel_obj = ams_panel.get_panel();
    if (panel_obj) {
        NavigationManager::instance().push_overlay(panel_obj);
    }
}

// ============================================================================
// Static callback trampolines
// ============================================================================

void HomePanel::printer_status_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] printer_status_clicked_cb");
    (void)e;
    get_global_home_panel().handle_printer_status_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::ams_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] ams_clicked_cb");
    (void)e;
    get_global_home_panel().handle_ams_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

/// Returns true if the active input device is interacting with a widget that
/// consumes drag gestures — either scrolling (e.g., swiping a carousel) or
/// dragging an arc/slider knob (e.g., adjusting fan speed). LVGL fires
/// LONG_PRESSED based purely on hold duration, regardless of finger movement,
/// so we must check for these interactions to prevent false edit mode entry.
static bool should_suppress_edit_mode(lv_event_t* e) {
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

void HomePanel::on_home_grid_long_press(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HomePanel] on_home_grid_long_press");
    if (!should_suppress_edit_mode(e)) {
        auto& panel = get_global_home_panel();
        if (!panel.grid_edit_mode_.is_active()) {
            // Cancel the in-progress press to prevent the widget's click
            // action from firing on release.
            lv_indev_t* indev = lv_indev_active();
            if (indev) lv_indev_reset(indev, nullptr);

            // Clear PRESSED state from all descendants — the pressed button
            // may be deeply nested inside a widget (e.g., print_status card).
            auto* container = lv_obj_find_by_name(panel.panel_, "widget_container");
            clear_pressed_state_recursive(container);

            // Enter edit mode on first long-press
            auto& config = helix::PanelWidgetManager::instance().get_widget_config("home");
            panel.grid_edit_mode_.enter(container, &config);
            // Select the widget under the finger and start dragging immediately.
            panel.grid_edit_mode_.handle_click(e);
            if (panel.grid_edit_mode_.selected_widget()) {
                panel.grid_edit_mode_.handle_drag_start(e);
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
    if (!should_suppress_edit_mode(e)) {
        auto& panel = get_global_home_panel();
        if (panel.grid_edit_mode_.is_active()) {
            panel.grid_edit_mode_.handle_released(e);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HomePanel::exit_grid_edit_mode() {
    if (grid_edit_mode_.is_active()) {
        grid_edit_mode_.exit();
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
