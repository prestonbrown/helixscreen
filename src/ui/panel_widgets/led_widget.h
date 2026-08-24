// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>

class IMoonrakerAPI;

namespace helix {
class PrinterState;
}

namespace helix {

class LedWidget : public PanelWidget {
  public:
    LedWidget(PrinterState& printer_state, IMoonrakerAPI* api);
    ~LedWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "led";
    }

    // XML event callbacks (public for early registration in register_led_widget)
    static void light_toggle_cb(lv_event_t* e);

  private:
    PrinterState& printer_state_;
    IMoonrakerAPI* api_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* light_icon_ = nullptr;

    bool light_on_ = false;

    ObserverGuard led_version_observer_;
    ObserverGuard led_state_observer_;
    ObserverGuard led_brightness_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void handle_light_toggle();
    void update_light_icon();
    void flash_light_icon();
    void bind_led();
    void on_led_state_changed(int state);
};

} // namespace helix
