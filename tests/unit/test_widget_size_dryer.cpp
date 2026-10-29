// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_dryer.cpp
 * @brief The dryer widget draws what its ladder promises at each rung: which parts carry
 * HIDDEN, which text they show, the re-decision when the drying state changes, which
 * instance publishes, and where a tap goes.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * dryer_layout.h's; these tests pin its wiring to the real fonts and XML. The summary is
 * published straight to the model, which has its own tests.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][dryer]"
 */

#include "ui_humidity_drop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tile_boxes.h"
#include "../test_helpers/update_queue_test_access.h"
#include "dryer_summary.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "src/ui/panel_widgets/dryer_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::printer::EnvironmentZone;
using helix::printer::ZoneDryingState;
using helix::test::Box;
using helix::test::kCell;
using helix::test::kColumn;
using helix::test::kGlance;
using helix::test::kStrip;
using helix::test::kWide;
using helix::ui::humidity_drop_value;

namespace {

struct DryerFixture : public LVGLUITestFixture {
    DryerFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        publish({});
    }
    ~DryerFixture() override {
        set_dryer_open_hook_for_test({});
        publish({});
        dryer_model::release_for_test();
        drain();
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    void publish(const DryerSummaryInput& in) {
        dryer_model::publish_for_test(summarize_dryers(in));
        drain();
    }
};

/// A box zone with a dryer; drying when @p remaining is >= 0.
EnvironmentZone box(int unit, const char* label, float rh, int remaining = -1, int duration = 240) {
    EnvironmentZone z;
    z.id = "unit-" + std::to_string(unit);
    z.label = label;
    z.unit_index = unit;
    z.dryer.supported = true;
    z.dryer.active = remaining >= 0;
    z.dryer.remaining_min = remaining;
    z.dryer.duration_min = duration;
    z.dryer.target_temp_c = 55.0f;
    z.dryer.current_temp_c = 47.0f;
    z.state = remaining >= 0 ? ZoneDryingState::Active : ZoneDryingState::Idle;
    z.env.has_humidity = rh >= 0.0f;
    z.env.humidity_pct = rh;
    z.env.temperature_c = 47.0f;
    return z;
}

DryerSummaryInput one_box_idle() {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 46.0f)};
    return in;
}

DryerSummaryInput one_box_drying() {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 31.0f, 150, 240)};
    return in;
}

DryerSummaryInput two_drying() {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 31.0f, 150, 240)};
    in.chamber = {true, true, 45, 120, 38, 55};
    return in;
}

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

int rung_subject() {
    return subject_int("home_dryer_rung");
}

bool hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (hidden(o))
            return false;
    }
    return true;
}

lv_obj_t* part(PanelWidgetHarness<DryerWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return obj;
}

} // namespace

TEST_CASE_METHOD(DryerFixture, "dryer: medium-tier budgets land on their rung, idle and drying",
                 "[widget_size][dryer]") {
    PanelWidgetHarness<DryerWidget> h(test_screen());
    for (const auto& input : {one_box_idle(), one_box_drying(), two_drying()}) {
        publish(input);
        for (const auto& b : test::kMediumLadder) {
            INFO(b.w << "x" << b.h << " drying=" << summarize_dryers(input).drying_count);
            h.resize(1, 1, b.w, b.h);
            CHECK(rung_subject() == static_cast<int>(b.rung));
        }
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: idle shows the highest humidity, then the state",
                 "[widget_size][dryer]") {
    DryerSummaryInput in = one_box_idle();
    in.zones.push_back(box(1, "Box 2", 52.0f));
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 55, 54);
    CHECK(subject_text("home_dryer_rh_text") == "52%");
    CHECK(shown(part(h, "dryer_drop")));
    CHECK(humidity_drop_value(part(h, "dryer_drop")) == 520);
    CHECK_FALSE(shown(part(h, "dryer_icon")));
    CHECK(shown(part(h, "dryer_rh")));
    CHECK_FALSE(shown(part(h, "dryer_status")));
    CHECK_FALSE(shown(part(h, "dryer_ring_box")));

    h.resize(1, 1, 114, 113);
    CHECK(subject_text("home_dryer_rh_text") == "52% RH");
    CHECK(subject_text("home_dryer_status_text") == "Not drying");
    CHECK(shown(part(h, "dryer_status")));

    h.resize(1, 1, 234, 113);
    CHECK(subject_text("home_dryer_rh_text") == "52% RH");
    CHECK(shown(part(h, "dryer_status")));
    CHECK(shown(part(h, "dryer_drop")));
    CHECK_FALSE(shown(part(h, "dryer_icon")));
}

