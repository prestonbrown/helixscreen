// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Advanced > HelixPrint Plugin: what the installed plugin runs. Read-only: the
// version, then one line per feature in the plugin's status map (active, off,
// or unavailable with the plugin's reason). A feature with a setting leads to
// the page that holds its switch; the switches live there, never here. The
// lines are the plugin's feature map, which the XML cannot enumerate.

#include "ui_observer_guard.h"

#include "core/static_panel_registry.h"
#include "helix_plugin_features.h"
#include "overlay_base.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

class PluginOverviewOverlay : public OverlayBase {
  public:
    ~PluginOverviewOverlay() override;

    void register_callbacks() override;

    const char* get_name() const override {
        return "HelixPrint Plugin";
    }
    const char* xml_component() const override {
        return "plugin_overview_overlay";
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    /// Rebuild the lines from the plugin's current status.
    void refresh();

    const std::vector<PluginFeatureLine>& lines() const {
        return lines_;
    }

  protected:
    void before_show() override;

  private:
    void create_line(const PluginFeatureLine& line);
    void create_row(const char* component, std::vector<std::pair<std::string, std::string>> attrs,
                    const std::string& name);
    static void on_feature_clicked(lv_event_t* e);

    lv_obj_t* rows_container_ = nullptr;
    std::vector<PluginFeatureLine> lines_;
    ObserverGuard probe_observer_;
};

/// A feature line's short status as the overview shows it at the right:
/// Active, Off or Unavailable.
std::string plugin_feature_status_text(const PluginFeatureLine& line);

/// Why an unavailable line's feature cannot run, shown under its name
/// (plugin_reason_text); empty for an available line or one with no reason.
std::string plugin_feature_reason_text(const PluginFeatureLine& line);

inline PluginOverviewOverlay& get_plugin_overview_overlay() {
    return lazy_global<PluginOverviewOverlay>("PluginOverviewOverlay");
}

} // namespace helix::ui
