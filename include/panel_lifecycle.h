// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file panel_lifecycle.h
 * @brief Common lifecycle interface for panels and overlays
 *
 * Defines the minimal interface that both PanelBase and OverlayBase
 * implement for NavigationManager to dispatch lifecycle events.
 *
 * ## Implemented by:
 * - PanelBase: Main UI panels (enum-indexed, setup() pattern)
 * - OverlayBase: Modal overlays (widget-indexed, create() pattern)
 *
 * ## Lifecycle Contract:
 * - on_deactivate(reason) called BEFORE a panel/overlay becomes hidden
 * - on_activate() called AFTER animation completes and panel/overlay is visible
 * - get_name() used for debugging/logging only
 *
 * ## Async Lifetime Contract:
 * ViewLifecycleBase (below) owns the two AsyncLifetimeGuards every panel and
 * overlay gets, and owns the deactivation sequence. Subclasses override
 * on_deactivating(DeactivateReason), which the base calls before it invalidates
 * the screen-scoped guard — there is no base call to forget.
 *
 * @threading Main thread only
 */

#pragma once

#include "async_lifetime_guard.h"

#include <utility>

/**
 * @enum DeactivateReason
 * @brief Why a view is being deactivated
 *
 * Deactivation is one notification with three meanings, and a view that reacts
 * to it usually cares which. The reason arrives as a parameter so no view has
 * to query NavigationManager to find out.
 */
// NAMESPACE_OK: joins the global view-lifecycle API (prestonbrown/helixscreen#1516)
enum class DeactivateReason {
    /// Another view is taking the screen. This one stays constructed and gets
    /// on_activate() again if the user comes back.
    NavigateAway,
    /// The application is tearing down. Nothing will reactivate this view, and
    /// an operation started here has no UI left to report into.
    Shutdown,
    /// XML hot-reload is re-creating this view's widget tree. Every widget
    /// pointer the view holds is about to dangle; the view object itself
    /// survives and create()/setup() runs again immediately. Dev-only path.
    Rebuild,
    /// The screen went idle - the screensaver is up, the display slept, or the
    /// app moved to the background. The view stays on the stack and gets
    /// on_activate() again on the next wake, so an operation started here still
    /// has a UI to report into. A view must NOT cancel in-flight work on this:
    /// nobody walked away, and the work outlives the blanked screen. It MUST
    /// still stop timers, animations and polling - a suspended view that keeps
    /// drawing burns the CPU the suspend exists to free. Guard per statement,
    /// not per function: the two jobs live in the same on_deactivating().
    Suspended,
};

/// Human-readable reason, for log lines that need to say which one fired.
// NAMESPACE_OK: joins the global view-lifecycle API (prestonbrown/helixscreen#1516)
inline const char* deactivate_reason_name(DeactivateReason reason) {
    switch (reason) {
    case DeactivateReason::NavigateAway:
        return "navigate-away";
    case DeactivateReason::Shutdown:
        return "shutdown";
    case DeactivateReason::Rebuild:
        return "rebuild";
    case DeactivateReason::Suspended:
        return "suspended";
    }
    return "unknown";
}

/**
 * @class IPanelLifecycle
 * @brief Common lifecycle interface for NavigationManager dispatch
 *
 * This interface enables NavigationManager to handle both panels and overlays
 * polymorphically for lifecycle event dispatch.
 */
class IPanelLifecycle {
  public:
    virtual ~IPanelLifecycle() = default;

    /**
     * @brief Called when panel/overlay becomes visible
     *
     * Used to start background operations (scanning, subscriptions, timers).
     * Safe to call multiple times (implementations should be idempotent).
     */
    virtual void on_activate() = 0;

    /**
     * @brief Called when panel/overlay is being hidden
     *
     * Used to stop background operations before animation starts.
     * Safe to call multiple times (implementations should be idempotent).
     *
     * Panels and overlays inherit this through ViewLifecycleBase, which makes
     * it final and dispatches to on_deactivating(reason) instead.
     *
     * @param reason Why the view is going away
     */
    virtual void on_deactivate(DeactivateReason reason) = 0;

    /**
     * @brief Tear down and re-create this view from its XML component definition
     *
     * Dev-only hook called by NavigationManager after XML hot-reload re-registers
     * a component. Default is a no-op for lifecycles that wrap non-XML or
     * non-rebuildable widgets. Concrete panel/overlay bases override to rebuild.
     *
     * @return true if rebuilt, false if skipped (not yet shown, not this instance, etc.)
     */
    virtual bool rebuild() {
        return false;
    }

    /**
     * @brief Whether this view is a destination rather than a transient layer
     *
     * Destinations render full width (screen - nav) and occlude the backdrop;
     * their drill-downs inherit that. Transient layers render gapped
     * (screen - nav - space_lg) so the backdrop shows at the leading edge,
     * signalling "you opened this and will return."
     *
     * Default false — most overlays are tools you return from. Override to true
     * only for screens users park on for long stretches (AMS, AMS Overview,
     * Print Status). Declaring it here rather than at the push site means the
     * promotion travels with the panel: AmsPanel is reachable from Home, the
     * Printer Manager overlay and the AMS Overview, and must be full width from
     * all three.
     *
     * NavigationManager::push_overlay() reads this. See include/overlay_class.h
     * and prestonbrown/helixscreen#1178.
     */
    virtual bool is_destination() const {
        return false;
    }

