// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file overlay_base.h
 * @brief Abstract base class for overlay panels with lifecycle hooks
 *
 * @pattern Two-phase init (init_subjects -> XML -> callbacks); lifecycle hooks via
 * NavigationManager
 * @threading Main thread only
 *
 * OverlayBase provides lifecycle management for overlay panels:
 * - on_activate() called when overlay becomes visible (slide-in complete)
 * - on_deactivate() called when overlay is being hidden (before slide-out)
 *
 * ## Lifecycle Flow
 *
 * ### Overlay pushed:
 * 1. If first overlay: main panel's on_deactivate() is called
 * 2. If nested: previous overlay's on_deactivate() is called
 * 3. Overlay shows with slide-in animation
 * 4. Overlay's on_activate() is called
 *
 * ### Overlay popped (go_back):
 * 1. Overlay's on_deactivate() is called
 * 2. Slide-out animation plays
 * 3. If returning to main panel: main panel's on_activate() is called
 * 4. If returning to previous overlay: previous overlay's on_activate() is called
 *
 * ## Usage Pattern:
 *
 * A behaviour-free overlay is its name and XML component; show() does the rest
 * (subjects once, callbacks + create on each build, register + push):
 *
 * @code
 * class MyOverlay : public OverlayBase {
 *   public:
 *     const char* get_name() const override { return "My Overlay"; }
 *     const char* xml_component() const override { return "my_overlay"; }
 * };
 * MyOverlay& get_my_overlay() { return helix::lazy_global<MyOverlay>("MyOverlay"); }
 *
 * get_my_overlay().show(parent_screen);
 * @endcode
 *
 * Override init_subjects() for owned subjects, register_callbacks() for XML
 * callbacks, before_show() for per-open work that needs the root, and
 * destroy_on_close() to free the widget tree whenever the overlay is popped.
 *
 * @see NetworkSettingsOverlay for reference implementation
 */

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <optional>

// Include for SubjectManager (needed for deinit_subjects_base)
#include "subject_managed_panel.h"

/**
 * @class OverlayBase
 * @brief Abstract base class for overlay panels with lifecycle management
 *
 * Provides shared infrastructure for overlay panels including:
 * - Lifecycle hooks (on_activate/on_deactivating) called by NavigationManager
 * - Two-phase initialization (init_subjects -> create -> register_callbacks)
 * - Async-safe cleanup pattern
 *
 * @implements IPanelLifecycle for NavigationManager dispatch
 * @see ViewLifecycleBase for the two lifetime guards and which one to use
 */
class OverlayBase : public ViewLifecycleBase {
  public:
    /**
     * @brief Virtual destructor for proper cleanup
     */
    virtual ~OverlayBase();

    // Non-copyable (unique overlay instances)
    OverlayBase(const OverlayBase&) = delete;
    OverlayBase& operator=(const OverlayBase&) = delete;

    //
    // === Core Interface (must implement) ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Runs BEFORE create() so bindings resolve. show() calls it once and sets
     * subjects_initialized_; the default has nothing to initialize.
     */
    virtual void init_subjects() {}

    /**
     * @brief Create overlay UI
     *
     * Default: create_overlay_from_xml(parent, xml_component()). Override when
     * the overlay passes attributes to lv_xml_create or builds its own rows.
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     *
     * Implementations should store result in overlay_root_.
     */
    virtual lv_obj_t* create(lv_obj_t* parent);

    /**
     * @brief XML component the default create() instantiates
     * @return Component name, or nullptr when the subclass overrides create()
     */
    virtual const char* xml_component() const {
        return nullptr;
    }

    /**
     * @brief Free the widget tree every time show()'s overlay is popped
     *
     * The object (subjects, state) survives and the next show() re-creates the
     * tree (#1329, #1246). Default: built once, retained for the session.
     */
    virtual bool destroy_on_close() const {
        return false;
    }

    /**
     * @brief Open this overlay: the one lazy-create-and-push sequence
     *
     * Initializes subjects once, and on each build registers callbacks (XML
     * callback slots are last-write-wins) and calls create(). Then runs
     * before_show(), registers the lifecycle and pushes. With
     * destroy_on_close(), registers the close callback that frees the tree.
     *
     * For overlays that own their root for the session. An overlay whose
     * object is owned elsewhere and dies with its screen (plugin settings
     * screens) is pushed by its owner, whose close callback deletes it:
     * NavigationManager keeps one close callback per root, so show() refuses a
     * root that already carries someone else's (abort in --test and unit
     * tests, error log in release).
     *
     * @param parent_screen Screen to build on
     * @return false (after a toast) when the root could not be created
     */
    bool show(lv_obj_t* parent_screen);

