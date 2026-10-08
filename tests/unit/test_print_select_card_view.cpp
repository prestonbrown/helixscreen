// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_card_view.cpp
 * @brief Unit tests for PrintSelectCardView thumbnail path helpers
 *
 * Tests is_placeholder_thumbnail() and has_real_thumbnail() to prevent
 * regressions where LVGL "A:" drive prefixes break std::filesystem::exists().
 */

#include "ui_panel_print_select.h"
#include "ui_print_select_card_view.h"
#include "ui_virtual_list.h"

#include "../lvgl_ui_test_fixture.h"
#include "lvgl/src/display/lv_display_private.h" // inv_areas
#include "lvgl/src/misc/lv_area_private.h"       // lv_area_is_in

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::PrintSelectCardView;

// ============================================================================
// is_placeholder_thumbnail
// ============================================================================

TEST_CASE("CardView: is_placeholder_thumbnail treats empty path as placeholder",
          "[ui][card_view]") {
    REQUIRE(PrintSelectCardView::is_placeholder_thumbnail(""));
}

TEST_CASE("CardView: is_placeholder_thumbnail rejects real paths", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail(
        "A:/home/user/.cache/helix/abc123_160x160_ARGB8888.bin"));
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail("A:/tmp/thumb.png"));
}

// ============================================================================
// has_real_thumbnail
// ============================================================================

TEST_CASE("CardView: has_real_thumbnail returns false for empty path", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail(""));
}

TEST_CASE("CardView: has_real_thumbnail returns false for nonexistent file", "[ui][card_view]") {
    REQUIRE_FALSE(
        PrintSelectCardView::has_real_thumbnail("A:/tmp/does_not_exist_helix_test_thumb.bin"));
}

TEST_CASE("CardView: has_real_thumbnail with A: prefix finds existing file", "[ui][card_view]") {
    // Create a temporary file to test against
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail without A: prefix finds existing file", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb2.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail(tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail returns false after file deleted", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb3.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }
    std::filesystem::remove(tmp);

    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));
}

// ============================================================================
// Card pool sizing
// ============================================================================

namespace {

std::vector<PrintFileData> make_files(int n) {
    std::vector<PrintFileData> files(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        files[static_cast<size_t>(i)].filename = "file_" + std::to_string(i) + ".gcode";
    }
    return files;
}

/// Cards a window needs at the container's current scroll offset.
size_t window_cards(lv_obj_t* container, const CardDimensions& dims, int file_count) {
    const int stride = dims.card_height + lv_obj_get_style_pad_row(container, LV_PART_MAIN);
    const int rows = (file_count + dims.num_columns - 1) / dims.num_columns;
    const auto w =
        helix::ui::compute_window(lv_obj_get_scroll_y(container), lv_obj_get_height(container),
                                  stride, rows, PrintSelectCardView::BUFFER_ROWS);
    return static_cast<size_t>(std::min(file_count, w.last * dims.num_columns) -
                               w.first * dims.num_columns);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "CardView: the pool holds the visible window, not a fixed 24",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(container, 10, LV_PART_MAIN);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(60);

    view.populate(files, dims);
    const size_t first = window_cards(container, dims, 60);
    CHECK(view.pool_size() == first);
    CHECK(first < 24);

    // Scrolled into the middle, the window spans overscan rows on both sides.
    lv_obj_scroll_to_y(container, 1000, LV_ANIM_OFF);
    view.update_visible(files, dims);
    const size_t middle = window_cards(container, dims, 60);
    REQUIRE(middle > first);
    CHECK(view.pool_size() == middle);

    // Shown cards are exactly the window; the two spacers are never hidden.
    int shown = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        if (!lv_obj_has_flag(lv_obj_get_child(container, static_cast<int32_t>(i)),
                             LV_OBJ_FLAG_HIDDEN)) {
            ++shown;
        }
    }
    CHECK(static_cast<size_t>(shown - 2) == middle);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: the whole range is exactly the cards inside the viewport",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(container, 10, LV_PART_MAIN);
    const int32_t pad_top = GENERATE(0, 12);
    CAPTURE(pad_top);
    lv_obj_set_style_pad_top(container, pad_top, LV_PART_MAIN);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(60);
    view.populate(files, dims);

