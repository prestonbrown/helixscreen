// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager_test_access.h"
#include "theme_manager.h"

#include <lvgl.h>

/// A display of one layout class for one scope: the LayoutManager override
/// that picks ui_xml/<layout>/ variants, the size-class tokens and the class's
/// XML subject (ui_is_portrait, ui_is_ultrawide) at 1. The tokens are repainted
/// for the fixture's display at the next case boundary (see ScopedResolution).
class ScopedLayoutClass {
  public:
    ScopedLayoutClass(const char* layout, const char* subject, int32_t w, int32_t h)
        : res_(lv_display_get_default(), w, h), subject_(lv_xml_get_subject(nullptr, subject)) {
        auto& lm = helix::LayoutManager::instance();
        LayoutManagerTestAccess::reset(lm);
        lm.set_override(layout);
        lm.init(w, h);
        theme_manager_refresh_layout_constants(lv_display_get_default());
        if (subject_) {
            was_ = lv_subject_get_int(subject_);
            lv_subject_set_int(subject_, 1);
        }
    }
    ~ScopedLayoutClass() {
        if (subject_) {
            lv_subject_set_int(subject_, was_);
        }
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }
    ScopedLayoutClass(const ScopedLayoutClass&) = delete;
    ScopedLayoutClass& operator=(const ScopedLayoutClass&) = delete;

  private:
    ScopedResolution res_;
    lv_subject_t* subject_;
    int was_ = 0;
};

/// A portrait display (480x800 unless given) for one scope.
class ScopedPortraitLayout : public ScopedLayoutClass {
  public:
    explicit ScopedPortraitLayout(int32_t w = 480, int32_t h = 800)
        : ScopedLayoutClass("portrait", "ui_is_portrait", w, h) {}
};
