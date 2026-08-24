// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_printer_switch_menu.h"

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations for lifecycle dispatch
class PanelBase;
class OverlayBase;
class IPanelLifecycle;

namespace helix {
/// Callback type for overlay close notifications
using OverlayCloseCallback = std::function<void()>;
} // namespace helix

/**
 * @brief Navigation panel identifiers
 *
 * Order matches app_layout.xml panel children for index-based access.
 */
namespace helix {
enum class PanelId {
    Home = 0,    ///< Panel 0: Home
    PrintSelect, ///< Panel 1: Print Select (beneath Home)
    Controls,    ///< Panel 2: Controls
    Filament,    ///< Panel 3: Filament
    Settings,    ///< Panel 4: Settings
    Advanced,    ///< Panel 5: Advanced
    Count        ///< Total number of panels
};

/**
 * @brief Whether overlays pushed from a nav root are destinations by default.
 *
 * Settings is the one root users navigate *within* rather than launch things
 * from: Settings > Network is a sub-screen of Settings, not a layer over it, so
 * it renders at destination width (iOS push semantics). Every other root
 * launches tools you return from, which get the gapped transient width.
 *
 * See include/overlay_class.h and prestonbrown/helixscreen#1178.
 */
constexpr bool nav_root_is_destination(PanelId id) {
    return id == PanelId::Settings;
}
} // namespace helix

// Legacy aliases for backward compatibility
constexpr int UI_PANEL_COUNT = static_cast<int>(helix::PanelId::Count);

/**
 * @brief Singleton manager for navigation and panel management
 *
 * Manages the navigation system including:
 * - Panel switching via navbar buttons
 * - Overlay panel stack with slide animations
 * - Backdrop visibility for modal dimming
 * - Connection gating (redirect to home when disconnected)
 *
 * Uses RAII observer guards for automatic cleanup and LVGL subjects
 * for reactive XML bindings.
 *
 * Usage:
 *   NavigationManager::instance().init();  // Before XML creation
 *   // Create XML...
 *   NavigationManager::instance().wire_events(navbar);
 *   NavigationManager::instance().set_panels(panel_widgets);
 */
class NavigationManager {
    friend class NavigationManagerTestAccess;

  public:
    /**
     * @brief Get singleton instance
     * @return Reference to the NavigationManager singleton
     */
    static NavigationManager& instance();

    /**
     * @brief Check if singleton has been destroyed
     *
     * Guards against Static Destruction Order Fiasco. During program shutdown,
     * static objects are destroyed in undefined order across translation units.
     * This allows destructors to safely skip operations that require the
     * NavigationManager singleton.
     *
     * @return true if singleton has been destroyed, false if still valid
     */
    static bool is_destroyed();

    // Non-copyable, non-movable (singleton)
    NavigationManager(const NavigationManager&) = delete;
    NavigationManager& operator=(const NavigationManager&) = delete;
    NavigationManager(NavigationManager&&) = delete;
    NavigationManager& operator=(NavigationManager&&) = delete;

    /**
     * @brief Initialize navigation system with reactive subjects
     *
     * Sets up reactive subjects for icon colors and panel visibility.
     * MUST be called BEFORE creating navigation bar XML.
     */
    void init();

    /**
     * @brief Initialize overlay backdrop widget
     *
     * Creates a shared backdrop widget used by all overlay panels.
     * Should be called after screen is available.
     *
     * @param screen Screen to add backdrop to
     */
    void init_overlay_backdrop(lv_obj_t* screen);

    /**
     * @brief Set app_layout widget reference
     *
     * Stores reference to prevent hiding app_layout when dismissing
     * overlay panels.
     *
     * @param app_layout Main application layout widget
     */
    void set_app_layout(lv_obj_t* app_layout);

