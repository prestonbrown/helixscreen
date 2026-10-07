// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_exclude_object_side_list.cpp
 * @brief The exclude-object side list rebuilds rows only when the defined object
 *        set changes; exclusions and the printing object restyle rows in place.
 *
 * The printing object changes many times per layer, so a rebuild there would
 * reset the list's scroll position under the user's finger.
 */

#include "ui_exclude_object_side_list.h"
#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::vector<std::string> object_names(int n, const char* prefix = "obj_") {
    std::vector<std::string> names;
    for (int i = 0; i < n; ++i) {
        names.push_back(prefix + std::to_string(i));
    }
    return names;
}

std::vector<lv_obj_t*> rows_of(lv_obj_t* container) {
    std::vector<lv_obj_t*> rows;
    const uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        rows.push_back(lv_obj_get_child(container, static_cast<int32_t>(i)));
    }
    return rows;
}

/// Whether @p row shows a visible label reading @p text. Checks what the user
/// sees, not how the row is built.
bool shows_text(lv_obj_t* row, const char* text) {
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_get_style_opa(child, LV_PART_MAIN) == LV_OPA_TRANSP) {
            continue;
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* t = lv_label_get_text(child);
            if (t && std::strcmp(t, text) == 0) {
                return true;
            }
        }
        if (shows_text(child, text)) {
            return true;
        }
    }
    return false;
}

class SideListFixture : public LVGLUITestFixture {
  public:
    SideListFixture() {
        objects().set_defined_objects(object_names(20));
        objects().set_current_object("obj_0");
        open(ExcludeTapMode::ExcludeOnly, exclude_side_list_geometry(false));
    }

    ~SideListFixture() override {
        list.destroy();
        objects().clear_objects();
        settle();
    }

    void open(ExcludeTapMode mode, SideListGeometry geom) {
        list.destroy();
        settle();
        list.create(
            test_screen(), &objects(), [this](const std::string& name) { taps.push_back(name); },
            mode, geom);
        settle();
        container = lv_obj_find_by_name(list.root(), "rows_container");
    }

    PrinterExcludedObjectsState& objects() {
        return state().excluded_objects_state();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(400);
    }

    ExcludeObjectSideList list;
    lv_obj_t* container = nullptr;
    std::vector<std::string> taps;
};

} // namespace

TEST_CASE_METHOD(SideListFixture,
                 "Side list keeps its rows and scroll position when the printing object changes",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);

    lv_obj_scroll_to_y(container, 60, LV_ANIM_OFF);
    REQUIRE(lv_obj_get_scroll_y(container) == 60);

    objects().set_current_object("obj_5");
    settle();

    REQUIRE(rows_of(container) == before);
    CHECK(lv_obj_get_scroll_y(container) == 60);
    CHECK(shows_text(before[5], "Printing now"));
    CHECK_FALSE(shows_text(before[0], "Printing now"));
}

TEST_CASE_METHOD(SideListFixture, "Side list restyles an excluded row in place",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);
    REQUIRE(lv_obj_has_flag(before[3], LV_OBJ_FLAG_CLICKABLE));
    REQUIRE(lv_obj_get_style_opa(before[3], LV_PART_MAIN) == LV_OPA_COVER);

    objects().set_excluded_objects({"obj_3"});
    settle();

    REQUIRE(rows_of(container) == before);
    CHECK_FALSE(lv_obj_has_flag(before[3], LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(before[3], LV_PART_MAIN) == 150);
    CHECK(shows_text(before[3], "Excluded"));
    CHECK(lv_obj_has_flag(before[4], LV_OBJ_FLAG_CLICKABLE));
    CHECK_FALSE(shows_text(before[4], "Excluded"));
}

TEST_CASE_METHOD(SideListFixture, "Side list rebuilds its rows when the defined objects change",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);

    objects().set_defined_objects(object_names(3, "part_"));
    settle();

    const auto after = rows_of(container);
    REQUIRE(after.size() == 3);
    for (lv_obj_t* row : after) {
        CHECK(std::find(before.begin(), before.end(), row) == before.end());
    }
}

