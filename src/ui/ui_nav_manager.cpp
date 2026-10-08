// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_manager.h"

#include "ui_effects.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_next_tick.h"
#include "ui_panel_base.h"
#include "ui_panel_home.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "backdrop_blur.h"
#include "connection_state.h" // For ConnectionState enum
#include "display_settings_manager.h"
#include "env_knobs.h"
#include "layout_manager.h"
#include "observer_factory.h"
#include "overlay_base.h"
#include "overlay_class.h"
#include "page_scroll_auto_inject.h"
#include "printer_state.h" // For KlippyState enum
#include "settings_manager.h"
#include "sound_manager.h"
#include "static_subject_registry.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::observe;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#if defined(HELIX_PLATFORM_ESP32)
namespace {
// "Loading..." pill on the TOP layer, painted before a (possibly multi-second)
// first build of a panel. STATIC label, NOT a spinner: the build blocks the LVGL
// thread, so no animation timer can run and a spinner would freeze and read as a
// hang. Small on purpose: painting or lifting a full-screen scrim re-renders the
// whole screen, which costs ~600ms on the ESP32; the pill costs a few dozen. Taps
// need no absorbing: touch is polled on the thread the build blocks.
lv_obj_t* make_loading_scrim() {
    lv_obj_t* pill = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 12, LV_PART_MAIN);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(pill);
    lv_label_set_text(lbl, "Loading...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(pill);
    return pill;
}

// RAII busy indicator wrapping a panel transition (the deferred first-build now
// runs UNDER this scrim — one mechanism, not two). ctor shows the scrim and
// forces it to paint BEFORE the blocking transition body (the LVGL thread is
// about to block; a state that only appears after is useless). dtor paints the
// now-un-hidden new panel under the scrim, then lifts the scrim with
// safe_delete_deferred, an LVGL async delete: the transition can run inside a
// queued callback, where a sync delete can corrupt LVGL's event list (#776). The
// new panel is painted before the scrim lifts, so there is no old-panel flash.
// The async delete runs in the same or the next lv_timer_handler() pass, not at
// an UpdateQueue drain. Only the OUTERMOST transition owns a
// scrim — switch_to_panel_impl can cascade into handle_active_panel_change via
// the active_panel subject, and we must not nest two.
class NavTransitionScrim {
  public:
    // `needed`: the transition has a panel to build. Switching between built
    // panels only flips visibility, so a scrim would add two forced full renders
    // for nothing.
    NavTransitionScrim(bool& active, bool needed) : active_(active), owns_(!active && needed) {
        if (owns_) {
            active_ = true;
            scrim_ = make_loading_scrim();
            lv_refr_now(lv_display_get_default());
        }
    }
    ~NavTransitionScrim() {
        if (owns_) {
            lv_refr_now(lv_display_get_default());
            helix::ui::safe_delete_deferred(scrim_);
            active_ = false;
        }
    }
    NavTransitionScrim(const NavTransitionScrim&) = delete;
    NavTransitionScrim& operator=(const NavTransitionScrim&) = delete;

  private:
    bool& active_;
    bool owns_;
    lv_obj_t* scrim_ = nullptr;
};
} // namespace
#endif

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

// Flag set by NavigationManager destructor to detect static destruction.
// Using namespace-scope static ensures it's initialized before main() and
// outlives all function-local statics, including the singleton itself.
namespace {
bool g_nav_manager_destroyed = false;

// Strict overlay-registration check (dev/test only). Mirrors the L081
// HELIX_STRICT_BG_THREAD_CHECK machinery: opt-in via env or setter, compiled
// out in release builds. See NavigationManager::set_overlay_registration_strict.
std::atomic<bool> g_overlay_strict{false};
std::atomic<bool> g_overlay_strict_env_read{false};

bool overlay_registration_strict() {
#ifdef HELIX_RELEASE_BUILD
    return false;
#else
    if (!g_overlay_strict_env_read.load(std::memory_order_acquire)) {
        if (helix::env_flag("HELIX_STRICT_OVERLAY_CHECK")) {
            g_overlay_strict.store(true, std::memory_order_release);
        }
        g_overlay_strict_env_read.store(true, std::memory_order_release);
    }
    return g_overlay_strict.load(std::memory_order_acquire);
#endif
}

// Back to the resting transform and opacity, so a hidden overlay shows up
// correctly the next time it is reused. Written unconditionally: reading the
// current transform and skipping the write when it looks clean leaves stale
// scale values behind under SDL's logical scaling on Android, which corrupts the
// display, and the four writes cost less than that.
void reset_overlay_transform(lv_obj_t* obj) {
    lv_obj_set_style_translate_x(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(obj, 256, LV_PART_MAIN);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}
} // namespace

void NavigationManager::set_overlay_registration_strict(bool enabled) noexcept {
    g_overlay_strict.store(enabled, std::memory_order_release);
    // An explicit setter call wins over the env var — mark as resolved so a
    // later env read can't clobber the test's choice.
    g_overlay_strict_env_read.store(true, std::memory_order_release);
}

NavigationManager::~NavigationManager() {
    g_nav_manager_destroyed = true;
}

NavigationManager& NavigationManager::instance() {
    static NavigationManager inst;
    return inst;
}

bool NavigationManager::is_destroyed() {
    // Guard against Static Destruction Order Fiasco.
    // This flag is set by NavigationManager's destructor, so it accurately
    // reflects whether the singleton's internal data structures are valid.
    return g_nav_manager_destroyed;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

const char* NavigationManager::panel_id_to_name(PanelId id) {
    static const char* names[] = {"home_panel",     "print_select_panel", "controls_panel",
                                  "filament_panel", "settings_panel",     "advanced_panel"};
    if (static_cast<int>(id) < UI_PANEL_COUNT) {
        return names[static_cast<int>(id)];
    }
    return "unknown_panel";
}

bool NavigationManager::panel_requires_connection(PanelId panel) {
    return panel == PanelId::Controls || panel == PanelId::Filament;
}

bool NavigationManager::is_printer_connected() const {
    auto* subject = get_printer_state().network_state().get_printer_connection_state_subject();
    return lv_subject_get_int(subject) == 2;
}

bool NavigationManager::is_klippy_ready() const {
    auto* subject = get_printer_state().network_state().get_klippy_state_subject();
    return lv_subject_get_int(subject) == 0; // KlippyState::READY
}

// Runs an overlay close callback on the next LVGL tick. Close callbacks delete
// widgets, and the paths that fire them run inside UpdateQueue drains or LVGL
// animation callbacks, where a synchronous delete corrupts LVGL's event list
// (prestonbrown/helixscreen#637). An overlay registered when it closes and
// unregistered by the tick lost its owner in between, and the callback captures
// that owner, so it is dropped.
void NavigationManager::defer_close_callback(OverlayCloseCallback callback, lv_obj_t* overlay) {
    const bool had_owner = is_overlay_registered(overlay);
    helix::ui::run_next_tick([cb = std::move(callback), overlay, had_owner]() {
        if (g_nav_manager_destroyed) {
            return;
        }
        if (had_owner && !NavigationManager::instance().is_overlay_registered(overlay)) {
            spdlog::debug("[NavigationManager] Owner of overlay {} left before its close "
                          "callback ran; dropping it",
                          (void*)overlay);
            return;
        }
        cb();
    });
}

bool NavigationManager::is_overlay_registered(lv_obj_t* overlay) const {
    overlay = resolve_rebuilt(overlay);
    return overlay_instances_.count(overlay) > 0 ||
           persistent_overlay_instances_.count(overlay) > 0;
}

void NavigationManager::retire_overlay(lv_obj_t* overlay) {
    // Both run inside UpdateQueue drains or LVGL animation callbacks, where a
    // synchronous delete corrupts LVGL's event list (#637, #620).
    backdrop_.retire(overlay);

    auto callback_it = overlay_close_callbacks_.find(overlay);
    if (callback_it != overlay_close_callbacks_.end()) {
        spdlog::trace("[NavigationManager] Deferring close callback for overlay {}",
                      (void*)overlay);
        defer_close_callback(std::move(callback_it->second), overlay);
        overlay_close_callbacks_.erase(callback_it);
    }
}

void NavigationManager::clear_overlay_stack() {
    // L081 Mech D defense: cancel in-flight pointer input before bulk teardown.
    // Stale clicks queued in indev would otherwise dispatch to widgets we're
    // about to destroy, with the dispatch reading a freed event_dsc_t array.
    lv_indev_reset(nullptr, nullptr);
    crash_handler::breadcrumb::note("indev_rst", "clear_stack", 0);
    spdlog::debug("[NavigationManager] indev_rst:clear_stack");

    // Hide all overlay panels immediately (no animation for connection loss)
    while (panel_stack_.size() > 1) {
        lv_obj_t* overlay = panel_stack_.back();
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        reset_overlay_transform(overlay);

        // Deactivate the overlay to stop background work (camera, timers, etc.)
        // and invalidate lifetime tokens. Without this, overlays like QrScanner
        // keep their camera running after connection loss. (#632)
        auto inst_it = overlay_instances_.find(overlay);
        if (inst_it != overlay_instances_.end() && inst_it->second) {
            inst_it->second->on_deactivate(DeactivateReason::NavigateAway);
        }

        // clear_overlay_stack() is called from subject observers (connection
        // loss, klippy shutdown) which fire inside process_pending(), so the
        // overlay's teardown must not run synchronously here.
        retire_overlay(overlay);

        panel_stack_.pop_back();
        spdlog::trace("[NavigationManager] Cleared overlay {} from stack", (void*)overlay);
    }

    overlay_is_destination_.clear();
    overlay_width_unmanaged_.clear();

    backdrop_.release_primary();

    spdlog::trace("[NavigationManager] Overlay stack cleared (connection gating)");
}

// ============================================================================
// ANIMATION HELPERS
// ============================================================================

void NavigationManager::overlay_slide_out_complete_cb(lv_anim_t* anim) {
    if (NavigationManager::instance().is_shutting_down()) {
        return; // Shutdown in progress — widget may be freed
    }
    lv_obj_t* panel = static_cast<lv_obj_t*>(anim->var);
    if (!lv_obj_is_valid(panel)) {
        spdlog::warn("[NavigationManager] Animation completed but panel {} already freed",
                     anim->var);
        return;
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    reset_overlay_transform(panel);
    spdlog::trace("[NavigationManager] Overlay slide+fade-out complete, panel {} hidden",
                  (void*)panel);

    // Animation callbacks fire from inside lv_timer_handler() →
    // lv_display_refr_timer(), and deleting objects mid-layout is a
    // use-after-free in layout_update_core → lv_obj_scrollbar_invalidate, so the
    // close callback waits for the next tick.
    auto& mgr = NavigationManager::instance();
    mgr.retire_overlay(panel);

    // Lifecycle: activate what's now visible. go_back() consumes the latch
    // itself before it returns, so this is the fallback for any close path that
    // armed it without reaching that point — a no-op once consumed.
    mgr.activate_restored_target();
}

void NavigationManager::activate_restored_target() {
    if (!restore_activation_pending_) {
        return;
    }
    // Clear BEFORE dispatching: on_activate() may navigate (PrintSelectPanel's
    // Print-Last flow calls set_active()), which can queue another go_back().
    restore_activation_pending_ = false;

    if (panel_stack_.size() == 1) {
        // Back to main panel - activate it
        main_panel_deactivated_for_overlay_ = false;
        if (panels_.instance(static_cast<int>(active_panel_))) {
            spdlog::trace("[NavigationManager] Activating main panel {} after overlay closed",
                          static_cast<int>(active_panel_));
            panels_.instance(static_cast<int>(active_panel_))->on_activate();
        }
    } else if (panel_stack_.size() > 1) {
        // Back to previous overlay - activate it
        lv_obj_t* now_visible = panel_stack_.back();
        auto overlay_it = overlay_instances_.find(now_visible);
        if (overlay_it != overlay_instances_.end() && overlay_it->second) {
            spdlog::trace("[NavigationManager] Activating previous overlay {}",
                          overlay_it->second->get_name());
            overlay_it->second->on_activate();
        }
    }
}

// Overlays enter from and exit to the right edge in every orientation.
static void overlay_translate_x(void* obj, int32_t v) {
    if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
        return;
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), v, LV_PART_MAIN);
}

static int32_t overlay_slide_offset(lv_obj_t* panel, int32_t fallback) {
    const int32_t width = lv_obj_get_width(panel);
    return width != 0 ? width : fallback;
}

void NavigationManager::overlay_animate_slide_in(lv_obj_t* panel) {
    const int32_t offset = overlay_slide_offset(panel, OVERLAY_SLIDE_OFFSET);

    // Skip animation if disabled - show panel in final state
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_x(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        spdlog::trace("[NavigationManager] Animations disabled - showing overlay instantly");
        return;
    }

    // Set initial state: off-screen and transparent
    lv_obj_set_style_translate_x(panel, offset, LV_PART_MAIN);
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, offset, 0);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide_anim, overlay_translate_x);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from transparent to opaque (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-in for panel {} (offset={})",
                  (void*)panel, offset);
}