    /**
     * @brief Close this overlay through NavigationManager::close_overlay()
     *
     * Pops it when on top, drops it silently when buried, no-op when it is
     * already gone; safe to call whatever the stack looks like.
     */
    void close();

    /**
     * @brief Get human-readable overlay name
     *
     * Used in logging and debugging.
     *
     * @return Overlay name (e.g., "Network Settings")
     */
    const char* get_name() const override = 0;

    //
    // === Optional Hooks (override as needed) ===
    //

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Called after create() to register XML event callbacks.
     * Default implementation does nothing.
     */
    virtual void register_callbacks() {}

    /**
     * @brief Called when overlay becomes visible
     *
     * Override to start scanning, refresh data, begin animations, etc.
     * Called by NavigationManager after slide-in animation starts.
     * Default implementation sets visible_ = true.
     */
    void on_activate() override;

    /**
     * @brief Rebuild this overlay's widget tree from its XML component
     *
     * Dev-only. Called by NavigationManager after XML hot-reload re-registers
     * a component. Deactivates with DeactivateReason::Rebuild to drop state,
     * calls create() for a new widget, rekeys NavigationManager maps, restores
     * visibility, and schedules async deletion of the old widget.
     *
     * @return true if rebuilt, false if no current widget or create() failed
     */
    bool rebuild() override;

    /**
     * @brief Clean up resources for async-safe destruction
     *
     * Call this before destroying the overlay to handle any pending
     * async callbacks safely. Invalidates BOTH lifetime guards — including
     * object_lifetime_, whose whole point is surviving deactivation — and sets
     * cleanup_called_.
     */
    virtual void cleanup();

    //
    // === State Queries ===
    //

    /**
     * @brief Check if overlay is currently visible
     * @return true if overlay is visible
     */
    bool is_visible() const {
        return visible_;
    }

    /**
     * @brief Check if cleanup has been called
     * @return true if cleanup() was called
     */
    bool cleanup_called() const {
        return cleanup_called_;
    }

    /**
     * @brief Get root overlay widget
     * @return Root widget, or nullptr if not created
     */
    lv_obj_t* get_root() const {
        return overlay_root_;
    }

    /**
     * @brief Destroy overlay widget tree to free memory
     *
     * Called by the close callback show() registers when destroy_on_close()
     * is true. Performs:
     * 1. Drains UpdateQueue (process pending deferred callbacks)
     * 2. Unregisters close callback from NavigationManager
     * 3. Unregisters overlay instance from NavigationManager
     * 4. Defers widget-tree deletion via safe_delete_deferred() — sync
     *    deletion inside an overlay close callback corrupts LVGL's global
     *    event list when chained from a UpdateQueue batch (#776, #840)
     * 5. Calls on_ui_destroyed() to null derived class widget pointers
     *    (child widgets are still valid at this point, just hidden and
     *    reparented to top layer; actual deletion runs on the next tick)
     *
     * The overlay object (subjects, state) survives — only the widget tree
     * is destroyed. The next show() re-creates it.
     *
     * @param cached_panel A caller's second copy of the root, nulled too
     */
    void destroy_overlay_ui(lv_obj_t*& cached_panel);

    /// destroy_overlay_ui() for an overlay no caller keeps a second copy of.
    void destroy_overlay_ui() {
        lv_obj_t* no_cache = nullptr;
        destroy_overlay_ui(no_cache);
    }

    /**
     * @brief Check if subjects have been initialized
     * @return true if init_subjects() was called
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

  protected:
    /**
     * @brief Default constructor (protected - use derived classes)
     */
    OverlayBase() = default;

    /**
     * @brief Create overlay from XML with standard setup
     *
     * Helper method that consolidates common overlay creation boilerplate:
     * - Sets parent_screen_ and resets cleanup_called_
     * - Creates overlay from XML using lv_xml_create()
     * - Applies standard overlay setup (header, content padding)
     * - Hides overlay initially
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @param component_name XML component name to create (e.g., "console_panel")
     * @return Root object of overlay, or nullptr on failure
     *
     * @code
     * lv_obj_t* MyOverlay::create(lv_obj_t* parent) {
     *     if (!create_overlay_from_xml(parent, "my_overlay")) {
     *         return nullptr;
     *     }
     *     // Find additional widgets and setup panel-specific state...
     *     return overlay_root_;
     * }
     * @endcode
     */
    lv_obj_t* create_overlay_from_xml(lv_obj_t* parent, const char* component_name);

