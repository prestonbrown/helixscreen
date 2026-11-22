// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The spool render cache is bounded by bytes, not entries: where spools never
// turn it keeps only a few still renders, and where they turn it keeps a full
// turn of a large tray spool.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "board_identity.h"
#include "filament_tube_stroker.h"
#include "spool_vents.h"
#include "src/ui/filament/ui_spool_canvas_internal.h"

#include <cstring>
#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t kTraySpool = 96; // the largest spool the 800x480 tray draws
constexpr size_t kTraySpoolBytes = kTraySpool * kTraySpool * 4;

struct BoardOverride {
    explicit BoardOverride(helix::board::Board b) {
        helix::board::set_current_override(b);
        ui_spool_canvas_invalidate_cache();
    }
    ~BoardOverride() {
        helix::board::set_current_override(std::nullopt);
        ui_spool_canvas_invalidate_cache();
    }
};

std::vector<uint8_t> pixels_of(lv_obj_t* canvas) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    return {buf->data, buf->data + buf->data_size};
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas cache: where spools never turn, it keeps a few still renders",
                 "[spool_canvas][spool_cache]") {
    // The K2's board row asks for reduced effects, as low memory does on the AD5M and CC1.
    BoardOverride k2(helix::board::Board::K2);
    REQUIRE(helix::ui::reduced_effects());
    // A quarter megabyte: seven still 96 px renders.
    constexpr size_t budget = 256 * 1024;

    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kTraySpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));
    const std::vector<uint8_t> red = pixels_of(canvas);

    // Twenty other filaments: far more renders than the budget holds.
    for (uint32_t i = 1; i <= 20; ++i) {
        ui_spool_canvas_set_color(canvas, lv_color_hex(0x101010 * i));
        CHECK(helix::ui::spool_render_cache_bytes() <= budget);
    }
    CHECK(helix::ui::spool_render_cache_bytes() > 0);

    // The red render was evicted long ago; coming back draws it again.
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));
    CHECK(pixels_of(canvas) == red);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas cache: where spools turn, a large tray spool's whole turn stays",
                 "[spool_canvas][spool_cache]") {
    BoardOverride generic(helix::board::Board::Generic);
    if (helix::ui::reduced_effects())
        SKIP("this host has the memory of a constrained board");

    const int phases = helix::ui::spool_vent_phase_count(helix::ui::spool_vent_detail(kTraySpool));
    REQUIRE(phases == 24);

    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kTraySpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));
    auto* data = helix::ui::spool_canvas_data(canvas);
    REQUIRE(data != nullptr);
    ui_spool_canvas_invalidate_cache();

    for (int phase = 0; phase < phases; ++phase) {
        data->vent_phase = phase;
        REQUIRE(helix::ui::redraw_spool_pixels(data));
    }
    CHECK(helix::ui::spool_render_cache_bytes() == phases * kTraySpoolBytes);
}