void NavigationManager::overlay_animate_slide_out(lv_obj_t* panel) {
    // Disable clicks immediately to prevent interaction during animation
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Skip animation if disabled - hide panel immediately and invoke callback
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        reset_overlay_transform(panel);
        spdlog::trace("[NavigationManager] Animations disabled - hiding overlay instantly");

        retire_overlay(panel);

        // Deliberately NO activation here. This runs from inside go_back(),
        // which un-hides the restored panel *after* this returns; an
        // on_activate() that navigates (PrintSelectPanel's Print-Last flow
        // calls set_active(Home)) would be silently undone by that un-hide.
        // go_back() owns the restored panel's activation via
        // activate_restored_target(), fired once, below its un-hide.
        return;
    }

    const int32_t offset = overlay_slide_offset(panel, OVERLAY_SLIDE_OFFSET);

    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, panel);
    lv_anim_set_values(&slide_anim, 0, offset);
    lv_anim_set_duration(&slide_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide_anim, overlay_translate_x);
    lv_anim_set_completed_cb(&slide_anim, overlay_slide_out_complete_cb);
    lv_anim_start(&slide_anim);

    // Fade animation: opacity from opaque to transparent (runs simultaneously)
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, panel);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, OVERLAY_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        if (!lv_obj_is_valid(static_cast<lv_obj_t*>(obj)))
            return;
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);

    spdlog::trace("[NavigationManager] Started slide+fade-out for panel {} (offset={})",
                  (void*)panel, offset);
}

// ============================================================================
// OBSERVER HANDLERS (used by factory-created observers)
// ============================================================================

void NavigationManager::handle_active_panel_change(int32_t new_active_panel) {
#if defined(HELIX_PLATFORM_ESP32)
    // Busy scrim + input block for the whole transition (ESP32-only; no-op on the
    // nested inner change if switch_to_panel_impl cascaded here).
    NavTransitionScrim scrim_guard(nav_scrim_active_, panels_.needs_build(new_active_panel));
#endif
    // Deferred bring-up for writes to active_panel that bypass set_active() and
    // switch_to_panel_impl(). No-op on desktop and for already-built panels.
    panels_.ensure_built(new_active_panel);
    panels_.show_only(new_active_panel);
}

void NavigationManager::handle_connection_state_change(int state) {
    bool was_connected =
        (previous_connection_state_ == static_cast<int>(ConnectionState::CONNECTED));
    bool is_connected = (state == static_cast<int>(ConnectionState::CONNECTED));

    // Only redirect if we were previously connected and are now disconnected
    if (was_connected && !is_connected) {
        if (disconnect_expected_) {
            // Consume the one-shot ON THE FALLING EDGE, not on entry. The latch
            // cannot live in previous_connection_state_: every deferred
            // connection apply writes that field, so a CONNECTED apply still
            // undrained when the app backgrounds would overwrite it and the
            // synthetic DISCONNECTED behind it would read as real (#1245).
            disconnect_expected_ = false;
            spdlog::debug("[NavigationManager] Expected disconnect on panel {} - staying put",
                          static_cast<int>(active_panel_));
        } else if (panel_requires_connection(active_panel_)) {
            spdlog::info("[NavigationManager] Connection lost on panel {} - navigating to home",
                         static_cast<int>(active_panel_));

            clear_overlay_stack();
            set_active(PanelId::Home);
        }
    }

    previous_connection_state_ = state;
}

void NavigationManager::mark_disconnect_expected() {
    // Arm a one-shot that the next CONNECTED→DISCONNECTED transition consumes,
    // so the disconnect queued by on_enter_background() doesn't clear the
    // overlay stack when it drains on resume (#1245).
    disconnect_expected_ = true;
}

void NavigationManager::handle_klippy_state_change(int state) {
    bool was_ready = (previous_klippy_state_ == static_cast<int>(KlippyState::READY));
    bool is_ready = (state == static_cast<int>(KlippyState::READY));

    // A SAVE_CONFIG or user-initiated restart bounces Klipper through a transient
    // SHUTDOWN. Don't yank the user off their calibration panel to Home for it —
    // the panel is still valid and klippy returns to READY within seconds.
    if (was_ready && !is_ready && EmergencyStopOverlay::instance().is_expected_restart()) {
        spdlog::debug("[NavigationManager] Klippy left READY during expected restart on panel {} "
                      "- staying put",
                      static_cast<int>(active_panel_));
        previous_klippy_state_ = state;
        return;
    }

    // Redirect to home if klippy enters non-READY state (SHUTDOWN/ERROR) while on restricted panel
    if (was_ready && !is_ready && panel_requires_connection(active_panel_)) {
        const char* state_name = (state == static_cast<int>(KlippyState::SHUTDOWN)) ? "SHUTDOWN"
                                 : (state == static_cast<int>(KlippyState::ERROR))  ? "ERROR"
                                                                                    : "non-READY";
        spdlog::info("[NavigationManager] Klippy {} on panel {} - navigating to home", state_name,
                     static_cast<int>(active_panel_));

        clear_overlay_stack();
        set_active(PanelId::Home);
    }

    previous_klippy_state_ = state;
}

// ============================================================================
// EVENT CALLBACKS
// ============================================================================

lv_obj_t* NavigationManager::navbar_target_at(lv_obj_t* navbar, const lv_point_t& point) {
    if (!navbar) {
        return nullptr;
    }
    static constexpr const char* kTargets[] = {"nav_btn_home",     "nav_btn_print_select",
                                               "nav_btn_controls", "nav_btn_filament",
                                               "nav_btn_settings", "nav_btn_advanced"};
    for (const char* name : kTargets) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, name);
        if (!btn || lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(btn, &area);
        if (point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2) {
            return btn;
        }
    }
    return nullptr;
}

