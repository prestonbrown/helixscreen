// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_presentation_wiring.cpp
 * @brief Each consumer of the empty-lane rule actually applies its four outputs.
 *
 * The rule itself is pure and covered exhaustively in
 * test_ams_slot_presentation.cpp. What is left is per-consumer wiring: that
 * resolve_slot_presentation()'s spool_opa / show_spool / show_placeholder /
 * label reach the right widgets on the right surface, and keep reaching them
 * when only ONE of the two subjects that feed the rule has moved.
 *
 * That last case is why these tests exist. An identity-only change — assigning
 * a spool to an already-EMPTY lane — moves the material subject but NOT the
 * status subject, so apply_slot_status() does not re-run on its own and the
 * spool stays hidden behind the unassigned-empty placeholder while the label
 * reads the new material. The fix (97106fc51) re-derives the spool presentation
 * from the material observer; it shipped on hardware evidence because three
 * attempts to test it through the subject plumbing were all vacuous.
 *
 * The reason they were vacuous, and what this file does instead:
 * AmsBackendMock::set_slot_info() emits EVENT_SLOT_CHANGED, which queues a full
 * AmsState::sync_from_backend() and republishes EVERY per-slot subject. That
 * hands the widget a complete, consistent repaint and closes the one-subject-
 * moved window before any assertion runs. QuietIdentityMock below moves the
 * identity by overriding get_slot_info() instead, which emits nothing at all,
 * so the window stays open for exactly the notification under test.
 */

#include "ui_ams_mini_status.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_slot_presentation.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <map>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/**
 * @brief A mock whose lane identity can change with NO event of any kind.
 *
 * set_slot_info() is the trap: it emits EVENT_SLOT_CHANGED, AmsState queues a
 * full sync_from_backend(), and every per-slot subject is republished. The
 * widget then repaints from a complete picture and the partial-update window
 * this file is about never exists. Overriding the read instead moves the
 * identity with no notification, so the test controls precisely which subject
 * fires.
 */
class QuietIdentityMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override {
        SlotInfo info = AmsBackendMock::get_slot_info(slot_index);
        auto it = quiet_material_.find(slot_index);
        if (it != quiet_material_.end()) {
            info.material = it->second;
        }
        return info;
    }

    /// Give a lane a retained material without notifying anything.
    void set_quiet_material(int slot_index, std::string material) {
        quiet_material_[slot_index] = std::move(material);
    }

  private:
    std::map<int, std::string> quiet_material_;
};

/// Installs a backend into AmsState and takes it back out, so a failed REQUIRE
/// cannot leak one into the next test in the binary.
struct ScopedQuietBackend {
    explicit ScopedQuietBackend(int slot_count) {
        auto owned = std::make_unique<QuietIdentityMock>(slot_count);
        owned->set_operation_delay(0);
        mock = owned.get();
        AmsState::instance().set_backend(std::move(owned));
    }
    ~ScopedQuietBackend() {
        AmsState::instance().clear_backends();
    }
    ScopedQuietBackend(const ScopedQuietBackend&) = delete;
    ScopedQuietBackend& operator=(const ScopedQuietBackend&) = delete;

    QuietIdentityMock* mock = nullptr;
};

/// Deletes a widget at scope exit, ahead of the backend teardown, so the slot's
/// observers are gone before the subjects they watch.
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

bool placeholder_visible(lv_obj_t* slot) {
    lv_obj_t* ph = lv_obj_find_by_name(slot, "empty_placeholder");
    REQUIRE(ph != nullptr);
    return !lv_obj_has_flag(ph, LV_OBJ_FLAG_HIDDEN);
}

/// The spool body itself. Complementary to the placeholder — the other half of
/// show_spool/show_placeholder, asserted separately so a wiring that moved only
/// one of them cannot pass. "spool_graphic" is the style-independent name: the 3D
/// branch puts it on the canvas, the flat branch on the filament ring.
bool spool_body_visible(lv_obj_t* slot) {
    lv_obj_t* body = lv_obj_find_by_name(slot, "spool_graphic");
    REQUIRE(body != nullptr);
    return !lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN);
}

/// Strip every identity handle off a lane so it reads as "never assigned", then
/// eject it. Status is firmware-derived, so it goes through force_slot_status()
/// rather than set_slot_info().
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

} // namespace