    //
    // === Protected State ===
    //

    helix::ui::WidgetRef overlay_root_; ///< Root widget of overlay UI
    lv_obj_t* parent_screen_ = nullptr; ///< Parent screen (for overlay setup)
    bool subjects_initialized_ = false; ///< True after init_subjects() called
    bool visible_ = false;              ///< True when overlay is visible
    bool cleanup_called_ = false;       ///< True after cleanup() called

    /**
     * @brief Per-open work that needs the root (populate, seed state)
     *
     * show() runs it on every open, after create() and before the push.
     */
    virtual void before_show() {}

    /**
     * @brief Called when a destroy_on_close() overlay leaves the stack
     *
     * Runs from the close callback show() registers, whatever popped the
     * overlay (its own Back, a backdrop tap, another screen's navigation),
     * before the tree is destroyed. An overlay that is shown again while
     * its slide-out is still running stays open, and its next close runs
     * this once.
     *
     * Default implementation does nothing.
     */
    virtual void on_dismissed() {}

    /**
     * @brief Called after widget tree is destroyed by destroy_overlay_ui()
     *
     * Override to null derived-class widget pointers so that create()
     * works correctly when re-invoked. The base overlay_root_ is already
     * nulled before this is called.
     *
     * Default implementation does nothing.
     */
    virtual void on_ui_destroyed() {}

    //
    // === Subject Init/Deinit Guards ===
    //

    /**
     * @brief Execute init function with guard against double initialization
     *
     * Wraps the actual subject initialization code with a guard that prevents
     * double initialization and logs appropriately.
     *
     * @tparam Func Callable type (typically lambda)
     * @param init_func Function to execute if not already initialized
     * @return true if initialization was performed, false if already initialized
     *
     * Example:
     * @code
     * void MyOverlay::init_subjects() {
     *     init_subjects_guarded([this]() {
     *         UI_MANAGED_SUBJECT_INT(my_subject_, 0, "my_subject", subjects_);
     *     });
     * }
     * @endcode
     */
    template <typename Func> bool init_subjects_guarded(Func&& init_func) {
        if (subjects_initialized_) {
            spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
            return false;
        }
        init_func();
        subjects_initialized_ = true;
        spdlog::debug("[{}] Subjects initialized", get_name());
        return true;
    }

    /**
     * @brief Deinitialize subjects via SubjectManager with guard
     *
     * Checks subjects_initialized_ flag before deinitializing.
     * Resets the flag after cleanup.
     *
     * @param subjects Reference to the overlay's SubjectManager
     *
     * Example:
     * @code
     * void MyOverlay::deinit_subjects() {
     *     deinit_subjects_base(subjects_);
     * }
     * @endcode
     */
    void deinit_subjects_base(SubjectManager& subjects) {
        if (!subjects_initialized_) {
            return;
        }
        subjects.deinit_all();
        subjects_initialized_ = false;
        spdlog::trace("[{}] Subjects deinitialized", get_name());
    }

  private:
    void on_view_hidden() override;
    void report_foreign_close_callback() const;

    /// The root show() last pushed, and a handle LVGL nulls when it is deleted:
    /// a root freed with its screen still reads non-null in overlay_root_.
    lv_obj_t* shown_root_ = nullptr;
    helix::ui::WidgetRef shown_root_ref_;

    /// Expires only with this object: destroy_overlay_ui() hands it to
    /// teardown_overlay_ui() as the owner that must outlive its drain.
    helix::AsyncLifetimeGuard teardown_owner_;
};

/**
 * @namespace helix::ui
 * @brief Shared overlay teardown sequence
 */
namespace helix::ui {

/**
 * @enum TeardownDelete
 * @brief How teardown_overlay_ui() frees the widget tree
 */
enum class TeardownDelete {
    /**
     * Reparent the root to the top layer and async-delete it. The plain
     * overlay path: the root carries no grid/flex layout of its own, so a
     * racing ancestor relayout cannot corrupt anything.
     */
    Deferred,

