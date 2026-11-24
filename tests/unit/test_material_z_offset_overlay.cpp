// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings > Z Offset by Material, built from the production XML
// (ui_xml/material_z_offset_overlay.xml): each one-time setup step shows until
// its own condition says it is done, and the setup group goes only when every
// step has.

#include "ui_settings_material_z_offset.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "../test_helpers/xml_stand_in_subject.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "material_z_offset_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

struct MaterialSetupFixture : public LVGLUITestFixture {
    MaterialSetupFixture() {
        helix::test::xml_stand_in("firmware_supervised", 0);
        helix::material_zoffset::MaterialZOffsetState::instance().init_subjects();
        // The overlay is a process-wide singleton: its subjects outlive a case.
        if (!lv_xml_get_subject(nullptr, "material_zoffset_step_slicer")) {
            helix::settings::get_material_z_offset_overlay().init_subjects();
        }
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "material_z_offset_overlay", nullptr));
    }

    ~MaterialSetupFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
    }

    /// Status 0 no storage, 2 ready; each step 1 while it is still to do.
    void set(int status, int print_start, int slicer) {
        lv_subject_set_int(subject("material_zoffset_status"), status);
        lv_subject_set_int(subject("material_zoffset_step_print_start"), print_start);
        lv_subject_set_int(subject("material_zoffset_step_slicer"), slicer);
        UpdateQueue::instance().drain();
    }

    bool shown(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root_, name);
        REQUIRE(obj != nullptr);
        return !lv_obj_is_hidden(obj);
    }

    static lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(MaterialSetupFixture, "material Z offset setup: each step shows until it is done",
                 "[material_zoffset][material_setup][pa_mutation]") {
    REQUIRE(root_ != nullptr);

    // Nothing installed: all three steps.
    set(0, 1, 1);
    CHECK(shown("group_setup"));
    CHECK(shown("step_install"));
    CHECK(shown("step_print_start"));
    CHECK(shown("step_slicer"));

    // The macros are in, PRINT_START does not call them yet: the group stays.
    set(2, 1, 1);
    CHECK(shown("group_setup"));
    CHECK_FALSE(shown("step_install"));
    CHECK(shown("step_print_start"));
    CHECK(shown("step_slicer"));

    // PRINT_START calls the helper; the slicer step is still to do.
    set(2, 0, 1);
    CHECK(shown("group_setup"));
    CHECK_FALSE(shown("step_print_start"));
    CHECK(shown("step_slicer"));

    // The owner said the slicer passes the material, PRINT_START does not
    // call the helper yet.
    set(2, 1, 0);
    CHECK(shown("group_setup"));
    CHECK(shown("step_print_start"));
    CHECK_FALSE(shown("step_slicer"));

    // Every step done: the group goes.
    set(2, 0, 0);
    CHECK_FALSE(shown("group_setup"));
}