    /**
     * @brief Wire up event handlers to navigation bar widget
     *
     * Attaches click handlers to navbar icons for panel switching.
     * Call this after creating navigation_bar component from XML.
     *
     * @param navbar Navigation bar widget created from XML
     */
    void wire_events(lv_obj_t* navbar);

    /**
     * @brief Wire up status icons in navbar
     *
     * Applies responsive scaling and theming to status icons.
     *
     * @param navbar Navigation bar widget containing status icons
     */
    void wire_status_icons(lv_obj_t* navbar);

    /**
     * @brief Set active panel
     *
     * Updates active panel state and triggers reactive icon color updates.
     * Also calls on_deactivate() on old panel and on_activate() on new panel
     * if C++ panel instances have been registered.
     *
     * @param panel_id Panel identifier to activate
     */
    void set_active(helix::PanelId panel_id);

    /// How the caller wants the panel switch itself run. An LVGL event callback
    /// must queue it — mutating widgets mid-render corrupts the draw. A caller
    /// already inside an UpdateQueue callback (RemoteControlServer's
    /// execute_on_ui_thread) is in exactly the context switch_to_panel_impl()
    /// is written for and runs it inline, so the state it changes is readable
    /// the moment the call returns.
    enum class SwitchDispatch { Queued, Inline };

    /// What request_panel() did, so a programmatic caller can tell a real
    /// switch from a request that was silently declined.
    enum class PanelRequest {
        Switched,              ///< The panel switch ran (or was queued)
        AlreadyActive,         ///< Already there with nothing stacked over it
        HomeRetapped,          ///< Already on Home — the carousel reset instead
        BlockedDisconnected,   ///< Panel needs a printer connection
        BlockedKlippyNotReady, ///< Panel needs Klipper ready
    };

    /**
     * @brief Do what tapping this panel's navbar button does
     *
     * The whole navbar-tap decision: the already-there special cases (a second
     * tap on Home resets the carousel), the connection/Klipper gating, and the
     * switch — which clears any open overlay stack, unlike set_active(), whose
     * job is to swap the base panel *underneath* whatever is stacked on it.
     *
     * Shared so `helix-screen ctl navigate` and a finger produce the same
     * result; they differ only in @p dispatch.
     */
    PanelRequest request_panel(helix::PanelId panel_id, SwitchDispatch dispatch);

    /**
     * @brief Register C++ panel instance for lifecycle callbacks
     *
     * Associates a PanelBase-derived instance with a panel ID. When panels
     * are switched via set_active(), the corresponding on_activate() and
     * on_deactivate() methods will be called automatically.
     *
     * @param id Panel identifier
     * @param panel Pointer to PanelBase-derived instance (may be nullptr)
     */
    void register_panel_instance(helix::PanelId id, PanelBase* panel);

    /**
     * @brief Find the PanelId owned by a given PanelBase instance
     *
     * Searches panel_instances_ for a matching pointer. Used by
     * hot-reload rebuild to let panels locate themselves without
     * needing per-subclass get_panel_id() overrides.
     *
     * @param panel Pointer to compare against registered instances
     * @return PanelId if found, PanelId::Count if not registered
     */
    helix::PanelId find_panel_id(const PanelBase* panel) const;

    /**
     * @brief Replace the cached widget pointer for a panel
     *
     * Used by hot-reload rebuild. Does NOT free the old widget;
     * caller is responsible for teardown (via safe_delete_deferred).
     *
     * @param id Panel identifier
     * @param new_widget New widget to register in panel_widgets_[id]
     */
    void replace_panel_widget(helix::PanelId id, lv_obj_t* new_widget);

    /**
     * @brief Get the cached widget pointer for a panel
     *
     * @param id Panel identifier
     * @return Widget pointer, or nullptr if id is out of range or panel
     *         not yet registered via set_panels()/replace_panel_widget()
     */
    lv_obj_t* get_panel_widget(helix::PanelId id) const;