void NavigationManager::backdrop_click_event_cb(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only respond if the event was directly on the backdrop (not bubbled)
    if (target != current) {
        return;
    }

    auto& mgr = NavigationManager::instance();

    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        // Capture keyboard visibility at PRESS time. LVGL's indev_proc_press
        // sends PRESSED before indev_click_focus, whose DEFOCUS hides the
        // keyboard — so by CLICKED, is_visible() would already read false.
        mgr.backdrop_.note_press(KeyboardManager::instance().is_visible());
        return;
    }

    // LV_EVENT_CLICKED — a tap that dismissed the on-screen keyboard must not
    // also dismiss (or navigate away from) the overlay behind it. Consume this
    // tap for the keyboard dismiss only; a second tap dismisses the overlay.
    if (mgr.backdrop_.take_keyboard_dismiss()) {
        spdlog::trace(
            "[NavigationManager] Backdrop tap dismissed on-screen keyboard; overlay kept");
        return;
    }

    // Only process if there's an overlay to close (stack > 1 means overlays exist)
    if (mgr.panel_stack_.size() <= 1) {
        return;
    }

    // Get click position to check if it's in the navbar area
    lv_point_t click_point;
    lv_indev_get_point(lv_indev_active(), &click_point);

    // Check if click is in navbar area and find which button was clicked.
    //
    // Tested against the bar's own coordinates rather than "x < navbar_width".
    // The width comparison assumes the bar is a full-height strip pinned to the
    // leading edge, which only holds in landscape. In portrait the bar lies along
    // the bottom at width="100%", where that test is true for every point on the
    // screen.
    if (mgr.navbar_widget_) {
        lv_area_t navbar_area;
        lv_obj_get_coords(mgr.navbar_widget_, &navbar_area);

        if (click_point.x >= navbar_area.x1 && click_point.x <= navbar_area.x2 &&
            click_point.y >= navbar_area.y1 && click_point.y <= navbar_area.y2) {
            if (lv_obj_t* target = navbar_target_at(mgr.navbar_widget_, click_point)) {
                spdlog::trace("[NavigationManager] Backdrop click forwarded to navbar '{}'",
                              lv_obj_get_name(target) ? lv_obj_get_name(target) : "?");
                // Simulate the navbar button click by sending a clicked event
                lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
                return;
            }

            // Click was in navbar area but not on a button - just close overlay
            spdlog::trace("[NavigationManager] Backdrop clicked in navbar area (no button hit)");
        }
    }

    // Regular backdrop click - close topmost overlay
    spdlog::trace("[NavigationManager] Backdrop clicked, closing topmost overlay");
    mgr.go_back();
}

bool NavigationManager::take_backdrop_keyboard_dismiss() {
    return backdrop_.take_keyboard_dismiss();
}

void NavigationManager::nav_button_clicked_cb(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("nav_button_clicked_cb");

    auto& mgr = NavigationManager::instance();
    lv_event_code_t code = lv_event_get_code(event);
    int panel_id = (int)(uintptr_t)lv_event_get_user_data(event);

    spdlog::trace("[NavigationManager] nav_button_clicked_cb fired: code={}, panel_id={}, "
                  "active_panel={}",
                  static_cast<int>(code), panel_id, static_cast<int>(mgr.active_panel_));

    if (code == LV_EVENT_CLICKED) {
        // Queued, not inline: this runs from an LVGL event during the render
        // phase, where mutating the widget tree corrupts the draw.
        mgr.request_panel(static_cast<PanelId>(panel_id), SwitchDispatch::Queued);
    }

    LVGL_SAFE_EVENT_CB_END();
}

NavigationManager::PanelRequest NavigationManager::request_panel(PanelId panel_id,
                                                                 SwitchDispatch dispatch) {
    const int id = static_cast<int>(panel_id);

    // Already on this panel with no overlays: special handling per panel
    if (panel_id == active_panel_ && !has_open_overlays()) {
        if (panel_id == PanelId::Home) {
            // Tapping home while on home scrolls carousel to page 0
            spdlog::debug("[NavigationManager] Already on Home - navigating to main page");
            get_global_home_panel().go_to_main_page();
            return PanelRequest::HomeRetapped;
        }
        spdlog::debug("[NavigationManager] Skipping - already on panel {} with no overlays", id);
        return PanelRequest::AlreadyActive;
    }

    // Block navigation to connection-required panels when disconnected or klippy not ready
    if (panel_requires_connection(panel_id)) {
        if (!is_printer_connected()) {
            spdlog::info("[NavigationManager] Navigation to panel {} blocked - not connected", id);
            return PanelRequest::BlockedDisconnected;
        }
        if (!is_klippy_ready()) {
            spdlog::info("[NavigationManager] Navigation to panel {} blocked - klippy not ready",
                         id);
            return PanelRequest::BlockedKlippyNotReady;
        }
    }

    if (dispatch == SwitchDispatch::Queued) {
        // Queued: the switch runs in a later UpdateQueue drain, never during a render
        spdlog::trace("[NavigationManager] Queuing switch to panel {}", id);
        helix::ui::queue_update("NavigationManager::request_panel",
                                [id]() { NavigationManager::instance().switch_to_panel_impl(id); });
    } else {
        spdlog::trace("[NavigationManager] Switching to panel {} inline", id);
        switch_to_panel_impl(id);
    }
    return PanelRequest::Switched;
}

void NavigationManager::switch_to_panel_impl(int panel_id) {
#if defined(HELIX_PLATFORM_ESP32)
    // Busy scrim + input block for the whole transition (ESP32-only). Outermost
    // owner; a cascade into handle_active_panel_change won't create a second one.
    NavTransitionScrim scrim_guard(nav_scrim_active_, panels_.needs_build(panel_id));
#endif
    auto switch_start = std::chrono::steady_clock::now();
    spdlog::trace("[NavigationManager] switch_to_panel_impl executing for panel {}", panel_id);

    // Deferred bring-up (ESP32): build the target panel on first navigation so
    // the overlay/stack/show logic below sees a real widget. No-op on desktop
    // and for already-built panels. The builder paints a loading state before
    // the blocking create; see PanelFactory::build_deferred_panel.
    panels_.ensure_built(panel_id);

    // L081 Mech D defense: cancel in-flight pointer input before panel switch.
    // Sends LV_EVENT_INDEV_RESET to current act_obj while it's still alive,
    // then nulls indev->pointer.act_obj so the next dispatch can't target a
    // widget we're about to tear down.
    lv_indev_reset(nullptr, nullptr);
    crash_handler::breadcrumb::note("indev_rst", "switch_panel", panel_id);
    spdlog::debug("[NavigationManager] indev_rst:switch_panel({})", panel_id);

    // Hide ALL visible overlay panels
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
            lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }

            // Screen chrome (the navbar E-stop) is not an overlay.
            if (child == app_layout_widget_ || helix::ui::is_screen_chrome(child)) {
                continue;
            }

            if (!panels_.is_main_panel(child)) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                reset_overlay_transform(child);
                spdlog::trace("[NavigationManager] Hiding overlay panel {} (nav button clicked)",
                              (void*)child);
            }
        }
    }

    panels_.show_only(-1);

    // Deactivate overlays, invoke close callbacks, and clean up backdrops
    // for any overlays being cleared (e.g., settings overlay needs on_deactivate to save).
    // Persistent overlays (e.g., PrintStatusPanel) are SKIPPED — they continue
    // collecting data (temperature history) in the background across navbar switches.
    for (lv_obj_t* panel : panel_stack_) {
        bool is_persistent = persistent_overlay_instances_.count(panel) > 0;

        if (!is_persistent) {
            // Call on_deactivate() if this overlay has a registered instance
            auto inst_it = overlay_instances_.find(panel);
            if (inst_it != overlay_instances_.end() && inst_it->second) {
                spdlog::trace("[NavigationManager] Calling on_deactivate() for overlay {} (navbar)",
                              (void*)panel);
                inst_it->second->on_deactivate(DeactivateReason::NavigateAway);
            }
        } else {
            spdlog::trace(
                "[NavigationManager] Skipping on_deactivate() for persistent overlay {} (navbar)",
                (void*)panel);
        }

        // switch_to_panel_impl() can be called from subject observers inside
        // process_pending(), so the overlay's teardown must not run synchronously.
        retire_overlay(panel);
    }

    // Clear the panel stack and the per-open bookkeeping, but NOT the overlay
    // registrations. overlay_instances_ pairs a widget with its lifecycle for as
    // long as that widget exists; ensure_delete_hook() arms every registration so
    // scrub_deleted_widget() drops the entry when LVGL deletes the widget.
    // Clearing the map here unpairs every cached overlay, and the next push of one
    // then arrives with no lifecycle, so its on_deactivate() never runs. Whether
    // an overlay is open is answered by panel_stack_, not by this map.
    panel_stack_.clear();
    // What is left belongs to overlays already popped and still sliding out: their
    // callbacks would run when the slide-out ends, and that completion finds
    // nothing once the map is cleared. Run them now instead, so each overlay's
    // owner still tears it down.
    for (auto& [overlay, callback] : overlay_close_callbacks_) {
        defer_close_callback(std::move(callback), overlay);
    }
    overlay_close_callbacks_.clear();
    // Delete any remaining dynamic backdrops the loop above didn't reach
    // (orphaned entries not in panel_stack_).
    backdrop_.retire_all();
    spdlog::trace("[NavigationManager] Panel stack and overlay maps cleared (nav button clicked)");

    // The primary snapshot goes too: every overlay is being cleared
    backdrop_.release_primary();

    // Show the clicked panel
    lv_obj_t* new_panel = panels_.widget(static_cast<int>(panel_id));
    if (new_panel) {
        lv_obj_remove_flag(new_panel, LV_OBJ_FLAG_HIDDEN);
        panel_stack_.push_back(new_panel);
        spdlog::trace("[NavigationManager] Showing panel {} (stack depth: {})", (void*)new_panel,
                      panel_stack_.size());
    }

    // Opening an overlay never moved active_panel_ — push_overlay() only calls
    // on_deactivate() on the panel underneath. So a navbar tap onto the panel
    // we are already on lands here with that panel deactivated, and set_active()
    // below short-circuits on panel_id == active_panel_ without activating
    // anything. Re-activate it through the same latch go_back() uses, or the
    // panel stays visible-but-deactivated and everything on_activate() restarts
    // (CameraWidget::start_stream) never runs again.
    const bool activation_owed =
        (static_cast<PanelId>(panel_id) == active_panel_) && main_panel_deactivated_for_overlay_;

    set_active((PanelId)panel_id);

    if (activation_owed) {
        restore_activation_pending_ = true;
        activate_restored_target();
    }

    SoundManager::instance().play("nav_forward");

    auto switch_elapsed = std::chrono::steady_clock::now() - switch_start;
    spdlog::info("[NavigationManager] Panel switch to {} took {:.1f}ms", panel_id,
                 std::chrono::duration<double, std::milli>(switch_elapsed).count());
}

