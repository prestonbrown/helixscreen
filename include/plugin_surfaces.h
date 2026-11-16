// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "plugin_dialog.h"
#include "plugin_overlay_host.h"
#include "plugin_presenter.h"

#include <list>
#include <string>

namespace helix::plugin {

/**
 * @brief Every plugin dialog and overlay, from request to its one close report
 *
 * Each request gets a handle at once and passes through the PluginPresenter
 * queue; the handle stays valid while the surface waits, shows, and until its
 * close is reported. Overlays are shown through PluginOverlayHost, dialogs
 * through PluginDialog. One per PluginHost. Main thread only.
 */
class PluginSurfaces {
  public:
    PluginSurfaces(PluginOverlayHost& overlays, PluginPresenter& presenter);
    /// Closes the dialogs still showing, reporting nothing: their plugins go with
    /// the host.
    ~PluginSurfaces();
    PluginSurfaces(const PluginSurfaces&) = delete;
    PluginSurfaces& operator=(const PluginSurfaces&) = delete;

    /// A handle > 0, or 0 when the surface was presented at once and could not
    /// be created. An overlay for a component the plugin has already requested
    /// returns that request's handle and keeps its callbacks.
    int open_overlay(const std::string& plugin_id, const std::string& component,
                     PluginUi::Callbacks cb, const PluginUi::Attrs& attrs);
    int open_dialog(const std::string& plugin_id, const std::string& component,
                    PluginUi::Callbacks cb, const PluginUi::Attrs& attrs);
    /// Closes a shown surface ("plugin") or withdraws a queued one ("cancelled").
    void close(int handle);
    bool is_open(int handle) const;
    size_t overlay_count(const std::string& plugin_id) const;
    size_t dialog_count(const std::string& plugin_id) const;
    void queue_confirm(const std::string& plugin_id, std::function<bool()> present,
                       std::function<void()> cancelled);
    /// Forgets everything @p plugin_id has requested, reporting nothing, and
    /// closes its dialogs. Its overlays are closed by PluginOverlayHost::close_all.
    void drop_plugin(const std::string& plugin_id);

  private:
    struct Record {
        int handle = 0;
        std::string plugin_id;
        std::string component;
        SurfaceKind kind = SurfaceKind::Overlay;
        PluginUi::Attrs attrs;
        PluginUi::Callbacks cb;
        int ticket = 0;
        bool shown = false;
        int overlay_handle = 0;
        helix::ui::WeakWidget dialog;
        bool closing_by_plugin = false;
    };

    int request(Record rec);
    bool present(int handle);
    void finish(int handle, CloseHow how);
    Record* find(int handle);
    const Record* find(int handle) const;

    PluginOverlayHost& overlays_;
    PluginPresenter& presenter_;
    std::list<Record> records_;
    int next_handle_ = 1;
    AsyncLifetimeGuard guard_;
};

} // namespace helix::plugin