    /**
     * @brief Register a builder for panels that are instantiated lazily.
     *
     * On the ESP32 firmware only the home panel is built at boot (app_layout
     * defers the rest — see build_deferred_panel). When navigation targets a
     * panel whose widget slot is still null, switch_to_panel_impl() invokes this
     * builder to construct it on first use. The builder must create the panel,
     * call setup(), and register it via replace_panel_widget() +
     * register_panel_instance() so the slot is filled before the switch shows it.
     * Never set on desktop (all panels are resident), so the deferred path is
     * inert there.
     *
     * @param builder Callable taking the panel id (int); empty to disable.
     */
    void set_deferred_panel_builder(std::function<void(int)> builder);

    /**
     * @brief Re-key overlay maps: swap old_widget → new_widget for the same lifecycle.
     *
     * Used by hot-reload overlay rebuild. Touches overlay_instances_,
     * persistent_overlay_instances_, overlay_backdrops_, overlay_close_callbacks_,
     * zoom_source_rects_, and panel_stack_. Does NOT free either widget —
     * caller handles old widget teardown.
     */
    void rekey_overlay_widget(lv_obj_t* old_widget, lv_obj_t* new_widget);

    /**
     * @brief Rebuild every on-screen widget tree (active panel + all overlays + top modal)
     *
     * Called by XmlHotReloader's after-reload callback. Iterates:
     * - the active main panel
     * - every overlay in overlay_instances_ (visible or hidden)
     * - every overlay in persistent_overlay_instances_
     * - the top modal (stubbed in Task 7; implemented in Task 8)
     *
     * Safe to call on the LVGL main thread only.
     */
    void rebuild_active_views();

    /**
     * @brief Activate the initial panel after all panels are registered
     *
     * Calls on_activate() on the current active panel. This should be called
     * once after all panel instances have been registered via register_panel_instance().
     * This is needed because set_panels() doesn't call on_activate() (instances
     * aren't registered yet at that point).
     */
    void activate_initial_panel();

    /**
     * @brief Register C++ overlay instance for lifecycle callbacks
     *
     * Associates an IPanelLifecycle-implementing instance with its root widget.
     * When overlays are pushed/popped, the corresponding on_activate() and
     * on_deactivate() methods will be called automatically.
     *
     * Call this after create() returns the overlay's root widget.
     *
     * @param widget Root widget of the overlay (from create())
     * @param overlay Pointer to IPanelLifecycle-implementing instance (OverlayBase or PanelBase)
     */
    void register_overlay_instance(lv_obj_t* widget, IPanelLifecycle* overlay,
                                   bool persistent = false);

    /**
     * @brief Enable strict overlay-registration checking (dev/test only).
     *
     * When enabled, push_overlay() on a widget that was never passed to
     * register_overlay_instance() (the "unreg" case — caller forgot to
     * register, so on_deactivate() will never fire on dismiss) aborts with a
     * diagnostic instead of merely logging a warning. Mirrors the L081
     * HELIX_STRICT_BG_THREAD_CHECK pattern: opt-in via the
     * HELIX_STRICT_OVERLAY_CHECK=1 env var or this setter; compiled out
     * entirely in release builds (HELIX_RELEASE_BUILD) so a stray opt-in can
     * never crash a user. HelixTestFixture enables it so any new unregistered
     * push fails the test suite. Intentional lifecycle-less overlays must
     * register with a null lifecycle (register_overlay_instance(widget,
     * nullptr)) to opt out — that is the "anon" case and is not flagged.
     */
    static void set_overlay_registration_strict(bool enabled) noexcept;

    /**
     * @brief Unregister C++ overlay instance
     *
     * Removes association between widget and overlay instance.
     * Call this before destroying an overlay.
     *
     * @param widget Root widget of the overlay
     */
    void unregister_overlay_instance(lv_obj_t* widget);

