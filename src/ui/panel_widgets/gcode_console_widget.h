// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

namespace helix {

class GCodeConsoleWidget : public PanelWidget {
    friend class GCodeConsoleWidgetTestAccess;

  public:
    GCodeConsoleWidget();
    ~GCodeConsoleWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "gcode_console";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Static: multiple widget instances share the same global ConsolePanel singleton
    static inline lv_obj_t* console_panel_ = nullptr;

    void handle_click();
};

void register_gcode_console_widget();

} // namespace helix
