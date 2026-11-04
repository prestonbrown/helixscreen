// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_host_health.cpp
 * @brief The host_health widget draws what its ladder promises at each rung: which blocks
 * carry HIDDEN, which text they show, the status and cause it publishes from the
 * performance subjects, the MCU rows the Full rung builds, and the tap.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large). The decision is host_health_layout.h's; these
 * tests pin its wiring to the real fonts, XML and performance subjects.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][host_health]"
 */

#include "ui_overlay_performance.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/performance_state_test_access.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/tile_boxes.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "panel_widget_manager.h"
#include "performance_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/host_health_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::perf::McuStat;
using helix::perf::PerformanceState;
using helix::perf::PerformanceStateTestAccess;
using helix::perf::PerfSample;

namespace {

struct HostHealthFixture : public LVGLUITestFixture {
    HostHealthFixture() {
        PerformanceState::instance().init_subjects();
        PanelWidgetManager::instance().init_widget_subjects();
        set_print_active(false);
        feed(sample());
    }
    ~HostHealthFixture() override {
        set_print_active(false);
        drain();
        PerformanceState::instance().deinit_subjects();
    }

    /// A calm host with two MCUs.
    static PerfSample sample() {
        PerfSample s;
        s.host_cpu_pct = 42.0f;
        s.host_cpu_temp_c = 61.4f;
        s.host_mem_free_mb = 812;
        s.host_mem_pct_used = 84.0f;
        s.host_disk_total_mb = 30000;
        s.host_disk_free_mb = 18841;
        McuStat a;
        a.name = "mcu";
        a.load = 0.10f;
        a.retransmits = 0;
        McuStat b;
        b.name = "mcu sb";
        b.load = 0.20f;
        b.retransmits = 0;
        s.mcus = {a, b};
        return s;
    }