// ============================================================================
// Consumer 1: the ams_slot widget
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_slot wiring: gaining identity on an EMPTY lane un-hides the spool",
                 "[ams][slot][presentation][wiring]") {
    // THE regression this file exists for (97106fc51). The lane is already
    // EMPTY and unassigned; only the material subject moves. Without the
    // re-derive in the material observer, apply_slot_status() never re-runs:
    // the label picks up "PLA" but the spool stays hidden behind the
    // placeholder at full opacity — a lane that claims a material and shows
    // nothing.
    ScopedQuietBackend backend(4);
    make_unassigned_empty(*backend.mock, 3);

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 3));
    REQUIRE(slot.obj != nullptr);

    // Drain first. force_slot_status() above emitted EVENT_SLOT_CHANGED, which
    // queued a full sync_from_backend(); if that is still in flight when the
    // window below opens, it republishes the status subject, apply_slot_status()
    // re-runs on its own, and the lane is repaired by something other than the
    // code under test. That is exactly how three earlier attempts at this test
    // came out vacuous. Settle it here so the only thing that moves afterwards
    // is the one subject this test drives.
    process_lvgl(20);

    // Baseline: unassigned + empty -> no spool, placeholder, "Empty".
    REQUIRE(material_text(slot.obj) == std::string(lv_tr("Empty")));
    REQUIRE(placeholder_visible(slot.obj));
    REQUIRE_FALSE(spool_body_visible(slot.obj));

    // Assign a spool to the ejected lane. get_slot_info() now reports the
    // material, and NOTHING has been notified — no EVENT_SLOT_CHANGED, no
    // sync_from_backend(), no status republish.
    backend.mock->set_quiet_material(3, "PLA");

    // Move ONLY the material subject. This is the whole window.
    lv_subject_t* material_subject = AmsState::instance().get_slot_material_subject(3);
    REQUIRE(material_subject != nullptr);
    lv_subject_copy_string(material_subject, "PLA");
    process_lvgl(20);

    // The lane is now "assigned, not present": spool KEPT and ghosted, dashed
    // placeholder gone, label showing the retained material.
    CHECK(material_text(slot.obj) == "PLA");
    CHECK(spool_body_visible(slot.obj));
    CHECK_FALSE(placeholder_visible(slot.obj));
    CHECK(material_opa(slot.obj) == helix::ui::SPOOL_OPA_GHOST);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_slot wiring: losing identity on an EMPTY lane restores the placeholder",
                 "[ams][slot][presentation][wiring]") {
    // The inverse window, so the re-derive is not a one-way ratchet that only
    // ever un-hides. Unassigning an already-ejected lane must put the
    // placeholder back and return the label to "Empty".
    ScopedQuietBackend backend(4);
    make_unassigned_empty(*backend.mock, 1);
    backend.mock->set_quiet_material(1, "ABS");

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    register_slot_widget_once();

    ScopedWidget slot(create_ams_slot(test_screen(), 1));
    REQUIRE(slot.obj != nullptr);

    lv_subject_t* material_subject = AmsState::instance().get_slot_material_subject(1);
    REQUIRE(material_subject != nullptr);
    lv_subject_copy_string(material_subject, "ABS");
    process_lvgl(20);
    REQUIRE(spool_body_visible(slot.obj));
    REQUIRE(material_opa(slot.obj) == helix::ui::SPOOL_OPA_GHOST);

    // Drop the identity, quietly, then move only the material subject.
    backend.mock->set_quiet_material(1, "");
    lv_subject_copy_string(material_subject, "");
    process_lvgl(20);

    CHECK(material_text(slot.obj) == std::string(lv_tr("Empty")));
    CHECK(placeholder_visible(slot.obj));
    CHECK_FALSE(spool_body_visible(slot.obj));
    // Nothing is dimmed here — the spool is gone, not faded.
    CHECK(material_opa(slot.obj) == helix::ui::SPOOL_OPA_FULL);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot wiring: a present lane is never ghosted",
                 "[ams][slot][presentation][wiring]") {
    // Control. If the wiring started reading the rule with the wrong status,
    // every lane would ghost; this is the case that catches it.
    ScopedQuietBackend backend(4);
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
    CHECK(material_opa(slot.obj) == helix::ui::SPOOL_OPA_FULL);
    CHECK(spool_body_visible(slot.obj));
    CHECK_FALSE(placeholder_visible(slot.obj));
}

// ============================================================================
// Consumer 2: the ams_mini_status spool strip
// ============================================================================

