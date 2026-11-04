// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Rendering contract of the ams_lane_spool widget: the spool graphic, the
// empty-lane placeholder, the fill level and the error dot all render from
// AmsState's per-slot subjects. The lane classification itself is AmsState's
// classify_lane() (test_ams_lane_state*.cpp); these tests pin what the widget
// does with each classification.

#include "ui_ams_lane_spool.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"
#include "ui_swatch.h"

#include "../test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

lv_obj_t* make_spool(lv_obj_t* parent, int slot_index) {
    const std::string idx = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", idx.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_lane_spool", attrs));
}

lv_obj_t* part(lv_obj_t* root, const char* name) {
    lv_obj_t* o = lv_obj_find_by_name(root, name);
    REQUIRE(o != nullptr);
    return o;
}

/// The error dot is the one root child that is not the spool graphic, the
/// empty placeholder or the glow layer (it is unnamed by the drawing utils).
lv_obj_t* find_error_dot(lv_obj_t* root) {
    lv_obj_t* graphic = lv_obj_find_by_name(root, "spool_graphic");
    lv_obj_t* placeholder = lv_obj_find_by_name(root, "empty_placeholder");
    lv_obj_t* glow = lv_obj_find_by_name(root, "spool_glow");
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        if (child && child != graphic && child != placeholder && child != glow) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: an Empty lane hides the spool, shows the placeholder",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Empty));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    CHECK(lv_obj_has_flag(part(spool, "spool_graphic"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a Ghosted lane keeps the spool, dimmed",
                 "[ams][lane_spool]") {
    // Both halves asserted: the spool stays visible (assigned, not present)
    // AND it renders at the ghost opacity, not full strength.
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Ghosted));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(graphic, LV_PART_MAIN) == ams_draw::kGhostOpa);
    CHECK(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a Present lane renders full strength",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(graphic, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: fill renders from the subject, -1 means no data",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* fill = AmsState::instance().get_slot_fill_subject(0);
    REQUIRE(fill != nullptr);
    lv_subject_set_int(fill, 50);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.50f));

    // A later change flows through the observer (deferred, #82).
    lv_subject_set_int(fill, 25);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.25f));

    // A no-data frame must not blank a lane that already rendered a value.
    lv_subject_set_int(fill, -1);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.25f));

    // Fill renders RAW in the spool family: 0 paints an empty spool, not the
    // bar family's visible-sliver floor (an empty spool is still a drawn spool).
    lv_subject_set_int(fill, 0);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.0f));

    lv_obj_delete(spool);
    lv_subject_set_int(fill, -1);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: color renders from the subject",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0xFF0000);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* canvas = part(spool, "spool_graphic");
    REQUIRE(lv_obj_check_type(canvas, &lv_canvas_class)); // 3D style default
    CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(0xFF0000)));

    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0x0000FF);
    process_lvgl(50);
    CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(0x0000FF)));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: has_error drives the error dot color and visibility",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_error_severity_subject(0),
                       static_cast<int>(SlotError::Severity::ERROR));
    lv_subject_set_int(AmsState::instance().get_slot_has_error_subject(0), 1);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* dot = find_error_dot(spool);
    REQUIRE(dot != nullptr);
    CHECK_FALSE(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(dot, LV_PART_MAIN),
                      ams_draw::severity_color(SlotError::Severity::ERROR)));

    // Clearing has_error hides the dot again.
    lv_subject_set_int(AmsState::instance().get_slot_has_error_subject(0), 0);
    process_lvgl(50);
    CHECK(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(spool);
    lv_subject_set_int(AmsState::instance().get_slot_error_severity_subject(0),
                       static_cast<int>(SlotError::Severity::INFO));
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot embeds ams_lane_spool and forwards a creation-time fill",
                 "[ams][lane_spool]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    // The per-slot fill subject outlives tests; -1 is "no data", which leaves
    // the creation-time attribute standing.
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), -1);

    const char* attrs[] = {"slot_index", "0", "fill_level", "0.25", nullptr};
    auto* slot = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_slot", attrs));
    REQUIRE(slot != nullptr);
    process_lvgl(20);

    // The embedded widget exists inside the slot's spool_container...
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);
    lv_obj_t* embedded = lv_obj_find_by_name(spool_container, "lane_spool");
    REQUIRE(embedded != nullptr);

    // ...and the slot's fill_level attribute reached it (a later per-slot fill
    // subject value overrides it, pinned by the fill test above).
    CHECK(helix::ui::ams_lane_spool_get_fill_level(embedded) == Catch::Approx(0.25f));
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(0.25f));

    lv_obj_delete(slot);
}

