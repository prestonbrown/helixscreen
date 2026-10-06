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
#include "ui_print_exclude_object_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <algorithm>
#include <cstring>
#include <string>
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
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
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
    SideListFixture() : manager(nullptr, state(), nullptr) {
        objects().set_defined_objects(object_names(20));
        objects().set_current_object("obj_0");
        list.create(test_screen(), &state(), &manager, exclude_side_list_geometry(false));
        settle();
        container = lv_obj_find_by_name(list.root(), "rows_container");
    }

    ~SideListFixture() override {
        list.destroy();
        objects().clear_objects();
        settle();
    }

    PrinterExcludedObjectsState& objects() {
        return *state().get_excluded_objects_state();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(400);
    }

    PrintExcludeObjectManager manager;
    ExcludeObjectSideList list;
    lv_obj_t* container = nullptr;
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
    CHECK(shows_text(before[5], "Printing"));
    CHECK_FALSE(shows_text(before[0], "Printing"));
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
