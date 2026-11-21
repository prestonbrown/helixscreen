// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Set ui_is_ultrawide for one case and put it back after.
class ScopedUltrawide {
  public:
    explicit ScopedUltrawide(int value) : subject_(lv_xml_get_subject(nullptr, "ui_is_ultrawide")) {
        REQUIRE(subject_ != nullptr);
        original_ = lv_subject_get_int(subject_);
        lv_subject_set_int(subject_, value);
    }
    ~ScopedUltrawide() {
        lv_subject_set_int(subject_, original_);
    }
    ScopedUltrawide(const ScopedUltrawide&) = delete;
    ScopedUltrawide& operator=(const ScopedUltrawide&) = delete;
    void set(int value) {
        lv_subject_set_int(subject_, value);
    }

  private:
    lv_subject_t* subject_;
    int original_ = 0;
};

/// A host tree shaped like controls_panel.xml: a row holding side_band_main
/// and side_band.
inline lv_obj_t* make_side_band_host(lv_obj_t* parent) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_t* main = lv_obj_create(root);
    lv_obj_set_name(main, "side_band_main");
    lv_obj_set_size(main, lv_pct(100), lv_pct(100));
    lv_obj_t* band = lv_obj_create(root);
    lv_obj_set_name(band, "side_band");
    lv_obj_set_height(band, lv_pct(100));
    lv_obj_set_flex_grow(band, 1);
    return root;
}

} // namespace helix::test
