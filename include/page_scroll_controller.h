// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <functional>

namespace helix::ui {

/** Attaches a reserved-gutter page-scroll control to a single scrollable
 *  container. Reserves a right gutter (content reflows narrower), hosts a
 *  floating chevron column, pages by ~90% of the viewport, and dims/disables
 *  each chevron at its end — hiding the whole gutter when content fits.
 *
 *  The chevrons name on_page_scroll_up / on_page_scroll_down, which find their
 *  controller by the gutter they sit in; the container's scroll, size and delete
 *  events are wired from C++. Main-thread only. */
class PageScrollController {
  public:
    PageScrollController() = default;
    ~PageScrollController();
    PageScrollController(const PageScrollController&) = delete;
    PageScrollController& operator=(const PageScrollController&) = delete;

    bool attach(lv_obj_t* container);
    void detach();

    void page_up();
    void page_down();
    void refresh_reach_state();

    /** Invoked when the managed container is destroyed (owner may prune). */
    void set_on_container_deleted(std::function<void()> cb) {
        on_deleted_ = std::move(cb);
    }

    lv_obj_t* container() const {
        return container_;
    }
    lv_obj_t* gutter() const {
        return gutter_;
    }
    /// The gutter: what the chevrons' callbacks find this controller by.
    lv_obj_t* root() const {
        return gutter_;
    }
    lv_obj_t* up_button() const {
        return up_btn_;
    }
    lv_obj_t* down_button() const {
        return down_btn_;
    }
    bool attached() const {
        return gutter_ != nullptr;
    }
    bool alive() const {
        return container_ != nullptr;
    }

  private:
    static void container_event_cb(lv_event_t* e);
    static void gutter_event_cb(lv_event_t* e);

    void scroll_by_page(int32_t direction); // -1 up, +1 down
    void apply_reserved_padding();
    void remove_reserved_padding();
    void on_container_deleted();
    void on_gutter_deleted();

    lv_obj_t* container_ = nullptr;
    lv_obj_t* gutter_ = nullptr;
    lv_obj_t* up_btn_ = nullptr;
    lv_obj_t* down_btn_ = nullptr;
    int32_t gutter_w_ = 0;
    int32_t saved_pad_right_ = 0;
    lv_scrollbar_mode_t saved_scrollbar_mode_ = LV_SCROLLBAR_MODE_AUTO;
    bool pad_applied_ = false;
    std::function<void()> on_deleted_;
};

} // namespace helix::ui