    void feed(const PerfSample& s) {
        PerformanceStateTestAccess::apply_sample(PerformanceState::instance(), s);
        drain();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

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
    return subject_int("host_health_rung");
}

lv_obj_t* part(PanelWidgetHarness<HostHealthWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return obj;
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

} // namespace

TEST_CASE_METHOD(HostHealthFixture, "host_health: medium-tier budgets land on their rung",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    const auto& boxes = test::kMediumLadder;
    for (const auto& b : boxes) {
        for (const bool printing : {false, true}) {
            INFO(b.w << "x" << b.h << " printing=" << printing);
            set_print_active(printing);
            h.resize(1, 1, b.w, b.h);
            CHECK(rung_subject() == static_cast<int>(b.rung));
        }
    }
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: half-cell boxes at large land on a rung",
                 "[widget_size][host_health]") {
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 51, 53);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Glance));
    h.resize(1, 1, 108, 53);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Strip));
    h.resize(1, 1, 51, 112);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Column));
    h.resize(1, 1, 108, 112);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Cell));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: each rung shows exactly its block",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    const struct {
        int w, h;
        const char* block;
    } rungs[] = {
        {55, 54, "host_health_glance"},  {114, 54, "host_health_strip"},
        {55, 113, "host_health_column"}, {114, 113, "host_health_cell"},
        {234, 113, "host_health_wide"},  {234, 232, "host_health_full"},
    };
    for (const auto& r : rungs) {
        INFO(r.block);
        h.resize(1, 1, r.w, r.h);
        for (const auto& other : rungs) {
            CHECK(shown(part(h, other.block)) == (std::string(other.block) == r.block));
        }
    }
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: texts follow the rung and the readings",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(subject_text("host_health_glance_text") == "42%");

    h.resize(1, 1, 114, 54);
    CHECK(subject_text("host_health_cpu_text") == "42%");
    CHECK(subject_text("host_health_temp_text") == "61\xc2\xb0"
                                                   "C");

    h.resize(1, 1, 55, 113);
    CHECK(subject_text("host_health_mem_text") == "84%");

    h.resize(1, 1, 114, 113);
    CHECK(subject_text("host_health_detail_text") == "61\xc2\xb0"
                                                     "C \xc2\xb7 84%");

    h.resize(1, 1, 234, 232);
    CHECK(subject_text("host_health_temp_text") == "61.4\xc2\xb0"
                                                   "C");
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: only the current rung's texts are recomposed",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    const std::string cpu_before = subject_text("host_health_cpu_text");
    const std::string detail_before = subject_text("host_health_detail_text");

    PerfSample busy = HostHealthFixture::sample();
    busy.host_cpu_pct = 55.0f;
    feed(busy);
    CHECK(subject_text("host_health_glance_text") == "55%");
    // The Glance rung does not draw these, so the tick leaves them alone.
    CHECK(subject_text("host_health_cpu_text") == cpu_before);
    CHECK(subject_text("host_health_detail_text") == detail_before);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: status and cause follow the performance subjects",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    CHECK(subject_int("host_health_status") == 0);
    CHECK(subject_int("host_health_cause") == 0);

    PerfSample s = HostHealthFixture::sample();
    s.host_cpu_pct = 75.0f;
    feed(s);
    CHECK(subject_int("host_health_status") == 1);
    CHECK(subject_int("host_health_cause") == 1);

    s.host_cpu_pct = 95.0f;
    feed(s);
    CHECK(subject_int("host_health_status") == 2);

    // Temperature at 82 C is Danger and, with CPU still high, the earlier cause wins.
    s.host_cpu_pct = 10.0f;
    s.host_cpu_temp_c = 82.0f;
    feed(s);
    CHECK(subject_int("host_health_status") == 2);
    CHECK(subject_int("host_health_cause") == 2);
    CHECK(subject_text("host_health_glance_text") == "82\xc2\xb0");

    // A throttle "now" bit is Danger; "since boot" alone is Warn.
    s.host_cpu_temp_c = 50.0f;
    s.host_throttle_bits = 0x50000;
    s.host_throttle_text = "Under-voltage previously";
    feed(s);
    CHECK(subject_int("host_health_status") == 1);
    CHECK(subject_int("host_health_cause") == 3);
    CHECK(subject_int("host_health_throttle_status") == 1);
    s.host_throttle_bits = 0x50005;
    feed(s);
    CHECK(subject_int("host_health_status") == 2);
    CHECK(subject_int("host_health_throttle_status") == 2);
    CHECK(subject_text("host_health_glance_text") == "!");

    // Each reading keeps its own status whatever the cause is: the throttle flag wins the
    // cause here, and the amber CPU and low disk still show their own colour.
    s.host_cpu_pct = 75.0f;
    s.host_disk_free_mb = 300;
    s.host_throttle_bits = 0x5;
    feed(s);
    CHECK(subject_int("host_health_cause") == 3);
    CHECK(subject_int("host_health_cpu_status") == 1);
    CHECK(subject_int("host_health_disk_status") == 1);
    CHECK(subject_int("host_health_temp_status") == 0);
    CHECK(subject_int("host_health_throttle_status") == 2);
    s.host_cpu_pct = 10.0f;
    s.host_disk_free_mb = 18841;
    feed(s);
    CHECK(subject_int("host_health_cpu_status") == 0);
    CHECK(subject_int("host_health_disk_status") == 0);

    // Low disk is Warn.
    s.host_throttle_bits = 0;
    s.host_disk_free_mb = 300;
    feed(s);
    CHECK(subject_int("host_health_status") == 1);
    CHECK(subject_int("host_health_cause") == 6);
    CHECK(subject_text("host_health_glance_text") == "300M");

    s.host_disk_free_mb = 18841;
    feed(s);
    CHECK(subject_int("host_health_status") == 0);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: an MCU load rise or a retransmit rise is judged",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);

    PerfSample s = HostHealthFixture::sample();
    s.mcus[1].load = 0.93f;
    feed(s);
    CHECK(subject_int("host_health_status") == 2);
    CHECK(subject_int("host_health_cause") == 4);
    CHECK(subject_text("host_health_glance_text") == "93%");

    s.mcus[1].load = 0.20f;
    feed(s);
    CHECK(subject_int("host_health_status") == 0);

    // A count higher than at the previous sample is a rise, and only for that sample.
    s.mcus[0].retransmits = 5;
    feed(s);
    CHECK(subject_int("host_health_status") == 1);
    CHECK(subject_int("host_health_cause") == 5);
    feed(s);
    CHECK(subject_int("host_health_status") == 0);
    s.mcus[0].retransmits = 9;
    feed(s);
    CHECK(subject_int("host_health_status") == 1);
    s.mcus[0].retransmits = 3;
    feed(s);
    CHECK(subject_int("host_health_status") == 0);
}

TEST_CASE_METHOD(HostHealthFixture,
                 "host_health: a re-decide between samples does not consume a retransmit rise",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);

    PerfSample s = HostHealthFixture::sample();
    s.mcus[0].retransmits = 5;
    feed(s);
    REQUIRE(subject_int("host_health_cause") == 5);

    // print_active, a resize and a rung change all re-run the decision without a sample.
    set_print_active(true);
    CHECK(subject_int("host_health_status") == 1);
    h.resize(1, 1, 114, 113);
    CHECK(subject_int("host_health_status") == 1);
    h.resize(1, 1, 55, 54);
    set_print_active(false);
    CHECK(subject_int("host_health_status") == 1);
    CHECK(subject_int("host_health_cause") == 5);

    // The next sample with the same count is the end of the rise.
    feed(s);
    CHECK(subject_int("host_health_status") == 0);
}

