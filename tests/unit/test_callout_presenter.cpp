// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_plate_glow.h"

#include "../lvgl_ui_test_fixture.h"
#include "callout_presenter.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::CalloutPresenter;

namespace {

constexpr int kAreaW = 342;
constexpr int kAreaH = 270;

struct PresenterFixture : public LVGLUITestFixture {
    lv_subject_t text_subj, shown_subj, compact_subj;
    char text_buf[16] = "210";
    lv_obj_t* area = nullptr;
    lv_obj_t* image = nullptr;

    PresenterFixture() {
        lv_subject_init_string(&text_subj, text_buf, nullptr, sizeof(text_buf), "210");
        lv_subject_init_int(&shown_subj, 1);
        lv_subject_init_int(&compact_subj, 0);
        lv_xml_register_subject(nullptr, "cp_text", &text_subj);
        lv_xml_register_subject(nullptr, "cp_shown", &shown_subj);
        lv_xml_register_subject(nullptr, "cp_compact", &compact_subj);
        area = lv_obj_create(test_screen());
        lv_obj_set_size(area, kAreaW, kAreaH);
        lv_obj_set_style_pad_all(area, 0, 0);
        lv_obj_set_style_border_width(area, 0, 0);
        image = lv_image_create(area);
        lv_obj_update_layout(area);
    }

    lv_obj_t* make_chip() {
        const char* attrs[] = {"text_subject",    "cp_text",    "shown_subject", "cp_shown",
                               "compact_subject", "cp_compact", nullptr};
        return static_cast<lv_obj_t*>(lv_xml_create(area, "activity_chip", attrs));
    }

    CalloutPresenter::Chip chip(CalloutKind k, bool active, bool expendable = false) {
        CalloutPresenter::Chip c;
        c.kind = k;
        c.chip = make_chip();
        REQUIRE(c.chip != nullptr);
        c.in_budget = true;
        c.active = active;
        c.expendable = expendable;
        c.w_budget = c.w_full = c.w_compact = 60;
        return c;
    }

    CalloutPresenter::Config config() {
        CalloutPresenter::Config cfg;
        cfg.chip_h = cfg.compact_chip_h = 20;
        cfg.gap = 4;
        cfg.min_line = 8;
        return cfg;
    }
};

ImageRegions k1c_regions() {
    ImageRegions r;
    r.src_w = 1601;
    r.src_h = 1204;
    r.nozzle = {0.5f, 0.3f};
    r.bed_left = {0.3f, 0.8f};
    r.bed_right = {0.7f, 0.8f};
    return r;
}

} // namespace

TEST_CASE_METHOD(PresenterFixture, "CalloutPresenter apply positions chips inside the area",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    REQUIRE(lv_obj_get_content_width(area) == kAreaW);
    parts.area = area;
    parts.image = image;
    parts.chips = {chip(CalloutKind::Nozzle, true), chip(CalloutKind::Bed, true)};
    const ImageRegions r = k1c_regions();
    CalloutPresenter presenter;
    const CalloutLayout out = presenter.apply(parts, &r, 1601, 1204, config());

    REQUIRE(out.mode != CalloutMode::ImageOnly);
    REQUIRE(out.chips.size() == 2);
    lv_obj_update_layout(area);
    for (const auto& c : parts.chips) {
        CAPTURE(int(c.kind));
        const int x = lv_obj_get_style_x(c.chip, LV_PART_MAIN);
        const int y = lv_obj_get_style_y(c.chip, LV_PART_MAIN);
        REQUIRE(x >= 0);
        REQUIRE(y >= 0);
        REQUIRE(x + lv_obj_get_style_width(c.chip, LV_PART_MAIN) <= kAreaW);
        REQUIRE(y + 20 <= kAreaH);
    }
}