    /**
     * @brief Re-apply C++-side content to a freshly rebuilt widget tree
     *
     * rebuild() re-runs setup()/create() and nothing else, so it reproduces
     * exactly what the XML describes. Content a view writes into its widgets
     * afterwards is not in the XML and does not come back on its own: dropdown
     * option lists, imperatively built rows, text set with lv_textarea_set_text,
     * colors applied to a swatch. Views that populate from a separate entry
     * point — a show_for_*(), a click handler — override this to re-apply that
     * content from the state they already hold, and must not re-seed that state
     * (doing so would discard the user's in-progress edits).
     *
     * Nothing is needed here for content bound to a subject: the rebuilt widgets
     * read the subject's current value when they bind. Nothing is needed either
     * for content populated inside create()/setup() or on_activate(), both of
     * which rebuild() already re-runs.
     *
     * Called after the new tree exists and before on_activate(). Dev-only path,
     * reached only via XML hot-reload.
     */
    virtual void repopulate() {}

    /**
     * @brief Get human-readable name for logging
     * @return Panel/overlay name (e.g., "Motion Panel", "Network Settings")
     */
    virtual const char* get_name() const = 0;
};

/**
 * @class ViewLifecycleBase
 * @brief Shared async-lifetime contract for PanelBase and OverlayBase
 *
 * Owns both lifetime guards and the deactivation sequence, so the two bases
 * agree on what deactivation costs a subclass's in-flight work.
 *
 * ## Which guard
 *
 * Every view gets two, and the choice is about what the callback is *for*:
 *
 * - `lifetime_` — the screen. The result only matters while the user is
 *   looking at this view: a fetch that populates visible widgets, a debounced
 *   repaint, an animation step. Dropped on every deactivation but a
 *   screensaver suspend, which leaves the view on the stack to wake into;
 *   on_activate() re-arms whatever still matters.
 * - `object_lifetime_` — the object. The view owes the completion to the
 *   machine rather than to the screen: an abort the printer must acknowledge,
 *   a settings write, a power-off handshake. It survives navigation and dies
 *   with the view, so its callbacks must be safe to run off screen — they may
 *   touch members, but must not assume any widget is visible.
 *
 * Reaching for `object_lifetime_` to stop a callback from being cancelled is
 * the wrong reason; reach for it when the work is genuinely not the screen's.
 *
 * ## Deactivation sequence
 *
 * on_deactivate() is final. It calls on_deactivating(reason) — the subclass's
 * one chance to act while its in-flight work is still live — and only then
 * invalidates `lifetime_`, for every reason but Suspended. A subclass that wants to schedule work
 * from inside the hook must park it on `object_lifetime_`, or the invalidation that follows will
 * drop it.
 *
 * @threading Main thread only
 */
// NAMESPACE_OK: joins the global view-lifecycle API (prestonbrown/helixscreen#1516)
class ViewLifecycleBase : public IPanelLifecycle {
  public:
    void on_deactivate(DeactivateReason reason) final {
        suspended_ = reason == DeactivateReason::Suspended;
        on_deactivating(reason);
        // A suspended view is still on the stack and wakes into on_activate():
        // work it started is still the screen's, so its results must land.
        if (!suspended_) {
            lifetime_.invalidate();
        }
        on_view_hidden();
    }

  protected:
    /**
     * @brief React to this view leaving the screen
     *
     * Stop scanning, cancel pending operations, pause timers. Runs before
     * `lifetime_` is invalidated, so anything this hook cancels by hand is
     * still live when it does so.
     *
     * @param reason Why the view is going away
     */
    virtual void on_deactivating(DeactivateReason reason) {
        (void)reason;
    }

    /// @see ViewLifecycleBase class docs — screen-scoped.
    helix::AsyncLifetimeGuard lifetime_;

    /// @see ViewLifecycleBase class docs — object-scoped.
    helix::AsyncLifetimeGuard object_lifetime_;

    /// Behind the screensaver now: deactivated with Suspended and not yet
    /// woken. Work that would draw waits for the wake.
    [[nodiscard]] bool suspended() const {
        return suspended_;
    }

    /**
     * @brief Whether this on_activate() is a wake from the screensaver
     *
     * Call once, from on_activate(): it answers and clears. A wake resumes what
     * the view showed - an in-flight run, a half-filled form, a result - where
     * any other activation may start fresh.
     */
    [[nodiscard]] bool take_wake() {
        return std::exchange(suspended_, false);
    }

  private:
    bool suspended_ = false;

    /**
     * @brief Base-class bookkeeping, after the subclass hook and the invalidate
     *
     * Private so that only PanelBase and OverlayBase implement it: a view
     * overrides on_deactivating() instead, and cannot displace the tracking
     * its own base does here.
     */
    virtual void on_view_hidden() {}
};