    /**
     * @brief Suspend the currently active panel/overlay lifecycle
     *
     * Calls on_deactivate() on whatever is currently visible (topmost overlay
     * or active main panel). Used by DisplayManager when the screensaver starts
     * to stop widget timers and prevent background redraws from bleeding through.
     *
     * Safe to call multiple times — tracks suspended state internally.
     */
    void suspend_active();

    /**
     * @brief Resume the previously suspended panel/overlay lifecycle
     *
     * Calls on_activate() on whatever is currently visible. Used by DisplayManager
     * when the screensaver stops to restart widget timers.
     *
     * No-op if not currently suspended.
     */
    void resume_active();

    /**
     * @brief Get current active panel
     * @return Currently active panel identifier
     */
    helix::PanelId get_active() const;

    /**
     * @brief Names of the overlays currently stacked on top of the base panel,
     *        bottom to top. Read-only; used by the remote-control breadcrumb.
     * @return Resolved widget names of panel_stack_ entries above the base.
     */
    std::vector<std::string> overlay_stack_names() const;

    /**
     * @brief Get the active panel subject for observation
     * @return Pointer to the LVGL subject tracking active panel ID
     */
    lv_subject_t* get_active_panel_subject() {
        return &active_panel_subject_;
    }

    /**
     * @brief Register panel widgets for show/hide management
     *
     * @param panels Array of panel widgets (size: UI_PANEL_COUNT)
     */
    void set_panels(lv_obj_t** panels);

    /**
     * @brief Push overlay panel onto navigation history stack
     *
     * Shows the overlay panel and pushes it onto history stack.
     *
     * @param overlay_panel Overlay panel widget to show
     * @param hide_previous If true (default), hide the previous panel. If false, keep it visible.
     */
    void push_overlay(lv_obj_t* overlay_panel, bool hide_previous = true);

    /**
     * @brief Push overlay with zoom-from-rect animation
     *
     * Shows the overlay panel with a zoom animation originating from the
     * source rectangle (e.g., a clicked card). Falls back to instant show
     * if animations are disabled.
     *
     * @param overlay_panel Overlay panel widget to show
     * @param source_rect Screen coordinates of the source element to zoom from
     */
    void push_overlay_zoom_from(lv_obj_t* overlay_panel, lv_area_t source_rect);

    /**
     * @brief Re-apply every live overlay's width after a resolution change
     *
     * Overlays cache their root widget across show/hide cycles, so a width
     * applied at push time goes stale when the canvas resizes (rotation, or an
     * Android navigation bar insetting the LVGL surface — #941). Each overlay's
     * resolved class is remembered, so this re-derives the pixel width from the
     * freshly-registered constants rather than guessing it back from the
     * current width. Call after theme_manager_refresh_layout_constants(). #1178
     */
    void reapply_overlay_widths();

    /**
     * @brief Exempt an overlay from push-time width management
     *
     * For overlays whose width is a deliberate design choice rather than one of
     * the two navigation classes — widget_catalog_overlay is 70% so the grid
     * stays visible behind it while you drag a widget out of the list. Call
     * once after creating the widget; push_overlay() then leaves its width
     * alone. #1178
     */
    void set_overlay_width_unmanaged(lv_obj_t* overlay);

    /**
     * @brief Register a callback to be called when an overlay is closed
     *
     * The callback is invoked when the overlay is popped from the stack
     * (via go_back or backdrop click). Useful for cleanup like freeing memory.
     *
     * @param overlay_panel The overlay panel to monitor
     * @param callback Function to call when the overlay closes
     */
    void register_overlay_close_callback(lv_obj_t* overlay_panel,
                                         helix::OverlayCloseCallback callback);

    /**
     * @brief Remove a registered close callback for an overlay
     *
     * @param overlay_panel The overlay panel to stop monitoring
     */
    void unregister_overlay_close_callback(lv_obj_t* overlay_panel);

    /**
     * @brief Navigate back to previous panel
     *
     * @return true if navigation occurred, false if history empty
     */
    bool go_back();