// The flat style is the spool canvas seen face-on, painted in the lane color.
TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: flat style draws the spool face-on",
                 "[ams][lane_spool]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0xFF0000);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    REQUIRE(lv_obj_check_type(graphic, &lv_canvas_class));
    CHECK(helix::ui::spool_canvas_get_view(graphic) == helix::ui::SpoolView::Front);
    CHECK(lv_color_eq(ui_spool_canvas_get_color(graphic), lv_color_hex(0xFF0000)));
    CHECK(lv_obj_find_by_name(spool, "spool_outer") == nullptr);
    CHECK(lv_obj_find_by_name(spool, "spool_hub") == nullptr);
    CHECK(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(spool);
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a flat lane dims when ghosted, hides when empty",
                 "[ams][lane_spool]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* lane = AmsState::instance().get_slot_lane_state_subject(0);
    lv_subject_set_int(lane, static_cast<int>(helix::ui::LaneState::Ghosted));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(graphic, LV_PART_MAIN) == ams_draw::kGhostOpa);

    lv_subject_set_int(lane, static_cast<int>(helix::ui::LaneState::Empty));
    process_lvgl(20);
    CHECK(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(spool);
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

// A size change or a spool-style flip rebuilds the visual layers; the same
// size in the same style must not (the strip calls this every rebuild).
TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: set_size rebuilds only on a real change",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);
    lv_obj_t* g1 = part(spool, "spool_graphic");
    // The canvas' max width is the size it was built at; step off it by a
    // delta rather than a constant so the case cannot collide with the
    // theme's default.
    const int32_t size1 = lv_obj_get_style_max_width(g1, LV_PART_MAIN);
    const int32_t size2 = size1 + 16;

    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    lv_obj_t* g2 = part(spool, "spool_graphic");
    CHECK(g2 != g1); // resized -> rebuilt
    CHECK(lv_obj_get_style_max_width(g2, LV_PART_MAIN) == size2);

    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    CHECK(part(spool, "spool_graphic") == g2); // same size + style -> no rebuild

    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    lv_obj_t* g3 = part(spool, "spool_graphic");
    CHECK(g3 != g2); // style flip -> rebuilt even at the same size
    CHECK(helix::ui::spool_canvas_get_view(g3) == helix::ui::SpoolView::Front);

    lv_obj_delete(spool);
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

// The spool family's shared material-label rule (ams_slot and the strip's
// spool cells both render through it).
TEST_CASE("lane_material_text: the spool family's label rule", "[ams][lane_spool]") {
    using helix::ui::LaneState;
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Empty, "PLA")) ==
          std::string(lv_tr("Empty")));
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Ghosted, "")) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, "")) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, nullptr)) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, "PLA")) == "PLA");
}