namespace {

/// A source that records whether the disk is wanted and sends nothing.
class WantSource : public perf::IPerformanceSource {
  public:
    void start() override {}
    void stop() override {}
    void set_callback(SampleCallback) override {}
    void set_disk_wanted(bool wanted) override {
        wanted_ = wanted;
    }
    bool wanted_ = false;
};

} // namespace

TEST_CASE_METHOD(HostHealthFixture, "host_health: the disk is wanted only while a tile is attached",
                 "[widget_size][host_health]") {
    auto source = std::make_unique<WantSource>();
    WantSource* probe = source.get();
    PerformanceState::instance().set_source(std::move(source));
    REQUIRE_FALSE(probe->wanted_);
    {
        PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
        CHECK(probe->wanted_);
        {
            PanelWidgetHarness<HostHealthWidget> second(test_screen(), state());
            CHECK(probe->wanted_);
        }
        CHECK(probe->wanted_); // the first tile still shows it
    }
    CHECK_FALSE(probe->wanted_);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: a source installed after a tile still hears it",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    auto source = std::make_unique<WantSource>();
    WantSource* probe = source.get();
    PerformanceState::instance().set_source(std::move(source));
    CHECK(probe->wanted_);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: the first retransmit count seen is a baseline",
                 "[widget_size][host_health]") {
    PerfSample s = HostHealthFixture::sample();
    s.mcus[0].retransmits = 7;
    feed(s);
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    feed(s);
    CHECK(subject_int("host_health_status") == 0);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: the Glance icon follows the cause",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    const struct {
        const char* icon;
        int cause;
    } icons[] = {{"host_health_glance_cpu", 0},  {"host_health_glance_cpu", 1},
                 {"host_health_glance_temp", 2}, {"host_health_glance_throttle", 3},
                 {"host_health_glance_mcu", 4},  {"host_health_glance_retx", 5},
                 {"host_health_glance_disk", 6}};
    const char* all[] = {"host_health_glance_cpu",      "host_health_glance_temp",
                         "host_health_glance_throttle", "host_health_glance_mcu",
                         "host_health_glance_retx",     "host_health_glance_disk"};
    for (const auto& i : icons) {
        INFO("cause " << i.cause);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "host_health_cause"), i.cause);
        for (const char* name : all) {
            CHECK(shown(part(h, name)) == (std::string(name) == i.icon));
        }
    }
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: an amber status draws the Glance value amber",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    const lv_color_t warning = theme_manager_get_color("warning");
    const lv_color_t danger = theme_manager_get_color("danger");
    lv_obj_t* value = part(h, "host_health_glance_text");
    CHECK_FALSE(lv_color_eq(lv_obj_get_style_text_color(value, LV_PART_MAIN), warning));

    PerfSample s = HostHealthFixture::sample();
    s.host_cpu_pct = 75.0f;
    feed(s);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(value, LV_PART_MAIN), warning));
    s.host_cpu_pct = 95.0f;
    feed(s);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(value, LV_PART_MAIN), danger));
}

