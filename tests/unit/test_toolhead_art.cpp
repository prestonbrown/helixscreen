// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_toolhead_badge.h"

#include "../lvgl_test_fixture.h"
#include "baked_art.h"
#include "toolhead_art.h"
#include "toolhead_style.h"

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::vector<uint8_t> flat(int32_t w, int32_t h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(w * h * 4));
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = b;
        px[i + 1] = g;
        px[i + 2] = r;
        px[i + 3] = a;
    }
    return px;
}

struct Canvas {
    lv_draw_buf_t* buf;
    lv_obj_t* obj;
    explicit Canvas(lv_obj_t* parent) {
        buf = lv_draw_buf_create(300, 300, LV_COLOR_FORMAT_ARGB8888, 0);
        obj = lv_canvas_create(parent);
        lv_canvas_set_draw_buf(obj, buf);
    }
    ~Canvas() {
        lv_obj_delete(obj);
        lv_draw_buf_destroy(buf);
    }
    bool draw(ToolheadStyle style, std::optional<lv_color_t> filament, int32_t scale) {
        lv_canvas_fill_bg(obj, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(obj, &layer);
        const bool drawn =
            draw_toolhead_art(&layer, style, 150, 150, filament, scale, LV_OPA_COVER);
        lv_canvas_finish_layer(obj, &layer);
        return drawn;
    }
    const uint8_t* at(int32_t x, int32_t y) const {
        return buf->data + y * buf->header.stride + x * 4;
    }
};

} // namespace

TEST_CASE("toolhead art: the tint fills the mask, the shade darkens it, the rest is untouched",
          "[toolhead-art]") {
    // Pixel 0 is under the mask and half shaded; pixel 1 is outside both.
    auto body = flat(2, 1, 10, 20, 30, 255);
    body[3] = 0;
    auto mask = flat(2, 1, 255, 255, 255, 255);
    mask[7] = 0;
    auto shade = flat(2, 1, 0, 0, 0, 128);
    shade[7] = 0;
    toolhead_art_compose(body.data(), mask.data(), shade.data(), 2, 1, lv_color_hex(0xff0000));
    CHECK((body[3] == 255 && body[1] == 0));
    CHECK(body[2] == Catch::Approx(127).margin(2));
    CHECK((body[4] == 10 && body[6] == 30));
}

TEST_CASE("toolhead art: resampling keeps colour and ignores transparent neighbours",
          "[toolhead-art]") {
    auto src = flat(2, 1, 0, 0, 200, 255);
    src[4] = 255; // the transparent pixel's colour must not bleed in
    src[7] = 0;
    std::vector<uint8_t> dst(4);
    baked_art_resample(src.data(), 2, 1, dst.data(), 1, 1);
    CHECK(dst[0] == 0);
    CHECK(dst[2] == 200);
    CHECK(dst[3] == Catch::Approx(127).margin(2));
}

TEST_CASE("toolhead art: a style without art keeps its renderer", "[toolhead-art]") {
    CHECK(toolhead_art_for(ToolheadStyle::DEFAULT) == nullptr);
    CHECK(toolhead_art_for(ToolheadStyle::AUTO) == nullptr);
}

TEST_CASE("toolhead art: the frame puts the nozzle tip on the filament column", "[toolhead-art]") {
    const ToolheadArt* art = toolhead_art_for(ToolheadStyle::ENDER3);
    REQUIRE(art != nullptr);
    const ToolheadArtRect r = toolhead_art_rect(*art, 100, 50, 10);
    CHECK(r.h == Catch::Approx(80 * art->height).margin(1));
    CHECK(r.w == Catch::Approx(r.h * art->aspect).margin(1));
    CHECK(100 - r.x == Catch::Approx(r.w * art->tip_x).margin(1));

    const ToolheadArtPoint e = toolhead_art_entry(*art, 100, 50, 10);
    CHECK(e.x == Catch::Approx(r.x + r.w * art->entry_x).margin(1));
    CHECK(e.y == Catch::Approx(r.y + r.h * art->entry_y).margin(1));
    const GlyphBounds g = toolhead_bounds(ToolheadStyle::ENDER3, 100, 50, 10);
    CHECK((g.left == r.x && g.top == r.y && r.y == 50 - r.h / 2));
    CHECK((g.right == r.x + r.w - 1 && g.bottom == r.y + r.h - 1));
}