TEST_CASE_METHOD(DryerFixture, "dryer: the idle drop grows with the tile", "[widget_size][dryer]") {
    publish(one_box_idle());
    PanelWidgetHarness<DryerWidget> h(test_screen());

    // Wide: the drop stands at least as tall as the two lines of text beside it.
    h.resize(1, 1, kWide.w, kWide.h);
    lv_obj_update_layout(test_screen());
    const int wide_rung = subject_int("home_dryer_drop_rung");
    const int text_h =
        lv_obj_get_height(part(h, "dryer_rh")) + lv_obj_get_height(part(h, "dryer_status"));
    CHECK(lv_obj_get_height(part(h, "dryer_drop")) >= text_h);

    // A Cell stacks it over the text and still grows it past the smallest face.
    h.resize(1, 1, kCell.w, kCell.h);
    const int cell_rung = subject_int("home_dryer_drop_rung");
    CHECK(cell_rung > 0);
    CHECK(cell_rung <= wide_rung);

    // A half cell draws it no larger than a full one.
    h.resize(1, 1, kGlance.w, kGlance.h);
    CHECK(subject_int("home_dryer_drop_rung") <= cell_rung);

    // The drop never overflows the tile it grew into.
    for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        lv_obj_update_layout(test_screen());
        lv_area_t tile;
        lv_area_t drop;
        lv_obj_get_coords(h.root(), &tile);
        lv_obj_get_coords(part(h, "dryer_drop"), &drop);
        CHECK(drop.y1 >= tile.y1);
        CHECK(drop.y2 <= tile.y2);
        CHECK(drop.x1 >= tile.x1);
        CHECK(drop.x2 <= tile.x2);
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: no humidity source leaves Not drying alone",
                 "[widget_size][dryer]") {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", -1.0f)};
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 114, 113);
    CHECK(subject_text("home_dryer_rh_text").empty());
    CHECK_FALSE(shown(part(h, "dryer_rh")));
    CHECK(shown(part(h, "dryer_status")));

    h.resize(1, 1, 55, 54);
    CHECK_FALSE(shown(part(h, "dryer_rh")));
    CHECK(shown(part(h, "dryer_drop")));
    CHECK(humidity_drop_value(part(h, "dryer_drop")) == helix::ui::humidity_drop::kUnknown);
}

TEST_CASE_METHOD(DryerFixture, "dryer: drying draws the ring at Glance, Cell and Wide",
                 "[widget_size][dryer]") {
    publish(one_box_drying());
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 55, 54);
    CHECK(shown(part(h, "dryer_ring_box")));
    CHECK(subject_text("home_dryer_time_text") == "2:30");
    CHECK(subject_int("home_dryer_ring_pct") == 38);
    CHECK_FALSE(shown(part(h, "dryer_icon")));
    CHECK_FALSE(shown(part(h, "dryer_drop")));
    CHECK_FALSE(shown(part(h, "dryer_time"))); // the ring carries it

    h.resize(1, 1, 114, 54);
    CHECK_FALSE(shown(part(h, "dryer_ring_box")));
    CHECK(shown(part(h, "dryer_time")));
    CHECK(shown(part(h, "dryer_temp")));
    CHECK(subject_text("home_dryer_temp_text") == "47 / 55\xC2\xB0"
                                                  "C");

    h.resize(1, 1, 55, 113);
    CHECK_FALSE(shown(part(h, "dryer_ring_box")));
    CHECK(shown(part(h, "dryer_icon"))); // the heater, not the humidity
    CHECK_FALSE(shown(part(h, "dryer_drop")));
    CHECK(subject_text("home_dryer_temp_text") == "47\xC2\xB0"
                                                  "C");

    h.resize(1, 1, 114, 113);
    CHECK(shown(part(h, "dryer_ring_box")));
    CHECK(shown(part(h, "dryer_temp")));
    CHECK_FALSE(shown(part(h, "dryer_name")));

    h.resize(1, 1, 234, 113);
    CHECK(shown(part(h, "dryer_ring_box")));
    CHECK(shown(part(h, "dryer_name")));
    CHECK(subject_text("home_dryer_name_text") == "Box 1");
    CHECK(subject_text("home_dryer_rh_detail_text") == "31% RH");
    CHECK(shown(part(h, "dryer_rh_detail")));
    CHECK(shown(part(h, "dryer_end")));
    CHECK_FALSE(shown(part(h, "dryer_count")));
}

