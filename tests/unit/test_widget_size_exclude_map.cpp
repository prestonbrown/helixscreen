// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_exclude_map.cpp
 * @brief The exclude_map widget draws what its ladder promises at each rung: which blocks
 * carry HIDDEN, which text they show, when it holds the mini map, and that every tap
 * only navigates.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * exclude_map_layout.h's; these tests pin its wiring to the real fonts, XML and
 * exclude-object state.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][exclude_map]"
 */

#include "ui_language_refresh.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "lvgl.h"
#include "panel_widget_manager.h"
#include "printer_excluded_objects_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/exclude_map_widget.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct ExcludeMapFixture : public LVGLUITestFixture {
    ExcludeMapFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        home_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_;
        NavigationManager::instance().set_panels(panels);

        PanelWidgetManager::instance().init_widget_subjects();
        reset_state();
    }
    ~ExcludeMapFixture() override {
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_});
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        ModalStack::instance().clear();
        reset_state();
        drain();
        process_lvgl(20);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    void reset_state() {
        objects().clear_objects();
        set_print_active(false);
        drain();
    }
    PrinterExcludedObjectsState& objects() {
        return state().excluded_objects_state();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    static void tap(lv_obj_t* obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }

    /// @p count objects laid out on a grid, each with a square outline.
    void define_objects(int count) {
        using ObjectInfo = PrinterExcludedObjectsState::ObjectInfo;
        std::vector<ObjectInfo> defined;
        for (int i = 0; i < count; ++i) {
            const float x = 20.0f + static_cast<float>(i % 3) * 60.0f;
            const float y = 20.0f + static_cast<float>(i / 3) * 60.0f;
            defined.push_back(PrinterExcludedObjectsState::make_object_info(
                "OBJ_" + std::to_string(i), glm::vec2{x + 20.0f, y + 20.0f},
                {{x, y}, {x + 40.0f, y}, {x + 40.0f, y + 40.0f}, {x, y + 40.0f}}));
        }
        objects().set_defined_objects_with_geometry(defined);
        drain();
    }
    /// A print of six objects, one skipped and one printing now.
    void print_six() {
        define_objects(6);
        objects().set_excluded_objects({"OBJ_2"});
        objects().set_current_object("OBJ_1");
        set_print_active(true);
    }

    lv_obj_t* home_ = nullptr;
    bool animations_were_enabled_ = true;
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
    return subject_int("exclude_map_rung");
}