namespace {

/// The material label of spool cell @p index in the strip's spool view.
lv_obj_t* spool_cell_material(lv_obj_t* widget, int index) {
    const std::string name = "spool_material_" + std::to_string(index);
    return UITest::find_by_name(widget, name.c_str());
}

/// The dashed placeholder belonging to spool cell @p index. Every cell names
/// its placeholder "empty_placeholder", so walk up from the cell's material
/// label to that cell rather than searching the whole strip.
lv_obj_t* spool_cell_placeholder(lv_obj_t* widget, int index) {
    lv_obj_t* mat = spool_cell_material(widget, index);
    REQUIRE(mat != nullptr);
    lv_obj_t* col = lv_obj_get_parent(mat); // text column
    REQUIRE(col != nullptr);
    lv_obj_t* cell = lv_obj_get_parent(col); // the cell itself
    REQUIRE(cell != nullptr);
    return lv_obj_find_by_name(cell, "empty_placeholder");
}

/// Build the strip wide enough to render the spool view, seeded from AmsState.
lv_obj_t* create_spool_strip(lv_obj_t* parent) {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(parent, 40);
    REQUIRE(w != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_width(w, 260);
    helix::ui::UpdateQueue::instance().drain();
    return w;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "mini_status wiring: an UNKNOWN lane is not labelled Empty",
                 "[ams][mini][presentation][wiring][unknown]") {
    // The divergence the extraction found. The strip used to key the empty
    // presentation on SlotInfo::is_present(), which is false for UNKNOWN as
    // well as EMPTY — so a backend that publishes no per-lane presence had
    // every lane drawn as a dashed "Empty" placeholder. That is the same
    // collapse slot_presence() in filament_op_slot_resolver.h exists to avoid.
    // QIDI, Snapmaker, AFC, Happy Hare and ACE all publish UNKNOWN, and
    // AmsState inits every per-slot status subject to it.
    ScopedQuietBackend backend(2);
    {
        SlotInfo info = backend.mock->get_slot_info(0);
        info.material = "PLA";
        REQUIRE(backend.mock->set_slot_info(0, info).success());
        backend.mock->force_slot_status(0, SlotStatus::UNKNOWN);
    }
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    ScopedWidget strip(create_spool_strip(test_screen()));
    lv_obj_t* mat = spool_cell_material(strip.obj, 0);
    REQUIRE(mat != nullptr);

    // "Unanswered" renders as a normal lane, not as a named blank.
    CHECK(std::string(lv_label_get_text(mat)) != std::string(lv_tr("Empty")));
    CHECK(lv_obj_get_style_text_opa(mat, LV_PART_MAIN) == helix::ui::SPOOL_OPA_FULL);

    lv_obj_t* ph = spool_cell_placeholder(strip.obj, 0);
    if (ph != nullptr) {
        CHECK(lv_obj_has_flag(ph, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "mini_status wiring: an ejected assigned lane ghosts its retained material",
                 "[ams][mini][presentation][wiring]") {
    ScopedQuietBackend backend(2);
    {
        SlotInfo info = backend.mock->get_slot_info(0);
        info.material = "PETG";
        REQUIRE(backend.mock->set_slot_info(0, info).success());
        backend.mock->force_slot_status(0, SlotStatus::EMPTY);
    }
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    ScopedWidget strip(create_spool_strip(test_screen()));
    lv_obj_t* mat = spool_cell_material(strip.obj, 0);
    REQUIRE(mat != nullptr);

    // Retained material, dimmed to read as "assigned, not present".
    CHECK(std::string(lv_label_get_text(mat)) == "PETG");
    CHECK(lv_obj_get_style_text_opa(mat, LV_PART_MAIN) == helix::ui::SPOOL_OPA_GHOST);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "mini_status wiring: an ejected unassigned lane reads Empty at full strength",
                 "[ams][mini][presentation][wiring]") {
    ScopedQuietBackend backend(2);
    make_unassigned_empty(*backend.mock, 0);
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    ScopedWidget strip(create_spool_strip(test_screen()));
    lv_obj_t* mat = spool_cell_material(strip.obj, 0);
    REQUIRE(mat != nullptr);

    CHECK(std::string(lv_label_get_text(mat)) == std::string(lv_tr("Empty")));
    // Not a ghost — a named blank.
    CHECK(lv_obj_get_style_text_opa(mat, LV_PART_MAIN) == helix::ui::SPOOL_OPA_FULL);

    lv_obj_t* ph = spool_cell_placeholder(strip.obj, 0);
    REQUIRE(ph != nullptr);
    CHECK_FALSE(lv_obj_has_flag(ph, LV_OBJ_FLAG_HIDDEN));
}
