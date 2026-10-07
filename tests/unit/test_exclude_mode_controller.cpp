// SPDX-License-Identifier: GPL-3.0-or-later

// Exclude mode is one controller owned by both print status and print
// details: the object list over the host's columns, the top-down map over the
// card in thumbnail mode, numbered badges on the render otherwise, and every
// tap routed to the host's callback.

#include "ui_exclude_mode_controller.h"
#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_excluded_objects_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

class ExcludeModeFixture : public LVGLUITestFixture {
  public:
    ExcludeModeFixture() {
        objects.init_subjects(false);
        objects.set_defined_objects({"A", "B", "C"});
        lv_subject_init_int(&map_active, 0);

        columns = lv_obj_create(test_screen());
        lv_obj_set_size(columns, 800, 400);
        lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
        card = lv_obj_create(columns);
        lv_obj_set_size(card, 400, 380);
        controls = lv_obj_create(columns);
        lv_obj_set_size(controls, 300, 380);
        lv_obj_set_name(controls, "controls_section");

        targets.card = card;
        targets.columns = columns;
        targets.controls_name = "controls_section";
        targets.map_active = &map_active;
        targets.thumbnail_mode = true;
        targets.bed_w_mm = 235.0f;
        targets.bed_h_mm = 235.0f;
    }

    ~ExcludeModeFixture() override {
        controller.hide();
        settle();
        objects.deinit_subjects();
        lv_subject_deinit(&map_active);
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(50);
    }

    void show(ExcludeTapMode mode) {
        controller.show(targets, &objects, mode,
                        [this](const std::string& name) { taps.push_back(name); });
        settle();
    }

    std::vector<lv_obj_t*> rows() const {
        std::vector<lv_obj_t*> out;
        lv_obj_t* container = lv_obj_find_by_name(columns, "rows_container");
        if (!container) {
            return out;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
            out.push_back(lv_obj_get_child(container, static_cast<int32_t>(i)));
        }
        return out;
    }

    PrinterExcludedObjectsState objects;
    lv_subject_t map_active{};
    lv_obj_t* columns = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* controls = nullptr;
    ExcludeModeTargets targets;
    ExcludeModeController controller;
    std::vector<std::string> taps;
};

/// Create widgets with @p make until one lands at @p freed, the address LVGL
/// just released, so a stale pointer to the deleted widget would hit it.
/// Returns nullptr when the allocator never hands it back.
template <typename Make> lv_obj_t* allocate_at(lv_obj_t* freed, Make make) {
    for (int i = 0; i < 64; ++i) {
        lv_obj_t* obj = make();
        if (obj == freed) {
            return obj;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Thumbnail mode lays the map over the card and the list over the columns",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::ExcludeOnly);
    CHECK(controller.is_open());
    CHECK(lv_subject_get_int(&map_active) == 1);
    CHECK(lv_obj_find_by_name(card, "plate_area") != nullptr);
    CHECK(rows().size() == 3);

    controller.hide();
    settle();
    CHECK_FALSE(controller.is_open());
    CHECK(lv_subject_get_int(&map_active) == 0);
    CHECK(lv_obj_find_by_name(columns, "rows_container") == nullptr);
    CHECK(lv_obj_find_by_name(card, "plate_area") == nullptr);
}

TEST_CASE_METHOD(ExcludeModeFixture, "In toggle mode a picked row still reaches the host",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::Toggle);
    objects.set_excluded_objects({"B"});
    settle();
    const auto r = rows();
    REQUIRE(r.size() == 3);
    CHECK(lv_obj_has_flag(r[1], LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(r[1], LV_EVENT_CLICKED, nullptr);
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "B");
}

TEST_CASE_METHOD(ExcludeModeFixture, "In exclude-only mode an excluded row takes no tap",
                 "[exclude_mode][pre_start_exclude]") {
    show(ExcludeTapMode::ExcludeOnly);
    objects.set_excluded_objects({"B"});
    settle();
    const auto r = rows();
    REQUIRE(r.size() == 3);
    CHECK_FALSE(lv_obj_has_flag(r[1], LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Render mode routes viewer taps and badges while open, and drops them on hide",
                 "[exclude_mode][pre_start_exclude]") {
    lv_obj_t* viewer = ui_gcode_viewer_create(card);
    REQUIRE(viewer != nullptr);
    targets.gcode_viewer = viewer;
    targets.thumbnail_mode = false;

    show(ExcludeTapMode::Toggle);
    CHECK(lv_obj_find_by_name(card, "plate_area") == nullptr); // no map over a render
    CHECK(helix::test_access::gcode_viewer_excluded_badges_pickable(viewer));
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer).size() == 3);
    helix::test_access::gcode_viewer_fire_object_tap(viewer, "C");
    REQUIRE(taps.size() == 1);
    CHECK(taps[0] == "C");

    objects.set_excluded_objects({"A"});
    settle();
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer)[0].excluded);

    controller.hide();
    settle();
    CHECK_FALSE(helix::test_access::gcode_viewer_excluded_badges_pickable(viewer));
    CHECK(helix::test_access::gcode_viewer_object_badges(viewer).empty());
    helix::test_access::gcode_viewer_fire_object_tap(viewer, "A");
    CHECK(taps.size() == 1);
}