TEST_CASE_METHOD(HostHealthFixture,
                 "host_health: Wide swaps memory for the busiest MCU while printing",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(shown(part(h, "host_health_wide_mem_row")));
    CHECK_FALSE(shown(part(h, "host_health_wide_mcu_row")));

    set_print_active(true);
    CHECK(subject_int("host_health_show_mcu_row") == 1);
    CHECK_FALSE(shown(part(h, "host_health_wide_mem_row")));
    CHECK(shown(part(h, "host_health_wide_mcu_row")));
    CHECK(subject_text("host_health_busiest_name") == "mcu sb");
    CHECK(subject_int("host_health_busiest_load") == 20);
    CHECK(subject_text("host_health_busiest_text") == "20%");

    PerfSample s = HostHealthFixture::sample();
    s.mcus[0].load = 0.55f;
    s.mcus[0].retransmits = 14;
    feed(s);
    CHECK(subject_text("host_health_busiest_name") == "mcu");
    CHECK(subject_text("host_health_busiest_text") == "55% \xc2\xb7 14 retx");

    set_print_active(false);
    CHECK(subject_int("host_health_show_mcu_row") == 0);
    CHECK(shown(part(h, "host_health_wide_mem_row")));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: a printer with no MCU keeps the memory row",
                 "[widget_size][host_health]") {
    PerfSample s = HostHealthFixture::sample();
    s.mcus.clear();
    feed(s);
    set_print_active(true);
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(subject_int("host_health_show_mcu_row") == 0);
    CHECK(shown(part(h, "host_health_wide_mem_row")));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: Full builds one row per MCU the box holds",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    lv_obj_t* list = part(h, "host_health_mcu_list");

    h.resize(1, 1, 234, 113);
    CHECK(lv_obj_get_child_count(list) == 0);

    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("host_health_mcu_rows") == 2);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    lv_obj_t* name = lv_obj_find_by_name(lv_obj_get_child(list, 1), "host_health_mcu_name");
    REQUIRE(name != nullptr);
    CHECK(std::string(lv_label_get_text(name)) == "mcu sb");

    // Leaving Full drops the rows, so no sparkline repaints for a block that is hidden.
    h.resize(1, 1, 234, 113);
    CHECK(lv_obj_get_child_count(list) == 0);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: a tall Full box never builds more rows than MCUs",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 400, 460);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("host_health_mcu_rows") == 2);
    CHECK(lv_obj_get_child_count(part(h, "host_health_mcu_list")) == 2);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: the MCU set changing rebuilds the Full rows",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 400, 460);
    lv_obj_t* list = part(h, "host_health_mcu_list");
    REQUIRE(lv_obj_get_child_count(list) == 2);

    PerfSample s = HostHealthFixture::sample();
    McuStat c;
    c.name = "mcu zz";
    c.load = 0.3f;
    c.retransmits = 0;
    s.mcus.push_back(c);
    feed(s);
    CHECK(lv_obj_get_child_count(list) == 3);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: the throttle line shows only while a flag is set",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    lv_obj_t* line = part(h, "host_health_throttle");
    CHECK_FALSE(shown(line));

    PerfSample s = HostHealthFixture::sample();
    s.host_throttle_bits = 0x1;
    s.host_throttle_text = "Under-voltage detected";
    feed(s);
    CHECK(shown(line));
    CHECK(std::string(lv_label_get_text(line)) == "Under-voltage detected");

    s.host_throttle_bits = 0;
    feed(s);
    CHECK_FALSE(shown(line));
}

TEST_CASE_METHOD(HostHealthFixture,
                 "host_health: Full shows disk free and hides it when unreported",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    lv_obj_t* disk = part(h, "host_health_full_disk");
    CHECK(shown(disk));
    CHECK(std::string(lv_label_get_text(disk)) == "18.4 GB free");

    PerfSample s = HostHealthFixture::sample();
    s.host_disk_free_mb.reset();
    s.host_disk_total_mb.reset();
    feed(s);
    CHECK_FALSE(shown(disk));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: print_active re-decides the rung",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(subject_int("host_health_show_mcu_row") == 0);
    set_print_active(true);
    CHECK(subject_int("host_health_show_mcu_row") == 1);
    set_print_active(false);
    CHECK(subject_int("host_health_show_mcu_row") == 0);
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: fits_at refuses what Glance cannot draw",
                 "[widget_size][host_health]") {
    HostHealthWidget widget(state());
    CHECK(widget.fits_at(55, 54));
    CHECK(widget.fits_at(55, 113));
    CHECK(widget.fits_at(114, 54));
    CHECK_FALSE(widget.fits_at(20, 20));
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        CHECK(widget.fits_at(51, 53));
        CHECK(widget.fits_at(51, 112));
        CHECK(widget.fits_at(108, 53));
    }
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        CHECK(widget.fits_at(70, 64));
    }

    bool fitted = false;
    for (int w = 0; w <= 200; ++w) {
        const bool now = widget.fits_at(w, 113);
        if (fitted)
            CHECK(now);
        fitted = fitted || now;
    }
    bool fitted_h = false;
    for (int hgt = 0; hgt <= 200; ++hgt) {
        const bool now = widget.fits_at(114, hgt);
        if (fitted_h)
            CHECK(now);
        fitted_h = fitted_h || now;
    }
}

TEST_CASE_METHOD(HostHealthFixture,
                 "host_health: a transient instance does not publish over the live tile",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    HostHealthWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: a transient instance leaving hands the tile back",
                 "[widget_size][host_health]") {
    PanelWidgetHarness<HostHealthWidget> live(test_screen(), state());
    live.resize(1, 1, 234, 232);
    {
        PanelWidgetHarness<HostHealthWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    }
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(HostHealthFixture, "host_health: a tap opens the Performance overlay",
                 "[widget_size][host_health][actions]") {
    PanelWidgetHarness<HostHealthWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 232);
    CHECK(helix::ui::UiOverlayPerformance::instance().root() == nullptr);

    lv_obj_send_event(h.root(), LV_EVENT_CLICKED, nullptr);
    process_lvgl(300);
    lv_obj_t* overlay = helix::ui::UiOverlayPerformance::instance().root();
    REQUIRE(overlay != nullptr);
    CHECK_FALSE(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
}