    for (int scroll : {0, 5, 15, 100, 205, 225, 430, 640, 1000}) {
        lv_obj_scroll_to_y(container, scroll, LV_ANIM_OFF);
        view.update_visible(files, dims);
        lv_obj_update_layout(container);
        size_t first = 0;
        size_t end = 0;
        view.get_whole_range(first, end);

        lv_area_t viewport;
        lv_obj_get_coords(container, &viewport);
        std::set<size_t> whole;
        for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
            lv_obj_t* card = lv_obj_get_child(container, static_cast<int32_t>(i));
            if (lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_width(card) != 160) {
                continue; // a pool card not shown, or a spacer
            }
            lv_area_t a;
            lv_obj_get_coords(card, &a);
            if (a.y1 >= viewport.y1 && a.y2 <= viewport.y2) {
                whole.insert(reinterpret_cast<size_t>(lv_obj_get_user_data(card)));
            }
        }
        INFO("scroll " << lv_obj_get_scroll_y(container));
        if (whole.empty()) { // both rows in view cut by an edge
            CHECK(first == end);
            continue;
        }
        CHECK(first == *whole.begin());
        CHECK(end == *whole.rbegin() + 1);
        CHECK(end - first == whole.size());
    }

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "CardView: cards on a solid background draw an opaque gradient, otherwise a masked one",
    "[ui][card_view][print_select]") {
    lv_obj_t* page = lv_obj_create(test_screen());
    lv_obj_set_size(page, 720, 420);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_t* container = lv_obj_create(page);
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    const bool graded = GENERATE(false, true);
    CAPTURE(graded);
    if (graded) {
        lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(page, lv_color_hex(0x303030), LV_PART_MAIN);
    }

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    view.populate(make_files(8), CardDimensions{4, 2, 160, 200});

    lv_obj_t* gradient = lv_obj_find_by_name(container, "gradient_bg");
    REQUIRE(gradient != nullptr);
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(gradient));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == (graded ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_NATIVE));

    view.cleanup();
    lv_obj_delete(page);
}

namespace {

std::string fixture_path(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/" : "tests/fixtures/";
    return "A:" + std::filesystem::absolute(dir + name).string();
}

/// The file path a shown card's thumbnail image points at, "" for none.
std::string card_thumb_src(lv_obj_t* container, size_t file_index) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_t* card = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN) ||
            reinterpret_cast<size_t>(lv_obj_get_user_data(card)) != file_index) {
            continue;
        }
        lv_obj_t* img = lv_obj_find_by_name(card, "thumbnail");
        const void* src = img ? lv_image_get_src(img) : nullptr;
        return src && lv_image_src_get_type(src) == LV_IMAGE_SRC_FILE
                   ? static_cast<const char*>(src)
                   : "";
    }
    return "";
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "CardView: an arrived thumbnail updates only its own card",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    auto files = make_files(20);
    view.populate(files, dims);

    // Two files gain a thumbnail, but only file 3's arrival is reported.
    const std::string thumb = fixture_path("thumb_filters_rgba.png");
    files[1].thumbnail_path = thumb;
    files[3].thumbnail_path = thumb;
    REQUIRE(card_thumb_src(container, 3) != thumb);

    CHECK(view.update_thumbnail(3, files[3]));
    CHECK(card_thumb_src(container, 3) == thumb);
    CHECK(card_thumb_src(container, 1) != thumb);

    // A file no card shows changes nothing.
    files[19].thumbnail_path = thumb;
    CHECK_FALSE(view.update_thumbnail(19, files[19]));

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: every shown card names its own file, in order, as the window moves",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(container, 10, LV_PART_MAIN);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(60);
    view.populate(files, dims);

    const int stride = dims.card_height + 10;
    for (int y : {stride, 3 * stride, 2 * stride + 40, 7 * stride, 0, 5 * stride}) {
        CAPTURE(y);
        lv_obj_scroll_to_y(container, y, LV_ANIM_OFF);
        view.update_visible(files, dims);
        lv_obj_update_layout(container);

        const auto w =
            helix::ui::compute_window(lv_obj_get_scroll_y(container), lv_obj_get_height(container),
                                      stride, 15, PrintSelectCardView::BUFFER_ROWS);
        int expect = w.first * dims.num_columns;
        for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
            lv_obj_t* card = lv_obj_get_child(container, static_cast<int32_t>(i));
            lv_obj_t* label = lv_obj_find_by_name(card, "filename_label");
            if (!label || lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN))
                continue;
            CHECK(std::string(lv_label_get_text(label)) == "file_" + std::to_string(expect));
            ++expect;
        }
        CHECK(expect == std::min(60, w.last * dims.num_columns));
    }

    view.cleanup();
    lv_obj_delete(container);
}