namespace {
/// Whatever the name length, a status appearing must not reflow the name:
/// sweep lengths across the point where a name stops fitting beside a status,
/// since that is where a row would grow.
void check_row_heights_hold(SideListFixture& f) {
    for (int len = 4; len <= 64; len += 2) {
        const std::string name = "Part_" + std::string(static_cast<size_t>(len), 'm');
        INFO("name length " << name.size());
        f.objects().set_defined_objects({name, "obj_1"});
        f.objects().set_current_object("");
        f.settle();
        auto rows = rows_of(f.container);
        REQUIRE(rows.size() == 2);
        lv_obj_update_layout(f.container);
        const int32_t idle_h = lv_obj_get_height(rows[0]);

        f.objects().set_current_object(name);
        f.settle();
        lv_obj_update_layout(f.container);
        CHECK(lv_obj_get_height(rows[0]) == idle_h);

        f.objects().set_excluded_objects({name});
        f.settle();
        lv_obj_update_layout(f.container);
        CHECK(lv_obj_get_height(rows[0]) == idle_h);
        f.objects().set_excluded_objects({});
    }
}
} // namespace

TEST_CASE_METHOD(SideListFixture, "Side list rows keep their height as the printing object moves",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    check_row_heights_hold(*this);
}

TEST_CASE_METHOD(SideListFixture,
                 "Portrait side list rows keep their height and put the status beside the name",
                 "[exclude_side_list]") {
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    const int was = lv_subject_get_int(portrait);
    lv_subject_set_int(portrait, 1);
    open(ExcludeTapMode::ExcludeOnly, exclude_side_list_geometry(true));
    REQUIRE(container != nullptr);

    check_row_heights_hold(*this);

    // Side by side: the status slot sits right of the name, on the same line.
    objects().set_defined_objects({"Cube_id_1_copy_0", "obj_1"});
    objects().set_current_object("Cube_id_1_copy_0");
    settle();
    lv_obj_t* row = rows_of(container)[0];
    lv_obj_update_layout(row);
    lv_obj_t* name = lv_obj_find_by_name(row, "object_name");
    lv_obj_t* status = lv_obj_find_by_name(row, "status_printing");
    REQUIRE(name != nullptr);
    REQUIRE(status != nullptr);
    lv_area_t na, sa;
    lv_obj_get_coords(name, &na);
    lv_obj_get_coords(status, &sa);
    CHECK(sa.x1 > na.x2);                                      // right of the name
    CHECK(lv_area_get_width(&na) > lv_obj_get_width(row) / 2); // the name keeps most of the row
    const int32_t status_mid = (sa.y1 + sa.y2) / 2;
    CHECK(status_mid >= na.y1); // level with the name, not below it
    CHECK(status_mid <= na.y2);

    lv_subject_set_int(portrait, was);
}

namespace {
struct StateWatch {
    lv_obj_t* container = nullptr;
    std::vector<std::string> row_names_at_publish;
};

void record_rows(lv_observer_t* observer, lv_subject_t*) {
    auto* w = static_cast<StateWatch*>(lv_observer_get_user_data(observer));
    std::string names;
    const uint32_t n = lv_obj_get_child_count(w->container);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* label = lv_obj_find_by_name(
            lv_obj_get_child(w->container, static_cast<int32_t>(i)), "object_name");
        names += label ? lv_label_get_text(label) : "?";
        names += ",";
    }
    w->row_names_at_publish.push_back(names);
}
} // namespace