// ============================================================================
// NAVIGATION MANAGER IMPLEMENTATION
// ============================================================================

void NavigationManager::init() {
    if (subjects_initialized_) {
        spdlog::warn("[NavigationManager] Subjects already initialized");
        return;
    }

    spdlog::trace("[NavigationManager] Initializing navigation reactive subjects...");

    UI_MANAGED_SUBJECT_INT(active_panel_subject_, static_cast<int>(PanelId::Home), "active_panel",
                           subjects_);

    // Overlay backdrop starts hidden
    UI_MANAGED_SUBJECT_INT(overlay_backdrop_visible_subject_, 0, "overlay_backdrop_visible",
                           subjects_);

    active_panel_observer_ = observe<int>(
        &active_panel_subject_, this,
        [](NavigationManager* mgr, int value) { mgr->handle_active_panel_change(value); },
        get_subjects_lifetime());

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "NavigationManager", []() { NavigationManager::instance().deinit_subjects(); });

    spdlog::trace("[NavigationManager] Navigation subjects initialized successfully");
}

void NavigationManager::init_overlay_backdrop(lv_obj_t* screen) {
    // Backdrop is now created dynamically as a darkened snapshot when the first
    // overlay is pushed.  Nothing to pre-create.
    (void)screen;
    spdlog::trace("[NavigationManager] Overlay backdrop init (dynamic snapshot mode)");
}

void NavigationManager::set_app_layout(lv_obj_t* app_layout) {
    app_layout_widget_ = app_layout;
    spdlog::trace("[NavigationManager] App layout widget registered");
}

void NavigationManager::wire_events(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_events");
        return;
    }

    if (!subjects_initialized_) {
        spdlog::error("[NavigationManager] Subjects not initialized! Call init() first!");
        return;
    }

    // Store navbar reference for z-order management when showing overlays
    navbar_widget_ = navbar;

    lv_obj_remove_flag(navbar, LV_OBJ_FLAG_CLICKABLE);

    const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select", "nav_btn_controls",
                                  "nav_btn_filament", "nav_btn_settings",     "nav_btn_advanced"};

    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);

        if (!btn) {
            spdlog::trace("[NavigationManager] Nav button {} not found (may be intentional)", i);
            continue;
        }

        lv_obj_add_event_cb(btn, nav_button_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        // Remove focus ring — nav buttons use icon color swap for active state
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(btn);
    }

    // Register connection state observer for redirect on disconnect
    connection_state_observer_ = observe<int>(
        get_printer_state().network_state().get_printer_connection_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_connection_state_change(value); },
        get_printer_state().get_subjects_lifetime());

    // Register klippy state observer for redirect on SHUTDOWN/ERROR
    klippy_state_observer_ = observe<int>(
        get_printer_state().network_state().get_klippy_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_klippy_state_change(value); },
        get_printer_state().get_subjects_lifetime());

    printer_badge_.wire(navbar);

    // The printer badge is the one navbar element a setting can add or remove
    // while the user is looking at it, and its toggle lives inside an overlay —
    // so the navbar the user sees is the backdrop's frozen snapshot, not the
    // widget the binding just un-hid. Re-take the snapshot so the change lands
    // immediately instead of waiting for the stack to pop.
    printer_switcher_observer_ = observe<int>(
        SettingsManager::instance().subject_show_printer_switcher(), this,
        [](NavigationManager* mgr, int /* shown */) { mgr->refresh_overlay_backdrop(); },
        SettingsManager::instance().get_subjects_lifetime());

    // A live theme or mode switch from inside an overlay (Settings > Appearance)
    // repaints the navbar widget the snapshot hides, so re-take it too. The
    // change subject fires after the repaint, so the new shot shows the new mode.
    // It also fires on subscription and on re-applies of the same palette, which
    // change no pixel.
    theme_observer_ = observe<int>(
        theme_manager_get_changed_subject(), this,
        [](NavigationManager* mgr, int /* generation */) {
            if (mgr->backdrop_.palette_changed())
                mgr->refresh_overlay_backdrop();
        },
        subject_never_freed());

    navbar_estop_.create(navbar);
    navbar_drying_.create(navbar);

    spdlog::trace(
        "[NavigationManager] Navigation button events wired (with connection/klippy gating)");
}

void NavigationManager::wire_status_icons(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_status_icons");
        return;
    }

    const char* button_names[] = {"status_btn_printer", "status_btn_network",
                                  "status_notification_icon"};
    const char* icon_names[] = {"status_printer_icon", "status_network_icon",
                                "status_notification_icon"};
    const int status_icon_count = 3;

    for (int i = 0; i < status_icon_count; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);
        lv_obj_t* icon_widget = lv_obj_find_by_name(navbar, icon_names[i]);

        if (!btn || !icon_widget) {
            spdlog::warn("[NavigationManager] Status icon {}: btn={}, icon={} (may not exist yet)",
                         button_names[i], (void*)btn, (void*)icon_widget);
            continue;
        }

        lv_obj_add_flag(icon_widget, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(icon_widget, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        spdlog::trace("[NavigationManager] Status icon {} wired", button_names[i]);
    }
}

void NavigationManager::set_active(PanelId panel_id) {
    if (static_cast<int>(panel_id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID: {}", static_cast<int>(panel_id));
        return;
    }

    if (panel_id == active_panel_) {
        return;
    }

#if defined(HELIX_PLATFORM_ESP32)
    NavTransitionScrim scrim_guard(nav_scrim_active_,
                                   panels_.needs_build(static_cast<int>(panel_id)));
#endif
    // A deferred panel must exist before the stack update and on_activate()
    // below: activation is what starts its data (PrintSelectPanel's file list).
    panels_.ensure_built(static_cast<int>(panel_id));

    PanelId old_panel = active_panel_;

    // Replace only the base panel and keep any overlays above it: closing an
    // overlay must reveal the panel it was opened over, not Home.
    if (lv_obj_t* new_base = panels_.widget(static_cast<int>(panel_id))) {
        if (panel_stack_.empty()) {
            // Stack is empty - just push the new panel
            panel_stack_.push_back(new_base);
            spdlog::trace("[NavigationManager] Panel stack initialized with panel {}",
                          static_cast<int>(panel_id));
        } else if (panel_stack_.size() == 1) {
            // Only base panel in stack - replace it
            panel_stack_[0] = new_base;
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}",
                          static_cast<int>(panel_id));
        } else {
            // Overlays are present - update base panel but preserve overlays
            // This handles the case where connection changes while an overlay is open
            panel_stack_[0] = new_base;
            spdlog::trace("[NavigationManager] Panel stack base updated to panel {}, "
                          "preserving {} overlays",
                          static_cast<int>(panel_id), panel_stack_.size() - 1);
        }
    }

    // Call on_deactivate() BEFORE state update
    if (panels_.instance(static_cast<int>(old_panel))) {
        spdlog::trace("[NavigationManager] Calling on_deactivate() for panel {}",
                      static_cast<int>(old_panel));
        panels_.instance(static_cast<int>(old_panel))
            ->on_deactivate(DeactivateReason::NavigateAway);
    }

    // Update state
    lv_subject_set_int(&active_panel_subject_, static_cast<int>(panel_id));
    active_panel_ = panel_id;
    // Publish for off-main memory_warning context (relaxed: telemetry only).
    helix::telemetry_context::active_panel_int.store(static_cast<int>(panel_id),
                                                     std::memory_order_relaxed);

    // Crash-diagnostic breadcrumb: records which panel transition was in flight
    // if we crash during on_activate/layout/first-paint.
    {
        const char* name = panels_.instance(static_cast<int>(panel_id))
                               ? panels_.instance(static_cast<int>(panel_id))->get_name()
                               : nullptr;
        crash_handler::breadcrumb::note("nav", name ? name : "", static_cast<long>(panel_id));
    }

    // Call on_activate() AFTER state update. This runs even when an overlay is
    // still covering the panel (the connection-change path), so it settles the
    // activation debt switch_to_panel_impl() would otherwise pay later.
    main_panel_deactivated_for_overlay_ = false;
    if (panels_.instance(static_cast<int>(panel_id))) {
        spdlog::trace("[NavigationManager] Calling on_activate() for panel {}",
                      static_cast<int>(panel_id));
        panels_.instance(static_cast<int>(panel_id))->on_activate();
    }

    if (lv_obj_t* root = get_panel_widget(panel_id)) {
        helix::ui::PageScrollAutoInject::instance().on_root_shown(root);
    }
}

PanelId NavigationManager::get_active() const {
    return active_panel_;
}

std::vector<std::string> NavigationManager::overlay_stack_names() const {
    std::vector<std::string> names;
    // panel_stack_[0] is the base panel; entries above it are pushed overlays.
    for (size_t i = 1; i < panel_stack_.size(); ++i) {
        lv_obj_t* w = panel_stack_[i];
        if (!w) {
            continue;
        }
        char buf[128];
        lv_obj_get_name_resolved(w, buf, sizeof(buf));
        names.emplace_back(buf[0] != '\0' ? buf : "overlay");
    }
    return names;
}

void NavigationManager::set_panels(lv_obj_t** panels) {
    if (!panels) {
        spdlog::error("[NavigationManager] NULL panels array provided");
        return;
    }

    panels_.set_widgets(panels);
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        // The panel layer owns these trees; nothing tells nav when one dies.
        // The slot clears itself, but panel_stack_ holds the widget too and
        // needs the scrub.
        // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
        ensure_delete_hook(panels_.widget(i));
    }

    // Hide all panels except active one
    panels_.show_only(static_cast<int>(active_panel_));

    // Initialize panel stack
    panel_stack_.clear();
    if (lv_obj_t* active_widget = panels_.widget(static_cast<int>(active_panel_))) {
        panel_stack_.push_back(active_widget);
        spdlog::trace("[NavigationManager] Panel stack initialized with active panel {}",
                      (void*)active_widget);
    }

    spdlog::trace("[NavigationManager] Panel widgets registered for show/hide management");
}