namespace {

/// Every area the display has queued to redraw.
std::vector<lv_area_t> invalid_areas() {
    lv_display_t* disp = lv_display_get_default();
    return {disp->inv_areas, disp->inv_areas + disp->inv_p};
}

/// The prebuild timer is periodic, which the test harness only runs with a
/// finite repeat count; the tick still ends it once the pool is full.
void lend_prebuild_ticks(const PrintSelectCardView& view) {
    if (lv_timer_t* t = view.prebuild_timer_for_test()) {
        lv_timer_set_repeat_count(t, 1000);
    }
}

/// The shown card for `file_index`.
lv_obj_t* card_for(lv_obj_t* container, size_t file_index) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_t* card = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (!lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN) &&
            reinterpret_cast<size_t>(lv_obj_get_user_data(card)) == file_index) {
            return card;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: a metadata refresh repaints only the cards whose data changed",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    auto files = make_files(20);
    const std::string thumb = fixture_path("thumb_filters_rgba.png");
    for (size_t i = 0; i < 6; ++i) {
        files[i].thumbnail_path = thumb;
        files[i].print_time_str = "1h";
        files[i].filament_str = "10 g";
    }
    view.populate(files, dims);
    lv_refr_now(nullptr);
    REQUIRE(invalid_areas().empty());

    // The same data again: nothing on screen changes, so nothing is redrawn.
    view.refresh_content(files, dims);
    CHECK(invalid_areas().empty());

    // One file's metadata changes: only its card is redrawn.
    files[3].print_time_str = "2h";
    view.refresh_content(files, dims);
    lv_obj_t* card = card_for(container, 3);
    REQUIRE(card != nullptr);
    lv_area_t card_area;
    lv_obj_get_coords(card, &card_area);
    const auto areas = invalid_areas();
    REQUIRE_FALSE(areas.empty());
    for (const lv_area_t& a : areas) {
        CHECK(lv_area_is_in(&a, &card_area, 0));
    }

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: a thumbnail back from a placeholder at the same path is reloaded",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    auto files = make_files(4);
    const std::string thumb = fixture_path("thumb_filters_rgba.png");
    files[1].thumbnail_path = thumb;
    view.populate(files, dims);
    lv_refr_now(nullptr);
    lv_obj_t* card = card_for(container, 1);
    REQUIRE(card != nullptr);
    lv_obj_t* img = lv_obj_find_by_name(card, "thumbnail");
    REQUIRE(img != nullptr);
    REQUIRE(card_thumb_src(container, 1) == thumb);

    // A re-sliced file can land at the same cache path with new dimensions, so
    // the placeholder in between has to drop the old source.
    files[1].thumbnail_path.clear();
    view.refresh_content(files, dims);
    CHECK(lv_image_get_src(img) == nullptr);

    files[1].thumbnail_path = thumb;
    view.refresh_content(files, dims);
    CHECK(card_thumb_src(container, 1) == thumb);

    view.cleanup();
    lv_obj_delete(container);
}

namespace {

int count_objects(lv_obj_t* obj) {
    int n = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        n += count_objects(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "CardView: a card carries only the objects it draws, and no fill under its gradient",
    "[ui][card_view][print_select]") {
    // Rendering walks every child of a card once per display band, so each
    // object a card carries is paid for many times over on a full repaint.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(4);
    view.populate(files, dims);
    lv_obj_t* card = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container) && !card; ++i) {
        lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (lv_obj_find_by_name(child, "gradient_bg")) {
            card = child;
        }
    }
    REQUIRE(card != nullptr);

