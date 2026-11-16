// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_PLUGINS

#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "plugin_consent.h"
#include "plugin_host.h"

#include <functional>
#include <list>
#include <string>
#include <vector>

namespace helix::plugin {

/// Settings > Plugins: one row per discovered plugin, tapped to enable (after
/// consent), disable, or re-enable. Singleton, pattern ui_settings_safety.cpp.
class PluginsOverlay : public OverlayBase {
  public:
    /// The dialog step, injectable so tests can auto-confirm. Same shape as
    /// show_consent().
    using ConsentShower =
        std::function<void(const Manifest&, const std::vector<Permission>&, std::function<void()>)>;

    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

    const char* get_name() const override {
        return "Plugins";
    }
    const char* xml_component() const override {
        return "plugins_overlay";
    }

    lv_obj_t* root() const {
        return overlay_root_;
    }
    /// Rebuilds the rows from the live host: at open time and after every
    /// enable/disable, in place (safe_clean_children, then repopulate).
    void populate_rows();
    /// A row was tapped: consent-then-enable, the Settings/Disable choice, or
    /// Re-enable, by status. No-op for Invalid and Incompatible.
    void activate(const std::string& id);
    /// The binding whose address is `ud`, or null when it is not one of ours.
    const std::string* binding_at(const void* ud);
    /// Test hook replacing the consent dialog; nullptr restores show_consent().
    void set_consent_shower(ConsentShower shower);

  private:
    void show_consent_for(const Manifest& m, const std::vector<Permission>& grown,
                          std::function<void()> on_yes);

    /// Row payloads, pointed at by row user data. Append-only: rebuilt rows
    /// leave stale widgets whose user data must stay dereferenceable until the
    /// deferred delete lands.
    std::list<std::string> bindings_;
    ConsentShower consent_shower_;
};

/// The process-wide overlay instance (lazy, registered with StaticPanelRegistry).
inline PluginsOverlay& get_plugins_overlay() {
    return lazy_global<PluginsOverlay>("PluginsOverlay");
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
