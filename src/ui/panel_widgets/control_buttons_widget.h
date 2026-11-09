// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "panel_widget.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix {

/// How the two print-control buttons are laid out in a given box.
struct ControlButtonsLayout {
    bool column = false; ///< buttons stacked top to bottom
    bool labels = false; ///< buttons show their text beside the icon
    bool fill = false;   ///< buttons take the tile's full height, uncapped
};

/// Decide the layout for a tile of @p colspan x @p rowspan tracks drawn at
/// @p width_px x @p height_px. Buttons stack when the box is taller than it is
/// wide, and fill the tile's height once it is two cells tall. Labels
/// show only at two cells wide or more, never at the Tiny breakpoint, and only
/// when each button is at least @p button_need_px wide: the widest icon, gap,
/// label and horizontal padding a button draws. @p pad_px is the tile's inner
/// padding on each side and @p gap_px the space between the buttons.
ControlButtonsLayout decide_control_buttons_layout(int colspan, int rowspan, int width_px,
                                                   int height_px, int pad_px, int gap_px,
                                                   int button_need_px, bool tiny_breakpoint);

/// Home-panel widget with two print-control buttons: a primary Pause/Resume
/// button and a Stop button. Icon, label text, enabled state, and click
/// handling are owned by the shared helix::ui::PrintControlButtons singleton;
/// this widget only decides the layout for its size.
class ControlButtonsWidget : public PanelWidget {
  public:
    ControlButtonsWidget();
    ~ControlButtonsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "control_buttons";
    }

    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }

    /// Widest a labelled button draws: icon, gap, the widest label it can
    /// show and its inset. 0 before the tree exists.
    [[nodiscard]] int button_need_px() const;

  private:
    lv_obj_t* widget_obj_ = nullptr;
    ObserverGuard language_observer_;

    // Registered in the constructor: the manager parses this tile's XML
    // before attach() runs, and the parser drops a binding whose subject is
    // missing at parse time.
    std::string labels_name_{"control_buttons_labels"};
    std::string column_name_{"control_buttons_column"};
    std::string fill_name_{"control_buttons_fill"};
    lv_subject_t labels_subject_{};
    lv_subject_t column_subject_{};
    lv_subject_t fill_subject_{};
    SubjectManager subjects_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;
};

} // namespace helix