lv_obj_t* part(PanelWidgetHarness<ExcludeMapWidget>& h, const char* name) {
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

/// The mini map views the tile holds: children of its host.
uint32_t map_children(PanelWidgetHarness<ExcludeMapWidget>& h) {
    return lv_obj_get_child_count(part(h, "exclude_map_host"));
}

struct Box {
    int w;
    int h;
    SizeRung rung;
};

constexpr Box kGlance{55, 54, SizeRung::Glance};
constexpr Box kStrip{114, 54, SizeRung::Strip};
constexpr Box kColumn{55, 113, SizeRung::Column};
constexpr Box kCell{114, 113, SizeRung::Cell};
constexpr Box kWide{234, 113, SizeRung::Wide};
constexpr Box kFull{234, 232, SizeRung::Full};

} // namespace

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: medium-tier budgets land on their rung",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
        CHECK(subject_int("exclude_map_placeholder") == 0);
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: half-cell boxes at large land on a rung",
                 "[widget_size][exclude_map]") {
    print_six();
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, 51, 53);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Glance));
    h.resize(1, 1, 108, 53);
    CHECK(rung_subject() >= static_cast<int>(SizeRung::Glance));
    h.resize(1, 1, 51, 112);
    CHECK(rung_subject() >= static_cast<int>(SizeRung::Glance));
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: each rung shows exactly its blocks",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    struct Row {
        Box box;
        bool glance, strip, content, name, skip;
    };
    const Row rows[] = {
        {kGlance, true, false, false, false, false}, {kStrip, false, true, false, false, false},
        {kColumn, false, false, true, false, false}, {kCell, false, false, true, false, false},
        {kWide, false, false, true, true, false},    {kFull, false, false, true, true, true},
    };
    for (const auto& r : rows) {
        INFO(r.box.w << "x" << r.box.h);
        h.resize(1, 1, r.box.w, r.box.h);
        CHECK_FALSE(shown(part(h, "exclude_map_placeholder")));
        CHECK(shown(part(h, "exclude_map_glance")) == r.glance);
        CHECK(shown(part(h, "exclude_map_strip")) == r.strip);
        CHECK(shown(part(h, "exclude_map_content")) == r.content);
        CHECK(shown(part(h, "exclude_map_now_name")) == r.name);
        CHECK(shown(part(h, "exclude_map_skip_button")) == r.skip);
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: texts follow the rung and the counts",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());

    h.resize(1, 1, kGlance.w, kGlance.h);
    CHECK(subject_text("exclude_map_count_text") == "5/6");

    h.resize(1, 1, kStrip.w, kStrip.h);
    CHECK(subject_text("exclude_map_objects_text") == "6 objects");
    CHECK(subject_text("exclude_map_skipped_text") == "1 skipped");

    h.resize(1, 1, kColumn.w, kColumn.h);
    CHECK(subject_text("exclude_map_count_text") == "5/6");
    CHECK(shown(part(h, "exclude_map_short_count")));
    CHECK_FALSE(shown(part(h, "exclude_map_printing")));

    h.resize(1, 1, kCell.w, kCell.h);
    CHECK(subject_text("exclude_map_printing_text") == "5 of 6 printing");
    CHECK_FALSE(shown(part(h, "exclude_map_short_count")));
    CHECK(shown(part(h, "exclude_map_printing")));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_text("exclude_map_current_name") == "OBJ_1");
    CHECK(subject_int("exclude_map_has_current") == 1);
    CHECK(subject_text("exclude_map_skipped_text") == "1 skipped");
    CHECK(shown(part(h, "exclude_map_skipped")));

    objects().set_excluded_objects({"OBJ_2", "OBJ_4"});
    drain();
    CHECK(subject_text("exclude_map_printing_text") == "4 of 6 printing");
    CHECK(subject_text("exclude_map_skipped_text") == "2 skipped");

    objects().set_current_object("OBJ_3");
    drain();
    CHECK(subject_text("exclude_map_current_name") == "OBJ_3");
    objects().set_current_object("");
    drain();
    CHECK(subject_int("exclude_map_has_current") == 0);
    CHECK_FALSE(shown(part(h, "exclude_map_now_name")));
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: only the current rung's texts are recomposed",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kGlance.w, kGlance.h);
    const std::string objects_before = subject_text("exclude_map_objects_text");
    objects().set_excluded_objects({"OBJ_0", "OBJ_1", "OBJ_2"});
    drain();
    CHECK(subject_text("exclude_map_count_text") == "3/6");
    CHECK(subject_text("exclude_map_objects_text") == objects_before);
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: idle and single-object prints are a placeholder",
                 "[widget_size][exclude_map]") {
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());

    SECTION("not printing, with objects defined") {
        define_objects(6);
        for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide, kFull}) {
            INFO(b.w << "x" << b.h);
            h.resize(1, 1, b.w, b.h);
            CHECK(subject_int("exclude_map_placeholder") == 1);
            CHECK(shown(part(h, "exclude_map_placeholder")));
            CHECK_FALSE(shown(part(h, "exclude_map_glance")));
            CHECK_FALSE(shown(part(h, "exclude_map_strip")));
            CHECK_FALSE(shown(part(h, "exclude_map_content")));
            CHECK_FALSE(shown(part(h, "exclude_map_skip_button")));
            CHECK(subject_text("exclude_map_placeholder_text") == "No objects");
        }
    }

    SECTION("printing one object") {
        define_objects(1);
        set_print_active(true);
        h.resize(1, 1, kFull.w, kFull.h);
        CHECK(subject_int("exclude_map_placeholder") == 1);
        CHECK(subject_text("exclude_map_placeholder_text") == "Single object");
        CHECK_FALSE(shown(part(h, "exclude_map_content")));
    }

    SECTION("the caption needs more than the Glance box") {
        h.resize(1, 1, kGlance.w, kGlance.h);
        CHECK_FALSE(shown(part(h, "exclude_map_placeholder_caption")));
        h.resize(1, 1, kStrip.w, kStrip.h);
        CHECK(shown(part(h, "exclude_map_placeholder_caption")));
    }
}

