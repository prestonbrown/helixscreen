// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Advanced > HelixPrint Plugin: what the installed plugin runs. Read-only: the
// version, then one dense row per feature in the plugin's status map, grouped
// as running, off, and not available here (with the plugin's reason in plain
// words). A feature with a setting leads to the page that holds its switch;
// the switches live there, never here. The rows are the plugin's feature map,
// which the XML cannot enumerate.

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "helix_plugin_features.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

class PluginOverviewOverlay : public OverlayBase {
  public:
    ~PluginOverviewOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "HelixPrint Plugin";
    }
    const char* xml_component() const override {
        return "plugin_overview_overlay";
    }
    bool destroy_on_close() const override {
        return true;
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
    static void on_feature_clicked(lv_event_t* e);

    lv_obj_t* running_rows_ = nullptr;
    lv_obj_t* off_rows_ = nullptr;
    lv_obj_t* unavailable_rows_ = nullptr;
    std::vector<PluginFeatureLine> lines_;
    ObserverGuard probe_observer_;

    void on_ui_destroyed() override {
        running_rows_ = nullptr;
        off_rows_ = nullptr;
        unavailable_rows_ = nullptr;
        probe_observer_.reset();
    }

    SubjectManager subjects_;
    lv_subject_t version_subject_{};
    char version_buf_[32] = {};
    lv_subject_t running_count_subject_{};
    lv_subject_t off_count_subject_{};
    lv_subject_t unavailable_count_subject_{};
};

/// How the overview shows one feature: the group it sits in (its status) and,
/// for an unavailable feature only, why in plain words.
struct PluginOverviewRow {
    PluginFeatureStatus status = PluginFeatureStatus::Unavailable;
    std::string reason;
};

/// The overview row for @p line: an unavailable feature's reason through
/// plugin_reason_text(), or a plain fallback when the plugin gave none; a
/// running or off feature carries no reason.
PluginOverviewRow plugin_overview_row(const PluginFeatureLine& line);

inline PluginOverviewOverlay& get_plugin_overview_overlay() {
    return lazy_global<PluginOverviewOverlay>("PluginOverviewOverlay");
}

} // namespace helix::ui
