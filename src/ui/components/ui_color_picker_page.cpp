// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker_page.h"

#include "core/static_panel_registry.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

ColorPickerPage& get_color_picker_page() {
    return lazy_global<ColorPickerPage>("ColorPickerPage");
}

void ColorPickerPage::open(lv_obj_t* parent_screen, uint32_t initial_rgb,
                           const ColorPickerConfig& config, std::function<void(uint32_t)> on_change,
                           std::function<void()> on_close) {
    initial_ = initial_rgb & 0xFFFFFF;
    config_ = config;
    config_.tabs_in_body = false;
    on_change_ = std::move(on_change);
    on_close_ = std::move(on_close);
    ensure_color_picker_view_subjects();
    show(parent_screen);
}

void ColorPickerPage::before_show() {
    lv_obj_t* root = get_root();
    lv_obj_t* body = root ? lv_obj_find_by_name(root, "cpp_body") : nullptr;
    if (!body) {
        spdlog::error("[ColorPickerPage] cpp_body missing");
        return;
    }
    view_.attach(body, initial_, config_);
    view_.on_change([this](uint32_t rgb) {
        if (on_change_) {
            on_change_(rgb);
        }
    });
}

void ColorPickerPage::on_dismissed() {
    view_.detach();
    auto on_close = std::move(on_close_);
    on_close_ = nullptr;
    on_change_ = nullptr;
    if (on_close) {
        on_close();
    }
}

void ColorPickerPage::on_ui_destroyed() {
    view_.detach();
}

} // namespace helix::ui