TEST_CASE_METHOD(DryerFixture, "dryer: the drying temperature fits its tile on one line",
                 "[widget_size][dryer]") {
    publish(one_box_drying());
    PanelWidgetHarness<DryerWidget> h(test_screen());

    for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        lv_obj_update_layout(test_screen());
        lv_obj_t* temp = part(h, "dryer_temp");
        if (!shown(temp)) {
            continue;
        }
        INFO("text: " << lv_label_get_text(temp));
        lv_area_t tile;
        lv_area_t text;
        lv_obj_get_coords(h.root(), &tile);
        lv_obj_get_coords(temp, &text);
        CHECK(text.x1 >= tile.x1);
        CHECK(text.x2 <= tile.x2);
        const lv_font_t* font = lv_obj_get_style_text_font(temp, LV_PART_MAIN);
        CHECK(lv_obj_get_height(temp) <= lv_font_get_line_height(font));
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: an unknown progress draws the time with no ring",
                 "[widget_size][dryer]") {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 31.0f, 90, 0)};
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    for (const auto& b : {kGlance, kCell, kWide}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK_FALSE(shown(part(h, "dryer_ring_box")));
        CHECK(shown(part(h, "dryer_time")));
        CHECK(subject_text("home_dryer_time_text") == "1:30");
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: two sources drying list their times at Wide",
                 "[widget_size][dryer]") {
    publish(two_drying());
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(subject_text("home_dryer_count_text") == "2 drying");
    CHECK(subject_text("home_dryer_times_text") == "2:30  45m");
    CHECK(shown(part(h, "dryer_count")));
    CHECK(shown(part(h, "dryer_times")));
    CHECK_FALSE(shown(part(h, "dryer_ring_box")));
    CHECK_FALSE(shown(part(h, "dryer_name")));

    // Below Wide the tile shows the source that ends first.
    h.resize(1, 1, 114, 113);
    CHECK(subject_text("home_dryer_time_text") == "45m");
    CHECK_FALSE(shown(part(h, "dryer_count")));
}

TEST_CASE_METHOD(DryerFixture, "dryer: Full lists a row per source, as many as fit",
                 "[widget_size][dryer]") {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 31.0f, 150, 240), box(1, "Box 2", 40.0f), box(2, "Box 3", 22.0f),
                box(3, "Box 4", 18.0f), box(4, "Box 5", 29.0f)};
    in.chamber.available = true;
    in.bed.available = true;
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    const int rows = subject_int("home_dryer_row_count");
    CHECK(rows >= 3);
    CHECK(rows < 7);
    CHECK(shown(part(h, "dryer_row_0")));
    CHECK_FALSE(shown(part(h, std::string("dryer_row_" + std::to_string(rows)).c_str())));
    CHECK(subject_text("home_dryer_row_0_name") == "Box 1");
    CHECK(subject_text("home_dryer_row_0_time") == "2:30");
    CHECK(subject_int("home_dryer_row_0_pct") == 38);
    CHECK(subject_text("home_dryer_row_1_time") == "Idle");
    CHECK(subject_int("home_dryer_row_1_pct") == -1);
    CHECK_FALSE(shown(part(h, "dryer_row_bar_1")));
    CHECK_FALSE(shown(part(h, "dryer_single")));

    // More height, more rows, never more than there are sources.
    h.resize(1, 1, 234, 480);
    CHECK(subject_int("home_dryer_row_count") > rows);
    CHECK(subject_int("home_dryer_row_count") <= 7);
}

TEST_CASE_METHOD(DryerFixture, "dryer: the drying state re-decides the rung",
                 "[widget_size][dryer]") {
    publish(one_box_idle());
    PanelWidgetHarness<DryerWidget> h(test_screen());
    // A box that holds the idle Cell but is read as a Strip while drying: the ring and
    // the temperature cost more height.
    h.resize(1, 1, 114, 80);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Cell));

    publish(one_box_drying());
    CHECK(rung_subject() == static_cast<int>(SizeRung::Strip));
    publish(one_box_idle());
    CHECK(rung_subject() == static_cast<int>(SizeRung::Cell));
}

