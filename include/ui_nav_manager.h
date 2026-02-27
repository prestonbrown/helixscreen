// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <array>
#include <functional>
#include <unordered_map>
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
    void register_overlay_instance(lv_obj_t* widget, IPanelLifecycle* overlay);

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
     * @brief Get current active panel
     * @return Currently active panel identifier
     */
    helix::PanelId get_active() const;

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
     * @brief Check if any overlays are currently open
     * @return true if there are overlay panels on the stack
     */
    bool has_open_overlays() const;

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

    // Observer handlers (used by factory-created observers)
    void handle_active_panel_change(int32_t new_active_panel);
    void handle_connection_state_change(int state);
    void handle_klippy_state_change(int state);

    // Event callbacks
    static void backdrop_click_event_cb(lv_event_t* e);
    static void nav_button_clicked_cb(lv_event_t* event);

    // Active panel tracking
    lv_subject_t active_panel_subject_{};
    helix::PanelId active_panel_ = helix::PanelId::Home;

    // Panel widget tracking for show/hide
    lv_obj_t* panel_widgets_[UI_PANEL_COUNT] = {nullptr};

    // C++ panel instances for lifecycle dispatch (on_activate/on_deactivate)
    std::array<PanelBase*, UI_PANEL_COUNT> panel_instances_ = {};

    // C++ overlay instances for lifecycle dispatch (on_activate/on_deactivate)
    std::unordered_map<lv_obj_t*, IPanelLifecycle*> overlay_instances_;

    // App layout widget reference
    lv_obj_t* app_layout_widget_ = nullptr;

    // Panel stack: tracks ALL visible panels in z-order
    std::vector<lv_obj_t*> panel_stack_;

    // Overlay close callbacks (called when overlay is popped from stack)
    std::unordered_map<lv_obj_t*, helix::OverlayCloseCallback> overlay_close_callbacks_;

    // Shared overlay backdrop widget (for first overlay)
    lv_obj_t* overlay_backdrop_ = nullptr;

    // Dynamic backdrops for nested overlays (overlay → its backdrop)
    std::unordered_map<lv_obj_t*, lv_obj_t*> overlay_backdrops_;

    // Zoom animation source rects (overlay → source rect for reverse animation)
    std::unordered_map<lv_obj_t*, lv_area_t> zoom_source_rects_;

    // Navbar widget reference (for z-order management)
    lv_obj_t* navbar_widget_ = nullptr;

    // RAII observer guards
    ObserverGuard active_panel_observer_;
    ObserverGuard connection_state_observer_;
    ObserverGuard klippy_state_observer_;

    // Track previous states for detecting transitions
    int previous_connection_state_ = -1;
    int previous_klippy_state_ = -1;

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
};