    /**
     * @brief Check if a panel is in the overlay stack
     *
     * Used to determine if a specific panel (like PrintStatusPanel) is currently
     * visible as an overlay.
     *
     * @param panel Panel widget to check for
     * @return true if panel is in the overlay stack
     */
    bool is_panel_in_stack(lv_obj_t* panel) const;

    /**
     * @brief Check if a panel is the top of the overlay stack
     *
     * Stronger than is_panel_in_stack(): answers "will go_back() pop THIS
     * panel?". Any deferred callback that navigates on behalf of a screen it
     * pushed earlier must ask this first — by the time it runs, the user may
     * have navigated on, and a blind go_back() would pop whatever they are
     * looking at now (#1221).
     *
     * @param panel Panel widget to check for
     * @return true if panel is the topmost entry in the overlay stack
     */
    bool is_panel_on_top(lv_obj_t* panel) const;

    /**
     * @brief Check if any overlays are currently open
     * @return true if there are overlay panels on the stack
     */
    bool has_open_overlays() const;

    /**
     * @brief Consume the "on-screen keyboard was visible when the dismiss-
     * backdrop was pressed" latch.
     *
     * Overlays sit over a full-screen clickable dismiss-backdrop; a tap that
     * lands on the backdrop while the keyboard is up should dismiss the keyboard
     * only, not pop the overlay behind it. LVGL fires LV_EVENT_PRESSED before the
     * click-focus DEFOCUS that hides the keyboard, so backdrop_click_event_cb
     * latches visibility at PRESSED and consults it here at CLICKED (by which
     * point is_visible() would already read false).
     *
     * @return true (once) if that tap should be consumed for the keyboard
     *         dismiss; the caller must keep the overlay. Cleared on read.
     */
    bool take_backdrop_keyboard_dismiss();

    /**
     * @brief Mark that the next DISCONNECTED is expected (e.g. app backgrounding)
     *
     * Arms a one-shot consumed by the next CONNECTED→DISCONNECTED transition, so
     * the disconnect queued by disconnect() (drained on resume) does not clear
     * the overlay stack and bounce to Home (#1245). Immune to callback ordering:
     * a still-undrained CONNECTED apply cannot clear it, because it is not
     * carried in previous_connection_state_.
     */
    void mark_disconnect_expected();

    /**
     * @brief Shutdown navigation system during application exit
     *
     * Deactivates current overlay/panel and clears all registries.
     * Called from Application::shutdown() before StaticPanelRegistry::destroy_all().
     * This ensures UI is cleanly deactivated before panels are destroyed.
     */
    void shutdown();

    /// True after shutdown() is called — overlays should skip destructive actions
    [[nodiscard]] bool is_shutting_down() const {
        return shutting_down_;
    }

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     */
    void deinit_subjects();

    /**
     * @brief Set overlay backdrop visibility
     *
     * Updates the overlay_backdrop_visible subject which controls the
     * modal dimming backdrop visibility via XML binding.
     *
     * @param visible true to show backdrop, false to hide
     */
    void set_backdrop_visible(bool visible);

    /// Callback type for printer switch/add actions from the navbar badge menu
    using PrinterSwitchCallback = std::function<void(const std::string& printer_id)>;
    using AddPrinterCallback = std::function<void()>;

    /**
     * @brief Register callbacks for printer switching from navbar badge menu
     *
     * Application registers these during init so NavigationManager can trigger
     * printer switch and add-printer actions without depending on Application directly.
     *
     * @param switch_cb Called with printer_id when user selects a different printer
     * @param add_cb Called when user clicks "Add Printer" in the menu
     */
    void set_printer_callbacks(PrinterSwitchCallback switch_cb, AddPrinterCallback add_cb);

    /**
     * @brief Trigger a printer switch via the registered callback
     *
     * Used by overlays (PrinterListOverlay) that need to trigger printer switching
     * without depending on Application directly.
     *
     * @param printer_id The printer ID to switch to
     */
    void trigger_printer_switch(const std::string& printer_id);