TEST_CASE_METHOD(DryerFixture, "dryer: only the live tile publishes, even after a transient one",
                 "[widget_size][dryer]") {
    publish(one_box_drying());
    PanelWidgetHarness<DryerWidget> live(test_screen());
    live.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    {
        // A rebuild's incoming widget attaches and takes the subjects over.
        PanelWidgetHarness<DryerWidget> transient(test_screen());
        transient.resize(1, 1, 55, 54);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Glance));
    }
    // It detaches; the tile that is still on screen publishes again, texts included.
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_text("home_dryer_row_0_name") == "Box 1");
    CHECK(subject_int("home_dryer_row_count") >= 1);

    publish(two_drying());
    CHECK(subject_int("home_dryer_row_count") == 2);
    CHECK(subject_text("home_dryer_row_1_name") == "Chamber");

    // The rows still take taps.
    std::vector<DryerTap> taps;
    set_dryer_open_hook_for_test([&taps](const DryerTap& tap) { taps.push_back(tap); });
    lv_obj_send_event(part(live, "dryer_row_0"), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0].target == DryerTapTarget::Environment);

    // An instance that was never attached does not publish at all.
    DryerWidget probe;
    probe.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(DryerFixture, "dryer: nothing is composed for a rung that does not draw it",
                 "[widget_size][dryer]") {
    publish(one_box_drying());
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 55, 54);
    CHECK(subject_text("home_dryer_name_text").empty());
    CHECK(subject_text("home_dryer_end_text").empty());
    CHECK(subject_text("home_dryer_row_0_name").empty());

    // A tick while at Glance leaves the Wide and Full texts alone.
    DryerSummaryInput ticked;
    ticked.zones = {box(0, "Box 1", 33.0f, 149, 240)};
    publish(ticked);
    CHECK(subject_text("home_dryer_time_text") == "2:29");
    CHECK(subject_text("home_dryer_name_text").empty());
    CHECK(subject_text("home_dryer_rh_detail_text").empty());
    CHECK(subject_text("home_dryer_row_0_name").empty());

    h.resize(1, 1, 234, 113);
    CHECK(subject_text("home_dryer_name_text") == "Box 1");
    CHECK(subject_text("home_dryer_row_0_name").empty());
}

TEST_CASE_METHOD(DryerFixture, "dryer: fits_at refuses what Glance cannot draw",
                 "[widget_size][dryer]") {
    DryerWidget widget;
    CHECK(widget.fits_at(55, 54));
    CHECK(widget.fits_at(51, 53));
    CHECK_FALSE(widget.fits_at(20, 20));
    CHECK_FALSE(widget.fits_at(55, 20));
    CHECK_FALSE(widget.fits_at(20, 54));

    // Monotonic: whatever fits keeps fitting as either axis grows.
    for (int w = 10; w < 130; w += 3) {
        for (int h = 10; h < 130; h += 3) {
            if (!widget.fits_at(w, h))
                continue;
            INFO(w << "x" << h);
            CHECK(widget.fits_at(w + 3, h));
            CHECK(widget.fits_at(w, h + 3));
        }
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: half-cell boxes at large and micro land on a rung",
                 "[widget_size][dryer]") {
    publish(one_box_drying());
    PanelWidgetHarness<DryerWidget> h(test_screen());
    DryerWidget probe;
    for (const auto& b : {Box{51, 53, SizeRung::Glance}, Box{108, 53, SizeRung::Strip},
                          Box{51, 112, SizeRung::Column}, Box{70, 64, SizeRung::Glance}}) {
        INFO(b.w << "x" << b.h);
        CHECK(probe.fits_at(b.w, b.h));
    }
}

TEST_CASE_METHOD(DryerFixture, "dryer: content sits at the vertical centre of the tile",
                 "[widget_size][dryer]") {
    for (const auto& input : {one_box_idle(), one_box_drying()}) {
        publish(input);
        PanelWidgetHarness<DryerWidget> h(test_screen());
        for (const auto& b : test::kMediumLadder) {
            if (b.rung == SizeRung::Full)
                continue;
            INFO(b.w << "x" << b.h << " drying=" << summarize_dryers(input).drying_count);
            h.resize(1, 1, b.w, b.h);
            lv_area_t tile;
            lv_obj_get_coords(h.root(), &tile);
            lv_area_t block;
            lv_obj_get_coords(part(h, "dryer_single"), &block);
            const int tile_mid = (tile.y1 + tile.y2) / 2;
            const int block_mid = (block.y1 + block.y2) / 2;
            CHECK(std::abs(tile_mid - block_mid) <= 2);
        }
    }
}

namespace {

struct DryerTapFixture : public DryerFixture {
    DryerTapFixture() {
        set_dryer_open_hook_for_test([this](const DryerTap& tap) { taps.push_back(tap); });
    }
    std::vector<DryerTap> taps;
};

} // namespace

TEST_CASE_METHOD(DryerTapFixture, "dryer: a tile tap opens the environment overlay for a box",
                 "[widget_size][dryer][actions]") {
    DryerSummaryInput in = one_box_idle();
    in.zones.push_back(box(3, "Box 4", 20.0f, 60, 240));
    in.chamber.available = true;
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());
    h.resize(1, 1, 114, 113);

    lv_obj_send_event(h.root(), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0].target == DryerTapTarget::Environment);
    CHECK(taps[0].unit_index == 3); // the box that is drying
}