TEST_CASE_METHOD(SideListFixture, "Side list never publishes a new object's state onto an old row",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    objects().set_defined_objects({"old_a", "old_b"});
    objects().set_current_object("old_a");
    settle();
    REQUIRE(rows_of(container).size() == 2);

    lv_subject_t* row1 = lv_xml_get_subject(nullptr, "exclude_row_state_1");
    REQUIRE(row1 != nullptr);
    StateWatch watch{container, {}};
    lv_observer_t* obs = lv_subject_add_observer(row1, record_rows, &watch);
    watch.row_names_at_publish.clear(); // the add fires once

    // The excluded-version observer is queued before the defined-version one,
    // so it runs while the rows still show the old list.
    objects().set_excluded_objects({"new_d"});
    objects().set_defined_objects({"new_c", "new_d"});
    settle();
    lv_observer_remove(obs);

    REQUIRE_FALSE(watch.row_names_at_publish.empty());
    for (const auto& names : watch.row_names_at_publish) {
        INFO("rows when row 1's state was published: " << names);
        CHECK(names.find("old_") == std::string::npos);
    }
    CHECK(shows_text(rows_of(container)[1], "Excluded"));
}

TEST_CASE_METHOD(SideListFixture, "Side list chips follow a theme switch and keep the scroll",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    REQUIRE(rows_of(container).size() == 20);
    lv_obj_scroll_to_y(container, 60, LV_ANIM_OFF);
    REQUIRE(lv_obj_get_scroll_y(container) == 60);

    auto chip_text = [](lv_obj_t* row) {
        return lv_obj_get_style_text_color(lv_obj_get_child(lv_obj_get_child(row, 0), 0),
                                           LV_PART_MAIN);
    };
    std::vector<lv_color_t> before;
    for (lv_obj_t* row : rows_of(container)) {
        before.push_back(chip_text(row));
    }

    theme_manager_toggle_dark_mode(); // the defined list is unchanged
    settle();

    const auto rows = rows_of(container);
    REQUIRE(rows.size() == 20);
    bool any_changed = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        INFO("row " << i);
        lv_obj_t* disc = lv_obj_get_child(rows[i], 0);
        const lv_color_t fill = object_badge_color(static_cast<int>(i));
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(disc, LV_PART_MAIN), fill));
        CHECK(lv_color_eq(chip_text(rows[i]), object_badge_text_color(fill)));
        any_changed = any_changed || !lv_color_eq(chip_text(rows[i]), before[i]);
    }
    CHECK(lv_obj_get_scroll_y(container) == 60);

    theme_manager_toggle_dark_mode();
    settle();
    // The theme moved at least one chip's number colour, or this proves nothing.
    REQUIRE(any_changed);
}

TEST_CASE_METHOD(SideListFixture, "A row tap hands the object's name to the tap callback",
                 "[exclude_side_list][pre_start_exclude]") {
    REQUIRE(container != nullptr);
    lv_obj_send_event(rows_of(container)[4], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "obj_4");
}