    /**
     * @brief Trigger the add-printer wizard via the registered callback
     *
     * Used by overlays (PrinterListOverlay) that need to launch the setup wizard
     * without depending on Application directly.
     */
    void trigger_add_printer();

  private:
    // Private constructor/destructor for singleton
    NavigationManager() = default;
    ~NavigationManager(); // Defined in .cpp - sets g_nav_manager_destroyed flag

    // Panel ID to name mapping for E-Stop visibility
    static const char* panel_id_to_name(helix::PanelId id);

    // Check if panel requires Moonraker connection
    static bool panel_requires_connection(helix::PanelId panel);

    // Check if printer is connected
    bool is_printer_connected() const;

    // Check if klippy is in READY state
    bool is_klippy_ready() const;

    // Clear overlay stack (used during connection loss)
    void clear_overlay_stack();

    // Internal panel switch implementation (called via ui_queue_update)
    void switch_to_panel_impl(int panel_id);

    // Animation helpers
    void overlay_animate_slide_in(lv_obj_t* panel);
    void overlay_animate_slide_out(lv_obj_t* panel);
    static void overlay_slide_out_complete_cb(lv_anim_t* anim);

    // Zoom animation helpers
    void overlay_animate_zoom_in(lv_obj_t* panel, lv_area_t source_rect);
    void overlay_animate_zoom_out(lv_obj_t* panel, lv_area_t source_rect);

    // Activate the panel/overlay an overlay close restored, at most once per
    // close. go_back() arms restore_activation_pending_ and consumes it after
    // un-hiding the restored panel; the animation-completion callback consumes
    // it only if that never happened. Clears the latch before dispatching, so a
    // re-entrant navigation from on_activate() can arm a fresh close cleanly.
    void activate_restored_target();

    // Observer handlers (used by factory-created observers)
    void handle_active_panel_change(int32_t new_active_panel);
    void handle_connection_state_change(int state);
    void handle_klippy_state_change(int state);

    // Resolve overlay lifecycle instance, checking persistent map as fallback.
    // Restores persistent overlay registrations that survived panel switches.
    IPanelLifecycle* resolve_overlay_lifecycle(lv_obj_t* overlay_panel);

    // Self-healing against out-of-band widget deletion (bundle ZW6ATWSL).
    // When LVGL deletes a tracked widget through ANY path (e.g. a teardown that
    // bypasses go_back), LV_EVENT_DELETE fires synchronously just before the
    // memory is freed. scrub_deleted_widget() erases the widget from every
    // widget-keyed bookkeeping container — plus the scalars overlay_backdrop_,
    // app_layout_widget_ and the matching panel_widgets_ slots — so neither
    // panel_stack_.back() on the next push_overlay() nor the show/hide sweep in
    // handle_active_panel_change() can dereference freed memory.
    void scrub_deleted_widget(lv_obj_t* widget);
    // Attach the LV_EVENT_DELETE scrub callback to a widget exactly once.
    void ensure_delete_hook(lv_obj_t* widget);
    static void overlay_delete_event_cb(lv_event_t* e);
    // Create the darkened backdrop over `screen` and adopt it as
    // overlay_backdrop_, wiring its click handlers and the delete scrub. The
    // backdrop is a child of `screen`, so any path that deletes the screen frees
    // it without going through go_back(); the scrub hook is what keeps
    // overlay_backdrop_ from outliving it.
    void adopt_overlay_backdrop(lv_obj_t* screen);
    /**
     * @brief Re-take the overlay backdrop snapshot from the live widget tree
     *
     * The backdrop is a frozen bitmap of the screen as it looked when the first
     * overlay opened, so anything outside the overlay — the navigation bar —
     * stops tracking the widgets beneath it. A setting whose UI lives in an
     * overlay but whose effect lands in the navbar (show_printer_switcher) would
     * otherwise appear to do nothing until the stack popped.
     *
     * Hides every screen child except the app layout and un-hides the base
     * panel, so the new snapshot captures the same content the original did
     * rather than the overlays and the outgoing backdrop stacked on top of it.
     * The replacement is inserted directly above the outgoing one, which is then
     * deleted deferred — z-order is preserved without touching the overlays.
     *
     * No-op when no backdrop is live. Safe to call from a subject observer.
     */
    void refresh_overlay_backdrop();