void NavigationManager::register_panel_instance(PanelId id, PanelBase* panel) {
    if (static_cast<int>(id) >= UI_PANEL_COUNT) {
        spdlog::error("[NavigationManager] Invalid panel ID for registration: {}",
                      static_cast<int>(id));
        return;
    }
    panels_.set_instance(static_cast<int>(id), panel);
    spdlog::trace("[NavigationManager] Registered panel instance for ID {}", static_cast<int>(id));
}

helix::PanelId NavigationManager::find_panel_id(const PanelBase* panel) const {
    return panels_.find(panel);
}

void NavigationManager::replace_panel_widget(helix::PanelId id, lv_obj_t* new_widget) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= UI_PANEL_COUNT)
        return;
    // An open overlay keeps the main panel beneath it in panel_stack_; go_back()
    // must reveal the successor, not the widget it displaced (#1294).
    if (lv_obj_t* displaced = panels_.replace_widget(idx, new_widget))
        std::replace(panel_stack_.begin(), panel_stack_.end(), displaced, new_widget);
    // The successor needs its own scrub hook for panel_stack_; the outgoing
    // widget's does not transfer. Same reasoning as rekey_overlay_widget().
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    ensure_delete_hook(new_widget);
    spdlog::debug("[NavigationManager] Panel widget for {} swapped to {}", panel_id_to_name(id),
                  (void*)new_widget);
}

void NavigationManager::set_deferred_panel_builder(std::function<void(int)> builder) {
    panels_.set_deferred_builder(std::move(builder));
}

lv_obj_t* NavigationManager::get_panel_widget(helix::PanelId id) const {
    return panels_.widget(static_cast<int>(id));
}

void NavigationManager::rekey_overlay_widget(lv_obj_t* old_widget, lv_obj_t* new_widget) {
    if (!old_widget || !new_widget || old_widget == new_widget)
        return;

    auto rekey_life = [&](std::unordered_map<lv_obj_t*, IPanelLifecycle*>& m) {
        auto it = m.find(old_widget);
        if (it != m.end()) {
            auto* inst = it->second;
            m.erase(it);
            m[new_widget] = inst;
        }
    };
    rekey_life(overlay_instances_);
    rekey_life(persistent_overlay_instances_);

    backdrop_.rekey(old_widget, new_widget);

    auto cb_it = overlay_close_callbacks_.find(old_widget);
    if (cb_it != overlay_close_callbacks_.end()) {
        auto cb = std::move(cb_it->second);
        overlay_close_callbacks_.erase(cb_it);
        overlay_close_callbacks_[new_widget] = std::move(cb);
    }

    auto wc_it = overlay_is_destination_.find(old_widget);
    if (wc_it != overlay_is_destination_.end()) {
        bool cls = wc_it->second;
        overlay_is_destination_.erase(wc_it);
        overlay_is_destination_[new_widget] = cls;
    }

    std::replace(panel_stack_.begin(), panel_stack_.end(), old_widget, new_widget);

    // Roots cached before this or an earlier rebuild now resolve to new_widget.
    for (auto& [freed, successor] : rebuilt_overlays_) {
        if (successor == old_widget) {
            successor = new_widget;
        }
    }
    rebuilt_overlays_[old_widget] = new_widget;
    // Its delete must be seen, to tell it from a later object at its address.
    if (lv_obj_is_valid(old_widget)) {
        condemned_roots_.insert(old_widget);
        ensure_delete_hook(old_widget);
    }

    // The new widget needs its own delete hook — the old widget's hook does not
    // transfer (it fires for the old object only).
    ensure_delete_hook(new_widget);

    spdlog::debug("[NavigationManager] Rekeyed overlay widget {} → {}", (void*)old_widget,
                  (void*)new_widget);
}

lv_obj_t* NavigationManager::resolve_rebuilt(lv_obj_t* widget) const {
    auto it = widget ? rebuilt_overlays_.find(widget) : rebuilt_overlays_.end();
    if (it == rebuilt_overlays_.end()) {
        return widget;
    }
    // A live object that is not the condemned root is a new object reusing a
    // freed root's address. lv_obj_is_valid() reads no freed memory.
    if (condemned_roots_.count(widget) || !lv_obj_is_valid(widget)) {
        return it->second;
    }
    return widget;
}

lv_obj_t* NavigationManager::resolve_arriving(lv_obj_t* widget) {
    lv_obj_t* resolved = resolve_rebuilt(widget);
    if (resolved == widget && widget) {
        rebuilt_overlays_.erase(widget);
    }
    return resolved;
}

void NavigationManager::set_overlay_width_unmanaged(lv_obj_t* overlay) {
    if (overlay) {
        overlay_width_unmanaged_.insert(overlay);
    }
}

bool NavigationManager::apply_overlay_width(lv_obj_t* overlay, bool is_first_overlay) {
    // Deliberate custom width — not one of the two navigation classes.
    if (overlay_width_unmanaged_.count(overlay)) {
        return false;
    }

    // A promotion declared on the panel class travels with the panel, so a
    // long-dwell screen reachable from several places (AmsPanel: Home, Printer
    // Manager, AMS Overview) is full width from all of them.
    auto* lifecycle = resolve_overlay_lifecycle(overlay);
    const helix::OverlayClass requested = (lifecycle && lifecycle->is_destination())
                                              ? helix::OverlayClass::Destination
                                              : helix::OverlayClass::Inherit;

    // panel_stack_.back() is still the widget beneath this one — the caller has
    // not pushed yet. is_first_overlay guards the empty/root-only cases where
    // back() is the main panel rather than an overlay.
    bool parent_is_destination = false;
    if (!is_first_overlay && !panel_stack_.empty()) {
        auto it = overlay_is_destination_.find(panel_stack_.back());
        if (it != overlay_is_destination_.end()) {
            parent_is_destination = it->second;
        }
    }

    const bool is_destination =
        helix::resolve_overlay_is_destination(requested, !is_first_overlay, parent_is_destination,
                                              helix::nav_root_is_destination(active_panel_));

    overlay_is_destination_[overlay] = is_destination;

    // Re-applied on EVERY push, not just at creation: OverlayBase caches its
    // root widget across show/hide cycles, and the same cached widget can be
    // reached from a transient parent one time and a destination parent the
    // next.
    ui_set_overlay_geometry(overlay, is_destination);

    // LV_STATE_USER_1 == "this is a transient layer". overlay_panel.xml hangs a
    // leading-edge treatment off it so the panel reads as something sitting ON
    // TOP of what is behind it, rather than as a seam between two pieces of
    // chrome — the reporter's actual objection in #1178. Only the state bit is
    // set here; what it looks like stays in XML.
    if (is_destination) {
        lv_obj_remove_state(overlay, LV_STATE_USER_1);
    } else {
        lv_obj_add_state(overlay, LV_STATE_USER_1);
    }

    spdlog::trace("[NavigationManager] Overlay {} width class: {}", (void*)overlay,
                  is_destination ? "destination" : "transient");
    return is_destination;
}

void NavigationManager::reapply_overlay_widths() {
    for (const auto& [overlay, is_destination] : overlay_is_destination_) {
        // Entries are erased on widget delete (scrub_deleted_widget), but guard
        // anyway — this runs from a display resize callback, outside the normal
        // push/pop ordering.
        if (lv_obj_is_valid(overlay)) {
            ui_set_overlay_geometry(overlay, is_destination);
        }
    }
    spdlog::debug("[NavigationManager] Re-applied width to {} overlay(s)",
                  overlay_is_destination_.size());
}

void NavigationManager::scrub_deleted_widget(lv_obj_t* widget) {
    if (!widget)
        return;

    // Erase from every widget-keyed bookkeeping container. Mirrors the canonical
    // list documented on rekey_overlay_widget(), but ERASES instead of rekeys.
    // Does NOT free anything — LVGL is mid-deletion and owns the memory. We only
    // drop the (now-dangling) references so the next push_overlay() can't deref
    // panel_stack_.back() after the memory is freed (bundle ZW6ATWSL).
    overlay_instances_.erase(widget);
    persistent_overlay_instances_.erase(widget);
    // Drop the backdrop map entry only — out of scope to delete the backdrop here.
    backdrop_.scrub(widget);
    overlay_close_callbacks_.erase(widget);
    overlay_is_destination_.erase(widget);
    overlay_width_unmanaged_.erase(widget);
    panel_stack_.erase(std::remove(panel_stack_.begin(), panel_stack_.end(), widget),
                       panel_stack_.end());

    delete_hooked_.erase(widget);
    // A replaced root's own deferred delete keeps its forwarding entry: that
    // key is the address callers still hold. Any other object deleted at a key
    // is a later tenant of the address, and a dead successor ends its entries.
    if (condemned_roots_.erase(widget) == 0) {
        rebuilt_overlays_.erase(widget);
        // A replaced root's pushes forward to its successor; any other dead
        // root's pushes have nothing left to show.
        pending_pushes_.erase(
            std::remove_if(pending_pushes_.begin(), pending_pushes_.end(),
                           [widget](const PendingPush& p) { return p.panel == widget; }),
            pending_pushes_.end());
    }
    for (auto it = rebuilt_overlays_.begin(); it != rebuilt_overlays_.end();) {
        it = it->second == widget ? rebuilt_overlays_.erase(it) : std::next(it);
    }

    spdlog::trace("[NavigationManager] Scrubbed deleted widget {} from nav bookkeeping",
                  (void*)widget);
}

void NavigationManager::overlay_delete_event_cb(lv_event_t* e) {
    if (NavigationManager::is_destroyed())
        return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    NavigationManager::instance().scrub_deleted_widget(target);
}

void NavigationManager::adopt_overlay_backdrop(lv_obj_t* screen, lv_obj_t* arriving) {
    backdrop_.adopt(screen, arriving, backdrop_click_event_cb);
}

void NavigationManager::set_navbar_estop_keyboard_top(int32_t top) {
    navbar_estop_.set_keyboard_top(top);
}