TEST_CASE_METHOD(SideListFixture, "In toggle mode a picked row reads Excluded and stays tappable",
                 "[exclude_side_list][pre_start_exclude]") {
    objects().set_current_object("");
    open(ExcludeTapMode::Toggle, exclude_side_list_geometry(false));
    REQUIRE(container != nullptr);

    objects().set_excluded_objects({"obj_3"});
    settle();

    const auto rows = rows_of(container);
    CHECK(lv_obj_has_flag(rows[3], LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(rows[3], LV_PART_MAIN) == 150);
    CHECK(shows_text(rows[3], "Excluded"));
    CHECK_FALSE(shows_text(rows[4], "Excluded"));

    lv_obj_send_event(rows[3], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "obj_3");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Each open side list's close button closes only that list",
                 "[exclude_side_list][pre_start_exclude]") {
    PrinterExcludedObjectsState first_state;
    PrinterExcludedObjectsState second_state;
    first_state.init_subjects(false);
    second_state.init_subjects(false);
    first_state.set_defined_objects({"a0", "a1"});
    second_state.set_defined_objects({"b0", "b1"});

    int first_closed = 0;
    int second_closed = 0;
    {
        ExcludeObjectSideList first;
        ExcludeObjectSideList second;
        first.set_close_callback([&] { ++first_closed; });
        second.set_close_callback([&] { ++second_closed; });
        first.create(test_screen(), &first_state, {}, ExcludeTapMode::ExcludeOnly,
                     exclude_side_list_geometry(false));
        second.create(test_screen(), &second_state, {}, ExcludeTapMode::ExcludeOnly,
                      exclude_side_list_geometry(false));
        UpdateQueue::instance().drain();

        lv_obj_send_event(lv_obj_find_by_name(first.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(first_closed == 1);
        CHECK(second_closed == 0);

        lv_obj_send_event(lv_obj_find_by_name(second.root(), "close_btn"), LV_EVENT_CLICKED,
                          nullptr);
        CHECK(second_closed == 1);

        first.destroy();
        second.destroy();
        UpdateQueue::instance().drain();
        process_lvgl(50);
    }
    first_state.deinit_subjects();
    second_state.deinit_subjects();
}

namespace {
/// A 2D viewer drawing one segment for "obj_4", so a highlight has an object to land on.
lv_obj_t* make_viewer_with_obj_4(helix::gcode::GCodeLayerRenderer*& renderer) {
    auto file = std::make_unique<helix::gcode::ParsedGCodeFile>();
    helix::gcode::Layer layer;
    layer.z_height = 0.2f;
    helix::gcode::ToolpathSegment seg;
    seg.start = {10.0f, 10.0f, 0.2f};
    seg.end = {50.0f, 10.0f, 0.2f};
    seg.is_extrusion = true;
    seg.object_name_index = file->intern_object_name("obj_4");
    layer.segments.push_back(seg);
    layer.bounding_box.expand(seg.start);
    layer.bounding_box.expand(seg.end);
    file->global_bounding_box = layer.bounding_box;
    layer.segment_count_extrusion = 1;
    file->layers.push_back(std::move(layer));
    file->total_segments = 1;

    lv_obj_t* viewer = ui_gcode_viewer_create(lv_screen_active());
    lv_obj_set_size(viewer, 200, 200);
    lv_obj_update_layout(viewer);
    ui_gcode_viewer_set_render_mode(viewer, helix::GcodeViewerRenderMode::Layer2D);
    renderer = helix::test_access::gcode_viewer_show_2d(viewer, std::move(file));
    return viewer;
}
} // namespace

TEST_CASE_METHOD(SideListFixture, "Only exclude-only mode highlights a tapped row in the viewer",
                 "[exclude_side_list][pre_start_exclude]") {
    const bool toggle = GENERATE(false, true);
    INFO((toggle ? "toggle" : "exclude-only"));
    open(toggle ? ExcludeTapMode::Toggle : ExcludeTapMode::ExcludeOnly,
         exclude_side_list_geometry(false));
    REQUIRE(container != nullptr);

    helix::gcode::GCodeLayerRenderer* renderer = nullptr;
    lv_obj_t* viewer = make_viewer_with_obj_4(renderer);
    REQUIRE(renderer != nullptr);
    list.set_gcode_viewer(viewer);

    lv_obj_send_event(rows_of(container)[4], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);

    const auto& highlighted =
        helix::gcode::GCodeLayerRendererTestAccess::selection(*renderer).highlighted();
    if (toggle) {
        CHECK(highlighted.empty());
    } else {
        CHECK(highlighted == std::unordered_set<std::string>{"obj_4"});
    }

    list.set_gcode_viewer(nullptr);
    lv_obj_delete(viewer);
}

TEST_CASE_METHOD(SideListFixture, "A destroyed list's rows deliver no taps before they are deleted",
                 "[exclude_side_list][pre_start_exclude]") {
    REQUIRE(container != nullptr);
    lv_obj_t* row = rows_of(container)[2];
    list.destroy();
    // The rows are deleted asynchronously, so this one is still alive here.
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    CHECK(taps.empty());
    settle();
}
