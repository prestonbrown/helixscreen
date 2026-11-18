// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_fast_path.cpp
 * @brief The printer widget prefers its exact-size copy over the tier image
 *
 * The tier render costs a decode plus a runtime CONTAIN scale on every paint. When a
 * pre-scaled copy for the widget's current size is on disk, that is what must reach
 * lv_image_set_src, so the expensive path is the first display at a size rather than
 * every visit to the home panel.
 */

#include "../../include/lvgl_image_writer.h"
#include "../../include/prerendered_images.h"
#include "../../src/ui/panel_widgets/printer_image_widget.h"
#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t kW = 234;
constexpr int32_t kH = 209;

/// Builds the widget subtree the XML component provides: a container holding an
/// lv_image named "printer_image", laid out so the widget sees a real size.
struct Subtree {
    lv_obj_t* root = nullptr;
    lv_obj_t* img = nullptr;
};

Subtree make_subtree() {
    Subtree s;
    s.root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s.root, 400, 300);
    s.img = lv_image_create(s.root);
    lv_obj_set_name(s.img, "printer_image");
    lv_obj_set_size(s.img, kW, kH);
    lv_obj_update_layout(s.root);
    return s;
}

/// The src LVGL is currently pointing at, as a string, or "" when it holds none.
std::string current_src(lv_obj_t* img) {
    const void* src = lv_image_get_src(img);
    if (src == nullptr || lv_image_src_get_type(src) != LV_IMAGE_SRC_FILE)
        return {};
    return static_cast<const char*>(src);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Printer widget prefers the exact-size copy",
                 "[printer_image][fastpath]") {
    // attach() publishes the widget's printer-info and callout subjects.
    helix::test::init_panel_widget_subjects();
    Subtree s = make_subtree();
    REQUIRE(lv_obj_get_width(s.img) == kW);

    helix::PrinterImageWidget widget;
    widget.attach(s.root, lv_screen_active());
    widget.refresh_printer_image();

    const std::string tier = current_src(s.img);
    REQUIRE_FALSE(tier.empty()); // it resolved something to show

    // Put a pre-scaled copy on disk for exactly this widget size.
    const std::string cache_path = helix::get_cached_printer_image_path(tier, kW, kH);
    std::filesystem::create_directories(std::filesystem::path(cache_path).parent_path());
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0x7F);
    REQUIRE(
        helix::write_lvgl_bin(cache_path, kW, kH, LV_COLOR_FORMAT_ARGB8888, px.data(), px.size()));

    widget.refresh_printer_image();

    // The exact-size copy must win; the tier image must not be re-selected.
    CHECK(current_src(s.img) == "A:" + cache_path);
    CHECK(current_src(s.img) != tier);

    std::filesystem::remove(cache_path);
    widget.detach();
}