void NavigationManager::refresh_overlay_backdrop() {
    if (shutting_down_)
        return;
    backdrop_.refresh(app_layout_widget_, panel_stack_.empty() ? nullptr : panel_stack_.front());
}

void NavigationManager::ensure_delete_hook(lv_obj_t* widget) {
    if (!widget)
        return;
    if (delete_hooked_.count(widget))
        return;
    delete_hooked_.insert(widget);
    lv_obj_add_event_cb(widget, overlay_delete_event_cb, LV_EVENT_DELETE, nullptr);
}

void NavigationManager::rebuild_active_views() {
    if (shutting_down_) {
        spdlog::debug("[NavigationManager] rebuild_active_views: shutting down, skipping");
        return;
    }
    spdlog::info("[NavigationManager] Rebuilding active views for hot-reload");

    // Active main panel
    if (auto* p = panels_.instance(static_cast<int>(active_panel_))) {
        p->rebuild();
    }

    // All overlays — snapshot first because rebuild() mutates the maps via rekey.
    std::vector<IPanelLifecycle*> overlays;
    overlays.reserve(overlay_instances_.size() + persistent_overlay_instances_.size());
    for (auto& [w, inst] : overlay_instances_) {
        if (inst)
            overlays.push_back(inst);
    }
    for (auto& [w, inst] : persistent_overlay_instances_) {
        if (inst)
            overlays.push_back(inst);
    }
    for (auto* inst : overlays) {
        inst->rebuild();
    }

    // Top modal — rebuild via Modal::rebuild_top()
    if (ModalStack::instance().top_dialog()) {
        Modal::rebuild_top();
    }
}

void NavigationManager::activate_initial_panel() {
    if (panels_.instance(static_cast<int>(active_panel_))) {
        spdlog::trace("[NavigationManager] Activating initial panel {}",
                      static_cast<int>(active_panel_));
        panels_.instance(static_cast<int>(active_panel_))->on_activate();
    }

    if (lv_obj_t* root = get_panel_widget(active_panel_)) {
        helix::ui::PageScrollAutoInject::instance().on_root_shown(root);
    }
}

void NavigationManager::suspend_active(DeactivateReason reason) {
    if (suspended_) {
        return;
    }
    suspended_ = true;

    // Deactivate whatever is currently visible — topmost overlay or main panel
    if (panel_stack_.size() > 1) {
        lv_obj_t* top_overlay = panel_stack_.back();
        auto it = overlay_instances_.find(top_overlay);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::debug("[NavigationManager] Suspending overlay {}", it->second->get_name());
            it->second->on_deactivate(reason);
        }
    } else if (panels_.instance(static_cast<int>(active_panel_))) {
        spdlog::debug("[NavigationManager] Suspending panel {}", static_cast<int>(active_panel_));
        panels_.instance(static_cast<int>(active_panel_))->on_deactivate(reason);
    }
}

void NavigationManager::resume_active() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;

    // Re-activate whatever is currently visible
    if (panel_stack_.size() > 1) {
        lv_obj_t* top_overlay = panel_stack_.back();
        auto it = overlay_instances_.find(top_overlay);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::debug("[NavigationManager] Resuming overlay {}", it->second->get_name());
            it->second->on_activate();
        }
    } else if (panels_.instance(static_cast<int>(active_panel_))) {
        spdlog::debug("[NavigationManager] Resuming panel {}", static_cast<int>(active_panel_));
        panels_.instance(static_cast<int>(active_panel_))->on_activate();
    }
}

void NavigationManager::register_overlay_instance(lv_obj_t* widget, IPanelLifecycle* overlay,
                                                  bool persistent) {
    widget = resolve_arriving(widget);
    if (!widget) {
        spdlog::error("[NavigationManager] Cannot register overlay with NULL widget");
        return;
    }
    ensure_delete_hook(widget);
    overlay_instances_[widget] = overlay;
    if (persistent) {
        persistent_overlay_instances_[widget] = overlay;
    }
    if (overlay) {
        spdlog::trace("[NavigationManager] Registered overlay instance {} for widget {}"
                      " (persistent={})",
                      overlay->get_name(), (void*)widget, persistent);
    } else {
        spdlog::trace("[NavigationManager] Registered overlay widget {} (no lifecycle)",
                      (void*)widget);
    }
}

void NavigationManager::unregister_overlay_instance(lv_obj_t* widget) {
    widget = resolve_rebuilt(widget);
    auto it = overlay_instances_.find(widget);
    if (it != overlay_instances_.end()) {
        spdlog::trace("[NavigationManager] Unregistered overlay instance for widget {}",
                      (void*)widget);
        overlay_instances_.erase(it);
    }
    persistent_overlay_instances_.erase(widget);
}

IPanelLifecycle* NavigationManager::resolve_overlay_lifecycle(lv_obj_t* overlay_panel) {
    auto it = overlay_instances_.find(overlay_panel);
    if (it == overlay_instances_.end()) {
        // Check persistent map — overlay may have survived a panel switch
        auto pit = persistent_overlay_instances_.find(overlay_panel);
        if (pit != persistent_overlay_instances_.end()) {
            overlay_instances_[overlay_panel] = pit->second;
            spdlog::trace("[NavigationManager] Restored persistent overlay {} registration",
                          (void*)overlay_panel);
            return pit->second;
        }
        return nullptr;
    }
    return it->second;
}

void NavigationManager::push_overlay(lv_obj_t* overlay_panel, bool hide_previous) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot push NULL overlay panel");
        return;
    }

    const uint64_t serial = ++next_push_serial_;
    pending_pushes_.push_back({overlay_panel, serial});
    // Its delete hook is what cancels the push if the root dies first.
    if (lv_obj_is_valid(overlay_panel)) {
        ensure_delete_hook(overlay_panel);
    }
    // Always queue - this is the safest pattern for overlay operations
    // which can be triggered from various contexts (events, observers, etc.)
    helix::ui::queue_update("NavigationManager::push_overlay", [overlay_panel, serial,
                                                                hide_previous]() mutable {
        auto& pending = NavigationManager::instance().pending_pushes_;
        auto entry = std::find_if(pending.begin(), pending.end(),
                                  [serial](const PendingPush& p) { return p.serial == serial; });
        if (entry == pending.end()) {
            spdlog::debug("[NavigationManager] push_overlay: push of {} was cancelled",
                          (void*)overlay_panel);
            return;
        }
        pending.erase(entry);
        // Resolved when the push runs, on the UI thread: a rebuild can land
        // between the queueing and now.
        overlay_panel = NavigationManager::instance().resolve_arriving(overlay_panel);
        // The captured overlay_panel is a raw lv_obj_t* — it can be destroyed
        // between queue time and now (rapid push→teardown, e.g. a print that
        // fails Klipper config validation and immediately tears its status
        // overlay back down before this deferred push drains). Dereferencing it
        // below (lv_obj_get_screen) would be a use-after-free. lv_obj_is_valid
        // searches the display tree for the pointer instead of dereferencing
        // it, so it is safe to call on a freed pointer. (bundle MBUX7WUN)
        if (!lv_obj_is_valid(overlay_panel)) {
            spdlog::warn("[NavigationManager] push_overlay: target {} destroyed before "
                         "deferred push ran; skipping",
                         (void*)overlay_panel);
            return;
        }

        auto& mgr = NavigationManager::instance();

        // Check for duplicate push
        if (std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), overlay_panel) !=
            mgr.panel_stack_.end()) {
            spdlog::warn("[NavigationManager] Overlay {} already in stack, ignoring duplicate push",
                         (void*)overlay_panel);
            return;
        }

        // <= 1 (not == 1) so an empty stack also counts as "first": the else
        // branch below derefs panel_stack_.back(), which is UB on an empty
        // vector. Production never pushes onto a truly empty stack (slot 0
        // always holds the active main panel), but a defensive guard here keeps
        // the empty case correct instead of undefined.
        bool is_first_overlay = (mgr.panel_stack_.size() <= 1);

        // Track overlay opens for telemetry panel_usage event.
        // Breadcrumb distinguishes three cases so crash analysis can tell
        // an intentional callback-based overlay from a missing-registration bug:
        //   - lifecycle present  -> use IPanelLifecycle::get_name()
        //   - registered w/ null -> "anon" (function-based, e.g. keypad,
        //                          notification, theme_explorer, factory_reset)
        //   - not registered     -> "unreg" (caller forgot register_overlay_instance)
        auto inst_it = mgr.overlay_instances_.find(overlay_panel);
        bool registered = inst_it != mgr.overlay_instances_.end() ||
                          mgr.persistent_overlay_instances_.count(overlay_panel) > 0;
        auto* lc = mgr.resolve_overlay_lifecycle(overlay_panel);
        std::string overlay_name = lc ? lc->get_name() : (registered ? "anon" : "unreg");
        if (!registered) {
            spdlog::warn("[NavigationManager] push_overlay({}): no register_overlay_instance "
                         "call before push — overlay invisible to lifecycle machinery",
                         (void*)overlay_panel);
#ifndef HELIX_RELEASE_BUILD
            // Strict mode (CI / test fixtures): abort so any new unregistered
            // push fails the build. Print loudly to stderr so the reason is
            // visible even at warn log level. Compiled out in release builds —
            // users only ever get the warning above + the "unreg" telemetry
            // breadcrumb below. Intentional lifecycle-less overlays register
            // with a null lifecycle and so never reach this branch.
            if (overlay_registration_strict()) {
                std::fprintf(stderr,
                             "\n[NavigationManager] STRICT MODE: push_overlay(%p) with no "
                             "register_overlay_instance() before push. Call "
                             "register_overlay_instance(widget, lifecycle) — or "
                             "register_overlay_instance(widget, nullptr) for an intentional "
                             "lifecycle-less overlay. See include/ui_nav_manager.h.\n",
                             (void*)overlay_panel);
                std::abort();
            }
#endif
        }
        TelemetryManager::instance().notify_overlay_opened(overlay_name);
        crash_handler::breadcrumb::note("overlay+", overlay_name.c_str());

        // Lifecycle: Deactivate what's currently visible before showing new overlay
        if (is_first_overlay) {
            // Deactivate main panel when first overlay covers it
            mgr.main_panel_deactivated_for_overlay_ = true;
            if (mgr.panels_.instance(static_cast<int>(mgr.active_panel_))) {
                spdlog::trace("[NavigationManager] Deactivating main panel {} for overlay",
                              static_cast<int>(mgr.active_panel_));
                mgr.panels_.instance(static_cast<int>(mgr.active_panel_))
                    ->on_deactivate(DeactivateReason::NavigateAway);
            }
        } else {
            // Deactivate previous overlay if stacking
            lv_obj_t* prev_overlay = mgr.panel_stack_.back();
            auto it = mgr.overlay_instances_.find(prev_overlay);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating previous overlay {}",
                              it->second->get_name());
                it->second->on_deactivate(DeactivateReason::NavigateAway);
            }
        }

        // Create snapshot-darkened backdrop BEFORE hiding — snapshot must capture
        // the visible content, not a blank screen.
        lv_obj_t* screen = lv_obj_get_screen(overlay_panel);
        if (screen && is_first_overlay) {
            mgr.adopt_overlay_backdrop(screen, overlay_panel);
        }

        // Resolve and apply the width class before the overlay becomes visible,
        // while panel_stack_.back() is still the widget beneath it. #1178
        const bool is_destination = mgr.apply_overlay_width(overlay_panel, is_first_overlay);

        // Optionally hide current top panel (after snapshot). A dim layer is
        // translucent, so beside a transient overlay the base panel stays drawn
        // through it; a destination overlay covers it, and drawing it there
        // would cost frames for nothing.
        const bool base_shows_through =
            is_first_overlay && !is_destination && mgr.backdrop_.primary_is_dim_layer();
        if (hide_previous && !mgr.panel_stack_.empty() && !base_shows_through) {
            lv_obj_t* current_top = mgr.panel_stack_.back();
            lv_obj_add_flag(current_top, LV_OBJ_FLAG_HIDDEN);
        }

        // Show overlay
        lv_obj_remove_flag(overlay_panel, LV_OBJ_FLAG_HIDDEN);
        helix::ui::bring_to_front(overlay_panel);
        // Claim taps landing anywhere within the panel bounds. Overlays are
        // narrower than the screen and sit in front of a full-screen, clickable
        // dismiss-backdrop. Without this, a touch that misses an interactive
        // child (a gap between buttons, padding) falls through to the backdrop
        // and dismisses the whole overlay (#1066).
        lv_obj_add_flag(overlay_panel, LV_OBJ_FLAG_CLICKABLE);
        mgr.ensure_delete_hook(overlay_panel);
        mgr.panel_stack_.push_back(overlay_panel);
        mgr.overlay_animate_slide_in(overlay_panel);

        // Lifecycle: Activate new overlay
        auto* lifecycle = mgr.resolve_overlay_lifecycle(overlay_panel);
        if (!lifecycle) {
            // Only warn if truly unregistered — overlays registered with nullptr
            // lifecycle (e.g. keypad) are intentionally function-based.
            bool registered = mgr.overlay_instances_.count(overlay_panel) ||
                              mgr.persistent_overlay_instances_.count(overlay_panel);
            if (!registered) {
                spdlog::warn("[NavigationManager] Overlay {} pushed without lifecycle registration",
                             (void*)overlay_panel);
            }
        } else {
            spdlog::trace("[NavigationManager] Activating overlay {}", lifecycle->get_name());
            lifecycle->on_activate();
        }

        helix::ui::PageScrollAutoInject::instance().on_root_shown(overlay_panel);

        SoundManager::instance().play("nav_forward");
        spdlog::trace("[NavigationManager] Pushed overlay {} (stack: {})", (void*)overlay_panel,
                      mgr.panel_stack_.size());
    });
}

