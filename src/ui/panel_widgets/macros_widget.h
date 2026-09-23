// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

namespace helix {

class MacrosWidget : public PanelWidget {
    friend class MacrosWidgetTestAccess;

  public:
    MacrosWidget();
    ~MacrosWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "macros";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    static inline lv_obj_t* macros_panel_ = nullptr;

    void handle_click();
};

void register_macros_widget();

} // namespace helix