    // Root, gradient, thumbnail, three state icons, overlay, filename, and the
    // metadata row with its two icon + text pairs.
    CHECK(count_objects(card) == 13);
    lv_obj_t* time_label = lv_obj_find_by_name(card, "time_label");
    REQUIRE(time_label != nullptr);
    CHECK(lv_obj_get_parent(time_label) == lv_obj_find_by_name(card, "metadata_row"));

    // The gradient covers the whole card, so a fill beneath it is never seen.
    lv_obj_t* gradient = lv_obj_find_by_name(card, "gradient_bg");
    REQUIRE(gradient != nullptr);
    lv_obj_update_layout(container);
    CHECK(lv_obj_get_width(gradient) == lv_obj_get_width(card));
    CHECK(lv_obj_get_height(gradient) == lv_obj_get_height(card));
    CHECK(lv_obj_get_style_bg_opa(card, LV_PART_MAIN) == LV_OPA_TRANSP);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: building the pool does not lay out the grid once per card",
                 "[ui][card_view][print_select]") {
    // A stretched image lays the whole screen out to size itself, so pointing
    // each new card's gradient at the shared buffer reflowed the grid per card.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    int layouts = 0;
    lv_obj_add_event_cb(
        container, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_LAYOUT_CHANGED, &layouts);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(20);
    view.populate(files, dims);
    REQUIRE(view.pool_size() >= 8);
    CHECK(layouts <= 2);

    // Every card still shows the one shared gradient.
    std::set<const void*> gradients;
    size_t cards = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_t* g = lv_obj_find_by_name(lv_obj_get_child(container, static_cast<int32_t>(i)),
                                          "gradient_bg");
        if (g) {
            ++cards;
            gradients.insert(lv_image_get_src(g));
        }
    }
    CHECK(cards == view.pool_size());
    CHECK(gradients.size() == 1);
    CHECK(gradients.count(nullptr) == 0);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "CardView: a prebuild builds the first screen a card per tick, so the fill builds none",
    "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(20);
    lv_obj_update_layout(container);
    const size_t window = window_cards(container, dims, 20);
    REQUIRE(window > 2);

    view.prebuild(dims, files.size());
    lend_prebuild_ticks(view);
    CHECK(view.is_prebuilding());
    CHECK(view.pool_size() == 0); // nothing is built in the caller's frame
    process_lvgl(120);
    CHECK(view.pool_size() == 1);
    for (int i = 0; i < 100 && view.is_prebuilding(); ++i) {
        process_lvgl(120);
    }
    CHECK_FALSE(view.is_prebuilding());
    CHECK(view.pool_size() == window);

    view.populate(files, dims);
    CHECK(view.pool_size() == window);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "CardView: a listing that lands mid-prebuild fills the window and ends it",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(20);
    lv_obj_update_layout(container);
    const size_t window = window_cards(container, dims, 20);

    view.prebuild(dims, files.size());
    lend_prebuild_ticks(view);
    process_lvgl(120);
    REQUIRE(view.pool_size() == 1);

    view.populate(files, dims);
    CHECK(view.pool_size() == window);
    for (int i = 0; i < 5; ++i) {
        process_lvgl(120);
    }
    CHECK_FALSE(view.is_prebuilding());
    CHECK(view.pool_size() == window);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(LVGLUITestFixture, "CardView: a stopped prebuild builds no more cards",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    view.prebuild(dims, 20);
    lend_prebuild_ticks(view);
    process_lvgl(120);
    REQUIRE(view.pool_size() == 1);

    view.stop_prebuild();
    CHECK_FALSE(view.is_prebuilding());
    for (int i = 0; i < 10; ++i) {
        process_lvgl(120);
    }
    CHECK(view.pool_size() == 1);

    // Cleanup mid-prebuild leaves no tick behind to reach the freed pool.
    view.prebuild(dims, 20);
    lend_prebuild_ticks(view);
    REQUIRE(view.is_prebuilding());
    view.cleanup();
    for (int i = 0; i < 5; ++i) {
        process_lvgl(120);
    }
    CHECK(view.pool_size() == 0);
    lv_obj_delete(container);
}