TEST_CASE_METHOD(LVGLTestFixture, "toolhead art: redraws reuse one composed image",
                 "[toolhead-art]") {
    Canvas c(test_screen());
    const lv_color_t red = lv_color_hex(0xff0000);
    REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, red, 11));
    const unsigned builds = toolhead_art_build_count();
    for (int i = 0; i < 5; ++i)
        REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, red, 11));
    CHECK(toolhead_art_build_count() == builds);

    REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, lv_color_hex(0x00ff00), 11));
    CHECK(toolhead_art_build_count() == builds + 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "toolhead art: drawn pixels fill the bounds, tip takes filament",
                 "[toolhead-art]") {
    Canvas c(test_screen());
    const ToolheadStyle style = ToolheadStyle::CREALITY_K2;
    REQUIRE(c.draw(style, lv_color_hex(0x00ff00), 16));
    const GlyphBounds g = toolhead_bounds(style, 150, 150, 16);

    int32_t x1 = 300, y1 = 300, x2 = -1, y2 = -1, green = 0;
    for (int32_t y = 0; y < 300; ++y) {
        for (int32_t x = 0; x < 300; ++x) {
            const uint8_t* p = c.at(x, y);
            if (p[3] <= 8)
                continue;
            x1 = std::min(x1, x);
            x2 = std::max(x2, x);
            y1 = std::min(y1, y);
            y2 = std::max(y2, y);
            green += (p[1] > 110 && p[0] < 60 && p[2] < 60) ? 1 : 0;
        }
    }
    CHECK(x1 >= g.left);
    CHECK(x2 <= g.right);
    CHECK(y1 >= g.top);
    CHECK(y2 <= g.bottom);
    CHECK(g.left >= x1 - 2);
    CHECK(g.right <= x2 + 2);
    CHECK(g.top >= y1 - 2);
    CHECK(g.bottom <= y2 + 2);
    CHECK(green > 4);
}

namespace {

// clang-format off
// Printers whose real toolhead has no art yet: the remaining scope of #1873. Shrink-only: an
// entry that gains a toolhead_style fails the walk until it is removed here.
const std::set<std::string> kNoArtYet = {
    "creality_cr10", "elegoo_neptune_4", "elegoo_neptune_4_pro", "elegoo_neptune_4_plus",
    "elegoo_neptune_4_max", "kingroon_klp1", "prusa_mk4", "prusawire", "qidi_x_max_3",
    "qidi_x_plus_3", "qidi_x_smart_3", "qidi_plus_5", "flsun_v400", "flsun_sr", "flsun_delta",
    "flsun_s1", "flsun_t1_pro", "doron_velta", "venture_delta", "ratrig_vcore3", "ratrig_vminion",
    "ratrig_vcore4", "ratrig_vcore_idex", "ratrig_vcore_pro", "rh3d_e3ng", "sovol_zero",
    "sovol_sv06", "sovol_sv06_ace", "sovol_sv06_plus", "sovol_sv06_plus_ace", "sovol_sv07",
    "anycubic_chiron", "anycubic_kobra_2", "anycubic_kobra_2_max", "anycubic_kobra_2_pro",
    "anycubic_kobra_3", "anycubic_kobra_3_v2", "anycubic_kobra_3_max", "anycubic_kobra_x",
    "anycubic_i3_mega", "anycubic_kobra", "anycubic_vyper", "anycubic_kobra_s1",
    "anycubic_kobra_s1_max", "anet_et5_pro", "artillery_sidewinder_x2", "artillery_genius_pro",
    "geralkom_x400_ht", "geralkom_x500_ht", "infimech_tx", "lemontron", "zerog_mercury_one",
    "zerog_mercury_one_plus", "zerog_mercury_one_hydra", "zerog_mercury_one_plus_hydra",
    "zerog_nebula_255", "zerog_nebula_370", "vzbot", "duender_medusahc", "ducr10_medusahc",
    "ducr10", "duender"};

// Aftermarket heads no stock printer ships with: drawn for the manual pick only.
const std::set<std::string> kAftermarketHeads = {
    "dragonburner", "a4t", "jabberwocky", "bambu", "xol"};
// clang-format on

} // namespace

TEST_CASE("toolhead art: every database style names drawn art, every stock head is named",
          "[toolhead-art]") {
    std::ifstream in("assets/config/printer_database.json");
    REQUIRE(in.good());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json db = nlohmann::json::parse(text, nullptr, false);
    REQUIRE(db.is_object());
    REQUIRE(db.contains("printers"));

    std::set<std::string> ids, named;
    for (const auto& printer : db["printers"]) {
        const std::string id = printer["id"].get<std::string>();
        const auto it = printer.find("toolhead_style");
        const std::string name =
            it != printer.end() && it->is_string() ? it->get<std::string>() : std::string();
        const bool feature = printer.contains("show_in_list") && printer["show_in_list"] == false;
        INFO(id << " toolhead_style=\"" << name << "\"");
        ids.insert(id);
        if (feature || kNoArtYet.count(id)) {
            CHECK(name.empty());
            continue;
        }
        const ToolheadStyle style = toolhead_style_from_name(name);
        REQUIRE(style != ToolheadStyle::AUTO);
        CHECK((style == ToolheadStyle::DEFAULT || toolhead_art_for(style) != nullptr));
        named.insert(name);
    }
    for (const std::string& id : kNoArtYet) {
        INFO("kNoArtYet names " << id << ", which the database does not have");
        CHECK(ids.count(id) == 1);
    }

    for (int i = 0; i < kToolheadArtCount; ++i) {
        const ToolheadArt& art = kToolheadArt[i];
        INFO("art " << art.id);
        CHECK(toolhead_art_for(art.style) == &art);
        CHECK(toolhead_style_from_name(art.id) == art.style);
        CHECK(named.count(art.id) + kAftermarketHeads.count(art.id) == 1);
    }
}
