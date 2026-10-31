// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_updates.h
 * @brief Updates settings overlay - update channel, check, install, and the
 *        firmware-managed / cannot-install notices
 *
 * Also owns the update download modal, which the "New Version Available"
 * notification opens without this overlay ever being shown.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see UpdateChecker for update logic
 */

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "observer_factory.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

class UpdatesSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Updates";
    }
    const char* xml_component() const override {
        return "settings_updates_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

    /**
     * @brief Point both Update Channel rows at the channel the updater is using
     *
     * settings_updates_overlay.xml carries a Stable/Beta row and a Stable/Beta/Dev
     * row. Neither binds its selection to update_channel, because a row must show
     * the *effective* channel rather than the stored one: Dev needs beta features,
     * so a locked install runs on Stable while /update/channel still reads Dev,
     * and the two-entry row has no index that renders a stored Dev at all.
     *
     * A row with too few options for the value is left alone rather than clamped,
     * since LVGL would render Dev's index 2 as Beta on the two-entry row.
     *
     * Both the tree and the channel are parameters so this is testable without
     * an overlay instance, a Config, or an UpdateChecker.
     *
     * @param root Overlay root to search, or nullptr for a no-op
     * @param effective_channel Channel index to show (UpdateChannel's enumerators)
     */
    static void sync_update_channel_rows(lv_obj_t* root, int effective_channel);

    // When start_immediately is true, skip the Confirming state and begin the
    // download directly: the user already confirmed on the "New Version
    // Available" notification modal.
    void show_update_download_modal(bool start_immediately = false);
    void hide_update_download_modal();

  private:
    /// Brings the download dialog back for an error reported after a backdrop
    /// tap, ESC or a modal-stack sweep closed it. The worker outlives the
    /// dialog, and update_download_modal is the only view of download_status,
    /// so without this a failed update leaves nothing on screen
    /// (prestonbrown/helixscreen#1727).
    void on_download_status(int status);

    helix::ui::WidgetRef update_download_modal_;
    /// Set by hide_update_download_modal(): the user's own Cancel/Close, which
    /// an error arriving afterwards must not override.
    bool download_modal_closed_by_user_ = false;
    ObserverGuard download_status_observer_;
};

inline UpdatesSettingsOverlay& get_updates_settings_overlay() {
    return lazy_global<UpdatesSettingsOverlay>("UpdatesSettingsOverlay");
}

} // namespace helix::settings