    // Event callbacks
    static void backdrop_click_event_cb(lv_event_t* e);

    /**
     * @brief Resolve and apply an overlay's width class at push time
     *
     * Destinations render full width, transient layers render gapped. Which one
     * an overlay gets depends on how the user reached it — the same
     * fan_control_overlay is a transient layer from Controls and a drill-down
     * from Settings > Fans — so this cannot live in XML. See
     * include/overlay_class.h and prestonbrown/helixscreen#1178.
     *
     * Must be called BEFORE the overlay is pushed onto panel_stack_, while
     * panel_stack_.back() is still the widget beneath it.
     *
     * @param overlay          Widget being pushed.
     * @param is_first_overlay True when nothing but the root panel is on the
     *                         stack, so the class comes from the nav root.
     * @return resolved class (true = destination), also stored in
     *         overlay_is_destination_.
     */
    bool apply_overlay_width(lv_obj_t* overlay, bool is_first_overlay);
    static void nav_button_clicked_cb(lv_event_t* event);

    // Active panel tracking
    lv_subject_t active_panel_subject_{};
    helix::PanelId active_panel_ = helix::PanelId::Home;
    bool suspended_ = false; // True when screensaver has suspended lifecycle

    // Panel widget tracking for show/hide
    lv_obj_t* panel_widgets_[UI_PANEL_COUNT] = {nullptr};

    // C++ panel instances for lifecycle dispatch (on_activate/on_deactivate)
    std::array<PanelBase*, UI_PANEL_COUNT> panel_instances_ = {};

    // Lazy panel builder (ESP32 deferred-panel bring-up). Empty on desktop.
    // Invoked by switch_to_panel_impl() when a target panel's widget slot is
    // still null, to build it on first navigation. See set_deferred_panel_builder().
    std::function<void(int)> deferred_panel_builder_;
    bool building_deferred_panel_ = false; // re-entrancy guard for the builder
    // Outermost-transition guard for the ESP32 nav busy scrim (NavTransitionScrim
    // in ui_nav_manager.cpp): switch_to_panel_impl can cascade into
    // handle_active_panel_change, and only the outer one owns/tears down a scrim.
    bool nav_scrim_active_ = false;

    // If panel_id has no widget yet and a deferred builder is set, build it now
    // (first-navigation lazy bring-up). No-op on desktop (builder unset) and for
    // already-built panels. Guarded against re-entrancy. Called from both
    // navigation choke points (switch_to_panel_impl + handle_active_panel_change).
    void ensure_panel_built(int panel_id);

    // C++ overlay instances for lifecycle dispatch (on_activate/on_deactivate)
    std::unordered_map<lv_obj_t*, IPanelLifecycle*> overlay_instances_;

    // Persistent overlay instances — survive panel switches (navbar navigation).
    // Used by overlays that cache their widget tree across navigations (e.g., PrintStatusPanel).
    std::unordered_map<lv_obj_t*, IPanelLifecycle*> persistent_overlay_instances_;

    // App layout widget reference
    lv_obj_t* app_layout_widget_ = nullptr;

    // Panel stack: tracks ALL visible panels in z-order
    std::vector<lv_obj_t*> panel_stack_;

    // Overlay close callbacks (called when overlay is popped from stack)
    std::unordered_map<lv_obj_t*, helix::OverlayCloseCallback> overlay_close_callbacks_;

