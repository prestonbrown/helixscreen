// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"

#include <functional>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin {

/// What the ui bindings need to open overlays. PluginHost provides the real one; tests
/// may provide a fake. Handles are positive and unique per process.
struct PluginUi {
    /// Extra attributes for lv_xml_create, already policy-checked by the caller.
    using Attrs = std::vector<std::pair<std::string, std::string>>;
    std::function<int(const std::string& component, std::function<void()> on_closed,
                      const Attrs& attrs)>
        open;
    std::function<void(int handle)> close;
    /// Overlays `plugin_id` has open, so the binding can tell a no-op open
    /// (the component is already showing) from one that created an overlay.
    std::function<size_t(const std::string& plugin_id)> open_count;
};

/// Plugin overlays on the navigation stack. Main thread.
class PluginOverlayHost {
  public:
    PluginOverlayHost() = default;
    PluginOverlayHost(const PluginOverlayHost&) = delete;
    PluginOverlayHost& operator=(const PluginOverlayHost&) = delete;
    /// Deletes the roots of overlays still open: once the host is gone navigation
    /// never finishes them (a printer switch drops the queued closes).
    ~PluginOverlayHost();

    /// Registers `lifecycle` and a close callback for `root`, then pushes it.
    /// `on_nav_closed` runs when navigation closes the overlay while this host
    /// lives; after that the host's owner deletes the root, so the callback is a
    /// no-op. Every plugin overlay and settings screen goes through here.
    void push(lv_obj_t* root, IPanelLifecycle* lifecycle, std::function<void()> on_nav_closed);

    /// Creates `component` on the active screen and pushes it. 0 when it cannot be
    /// created. A component the plugin already has open is a no-op returning the
    /// existing handle. `on_closed` runs when the overlay is closed for any reason
    /// but an unload of its plugin.
    int open(const std::string& plugin_id, const std::string& component,
             std::function<void()> on_closed, const PluginUi::Attrs& attrs = {});
    void close(int handle);
    /// Pops and deletes every overlay `plugin_id` has open, without running on_closed.
    void close_all(const std::string& plugin_id);
    size_t open_count(const std::string& plugin_id) const;

  private:
    /// No-op lifecycle: plugin overlays have no activation work in this phase, but
    /// every push needs a registered instance so deactivation dispatch finds one.
    struct OverlayLifecycle : IPanelLifecycle {
        void on_activate() override {}
        void on_deactivate(DeactivateReason) override {}
        const char* get_name() const override {
            return "plugin-overlay";
        }
    };
    /// The single process-lifetime instance every record registers. Navigation
    /// keeps these registrations for as long as the widget exists, which can
    /// outlive this host (printer switch), so the object must outlive it too.
    static OverlayLifecycle& overlay_lifecycle();

    struct Record {
        int handle = 0;
        std::string plugin_id;
        std::string component;
        lv_obj_t* root = nullptr;
        std::function<void()> on_closed;
    };

    std::list<Record>::iterator find(int handle);
    /// Unregisters `it` from navigation, deferred-deletes its root and erases it,
    /// returning the next record. Runs on_closed when the record still carries one.
    std::list<Record>::iterator finish(std::list<Record>::iterator it);
    /// The navigation close callback: the overlay left the stack.
    void on_nav_closed(int handle);

    std::list<Record> records_;
    int next_handle_ = 1;
    AsyncLifetimeGuard guard_;
};

} // namespace helix::plugin
