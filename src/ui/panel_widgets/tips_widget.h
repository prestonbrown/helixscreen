// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "panel_widget.h"
#include "tips_manager.h"

namespace helix {

class TipsWidget : public PanelWidget {
  public:
    TipsWidget();
    ~TipsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    /// Flips whether featured lines replace the tips, saves it and asks for a rebuild.
    bool on_edit_configure() override;
    /// Refuses a width whose title column cannot hold the widest unbreakable run of any tip title.
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "tips";
    }

    /// XML event callback — shows full tip detail
    static void tip_text_clicked_cb(lv_event_t* e);

  private:
    friend class TipsWidgetTestAccess;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;

    // Cached label for fade animation (looked up by name in widget_obj_)
    helix::ui::WidgetRef tip_label_;

    // Tip state
    PrintingTip current_tip_;
    PrintingTip pending_tip_;
    bool tip_animating_ = false;

    // Timer for rotating tips every 60 seconds
    lv_timer_t* tip_rotation_timer_ = nullptr;

    // Timer that checks Moonraker for a featured line (featured_tip.h) while active
    lv_timer_t* featured_poll_timer_ = nullptr;
    helix::AsyncLifetimeGuard lifetime_;

    // Featured lines replace the tips while one is current (featured_tip.h)
    bool featured_only_ = true;

    // The tile's size from on_size_changed; 0 until it is sized
    int tile_w_ = 0;
    int tile_h_ = 0;

    // Tip logic
    void update_tip_of_day();
    void show_tip(const PrintingTip& tip);
    /// Puts @p tip on the label now: text, the "Tip:" label, and its font.
    void set_displayed_tip(const PrintingTip& tip);
    /// The tile's text font, stepped down for a featured line until all of it fits.
    void fit_title_font();
    void poll_featured_tip();
    void apply_featured_tip(const nlohmann::json& value);
    void drop_featured_tip();
    void stop_timers();
    void start_tip_fade_transition(const PrintingTip& new_tip);
    void apply_pending_tip();
    void handle_tip_text_clicked();
    void handle_tip_rotation_timer();

    static void tip_rotation_timer_cb(lv_timer_t* timer);
    static void featured_poll_timer_cb(lv_timer_t* timer);
};

} // namespace helix