TEST_CASE_METHOD(PresenterFixture, "CalloutPresenter apply accounts for every active chip",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    REQUIRE(lv_obj_get_content_width(area) == kAreaW);
    parts.area = area;
    parts.image = image;
    parts.chips = {chip(CalloutKind::Nozzle, true), chip(CalloutKind::Bed, true),
                   chip(CalloutKind::Fan, true, true), chip(CalloutKind::Light, true, true)};
    const ImageRegions r = k1c_regions();
    CalloutPresenter presenter;
    const CalloutLayout out = presenter.apply(parts, &r, 1601, 1204, config());
    REQUIRE(out.chips.size() + out.dropped.size() == 4);
}

TEST_CASE_METHOD(PresenterFixture, "CalloutPresenter apply returns Docked for null regions",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    REQUIRE(lv_obj_get_content_width(area) == kAreaW);
    parts.area = area;
    parts.image = image;
    parts.chips = {chip(CalloutKind::Nozzle, true)};
    CalloutPresenter presenter;
    const CalloutLayout out = presenter.apply(parts, nullptr, 1601, 1204, config());
    REQUIRE(out.mode == CalloutMode::Docked);
    REQUIRE(out.chips.size() == 1);
}

TEST_CASE_METHOD(PresenterFixture, "CalloutPresenter place_image reports a change only once",
                 "[callout_presenter]") {
    CalloutPresenter presenter;
    const CalloutRect moved{10, 20, 100, 80};
    REQUIRE(presenter.place_image(image, &moved));
    REQUIRE_FALSE(presenter.place_image(image, &moved));
    REQUIRE(lv_obj_get_style_x(image, LV_PART_MAIN) == 10);
    REQUIRE(presenter.place_image(image, nullptr));
}

TEST_CASE_METHOD(PresenterFixture, "CalloutPresenter pins a tall chip at its compact height",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    parts.area = area;
    parts.image = image;
    auto bed = chip(CalloutKind::Bed, true);
    bed.in_budget = false; // no budget: a tagged image pins
    bed.h = 40;
    bed.h_compact = 26;
    parts.chips = {bed};
    const ImageRegions r = k1c_regions();
    CalloutPresenter presenter;
    const CalloutLayout out = presenter.apply(parts, &r, 1601, 1204, config());
    REQUIRE(out.mode == CalloutMode::Pinned);
    REQUIRE(out.chips.size() == 1);
    CHECK(out.chips[0].rect.h == 26);
}

TEST_CASE_METHOD(PresenterFixture, "a pinned chip covers more of the photo than a docked one",
                 "[callout_presenter]") {
    lv_obj_t* c = make_chip();
    REQUIRE(c != nullptr);
    REQUIRE(lv_subject_get_int(&compact_subj) == 0);
    CHECK(lv_obj_get_style_bg_opa(c, LV_PART_MAIN) == LV_OPA_COVER * 65 / 100);

    lv_subject_set_int(&compact_subj, 1);
    CHECK(lv_obj_get_style_bg_opa(c, LV_PART_MAIN) == LV_OPA_COVER * 90 / 100);

    lv_subject_set_int(&compact_subj, 0);
    CHECK(lv_obj_get_style_bg_opa(c, LV_PART_MAIN) == LV_OPA_COVER * 65 / 100);
}

TEST_CASE_METHOD(PresenterFixture, "the bed glow re-renders only when its plate moves",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    parts.area = area;
    parts.image = image;
    auto bed = chip(CalloutKind::Bed, true);
    bed.glow = lv_image_create(area);
    bed.glowing = true;
    parts.chips = {bed};
    const ImageRegions r = k1c_regions();
    CalloutPresenter presenter;
    REQUIRE(lv_image_get_src(bed.glow) == nullptr);

    presenter.apply(parts, &r, 1601, 1204, config());
    const void* const first = lv_image_get_src(bed.glow);
    REQUIRE(first != nullptr);

    SECTION("the same geometry keeps its mask") {
        presenter.apply(parts, &r, 1601, 1204, config());
        CHECK(lv_image_get_src(bed.glow) == first);
    }
    SECTION("a moved plate gets a new one") {
        ImageRegions moved = r;
        moved.bed_left.y = 0.7f;
        moved.bed_right.y = 0.7f;
        presenter.apply(parts, &moved, 1601, 1204, config());
        CHECK(lv_image_get_src(bed.glow) != first);
    }
    SECTION("a rebuilt widget with unchanged geometry still gets one") {
        parts.chips[0].glow = lv_image_create(area);
        presenter.apply(parts, &r, 1601, 1204, config());
        CHECK(lv_image_get_src(parts.chips[0].glow) != nullptr);
    }
    SECTION("a part that is not glowing draws no mask") {
        parts.chips[0].glow = lv_image_create(area);
        parts.chips[0].glowing = false;
        presenter.apply(parts, &r, 1601, 1204, config());
        CHECK(lv_image_get_src(parts.chips[0].glow) == nullptr);
    }
}

