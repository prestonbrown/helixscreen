// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "panel_widget.h"

#include <memory>
#include <string>

class ClogDetectionConfigModal;

namespace helix {
namespace ui {
class UiClogBar;
} // namespace ui

/// Panel widget for clog detection on the home panel: the FlowGuard bar.
///
/// A bar rather than an arc (#1017): the widget is authored wide and short,
/// the shape a horizontal scale wants, and both ends can carry a label, so a
/// Flowguard reading says which fault it is leaning toward. UiClogMeter's arc
/// is what the AMS sidebar and loaded card use.
class ClogDetectionWidget : public PanelWidget {
  public:
    ClogDetectionWidget() = default;
    ~ClogDetectionWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "clog_detection";
    }

    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

  private:
    void apply_config();

    nlohmann::json config_;
    helix::ui::WidgetRef widget_obj_;
    std::unique_ptr<ui::UiClogBar> clog_bar_;
    std::unique_ptr<ClogDetectionConfigModal> config_modal_;
};

} // namespace helix