    /**
     * Detach the root into a hidden, layout-less condemned container first,
     * then async-delete. For roots containing grid/flex layouts whose
     * grid_update/flex_update must be structurally unable to race teardown
     * (#983) — detaching off-tree makes an ancestor relayout of the original
     * parent incapable of iterating the doomed subtree.
     */
    DetachSubtree,
};

/**
 * @struct TeardownHooks
 * @brief The two owner callbacks teardown_overlay_ui() runs, named at the
 *        call site
 *
 * The slots differ only in when they run relative to the free, and that
 * difference is the whole point: a hook that drops widgets the subtree owns
 * has to run while the tree is still attached. Putting it in the other slot
 * reaches into an already-condemned subtree (#776/#983) and is the same type,
 * so build through before()/after() and the slot is spelled where the hook
 * is written.
 */
struct TeardownHooks {
    /// Step 4 — every pointer still valid, tree still attached.
    std::function<void()> before_delete;
    /// Step 7 — free already queued; tree hidden and off-tree but still alive.
    std::function<void()> after_delete;
    /// The object @p root lives in. It must outlive the step-1 drain: the
    /// teardown keeps writing through @p root and runs the hooks after it. An
    /// owner freed by the drain is a UI contract breach, and the teardown stops
    /// there. Every OverlayBase passes its own (destroy_overlay_ui()), which
    /// covers the heap-owned ones such as PluginSettingsOverlay, held by
    /// PluginHost.
    std::optional<helix::LifetimeToken> owner{};

    static TeardownHooks before(std::function<void()> fn) {
        return {std::move(fn), nullptr};
    }
    static TeardownHooks after(std::function<void()> fn) {
        return {nullptr, std::move(fn)};
    }
};

/**
 * @brief The one overlay teardown sequence — drain, unregister, breadcrumb,
 *        owner hooks, deferred free, pointer null-out
 *
 * OverlayBase::destroy_overlay_ui() and the AMS destroy_*_panel_ui() sites
 * all run this sequence; a site differs only in its delete strategy and in
 * which owner hooks it needs, never in the sequence itself. Re-implementing
 * it per site is how the copies drift.
 *
 * Performs, in order:
 * 1. Drains the UpdateQueue under a scoped freeze while all pointers are
 *    still valid (observe<int> callbacks capture raw panel pointers), and
 *    stops there when that drain tore the same root down (leaving @p root
 *    null, or pointing at a tree re-created after it)
 * 2. Unregisters the close callback and overlay instance from
 *    NavigationManager (before deletion, so a manual destroy while the panel
 *    is still stacked cannot double-invoke)
 * 3. Breadcrumbs "ovrl_dst" so crashes in the close path can be pinned to
 *    the overlay being torn down
 * 4. Runs hooks.before_delete while every pointer is still valid AND the tree
 *    is still attached — owners whose sub-objects own widgets in the subtree
 *    (AMS sidebars, context menus, modals) drop them here
 * 5. Deletes per @p how (both strategies defer the actual free — sync
 *    deletion inside an overlay close callback corrupts LVGL's global event
 *    list when chained from an UpdateQueue batch, #776/#840)
 * 6. Nulls @p root, and @p cached_panel when the caller passed one (it may
 *    point at @p root itself)
 * 7. Runs hooks.after_delete — the widget tree is still alive (hidden,
 *    off-tree) until the async tick, so owners can null child-widget
 *    pointers that must stay dereferenceable during teardown
 *
 * The owner object (subjects, state) survives — only the widget tree is
 * destroyed; the next open re-creates it.
 *
 * The owner must outlive the step-1 drain: @p root and the hooks point into it.
 * An owner that passes TeardownHooks::owner is checked after the drain. The AMS
 * panel teardowns pass none: their roots live in file statics and their hooks
 * look the panel up again, so no drain can free what they write through.
 *
 * @param root Root widget to tear down; nulled on return
 * @param owner_name Logged and breadcrumbed owner name
 * @param how Delete strategy (see TeardownDelete)
 * @param cached_panel Caller's cached copy of the root, nulled on return;
 *                     null when the caller keeps no second copy, and may
 *                     point at @p root itself
 * @param hooks Owner hooks for steps 4 and 7 (either may be empty)
 * @return true if a teardown happened; false when @p root was null, or a
 *         teardown the drain ran completed it first (no hooks run here in
 *         either case)
 */
bool teardown_overlay_ui(WidgetRef& root, const char* owner_name, TeardownDelete how,
                         lv_obj_t** cached_panel = nullptr, TeardownHooks hooks = {});

} // namespace helix::ui