TEST_CASE_METHOD(ExcludeMapFixture,
                 "exclude_map: the map exists only at its rungs, active and printing",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.widget().on_activate();

    for (const auto& b : {kGlance, kStrip}) {
        h.resize(1, 1, b.w, b.h);
        CHECK(map_children(h) == 0);
    }
    for (const auto& b : {kColumn, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(map_children(h) == 1);
    }

    SECTION("a hidden home panel keeps no map and a shown one restores it") {
        h.widget().on_deactivate();
        CHECK(map_children(h) == 0);
        objects().set_excluded_objects({"OBJ_0"});
        drain();
        process_lvgl(20);
        CHECK(map_children(h) == 0);
        h.widget().on_activate();
        process_lvgl(20);
        CHECK(map_children(h) == 1);
    }

    SECTION("the print ending turns the tile into a placeholder without the map") {
        set_print_active(false);
        process_lvgl(20);
        CHECK(subject_int("exclude_map_placeholder") == 1);
        CHECK(map_children(h) == 0);
        set_print_active(true);
        process_lvgl(20);
        CHECK(subject_int("exclude_map_placeholder") == 0);
        CHECK(map_children(h) == 1);
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: the map is not built before the panel is shown",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(map_children(h) == 0);
    h.widget().on_activate();
    CHECK(map_children(h) == 1);
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: the map follows the rung's slot",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.widget().on_activate();

    h.resize(1, 1, kCell.w, kCell.h);
    lv_obj_t* host = part(h, "exclude_map_host");
    lv_obj_t* view = lv_obj_get_child(host, 0);
    REQUIRE(view != nullptr);
    const int cell_h = lv_obj_get_height(host);
    CHECK(lv_obj_get_width(view) == lv_obj_get_width(host));

    h.resize(1, 1, kFull.w, kFull.h);
    host = part(h, "exclude_map_host");
    view = lv_obj_get_child(host, 0);
    REQUIRE(view != nullptr);
    CHECK(lv_obj_get_height(host) > cell_h);
    CHECK(lv_obj_get_width(view) == lv_obj_get_width(host));
    CHECK(lv_obj_get_height(view) == lv_obj_get_height(host));
}

TEST_CASE_METHOD(ExcludeMapFixture,
                 "exclude_map: the text sits under the map, and beside it at Wide",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    for (const auto& b : {kColumn, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        lv_obj_t* host = part(h, "exclude_map_host");
        lv_obj_t* text = part(h, "exclude_map_text");
        if (b.rung == SizeRung::Wide) {
            CHECK(lv_obj_get_x(text) >= lv_obj_get_x(host) + lv_obj_get_width(host));
        } else {
            CHECK(lv_obj_get_y(text) >= lv_obj_get_y(host) + lv_obj_get_height(host));
        }
        // Nothing spills out of the tile.
        lv_area_t tile, content;
        lv_obj_get_coords(h.root(), &tile);
        lv_obj_get_coords(part(h, "exclude_map_content"), &content);
        CHECK(content.y2 <= tile.y2);
        CHECK(content.x2 <= tile.x2);
        lv_area_t text_area;
        lv_obj_get_coords(text, &text_area);
        CHECK(text_area.y2 <= tile.y2);
        CHECK(text_area.x2 <= tile.x2);
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: a new object count re-decides the tile",
                 "[widget_size][exclude_map]") {
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(subject_int("exclude_map_placeholder") == 1);

    define_objects(1);
    set_print_active(true);
    CHECK(subject_text("exclude_map_placeholder_text") == "Single object");

    define_objects(6);
    CHECK(subject_int("exclude_map_placeholder") == 0);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_text("exclude_map_printing_text") == "6 of 6 printing");

    // A second digit widens the counts, which the Cell box budgets for.
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    define_objects(12);
    CHECK(subject_text("exclude_map_printing_text") == "12 of 12 printing");
    CHECK(rung_subject() <= static_cast<int>(SizeRung::Cell));
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: Glance, Strip and Cell content is centred",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    struct Row {
        Box box;
        const char* block;
    };
    for (const auto& r : {Row{kGlance, "exclude_map_glance"}, Row{kStrip, "exclude_map_strip"}}) {
        INFO(r.block);
        h.resize(1, 1, r.box.w, r.box.h);
        lv_obj_t* block = part(h, r.block);
        lv_area_t a;
        lv_obj_get_coords(block, &a);
        lv_area_t root;
        lv_obj_get_coords(h.root(), &root);
        const int block_mid = (a.y1 + a.y2) / 2;
        const int root_mid = (root.y1 + root.y2) / 2;
        CHECK(std::abs(block_mid - root_mid) <= 2);
    }
    SECTION("the placeholder too") {
        reset_state();
        h.resize(1, 1, kStrip.w, kStrip.h);
        lv_area_t a;
        lv_obj_get_coords(part(h, "exclude_map_placeholder"), &a);
        lv_area_t root;
        lv_obj_get_coords(h.root(), &root);
        CHECK(std::abs((a.y1 + a.y2) / 2 - (root.y1 + root.y2) / 2) <= 2);
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: fits_at refuses what Glance cannot draw",
                 "[widget_size][exclude_map]") {
    ExcludeMapWidget widget(state());
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

TEST_CASE_METHOD(ExcludeMapFixture,
                 "exclude_map: a transient instance does not publish over the live tile",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    ExcludeMapWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: a transient instance leaving hands the tile back",
                 "[widget_size][exclude_map]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> live(test_screen(), state());
    live.resize(1, 1, kFull.w, kFull.h);
    {
        PanelWidgetHarness<ExcludeMapWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    }
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(ExcludeMapFixture,
                 "exclude_map: a tap anywhere opens the full map in print status",
                 "[widget_size][exclude_map][actions]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == nullptr);

    tap(h.root());
    process_lvgl(20);
    drain();
    lv_obj_t* tree = PrintStatusPanel::get_cached_overlay();
    REQUIRE(tree != nullptr);
    CHECK(NavigationManager::instance().is_panel_in_stack(tree));
    CHECK(PrintStatusPanelTestAccess::exclude_open(get_global_print_status_panel()));
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: Skip objects... opens the map and only navigates",
                 "[widget_size][exclude_map][actions]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(part(h, "exclude_map_skip_button")));

    int tile_taps = 0;
    lv_obj_add_event_cb(
        h.root(), [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_CLICKED, &tile_taps);
    const auto excluded_before = objects().get_excluded_objects();

    tap(part(h, "exclude_map_skip_button"));
    process_lvgl(20);
    drain();
    lv_obj_t* tree = PrintStatusPanel::get_cached_overlay();
    REQUIRE(tree != nullptr);
    CHECK(PrintStatusPanelTestAccess::exclude_open(get_global_print_status_panel()));
    // The button's tap is not also a tap on the tile.
    CHECK(tile_taps == 0);
    CHECK(objects().get_excluded_objects() == excluded_before);
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: no tap excludes anything or asks to",
                 "[widget_size][exclude_map][actions]") {
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.widget().on_activate();
    const auto excluded_before = objects().get_excluded_objects();

    for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        tap(h.root());
        if (map_children(h) > 0)
            tap(lv_obj_get_child(part(h, "exclude_map_host"), 0));
        process_lvgl(20);
        drain();
        CHECK(objects().get_excluded_objects() == excluded_before);
        CHECK(ModalStack::instance().stack_empty());
    }
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: a placeholder tap goes nowhere",
                 "[widget_size][exclude_map][actions]") {
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(subject_int("exclude_map_placeholder") == 1);

    tap(h.root());
    process_lvgl(20);
    drain();
    CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: a size fits_at accepts holds a three-digit count",
                 "[widget_size][exclude_map]") {
    ExcludeMapWidget probe(state());
    define_objects(120);
    objects().set_excluded_objects({"OBJ_1"});
    set_print_active(true);
    int accepted = 0;
    for (const auto& b : {kGlance, kStrip, kColumn, kCell, kWide, kFull}) {
        if (!probe.fits_at(b.w, b.h))
            continue;
        ++accepted;
        INFO(b.w << "x" << b.h);
        PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
        h.resize(1, 1, b.w, b.h);
        lv_area_t tile;
        lv_obj_get_coords(h.root(), &tile);
        for (const char* name :
             {"exclude_map_glance", "exclude_map_strip", "exclude_map_content"}) {
            lv_obj_t* block = part(h, name);
            if (!shown(block))
                continue;
            lv_area_t a;
            lv_obj_get_coords(block, &a);
            CHECK(a.x1 >= tile.x1);
            CHECK(a.x2 <= tile.x2);
            for (uint32_t i = 0; i < lv_obj_get_child_count(block); ++i) {
                lv_obj_t* child = lv_obj_get_child(block, i);
                if (hidden(child))
                    continue;
                lv_obj_update_layout(child);
                lv_obj_refr_size(child);
                CHECK(lv_obj_get_self_width(child) <= lv_obj_get_width(block) + 1);
            }
        }
    }
    CHECK(accepted >= 4);
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: a language switch re-composes the object texts",
                 "[widget_size][exclude_map][i18n]") {
    helix::ui::init_language_refresh();
    auto& settings = SystemSettingsManager::instance();
    settings.set_language("en");
    drain();
    print_six();
    PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());

    h.resize(1, 1, kStrip.w, kStrip.h);
    REQUIRE(subject_text("exclude_map_objects_text") == "6 objects");
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_text("exclude_map_printing_text") == "5 of 6 printing");
    REQUIRE(std::string(lv_tr("Now printing")) == "Now printing");

    settings.set_language("ru");
    drain();
    REQUIRE(std::string(lv_tr("%d of %d printing")) != "%d of %d printing");
    char want[64];
    std::snprintf(want, sizeof(want), lv_tr("%d of %d printing"), 5, 6);
    CHECK(subject_text("exclude_map_printing_text") == want);
    std::snprintf(want, sizeof(want), lv_tr("%d skipped"), 1);
    CHECK(subject_text("exclude_map_skipped_text") == want);

    h.resize(1, 1, kStrip.w, kStrip.h);
    std::snprintf(want, sizeof(want), lv_tr("%d objects"), 6);
    CHECK(subject_text("exclude_map_objects_text") == want);

    h.resize(1, 1, kWide.w, kWide.h);
    lv_obj_t* caption = part(h, "exclude_map_now_caption");
    CHECK(std::string(lv_label_get_text(caption)) == lv_tr("Now printing"));

    settings.set_language("en");
    drain();
}

TEST_CASE_METHOD(ExcludeMapFixture, "exclude_map: the tree can be deleted under a live map",
                 "[widget_size][exclude_map]") {
    print_six();
    const uint32_t baseline = lv_obj_get_child_count(test_screen());
    SECTION("detach first, then the tree") {
        {
            PanelWidgetHarness<ExcludeMapWidget> h(test_screen(), state());
            h.widget().on_activate();
            h.resize(1, 1, kFull.w, kFull.h);
            REQUIRE(map_children(h) == 1);
        }
        lv_obj_delete(lv_obj_get_child(test_screen(), -1));
        process_lvgl(30);
        drain();
        CHECK(lv_obj_get_child_count(test_screen()) == baseline);
    }
    SECTION("the tree first, with the map still built") {
        auto h = std::make_unique<PanelWidgetHarness<ExcludeMapWidget>>(test_screen(), state());
        h->widget().on_activate();
        h->resize(1, 1, kFull.w, kFull.h);
        REQUIRE(map_children(*h) == 1);
        REQUIRE(subject_text("exclude_map_skipped_text") == "1 skipped");
        lv_obj_delete(h->root());
        process_lvgl(30);
        objects().set_excluded_objects({"OBJ_0", "OBJ_2"});
        drain();
        // A widget whose tree died publishes nothing: publishing reads that tree.
        CHECK(subject_text("exclude_map_skipped_text") == "1 skipped");
        h.reset();
        process_lvgl(30);
        CHECK(lv_obj_get_child_count(test_screen()) == baseline);
    }

    // A new widget on the same state builds a live map after the teardown.
    PanelWidgetHarness<ExcludeMapWidget> again(test_screen(), state());
    again.widget().on_activate();
    again.resize(1, 1, kFull.w, kFull.h);
    CHECK(map_children(again) == 1);
}