// A host's tree can be deleted under an open exclude mode (an overlay whose
// screen went away). LVGL may hand the freed address to the next widget, so
// teardown must recognise the deletion, not test the address.

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Hiding after the viewer was deleted leaves a widget at its address untouched",
                 "[exclude_mode][pre_start_exclude]") {
    lv_obj_t* viewer = ui_gcode_viewer_create(card);
    REQUIRE(viewer != nullptr);
    targets.gcode_viewer = viewer;
    targets.thumbnail_mode = false;
    show(ExcludeTapMode::Toggle);
    REQUIRE(controller.is_open());

    lv_obj_delete(viewer);
    // A plain widget reaches the freed address first; freeing it again leaves
    // that address next in line for the new viewer's object.
    lv_obj_t* probe = allocate_at(viewer, [this] { return lv_obj_create(card); });
    if (!probe) {
        SKIP("allocator never reused the deleted viewer's address");
    }
    lv_obj_delete(probe);
    lv_obj_t* fresh = ui_gcode_viewer_create(card);
    if (fresh != viewer) {
        SKIP("the new viewer did not land at the deleted viewer's address");
    }
    ui_gcode_viewer_set_excluded_badges_pickable(fresh, true);
    ui_gcode_viewer_set_object_badges(fresh, compute_object_badges(objects, nullptr));
    REQUIRE(helix::test_access::gcode_viewer_object_badges(fresh).size() == 3);

    controller.hide();
    settle();
    CHECK(helix::test_access::gcode_viewer_excluded_badges_pickable(fresh));
    CHECK(helix::test_access::gcode_viewer_object_badges(fresh).size() == 3);
}

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Destroying a side list whose root was deleted leaves a widget at its address",
                 "[exclude_side_list][pre_start_exclude]") {
    ExcludeObjectSideList list;
    list.create(columns, &objects, nullptr, ExcludeTapMode::ExcludeOnly,
                exclude_side_list_geometry(false));
    lv_obj_t* root = list.root();
    REQUIRE(root != nullptr);

    lv_obj_delete(root);
    CHECK_FALSE(list.is_active());
    lv_obj_t* fresh = allocate_at(root, [this] { return lv_obj_create(columns); });
    if (!fresh) {
        SKIP("allocator never reused the deleted root's address");
    }

    list.destroy();
    settle();
    CHECK(lv_obj_is_valid(fresh));
    objects.set_excluded_objects({"A"}); // no observer may reach the deleted rows
    settle();
}

TEST_CASE_METHOD(ExcludeModeFixture,
                 "Destroying a map view whose root was deleted leaves a widget at its address",
                 "[exclude_map][pre_start_exclude]") {
    ExcludeObjectMapView map;
    map.create(card, &objects, 235.0f, 235.0f, nullptr, ExcludeTapMode::ExcludeOnly);
    lv_obj_t* root = map.root();
    REQUIRE(root != nullptr);

    lv_obj_delete(root);
    CHECK_FALSE(map.is_active());
    lv_obj_t* fresh = allocate_at(root, [this] { return lv_obj_create(card); });
    if (!fresh) {
        SKIP("allocator never reused the deleted root's address");
    }

    map.destroy();
    settle();
    CHECK(lv_obj_is_valid(fresh));
    objects.set_defined_objects({"A", "B"}); // no observer may reach the deleted rects
    settle();
}
