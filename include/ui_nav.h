// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

class IPanelLifecycle;

namespace helix {

/// Callback type for overlay close notifications
using OverlayCloseCallback = std::function<void()>;

/**
 * @brief Navigation panel identifiers
 *
 * Order matches app_layout.xml panel children for index-based access.
 */
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
 * from: Settings > Connection > Network is a sub-screen of Settings, not a layer
 * over it, so it renders at destination width (iOS push semantics). Every other root
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

/// The navigation calls most callers need, without the NavigationManager class.
/// Each forwards to NavigationManager::instance(); include ui_nav_manager.h only
/// for the rest of its surface.
namespace helix::nav {

/// Show @p overlay on top of the stack. Queued; see NavigationManager::push_overlay().
void push_overlay(lv_obj_t* overlay, bool hide_previous = true);

/// Pop the top overlay. Queued; see NavigationManager::go_back().
bool go_back();

/// Pop a specific overlay, decided in queue order. See NavigationManager::close_overlay().
void close_overlay(lv_obj_t* overlay);

/// Pair @p widget with its lifecycle. See NavigationManager::register_overlay_instance().
void register_overlay(lv_obj_t* widget, IPanelLifecycle* overlay, bool persistent = false);

/// Drop @p widget's lifecycle pairing; a no-op once NavigationManager is destroyed.
void unregister_overlay(lv_obj_t* widget);

/// Run @p callback when @p overlay closes. See
/// NavigationManager::register_overlay_close_callback().
void on_close(lv_obj_t* overlay, OverlayCloseCallback callback);

/// Drop @p overlay's close callback; a no-op once NavigationManager is destroyed.
void clear_on_close(lv_obj_t* overlay);

/// Leave @p overlay's width to its owner; push_overlay() will not resize it.
void set_overlay_width_unmanaged(lv_obj_t* overlay);

/// Swap the base panel. See NavigationManager::set_active().
void set_active(PanelId panel_id);

/// Deactivate the current panel and overlay and clear the navigation registries. See
/// NavigationManager::shutdown().
void shutdown();

/// True when @p panel is the topmost stack entry, so go_back() would pop it.
bool is_on_top(lv_obj_t* panel);

/// True when @p panel is anywhere in the overlay stack.
bool is_in_stack(lv_obj_t* panel);

/// True from push_overlay(@p panel) until its queued push runs.
bool is_push_pending(lv_obj_t* panel);

/// True while @p panel is on screen or about to be: queued for a push, in the
/// stack, or off the stack but still drawn as it slides out. A guard against
/// opening a panel twice asks this.
bool is_showing(lv_obj_t* panel);

} // namespace helix::nav