void NavigationManager::register_overlay_close_callback(lv_obj_t* overlay_panel,
                                                        OverlayCloseCallback callback) {
    overlay_panel = resolve_arriving(overlay_panel);
    if (!overlay_panel || !callback) {
        return;
    }
    overlay_close_callbacks_[overlay_panel] = std::move(callback);
    spdlog::trace("[NavigationManager] Registered close callback for overlay {}",
                  (void*)overlay_panel);
}

void NavigationManager::unregister_overlay_close_callback(lv_obj_t* overlay_panel) {
    auto it = overlay_close_callbacks_.find(overlay_panel);
    if (it != overlay_close_callbacks_.end()) {
        overlay_close_callbacks_.erase(it);
        spdlog::trace("[NavigationManager] Unregistered close callback for overlay {}",
                      (void*)overlay_panel);
    }
}

bool NavigationManager::go_back() {
    // In queue order the newest pending push lands first and this pops it, so
    // that pair cancels out. A duplicate push of a stacked overlay is a no-op
    // when it lands, so the pop still has to run.
    if (!pending_pushes_.empty()) {
        lv_obj_t* newest = pending_pushes_.back().panel;
        if (!is_panel_in_stack(newest)) {
            cancel_pending_push(newest);
            return true;
        }
    }
    helix::ui::queue_update("NavigationManager::go_back",
                            []() { NavigationManager::instance().go_back_now(); });
    return true;
}

void NavigationManager::close_overlay(lv_obj_t* overlay_panel) {
    if (!overlay_panel) {
        spdlog::error("[NavigationManager] Cannot close NULL overlay panel");
        return;
    }
    cancel_pending_push(overlay_panel);
    helix::ui::queue_update("NavigationManager::close_overlay", [overlay_panel]() {
        // Decided here, in queue order: pushes queued ahead of this operation
        // have landed by now, so "on top" means what the user actually sees,
        // not what was on top when the caller asked.
        lv_obj_t* root = NavigationManager::instance().resolve_rebuilt(overlay_panel);
        if (!root || !lv_obj_is_valid(root)) {
            return; // deleted before this operation ran
        }
        auto& mgr = NavigationManager::instance();
        auto it = std::find(mgr.panel_stack_.begin(), mgr.panel_stack_.end(), root);
        if (it == mgr.panel_stack_.end()) {
            return; // already left the stack some other way
        }
        if (mgr.panels_.is_main_panel(root)) {
            return; // a main panel is not an overlay to close
        }
        if (it == mgr.panel_stack_.end() - 1) {
            mgr.go_back_now(); // on top: normal pop with restore path
            return;
        }
        // Buried: drop it from the stack and retire it without disturbing the
        // overlay that covers it (it is already hidden).
        mgr.panel_stack_.erase(it);
        mgr.retire_overlay(root);
    });
}

void NavigationManager::go_back_now() {
    auto& mgr = NavigationManager::instance();
    {
        spdlog::trace("[NavigationManager] go_back executing, stack depth: {}",
                      mgr.panel_stack_.size());
        crash_handler::breadcrumb::note("nav", "go_back",
                                        static_cast<long>(mgr.panel_stack_.size()));

        // L081 Mech D defense: cancel in-flight pointer input before teardown.
        // Stale clicks queued in indev would otherwise dispatch to widgets the
        // overlay-pop is about to destroy.
        lv_indev_reset(nullptr, nullptr);
        crash_handler::breadcrumb::note("indev_rst", "go_back", 0);
        spdlog::debug("[NavigationManager] indev_rst:go_back");

        // Dismiss keyboard before navigation to restore screen position
        if (KeyboardManager::instance().is_visible()) {
            KeyboardManager::instance().hide();
        }

        lv_obj_t* current_top = mgr.panel_stack_.empty() ? nullptr : mgr.panel_stack_.back();

        // Check if current top is an overlay
        const bool is_overlay = current_top && !mgr.panels_.is_main_panel(current_top);

        // Lifecycle: Deactivate the closing overlay before animation
        if (is_overlay && current_top) {
            // Remove overlay from focus group BEFORE closing to prevent LVGL from
            // auto-focusing the next element (which triggers scroll-on-focus)
            lv_group_t* group = lv_group_get_default();
            if (group) {
                lv_group_remove_obj(current_top);
            }

            auto it = mgr.overlay_instances_.find(current_top);
            if (it != mgr.overlay_instances_.end() && it->second) {
                spdlog::trace("[NavigationManager] Deactivating closing overlay {}",
                              it->second->get_name());
                it->second->on_deactivate(DeactivateReason::NavigateAway);
            }

            // Arm the exactly-once activation latch for this close. Consumed
            // below (after the restored panel is un-hidden) regardless of which
            // animation path ran, so the panel is activated exactly once —
            // on_activate() handlers are not all idempotent (PrintSelectPanel's
            // Print-Last counter, FirstRunTour::maybe_start).
            mgr.restore_activation_pending_ = true;
        }

        // Pop stack and clean up backdrop BEFORE animation — the no-animation path
        // and the animation completion callback both expect the stack to already
        // reflect the post-pop state when activating the previous panel/overlay.
        if (!mgr.panel_stack_.empty()) {
            lv_obj_t* popped = mgr.panel_stack_.back();
            mgr.panel_stack_.pop_back();
            mgr.backdrop_.retire(popped);
        }

        // Determine the previous panel (what will be visible after pop)
        lv_obj_t* previous_panel = mgr.panel_stack_.empty() ? nullptr : mgr.panel_stack_.back();

        // Animate out if overlay
        if (is_overlay && current_top) {
            mgr.overlay_animate_slide_out(current_top);
            SoundManager::instance().play("nav_back");
        }

        // Hide stale overlays (but skip current_top, previous panel, and system widgets)
        lv_obj_t* screen = lv_screen_active();
        if (screen) {
            for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
                lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
                if (child == mgr.app_layout_widget_ || child == mgr.backdrop_.primary() ||
                    child == current_top || child == previous_panel ||
                    helix::ui::is_screen_chrome(child)) {
                    continue;
                }
                if (!mgr.panels_.is_main_panel(child) &&
                    !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                    reset_overlay_transform(child);
                }
            }
        }

        // Destroy backdrop if no more overlays
        if (mgr.panel_stack_.size() <= 1) {
            mgr.backdrop_.release_primary();
        }

        // Fallback to home if empty
        if (mgr.panel_stack_.empty()) {
            spdlog::trace("[NavigationManager] go_back stack empty, falling back to HOME");
            mgr.panels_.show_only(-1);
            if (lv_obj_t* home = mgr.panels_.widget(static_cast<int>(PanelId::Home))) {
                lv_obj_remove_flag(home, LV_OBJ_FLAG_HIDDEN);
                mgr.panel_stack_.push_back(home);
                mgr.active_panel_ = PanelId::Home;
                lv_subject_set_int(&mgr.active_panel_subject_, static_cast<int>(PanelId::Home));
            }
            mgr.activate_restored_target();
            return;
        }

        // Show previous panel
        lv_obj_t* prev = mgr.panel_stack_.back();
        const int prev_idx = mgr.panels_.index_of(prev);
        if (prev_idx >= 0) {
            mgr.panels_.show_only(prev_idx);
            mgr.active_panel_ = static_cast<PanelId>(prev_idx);
            lv_subject_set_int(&mgr.active_panel_subject_, prev_idx);
        }
        lv_obj_remove_flag(prev, LV_OBJ_FLAG_HIDDEN);

        // Lifecycle: re-activate what the close restored — the main panel or the
        // overlay beneath. Runs LAST, after the un-hide above, so an
        // on_activate() that navigates away (set_active) cannot be undone by it,
        // and runs unconditionally so live resources restart even when the
        // close animation's completion callback never fires (on Android the
        // overlay widget can be freed first, so that callback bails at its
        // lv_obj_is_valid check and the camera stayed dead until a tab
        // switch — #1245). The latch makes it exactly once per close.
        mgr.activate_restored_target();
    }
}