namespace {
/// Alpha-weighted centroid of a canvas's draw buffer, in buffer pixels.
const lv_draw_buf_t* buf_of(lv_obj_t* obj) {
    if (lv_obj_check_type(obj, &lv_canvas_class))
        return lv_canvas_get_draw_buf(obj);
    return static_cast<const lv_draw_buf_t*>(lv_image_get_src(obj));
}

std::pair<double, double> alpha_centroid(lv_obj_t* canvas) {
    const lv_draw_buf_t* buf = buf_of(canvas);
    REQUIRE(buf != nullptr);
    double sx = 0, sy = 0, sa = 0;
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const auto* row = reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride);
        for (uint32_t x = 0; x < buf->header.w; x++) {
            sx += x * row[x].alpha;
            sy += y * row[x].alpha;
            sa += row[x].alpha;
        }
    }
    REQUIRE(sa > 0);
    return {sx / sa, sy / sa};
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: the glow is centred on the spool",
                 "[ams][lane_spool][highlight]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    helix::ui::ams_lane_spool_set_highlight(spool, helix::ui::SpoolHighlight::Steady);
    process_lvgl(20);
    lv_obj_t* glow = part(spool, "spool_glow");
    lv_obj_t* graphic = part(spool, "spool_graphic");
    REQUIRE_FALSE(lv_obj_has_flag(glow, LV_OBJ_FLAG_HIDDEN));

    const int32_t m = (static_cast<int32_t>(buf_of(glow)->header.w) -
                       static_cast<int32_t>(buf_of(graphic)->header.w)) /
                      2;
    REQUIRE(m > 0);
    // Same object rectangle; the buffer overhang sits m px outside it.
    lv_obj_update_layout(spool);
    lv_area_t a, b;
    lv_obj_get_coords(glow, &a);
    lv_obj_get_coords(graphic, &b);
    CHECK(a.x1 == b.x1);
    CHECK(a.y1 == b.y1);
    // Centred, not offset: an offset would stack on the centring.
    CHECK(lv_image_get_inner_align(glow) == LV_IMAGE_ALIGN_CENTER);
    CHECK(lv_image_get_offset_x(glow) == 0);
    CHECK(lv_image_get_offset_y(glow) == 0);

    const auto [gx, gy] = alpha_centroid(glow);
    const auto [sx, sy] = alpha_centroid(graphic);
    CHECK(std::abs(gx - (sx + m)) < 1.5);
    CHECK(std::abs(gy - (sy + m)) < 1.5);

    helix::ui::ams_lane_spool_set_highlight(spool, helix::ui::SpoolHighlight::None);
    CHECK(lv_obj_has_flag(glow, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_image_get_src(glow) == nullptr); // un-highlight drops the image
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a theme change repaints a lit glow",
                 "[ams][lane_spool][highlight]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    helix::ui::ams_lane_spool_set_highlight(spool, helix::ui::SpoolHighlight::Steady);
    process_lvgl(20);
    lv_obj_t* glow = part(spool, "spool_glow");
    const lv_draw_buf_t* before = buf_of(glow);
    REQUIRE(before != nullptr);

    // Dark/light flips the accent (tube_accent lightens it in light mode).
    const lv_color_t old_accent = helix::ui::tube_accent();
    theme_manager_toggle_dark_mode();
    const lv_color_t new_accent = helix::ui::tube_accent();
    REQUIRE_FALSE(lv_color_eq(old_accent, new_accent));
    process_lvgl(20);

    const lv_draw_buf_t* after = buf_of(glow);
    REQUIRE(after != nullptr);
    CHECK(after != before);
    // The far halo is pure accent: the first visible pixel in from the left
    // on the middle row.
    const uint32_t mid = after->header.h / 2;
    const auto* row =
        reinterpret_cast<const lv_color32_t*>(after->data + mid * after->header.stride);
    lv_color32_t best{};
    for (uint32_t x = 0; x < after->header.w && best.alpha <= 20; x++)
        best = row[x];
    REQUIRE(best.alpha > 0);
    CHECK(std::abs(best.red - new_accent.red) <= 2);
    CHECK(std::abs(best.green - new_accent.green) <= 2);
    CHECK(std::abs(best.blue - new_accent.blue) <= 2);

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a running pulse is never restarted",
                 "[ams][lane_spool][highlight]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* lane = AmsState::instance().get_slot_lane_state_subject(0);
    lv_subject_set_int(lane, static_cast<int>(helix::ui::LaneState::Present));
    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);
    lv_obj_t* glow = part(spool, "spool_glow");

    helix::ui::ams_lane_spool_set_highlight(spool, helix::ui::SpoolHighlight::Pulse);
    lv_anim_t* anim = lv_anim_get(glow, nullptr);
    REQUIRE(anim != nullptr);
    process_lvgl(200);
    const int32_t elapsed = anim->act_time;
    REQUIRE(elapsed > 0);

    // A re-apply from a lane-state update while still pulsing.
    lv_subject_set_int(lane, static_cast<int>(helix::ui::LaneState::Ghosted));
    process_lvgl(20);
    lv_subject_set_int(lane, static_cast<int>(helix::ui::LaneState::Present));
    process_lvgl(20);
    lv_anim_t* still = lv_anim_get(glow, nullptr);
    REQUIRE(still == anim);
    CHECK(still->act_time > elapsed);

    // Pulse to Steady stops it and restores full opacity.
    helix::ui::ams_lane_spool_set_highlight(spool, helix::ui::SpoolHighlight::Steady);
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    CHECK(lv_obj_get_style_opa(glow, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: a finish or multi-colour change under the same colour repaints",
                 "[ams][lane_spool][swatch]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    ui_ams_lane_spool_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto mock = std::make_unique<AmsBackendMock>();
    auto* backend = mock.get();
    backend->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    backend->start();

    SlotInfo slot = backend->get_slot_info(0);
    slot.color_rgb = 0xFF0000;
    slot.material = "PLA";
    slot.product_name.clear();
    slot.spool_name.clear();
    slot.multi_color_hexes.clear();
    helix::test::apply_edit(*backend, 0, slot);
    ams.sync_from_backend();
    lv_subject_set_int(ams.get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);
    lv_obj_t* canvas = part(spool, "spool_graphic");
    REQUIRE(helix::ui::spool_canvas_get_view(canvas) == helix::ui::SpoolView::Front);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    auto pixels = [buf] { return std::vector<uint8_t>(buf->data, buf->data + buf->data_size); };
    const std::vector<uint8_t> plain = pixels();

    // Only the finish changes; the colour subject does not.
    slot.material = "PLA Silk";
    helix::test::apply_edit(*backend, 0, slot);
    ams.sync_from_backend();
    process_lvgl(20);
    CHECK(pixels() != plain);

    // Only the multi-colour list changes: the lower-right slot (30 degrees,
    // a quarter of the vent geometry's size out) turns to the second colour.
    slot.multi_color_hexes = "#FF0000,#0000FF";
    helix::test::apply_edit(*backend, 0, slot);
    ams.sync_from_backend();
    process_lvgl(20);
    const float c = static_cast<float>(buf->header.w) / 2.0f;
    const float r = 0.25f * (c - 1.0f) / 0.42f;
    const int32_t x = static_cast<int32_t>(c + r * 0.866f);
    const int32_t y = static_cast<int32_t>(c + r * 0.5f);
    const lv_color32_t lower_right =
        reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride)[x];
    CHECK(lower_right.blue > lower_right.red + 40);

    lv_obj_delete(spool);
    backend->stop();
    ams.clear_backends();
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: a printer switch's cleanup leaves no observer on a live subject",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* theme = theme_manager_get_changed_subject();
    lv_subject_t* lane_state = AmsState::instance().get_slot_lane_state_subject(0);
    REQUIRE(theme != nullptr);
    const uint32_t theme_before = lv_ll_get_len(&theme->subs_ll);
    const uint32_t lane_before = lv_ll_get_len(&lane_state->subs_ll);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);
    REQUIRE(lv_ll_get_len(&theme->subs_ll) == theme_before + 1);
    REQUIRE(lv_ll_get_len(&lane_state->subs_ll) > lane_before);

    // Theme and AmsState subjects both outlive this cleanup here, as the theme
    // subject does across every switch.
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "AmsLaneSpoolWidgets", StaticSubjectRegistry::Scope::PrinterSwitch));
    CHECK(lv_ll_get_len(&theme->subs_ll) == theme_before);
    CHECK(lv_ll_get_len(&lane_state->subs_ll) == lane_before);
    lv_obj_delete(spool);
}