TEST_CASE_METHOD(DryerTapFixture,
                 "dryer: with no box dryer a tile tap opens the chamber, then the bed",
                 "[widget_size][dryer][actions]") {
    DryerSummaryInput in;
    in.bed.available = true;
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());
    h.resize(1, 1, 114, 113);

    lv_obj_send_event(h.root(), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0].target == DryerTapTarget::BedModal);

    in.chamber.available = true;
    publish(in);
    lv_obj_send_event(h.root(), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 2);
    CHECK(taps[1].target == DryerTapTarget::ChamberModal);
}

TEST_CASE_METHOD(DryerTapFixture, "dryer: a row tap opens that row's source and not the tile's",
                 "[widget_size][dryer][actions]") {
    DryerSummaryInput in = one_box_drying();
    in.zones.push_back(box(2, "Box 3", 20.0f));
    in.chamber.available = true;
    in.bed.available = true;
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    REQUIRE(subject_int("home_dryer_row_count") >= 4);

    lv_obj_send_event(part(h, "dryer_row_1"), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0].target == DryerTapTarget::Environment);
    CHECK(taps[0].unit_index == 2);

    lv_obj_send_event(part(h, "dryer_row_2"), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 2);
    CHECK(taps[1].target == DryerTapTarget::ChamberModal);

    lv_obj_send_event(part(h, "dryer_row_3"), LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 3);
    CHECK(taps[2].target == DryerTapTarget::BedModal);
}

TEST_CASE_METHOD(DryerTapFixture, "dryer: a tap on a row that is not listed does nothing",
                 "[widget_size][dryer][actions]") {
    publish(one_box_idle());
    PanelWidgetHarness<DryerWidget> h(test_screen());
    h.resize(1, 1, 234, 232);

    lv_obj_send_event(part(h, "dryer_row_5"), LV_EVENT_CLICKED, nullptr);
    CHECK(taps.empty());
}

TEST_CASE_METHOD(DryerFixture, "dryer: Wide lists at most four drying times",
                 "[widget_size][dryer]") {
    DryerSummaryInput in;
    in.zones = {box(0, "Box 1", 31.0f, 150), box(1, "Box 2", 30.0f, 120),
                box(2, "Box 3", 29.0f, 90), box(3, "Box 4", 28.0f, 60), box(4, "Box 5", 27.0f, 45)};
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(subject_text("home_dryer_count_text") == "5 drying");
    const std::string times = subject_text("home_dryer_times_text");
    size_t separators = 0;
    for (size_t at = times.find("  "); at != std::string::npos; at = times.find("  ", at + 2))
        ++separators;
    CHECK(separators == 3);
}

TEST_CASE_METHOD(DryerFixture, "dryer: a row names its box, or numbers it, and carries humidity",
                 "[widget_size][dryer]") {
    DryerSummaryInput in;
    in.zones = {box(0, "Left Box", 31.0f, 150), box(2, "", 40.0f)};
    publish(in);
    PanelWidgetHarness<DryerWidget> h(test_screen());

    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    REQUIRE(subject_int("home_dryer_row_count") >= 2);
    CHECK(subject_text("home_dryer_row_0_name") == "Left Box");
    CHECK(subject_text("home_dryer_row_1_name") == "Box 3");
    CHECK(subject_text("home_dryer_row_1_detail").find("40% RH") != std::string::npos);
}