bool NavigationManager::is_push_pending(lv_obj_t* panel) const {
    return std::any_of(pending_pushes_.begin(), pending_pushes_.end(),
                       [panel](const PendingPush& p) { return p.panel == panel; });
}

void NavigationManager::cancel_pending_push(lv_obj_t* panel) {
    lv_obj_t* root = resolve_rebuilt(panel);
    const auto before = pending_pushes_.size();
    pending_pushes_.erase(
        std::remove_if(pending_pushes_.begin(), pending_pushes_.end(),
                       [&](const PendingPush& p) { return resolve_rebuilt(p.panel) == root; }),
        pending_pushes_.end());
    if (pending_pushes_.size() == before) {
        return;
    }
    spdlog::debug("[NavigationManager] Cancelled pending push of overlay {}", (void*)root);
    // The caller sees an overlay that opened and closed: owners prime state in
    // show() and release it in on_deactivate() and the close callback, so both
    // run, in queue order like any close. A stacked overlay's pending push was
    // a duplicate, and its own close delivers both.
    helix::ui::queue_update("NavigationManager::cancel_pending_push", [root]() mutable {
        auto& mgr = NavigationManager::instance();
        root = mgr.resolve_rebuilt(root);
        if (!lv_obj_is_valid(root) || mgr.is_panel_in_stack(root)) {
            return;
        }
        if (auto* lifecycle = mgr.resolve_overlay_lifecycle(root)) {
            lifecycle->on_deactivate(DeactivateReason::NavigateAway);
        }
        mgr.retire_overlay(root);
    });
}

bool NavigationManager::is_panel_in_stack(lv_obj_t* panel) const {
    panel = resolve_rebuilt(panel);
    if (!panel) {
        return false;
    }
    return std::find(panel_stack_.begin(), panel_stack_.end(), panel) != panel_stack_.end();
}

bool NavigationManager::is_panel_on_top(lv_obj_t* panel) const {
    panel = resolve_rebuilt(panel);
    if (!panel || panel_stack_.empty()) {
        return false;
    }
    return panel_stack_.back() == panel;
}

bool NavigationManager::has_open_overlays() const {
    // Only check the panel stack — it tracks what's actually open/visible.
    // overlay_instances_ is a registration map (persistent overlays survive
    // panel switches) and must NOT be used here, or persistent registrations
    // cause this to return true even when nothing is visibly open.
    return panel_stack_.size() > 1;
}

void NavigationManager::shutdown() {
    spdlog::trace("[NavigationManager] Shutting down...");
    shutting_down_ = true;

    printer_badge_.shutdown();

    // Deactivate any overlays in the stack
    for (lv_obj_t* overlay_widget : panel_stack_) {
        auto it = overlay_instances_.find(overlay_widget);
        if (it != overlay_instances_.end() && it->second) {
            spdlog::trace("[NavigationManager] Deactivating overlay: {}", it->second->get_name());
            it->second->on_deactivate(DeactivateReason::Shutdown);
        }
    }

    // Clear overlay registry
    // Note: The actual panel objects are destroyed via StaticPanelRegistry,
    // we just clear our tracking references here
    overlay_instances_.clear();
    persistent_overlay_instances_.clear();

    panels_.clear_instances();

    // Clear panel stack
    panel_stack_.clear();
    pending_pushes_.clear();
    overlay_is_destination_.clear();
    overlay_width_unmanaged_.clear();

    spdlog::trace("[NavigationManager] Shutdown complete");
}

void NavigationManager::set_backdrop_visible(bool visible) {
    if (!subjects_initialized_) {
        spdlog::warn(
            "[NavigationManager] Subjects not initialized, cannot set backdrop visibility");
        return;
    }

    lv_subject_set_int(&overlay_backdrop_visible_subject_, visible ? 1 : 0);
    spdlog::trace("[NavigationManager] Overlay backdrop visibility set to: {}", visible);
}

void NavigationManager::set_printer_callbacks(PrinterSwitchCallback switch_cb,
                                              AddPrinterCallback add_cb) {
    printer_badge_.set_callbacks(std::move(switch_cb), std::move(add_cb));
}

void NavigationManager::trigger_printer_switch(const std::string& printer_id) {
    printer_badge_.trigger_switch(printer_id);
}

void NavigationManager::trigger_add_printer() {
    printer_badge_.trigger_add();
}

void NavigationManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Reset observer guards BEFORE deiniting subjects - they hold references
    // to subjects that will become invalid. Also handles observers attached
    // to external subjects (PrinterState) that may be reset separately.
    active_panel_observer_.reset();
    connection_state_observer_.reset();
    klippy_state_observer_.reset();

    subjects_.deinit_all();

    // Reset widget pointers - they become invalid when LVGL is reinitialized
    panels_.reset();
    overlay_instances_.clear();
    persistent_overlay_instances_.clear();
    overlay_close_callbacks_.clear();
    overlay_is_destination_.clear();
    overlay_width_unmanaged_.clear();
    delete_hooked_.clear();
    rebuilt_overlays_.clear();
    condemned_roots_.clear();
    panel_stack_.clear();
    pending_pushes_.clear();
    app_layout_widget_ = nullptr;
    backdrop_.reset();
    navbar_widget_ = nullptr;
    // The navbar buttons live on the screen, not in the app layout a printer
    // switch rebuilds, so they go explicitly or the rebuild leaves orphans.
    navbar_estop_.destroy();
    navbar_drying_.destroy();
    active_panel_ = PanelId::Home;
    previous_connection_state_ = -1;
    previous_klippy_state_ = -1;
    disconnect_expected_ = false;
    restore_activation_pending_ = false;
    main_panel_deactivated_for_overlay_ = false;

    // Allow re-initialization after soft restart (shutdown() sets this to true)
    shutting_down_ = false;

    subjects_initialized_ = false;
    spdlog::trace("[NavigationManager] Subjects deinitialized");
}

// ============================================================================
// helix::nav FORWARDS
// ============================================================================

namespace helix::nav {

void push_overlay(lv_obj_t* overlay, bool hide_previous) {
    NavigationManager::instance().push_overlay(overlay, hide_previous);
}

bool go_back() {
    return NavigationManager::instance().go_back();
}

void close_overlay(lv_obj_t* overlay) {
    NavigationManager::instance().close_overlay(overlay);
}

void register_overlay(lv_obj_t* widget, IPanelLifecycle* overlay, bool persistent) {
    NavigationManager::instance().register_overlay_instance(widget, overlay, persistent);
}

void unregister_overlay(lv_obj_t* widget) {
    if (!NavigationManager::is_destroyed()) {
        NavigationManager::instance().unregister_overlay_instance(widget);
    }
}

void on_close(lv_obj_t* overlay, OverlayCloseCallback callback) {
    NavigationManager::instance().register_overlay_close_callback(overlay, std::move(callback));
}

void clear_on_close(lv_obj_t* overlay) {
    if (!NavigationManager::is_destroyed()) {
        NavigationManager::instance().unregister_overlay_close_callback(overlay);
    }
}

void set_overlay_width_unmanaged(lv_obj_t* overlay) {
    NavigationManager::instance().set_overlay_width_unmanaged(overlay);
}

void set_active(PanelId panel_id) {
    NavigationManager::instance().set_active(panel_id);
}

void shutdown() {
    NavigationManager::instance().shutdown();
}

bool is_on_top(lv_obj_t* panel) {
    return NavigationManager::instance().is_panel_on_top(panel);
}

bool is_in_stack(lv_obj_t* panel) {
    return NavigationManager::instance().is_panel_in_stack(panel);
}

bool is_push_pending(lv_obj_t* panel) {
    return NavigationManager::instance().is_push_pending(panel);
}

bool is_showing(lv_obj_t* panel) {
    // lv_obj_is_valid() searches the tree rather than dereferencing, so a
    // pointer held past its widget's deletion is safe to pass.
    return panel && lv_obj_is_valid(panel) &&
           (is_in_stack(panel) || is_push_pending(panel) ||
            !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN));
}

} // namespace helix::nav
