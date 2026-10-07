// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager_test_access.h"
#include "theme_manager.h"

#include <lvgl.h>

/// A portrait display, LayoutManager type, size-class tokens and ui_is_portrait
/// for one scope. The tokens are repainted for the fixture's display at the next
/// case boundary (see ScopedResolution).
class ScopedPortraitLayout {
  public:
    explicit ScopedPortraitLayout(int32_t w = 480, int32_t h = 800)
        : res_(lv_display_get_default(), w, h),
          portrait_(lv_xml_get_subject(nullptr, "ui_is_portrait")) {
        auto& lm = helix::LayoutManager::instance();
        LayoutManagerTestAccess::reset(lm);
        lm.set_override("portrait");
        lm.init(w, h);
        theme_manager_refresh_layout_constants(lv_display_get_default());
        if (portrait_) {
            was_ = lv_subject_get_int(portrait_);
            lv_subject_set_int(portrait_, 1);
        }
    }
    ~ScopedPortraitLayout() {
        if (portrait_) {
            lv_subject_set_int(portrait_, was_);
        }
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }
    ScopedPortraitLayout(const ScopedPortraitLayout&) = delete;
    ScopedPortraitLayout& operator=(const ScopedPortraitLayout&) = delete;

  private:
    ScopedResolution res_;
    lv_subject_t* portrait_;
    int was_ = 0;
};
