// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_empty_lane_label.cpp
 * @brief The ams_slot material label reads the same on first paint and every repaint.
 *
 * The lane label has ONE steady-state rule, shared with the mini-status strip
 * the filament panel embeds (4da7a07db):
 *
 *   present              -> material
 *   ejected + assigned   -> retained material, ghosted at LV_OPA_20
 *   ejected + unassigned -> lv_tr("Empty") at full strength
 *
 * It used to have two implementations racing each other. apply_slot_status()
 * wrote "Empty" for the unassigned-ejected case; apply_slot_material() wrote
 * "--" for any empty material string and knew nothing about status.
 * setup_slot_observers() applies status FIRST and material SECOND, so first
 * paint always ended on "--" — and afterwards the lane flipped between the two
 * depending on which of the two independent subjects had notified last. With
 * the mini status fixed to say "Empty", the AMS panel disagreed both with the
 * strip and with itself.
 *
 * These tests pin first paint for both empty shapes, and pin that a
 * material-subject notification arriving after the status one does not undo it.
 */

#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void register_slot_widget_once() {
    static bool done = false;
    if (done) {
        return;
    }
    ui_spool_canvas_register();
    ui_ams_slot_register();
    done = true;
}

lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index_str = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index_str.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

std::string material_text(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "material_label");
    REQUIRE(label != nullptr);
    return std::string(lv_label_get_text(label));
}

lv_opa_t material_opa(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "material_label");
    REQUIRE(label != nullptr);
    return lv_obj_get_style_text_opa(label, LV_PART_MAIN);
}

/// The dashed "add filament here" circle that replaces the spool on a lane with
/// no identity. Created in both the flat and 3D spool branches, so unlike
/// spool_outer/canvas it is a style-independent handle on the spool visual.
bool placeholder_visible(lv_obj_t* slot) {
    lv_obj_t* ph = lv_obj_find_by_name(slot, "empty_placeholder");
    REQUIRE(ph != nullptr);
    return !lv_obj_has_flag(ph, LV_OBJ_FLAG_HIDDEN);
}

/// Strip every identity handle off a lane so it reads as "never assigned".
///
/// Status goes through force_slot_status(), not set_slot_info(): status is
/// firmware-derived, so no backend's set_slot_info() accepts it (the mock now
/// warns when a caller tries).
void make_unassigned_empty(AmsBackendMock& mock, int slot_index) {
    SlotInfo info = mock.get_slot_info(slot_index);
    info.material.clear();
    info.brand.clear();
    info.spool_name.clear();
    info.color_name.clear();
    info.spoolman_id = 0;
    REQUIRE(mock.set_slot_info(slot_index, info).success());
    mock.force_slot_status(slot_index, SlotStatus::EMPTY);
}

/// Eject a lane but leave its identity intact — the #1071 "assigned, not
/// present" shape the override deliberately survives.
void make_assigned_ejected(AmsBackendMock& mock, int slot_index, const std::string& material) {
    SlotInfo info = mock.get_slot_info(slot_index);
    info.material = material;
    REQUIRE(mock.set_slot_info(slot_index, info).success());
    mock.force_slot_status(slot_index, SlotStatus::EMPTY);
}

/// Installs a mock backend into the AmsState singleton and takes it back out,
/// so a failed REQUIRE cannot leak a backend into the next test in the binary.
struct ScopedMockBackend {
    explicit ScopedMockBackend(int slot_count) {
        auto owned = std::make_unique<AmsBackendMock>(slot_count);
        owned->set_operation_delay(0);
        mock = owned.get();
        AmsState::instance().set_backend(std::move(owned));
    }
    ~ScopedMockBackend() {
        AmsState::instance().clear_backends();
    }
    ScopedMockBackend(const ScopedMockBackend&) = delete;
    ScopedMockBackend& operator=(const ScopedMockBackend&) = delete;

    AmsBackendMock* mock = nullptr;
};

/// Deletes a widget at scope exit, ahead of ScopedMockBackend's teardown, so
/// the slot's observers are gone before the subjects they watch.
struct ScopedWidget {
    explicit ScopedWidget(lv_obj_t* o) : obj(o) {}
    ~ScopedWidget() {
        if (obj) {
            lv_obj_delete(obj);
        }
    }
    ScopedWidget(const ScopedWidget&) = delete;
    ScopedWidget& operator=(const ScopedWidget&) = delete;

    lv_obj_t* obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ams_slot: unassigned empty lane reads Empty on first paint",
                 "[ams][slot]") {
    ScopedMockBackend backend(4);
    make_unassigned_empty(*backend.mock, 3);

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 3));
    REQUIRE(slot.obj != nullptr);

    // FIRST PAINT, before any further tick: setup_slot_observers() applies
    // status then material, and the label must already read "Empty" rather than
    // the "--" the material pass used to leave behind.
    CHECK(material_text(slot.obj) == std::string(lv_tr("Empty")));
    // Full strength — an unassigned lane is not a ghost, it is a named blank.
    CHECK(material_opa(slot.obj) == LV_OPA_COVER);
    // ...and the spool visual is replaced by the dashed "add filament" circle,
    // the other half of the same rule.
    CHECK(placeholder_visible(slot.obj));
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot: assigned ejected lane keeps its material, ghosted",
                 "[ams][slot]") {
    ScopedMockBackend backend(4);
    make_assigned_ejected(*backend.mock, 2, "PETG");

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 2));
    REQUIRE(slot.obj != nullptr);

    CHECK(material_text(slot.obj) == "PETG");
    CHECK(material_opa(slot.obj) == LV_OPA_20);
    // A retained identity ghosts the real spool rather than swapping in the
    // placeholder — the inverse of the unassigned case above.
    CHECK_FALSE(placeholder_visible(slot.obj));
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot: present lane shows its material at full strength",
                 "[ams][slot]") {
    // Control case: the rule must not have turned every lane into "Empty".
    ScopedMockBackend backend(4);
    {
        SlotInfo info = backend.mock->get_slot_info(0);
        info.material = "PLA";
        REQUIRE(backend.mock->set_slot_info(0, info).success());
        backend.mock->force_slot_status(0, SlotStatus::AVAILABLE);
    }

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 0));
    REQUIRE(slot.obj != nullptr);

    CHECK(material_text(slot.obj) == "PLA");
    CHECK(material_opa(slot.obj) == LV_OPA_COVER);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot: a later material notify does not undo Empty",
                 "[ams][slot]") {
    // The repaint half of the bug. Status and material are separate subjects
    // with separate observers; whichever notified last used to decide the text.
    // Drive the material one AFTER the lane is already painted "Empty" — the
    // ordering that produced "--" — and the label must not move.
    ScopedMockBackend backend(4);
    make_unassigned_empty(*backend.mock, 3);

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 3));
    REQUIRE(slot.obj != nullptr);
    REQUIRE(material_text(slot.obj) == std::string(lv_tr("Empty")));

    lv_subject_t* material_subject = AmsState::instance().get_slot_material_subject(3);
    REQUIRE(material_subject != nullptr);
    // Round-trip through a non-empty value so the subject genuinely notifies
    // twice; the final state is the same empty material the lane started with.
    lv_subject_copy_string(material_subject, "PLA");
    process_lvgl(20);
    lv_subject_copy_string(material_subject, "");
    process_lvgl(20);

    CHECK(material_text(slot.obj) == std::string(lv_tr("Empty")));
}