    // Shared overlay backdrop widget (for first overlay)
    lv_obj_t* overlay_backdrop_ = nullptr;

    // Latched at the dismiss-backdrop's LV_EVENT_PRESSED with the on-screen
    // keyboard's visibility. LVGL's click-focus DEFOCUS (which hides the
    // keyboard) fires between PRESSED and CLICKED, so is_visible() is already
    // false by the time backdrop_click_event_cb handles CLICKED — the visibility
    // must be captured at press time. Consumed one-shot by
    // take_backdrop_keyboard_dismiss(): a tap that hides the keyboard must not
    // also dismiss the overlay behind it.
    bool backdrop_press_keyboard_visible_ = false;

    // Dynamic backdrops for nested overlays (overlay → its backdrop)
    std::unordered_map<lv_obj_t*, lv_obj_t*> overlay_backdrops_;

    // Zoom animation source rects (overlay → source rect for reverse animation)
    std::unordered_map<lv_obj_t*, lv_area_t> zoom_source_rects_;

    // Resolved width class per overlay (overlay → is_destination). Written by
    // apply_overlay_width() on every push, read by the next push to inherit and
    // by Application's resize handler to re-apply the right width without
    // guessing from the current pixel width. #1178
    std::unordered_map<lv_obj_t*, bool> overlay_is_destination_;

    // Overlays exempt from push-time width management (deliberate custom
    // widths, e.g. the 70% widget catalog). #1178
    std::unordered_set<lv_obj_t*> overlay_width_unmanaged_;

    // Widgets that already have the LV_EVENT_DELETE scrub hook attached.
    // Prevents double-registering the callback and is itself scrubbed on delete.
    std::unordered_set<lv_obj_t*> delete_hooked_;

    // Navbar widget reference (for z-order management)
    lv_obj_t* navbar_widget_ = nullptr;

    // RAII observer guards
    ObserverGuard active_panel_observer_;
    ObserverGuard connection_state_observer_;
    ObserverGuard klippy_state_observer_;
    ObserverGuard printer_dot_observer_;
    ObserverGuard printer_switcher_observer_;

    // Printer connection status dot widget
    lv_obj_t* printer_dot_widget_ = nullptr;

    // Track previous states for detecting transitions
    int previous_connection_state_ = -1;
    int previous_klippy_state_ = -1;

    // One-shot: the next CONNECTED→DISCONNECTED transition is expected and must
    // not clear the overlay stack. Set by mark_disconnect_expected().
    bool disconnect_expected_ = false;

    // Exactly-once latch for the restored panel/overlay activation of one
    // overlay close. See activate_restored_target().
    bool restore_activation_pending_ = false;

    // True while the active main panel sits deactivated underneath an overlay.
    // Set when the first overlay covers it, cleared by whoever activates a main
    // panel next. switch_to_panel_impl() needs it because a navbar tap onto the
    // panel you are already on makes set_active() a no-op, leaving nothing else
    // to re-activate it; the flag says whether that re-activation is owed, so a
    // panel that set_active() already activated under the overlay (the
    // connection-change path) is not activated a second time.
    bool main_panel_deactivated_for_overlay_ = false;

    // Animation constants
    static constexpr uint32_t OVERLAY_ANIM_DURATION_MS = 200;
    static constexpr int32_t OVERLAY_SLIDE_OFFSET = 400;
    static constexpr uint32_t ZOOM_ANIM_DURATION_MS = 250;

    // Subject management via RAII
    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Overlay backdrop visibility subject (for modal dimming)
    lv_subject_t overlay_backdrop_visible_subject_{};

    // Shutdown flag — overlays should skip destructive actions (e.g. ABORT)
    bool shutting_down_ = false;

    // Printer badge menu
    helix::ui::PrinterSwitchMenu printer_switch_menu_;
    void on_printer_badge_clicked();
    PrinterSwitchCallback printer_switch_cb_;
    AddPrinterCallback add_printer_cb_;
};