TEST_CASE_METHOD(PresenterFixture, "a heat glow breathes only while climbing, shown and animated",
                 "[callout_presenter]") {
    using helix::ui::temperature::HeatState;
    auto& display = DisplaySettingsManager::instance();
    const bool animations = display.get_animations_enabled();
    display.set_animations_enabled(true);
    lv_obj_t* glow = lv_image_create(area);
    const auto pulsing = [&] { return lv_anim_get(glow, nullptr) != nullptr; };

    helix::ui::heat_glow_update_pulse(glow, HeatState::Heating, true);
    CHECK(pulsing());
    helix::ui::heat_glow_update_pulse(glow, HeatState::Heating, false);
    CHECK_FALSE(pulsing());
    helix::ui::heat_glow_update_pulse(glow, HeatState::AtTemp, true);
    CHECK_FALSE(pulsing());
    display.set_animations_enabled(false);
    helix::ui::heat_glow_update_pulse(glow, HeatState::Heating, true);
    CHECK_FALSE(pulsing());
    display.set_animations_enabled(animations);
}

TEST_CASE_METHOD(PresenterFixture, "the chamber glow fills its windows, or a disc without them",
                 "[callout_presenter]") {
    CalloutPresenter::Parts parts;
    parts.area = area;
    parts.image = image;
    auto chamber = chip(CalloutKind::Chamber, true);
    chamber.glow = lv_image_create(area);
    chamber.glowing = true;
    parts.chips = {chamber};
    ImageRegions r = k1c_regions();
    r.chamber = NormPoint{0.5f, 0.5f};
    CalloutPresenter presenter;

    const auto drawn_w = [&] { return lv_obj_get_style_width(chamber.glow, LV_PART_MAIN); };
    const auto drawn_h = [&] { return lv_obj_get_style_height(chamber.glow, LV_PART_MAIN); };

    SECTION("no windows: a square disc on the chamber's point") {
        presenter.apply(parts, &r, 1601, 1204, config());
        REQUIRE(lv_image_get_src(chamber.glow) != nullptr);
        CHECK(drawn_w() == drawn_h());
        CHECK(drawn_w() > 0);
    }
    SECTION("windows: the mask spans them, and a moved window redraws it") {
        r.windows.push_back({{NormPoint{0.1f, 0.1f}, NormPoint{0.9f, 0.1f}, NormPoint{0.9f, 0.4f},
                              NormPoint{0.1f, 0.4f}}});
        presenter.apply(parts, &r, 1601, 1204, config());
        const void* const first = lv_image_get_src(chamber.glow);
        REQUIRE(first != nullptr);
        CHECK(drawn_w() > drawn_h());

        presenter.apply(parts, &r, 1601, 1204, config());
        CHECK(lv_image_get_src(chamber.glow) == first);

        r.windows[0][2].y = 0.6f;
        presenter.apply(parts, &r, 1601, 1204, config());
        const void* const second = lv_image_get_src(chamber.glow);
        CHECK(second != first);

        r.cutouts.push_back({{NormPoint{0.4f, 0.2f}, NormPoint{0.6f, 0.2f}, NormPoint{0.6f, 0.3f},
                              NormPoint{0.4f, 0.3f}}});
        presenter.apply(parts, &r, 1601, 1204, config());
        CHECK(lv_image_get_src(chamber.glow) != second);
    }
}
