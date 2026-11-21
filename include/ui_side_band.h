// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ui_side_band.h
 * @brief The ultrawide side band: a main panel hosts an overlay's content beside its own
 *
 * On an ultrawide screen a capped panel's content stops at content_max_width
 * (include/content_width_cap.h), leaving a band to its right. Controls fills it
 * with the Motion panel and Filament with the AMS panel. Each of those views is a
 * singleton with one widget tree, so the band and the overlay path share it:
 *
 * - The band builds (or moves) the tree into itself when its host activates and
 *   the gate (side_band_shows()) is true, and forwards the host's activation to
 *   the guest's lifecycle.
 * - An overlay open while the host is on screen is a no-op: the content is
 *   already showing. An open while the host is elsewhere takes the tree back
 *   first (take_open_request()), so the overlay builds its own and there is
 *   never a second tree. The band rebuilds it on the host's next activation,
 *   or sooner once the overlay's tree is freed (notify_released()).
 * - The gate going false, a host rebuild and the host tree's deletion all hand
 *   the tree back. A theme or mode change, and a guest that reports itself
 *   stale(), make the band rebuild the tree, as the overlay path rebuilds a
 *   cached tree built under an older theme generation.
 *
 * The host's XML wraps its content in a `side_band_main` column beside a
 * `<side_band>` component; on ultrawide the host keeps the full width
 * (uses_full_width()) and the band sizes the main column to the cap, so the
 * main content sits exactly where the panel cap would have put it.
 *
 * @threading Main thread only
 */

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "subject_managed_panel.h"

#include <memory>

namespace helix::ui {

/// What a side band hosts.
enum class SideBandContent {
    Motion, ///< The Motion panel, beside Controls
    Ams,    ///< The AMS panel, beside Filament
};

/**
 * @brief Whether a host's side band shows @p content
 *
 * Only on the ultrawide layout. Motion needs no hardware. The AMS view needs
 * @p lane_tray, has_lane_tray() (include/ui_ams_mini_status.h): a filament
 * system or several heads, with lanes to draw, the rule Print Status's lane tray
 * follows too. Without it the band stays empty.
 */
bool side_band_shows(SideBandContent content, bool ultrawide, bool lane_tray);

/// The view a side band hosts: its tree and its lifecycle.
class SideBandGuest {
  public:
    virtual ~SideBandGuest() = default;

    /// Build the view's tree in @p band, or move its existing one there. Null
    /// when the tree is open elsewhere (on the navigation stack) or cannot be built.
    virtual lv_obj_t* attach(lv_obj_t* band) = 0;

    /// Free the tree attach() returned; a no-op when it is already gone.
    virtual void detach() = 0;

    /// The view's lifecycle; null once the singleton is gone.
    virtual IPanelLifecycle* lifecycle() = 0;

    /// The host left the screen for real (not a screensaver suspend): the
    /// guest drops any place it was holding, as its overlay does on close.
    virtual void on_host_left() {}

    /// The tree attach() built is no longer the view this printer needs (the
    /// AMS view after the unit count changed sides). The band rebuilds it.
    virtual bool stale() {
        return false;
    }
};

/// The Motion panel as a side band guest.
std::unique_ptr<SideBandGuest> make_motion_side_band_guest();
/// The AMS panel as a side band guest.
std::unique_ptr<SideBandGuest> make_ams_side_band_guest();

class SideBand {
  public:
    /// @p subject_name names the 0/1 subject the host's XML binds: 1 while the
    /// band holds and shows its content (the band is visible and the host hides its own
    /// button for the overlay). @p hosted_subject_name is 1 while the guest's
    /// tree sits in the band, which its header binds to hide the back button.
    SideBand(SideBandContent content, std::unique_ptr<SideBandGuest> guest,
             const char* subject_name, const char* hosted_subject_name);
    ~SideBand();

    SideBand(const SideBand&) = delete;
    SideBand& operator=(const SideBand&) = delete;

    /// Register the subject; before the host's XML is created.
    void init_subjects();
    void deinit_subjects();

    /// Bind to a freshly built host tree: its `side_band_main` column and its
    /// `side_band`. Call from the host's setup(), every build.
    void bind(lv_obj_t* host_root);

    void on_host_activate();
    void on_host_deactivate(DeactivateReason reason);

    /// The host's uses_full_content_width(): true on ultrawide, where the band
    /// caps the host's main column itself and takes the rest of the width.
    [[nodiscard]] bool uses_full_width() const;

    /// The gate's current answer. The subject is 1 only while the band also
    /// holds the guest's tree.
    [[nodiscard]] bool shows() const {
        return shows_;
    }

    /// The guest tree the band holds; null when it holds none.
    [[nodiscard]] lv_obj_t* hosted() const {
        return hosted_.get();
    }

    /// Re-read the gate inputs and attach or detach. The observers call it;
    /// exposed so a test can drive it after changing an input.
    void refresh();

    /**
     * @brief An open request for @p content's overlay
     *
     * True when a host on screen already shows it in its band: there is nothing
     * to open. Otherwise a band holding the tree hands it back and this returns
     * false, so the overlay opens with a tree of its own.
     */
    static bool take_open_request(SideBandContent content);

    /// @p content's tree was freed or left the navigation stack: bands waiting
    /// for it try again on the next UpdateQueue drain.
    static void notify_released(SideBandContent content);

  private:
    void attach_guest();
    void detach_guest();
    void activate_guest();
    void deactivate_guest(DeactivateReason reason);
    void fit_main_column();
    void publish();
    void observe_inputs();
    static void on_band_deleted(lv_event_t* e);
    static void on_host_size_changed(lv_event_t* e);

    SideBandContent content_;
    std::unique_ptr<SideBandGuest> guest_;
    const char* subject_name_;
    const char* hosted_subject_name_;

    SubjectManager subjects_;
    helix::Subject shows_subject_{};
    helix::Subject hosted_subject_{};
    bool subjects_initialized_ = false;

    WidgetRef host_root_;
    WidgetRef main_;
    WidgetRef band_;
    WidgetRef hosted_;

    bool host_active_ = false;
    bool guest_active_ = false;
    bool shows_ = false;

    ObserverGuard ultrawide_observer_;
    ObserverGuard lane_tray_observer_;
    ObserverGuard lanes_observer_;
    ObserverGuard theme_observer_;
    ObserverGuard active_panel_observer_;

    /// An overlay covers the host: its tree keeps its place, unless the user
    /// switches to another main panel from under the overlay.
    bool covered_ = false;
    /// The active panel when the overlay covered the host.
    int panel_at_cover_ = -1;

    /// The hosted tree, if it is still in the band. A tree a cache pulled out
    /// (reparented to its condemned container) is no longer the band's, though
    /// its WidgetRef lives until the async delete.
    lv_obj_t* live_hosted() const;

    /// theme_manager_get_changed_subject()'s generation when the hosted tree
    /// was built; its widgets keep the colors of that generation.
    int theme_gen_ = 0;

    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
