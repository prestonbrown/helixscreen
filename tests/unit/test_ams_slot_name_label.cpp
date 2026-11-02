// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_name_label.cpp
 * @brief The AMS slot card names its spool above its material.
 *
 * Three layers:
 *
 * 1. slot_card_name() / slot_card_lines(): the pure decision of what the card
 *    reads, with no LVGL.
 * 2. AmsState's per-slot name subject: written wherever the material subject is,
 *    for the primary and secondary backends, bounded on a UTF-8 boundary.
 * 3. The ams_slot widget: name_label over material_label, repainting from the
 *    subjects alone.
 */

#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_lane_state.h"
#include "ams_state.h"
#include "filament_display_name.h"
#include "lvgl.h"
#include "utf8_validation.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendMock;
using helix::AmsState;
using helix::SlotCardLines;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::SpoolIdentity;

namespace {

using helix::text_io::is_valid_utf8;

lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index_str = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index_str.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

lv_obj_t* name_label(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "name_label");
    REQUIRE(label != nullptr);
    return label;
}

lv_obj_t* material_label(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "material_label");
    REQUIRE(label != nullptr);
    return label;
}

std::string text_of(lv_obj_t* label) {
    return std::string(lv_label_get_text(label));
}

bool shown(lv_obj_t* obj) {
    return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Seed slot @p i's name/material/lane_state subjects as one sync would.
void seed_slot(int i, const char* name, const char* material,
               helix::ui::LaneState state = helix::ui::LaneState::Present) {
    auto& ams = AmsState::instance();
    lv_subject_t* name_subject = ams.get_slot_name_subject(i);
    REQUIRE(name_subject != nullptr);
    lv_subject_copy_string(name_subject, name);
    lv_subject_copy_string(ams.get_slot_material_subject(i), material);
    lv_subject_set_int(ams.get_slot_lane_state_subject(i), static_cast<int>(state));
}

SlotInfo named_slot(const char* spool_name, const char* color_name, const char* material) {
    SlotInfo slot;
    slot.spool_name = spool_name;
    slot.color_name = color_name;
    slot.material = material;
    return slot;
}

} // namespace

// ============================================================================
// 1. The pure decision
// ============================================================================

TEST_CASE("slot_card_name: spool name, then Spoolman name, then product name",
          "[ams][slot_name][pure]") {
    SpoolIdentity identity;
    identity.filament_name = "Ambrosia Pink";
    identity.material = "PLA";

    SlotInfo slot = named_slot("Bone White", "White", "PLA");
    slot.product_name = "PLA+ 2.0";
    CHECK(helix::slot_card_name(slot, &identity) == "Bone White");
    slot.spool_name.clear();
    CHECK(helix::slot_card_name(slot, &identity) == "Ambrosia Pink");
    CHECK(helix::slot_card_name(slot, nullptr) == "PLA+ 2.0");
}

TEST_CASE("slot_card_name: a colour is never the card's name", "[ams][slot_name][pure]") {
    // The spool graphic under the label already shows the colour, so neither a
    // stored colour name nor one computed from the hex reaches the card.
    SlotInfo slot = named_slot("", "Medium Vibrant Teal", "PLA");
    slot.color_rgb = 0x00BCD4;
    CHECK(helix::slot_card_name(slot, nullptr).empty());

    // An identity with nothing a label can use is a miss, not a name.
    SpoolIdentity ids_only;
    ids_only.filament_id = 7;
    ids_only.color_hex = "#00BCD4";
    CHECK(helix::slot_card_name(slot, &ids_only).empty());
}

TEST_CASE("slot_card_lines: a product name that states the material keeps the material line",
          "[ams][slot_name][pure]") {
    const SlotCardLines lines = helix::slot_card_lines("PLA+ 2.0", "PLA", "PLA", 4, false);
    CHECK(lines.name == "PLA+ 2.0");
    CHECK(lines.lower == "PLA");
}

TEST_CASE("slot_card_lines: the name sits over the material", "[ams][slot_name][pure]") {
    const SlotCardLines lines = helix::slot_card_lines("Bone White", "PLA+", "PLA+", 4, false);
    CHECK(lines.name == "Bone White");
    CHECK(lines.lower == "PLA+");
}

TEST_CASE("slot_card_lines: no name reads exactly as the material-only label",
          "[ams][slot_name][pure]") {
    const SlotCardLines lines = helix::slot_card_lines("", "PLA", "PLA", 4, false);
    CHECK(lines.name.empty());
    CHECK(lines.lower == "PLA");

    // A lane present with no material keeps its placeholder.
    const SlotCardLines none = helix::slot_card_lines("", "", "--", 2, false);
    CHECK(none.name.empty());
    CHECK(none.lower == "--");
}

TEST_CASE("slot_card_lines: a name that states the material still sits over it",
          "[ams][slot_name][pure]") {
    // Names as an AFC lane reports them; the card is too narrow for the name to
    // stand in for the material.
    const SlotCardLines lines = helix::slot_card_lines("Overture Gray ASA", "ASA", "ASA", 4, false);
    CHECK(lines.name == "Overture Gray ASA");
    CHECK(lines.lower == "ASA");

    const SlotCardLines plus =
        helix::slot_card_lines("Fire Engine Red ABS+", "ABS", "ABS", 4, false);
    CHECK(plus.name == "Fire Engine Red ABS+");
    CHECK(plus.lower == "ABS");

    // A material that only shares a prefix with a word is still shown.
    const SlotCardLines distinct = helix::slot_card_lines("Gray ASA", "PLA", "PLA", 4, false);
    CHECK(distinct.name == "Gray ASA");
    CHECK(distinct.lower == "PLA");
}

TEST_CASE("slot_card_lines: a named lane with no material shows the name alone",
          "[ams][slot_name][pure]") {
    const SlotCardLines lines = helix::slot_card_lines("Bone White", "", "--", 4, false);
    CHECK(lines.name.empty());
    CHECK(lines.lower == "Bone White");
}

TEST_CASE("slot_card_lines: five or more slots keep the truncated material line",
          "[ams][slot_name][pure]") {
    const SlotCardLines crowded =
        helix::slot_card_lines("Bone White", "PETG-CF", "PETG-CF", 5, false);
    CHECK(crowded.name.empty());
    CHECK(crowded.lower == "PETG");

    // A name that states its material does not sneak in either.
    CHECK(helix::slot_card_lines("PLA Bone", "PLA", "PLA", 8, false).lower == "PLA");

    // Four is still roomy.
    CHECK(helix::slot_card_lines("Bone White", "PETG-CF", "PETG-CF", 4, false).lower == "PETG-CF");
}

TEST_CASE("slot_card_lines: an Empty lane reads its placeholder only", "[ams][slot_name][pure]") {
    const SlotCardLines lines = helix::slot_card_lines("Bone White", "PLA", "Empty", 2, true);
    CHECK(lines.name.empty());
    CHECK(lines.lower == "Empty");

    // The crowded-row cut never applies to the placeholder.
    CHECK(helix::slot_card_lines("", "", "Leer gelassen", 8, true).lower == "Leer gelassen");
}

TEST_CASE("slot_card_lines: a multibyte name is passed through whole", "[ams][slot_name][pure]") {
    const std::string cjk = "\xE8\xB1\xA1\xE7\x89\x99\xE7\x99\xBD\xE8\x89\xB2"; // 象牙白色
    const SlotCardLines lines = helix::slot_card_lines(cjk, "PLA", "PLA", 4, false);
    CHECK(lines.name == cjk);
    CHECK(is_valid_utf8(lines.name));

    const std::string accented = "Cr\xC3\xA8me Br\xC3\xBBl\xC3\xA9"
                                 "e Silk Matte Extra Long Name";
    CHECK(helix::slot_card_lines(accented, "PLA", "PLA", 4, false).name == accented);
}

// ============================================================================
// 2. AmsState's per-slot name subject
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: the slot name subject follows the backend",
                 "[ams][slot_name][state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* mock = owned.get();
    ams.set_backend(std::move(owned));

    SlotInfo info = mock->get_slot_info(0);
    info.spool_name = "Bone White";
    info.material = "PLA";
    REQUIRE(helix::test::apply_edit(*mock, 0, info).success());
    ams.sync_from_backend();
    CHECK(std::string(lv_subject_get_string(ams.get_slot_name_subject(0))) == "Bone White");
    CHECK(std::string(lv_subject_get_string(ams.get_slot_name_subject(0, 0))) == "Bone White");

    // A second PLA spool in the same lane: only the name moves.
    const int version = lv_subject_get_int(ams.get_slots_version_subject());
    info = mock->get_slot_info(0);
    info.spool_name = "Jet Black";
    REQUIRE(helix::test::apply_edit(*mock, 0, info).success());
    ams.sync_from_backend();
    CHECK(std::string(lv_subject_get_string(ams.get_slot_name_subject(0))) == "Jet Black");
    CHECK(lv_subject_get_int(ams.get_slots_version_subject()) > version);

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: a slot past the backend's count has no name",
                 "[ams][slot_name][state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    lv_subject_copy_string(ams.get_slot_name_subject(5), "Stale Spool");
    ams.set_backend(std::make_unique<AmsBackendMock>(4));
    ams.sync_from_backend();

    CHECK(std::string(lv_subject_get_string(ams.get_slot_name_subject(5))).empty());
    CHECK(ams.get_slot_name_subject(AmsState::kMaxSlots) == nullptr);
    CHECK(ams.get_slot_name_subject(-1) == nullptr);

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: a secondary backend publishes its own slot names",
                 "[ams][slot_name][state][multi_backend]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ams.set_backend(std::make_unique<AmsBackendMock>(4));
    auto owned = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* sec_mock = owned.get();
    const int second = ams.add_backend(std::move(owned));
    REQUIRE(second == 1);

    SlotInfo info = sec_mock->get_slot_info(1);
    info.spool_name = "Galaxy Black";
    REQUIRE(helix::test::apply_edit(*sec_mock, 1, info).success());
    ams.sync_backend(second);

    SubjectLifetime lt;
    lv_subject_t* sec_name = ams.get_slot_name_subject(second, 1, lt);
    REQUIRE(sec_name != nullptr);
    CHECK(lt != nullptr);
    CHECK(std::string(lv_subject_get_string(sec_name)) == "Galaxy Black");
    CHECK(ams.get_slot_name_subject(second, 4, lt) == nullptr);

    // A single-slot update reaches it too.
    info = sec_mock->get_slot_info(1);
    info.spool_name = "Galaxy Blue";
    REQUIRE(helix::test::apply_edit(*sec_mock, 1, info).success());
    ams.update_slot_for_backend(second, 1);
    CHECK(std::string(lv_subject_get_string(sec_name)) == "Galaxy Blue");

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: a long multibyte name is cut on a codepoint",
                 "[ams][slot_name][state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(2);
    AmsBackendMock* mock = owned.get();
    ams.set_backend(std::move(owned));

    // One ASCII byte then 40 three-byte codepoints: well past any slot-name
    // buffer, and offset so a plain byte cut lands mid-codepoint.
    std::string long_name = "A";
    for (int i = 0; i < 40; ++i)
        long_name += "\xE7\x99\xBD"; // 白
    SlotInfo info = mock->get_slot_info(1);
    info.spool_name = long_name;
    REQUIRE(helix::test::apply_edit(*mock, 1, info).success());
    ams.sync_from_backend();

    const std::string published(lv_subject_get_string(ams.get_slot_name_subject(1)));
    CHECK_FALSE(published.empty());
    CHECK(is_valid_utf8(published));
    CHECK(long_name.compare(0, published.size(), published) == 0);

    ams.clear_backends();
}

// ============================================================================
// 3. The widget
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: the name sits over the material",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Bone White", "PLA+");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    lv_obj_update_layout(test_screen());

    CHECK(shown(name_label(slot)));
    CHECK(text_of(name_label(slot)) == "Bone White");
    CHECK(text_of(material_label(slot)) == "PLA+");
    // Name first, material below it, both above the spool.
    lv_area_t name_a, mat_a, spool_a;
    lv_obj_get_coords(name_label(slot), &name_a);
    lv_obj_get_coords(material_label(slot), &mat_a);
    lv_obj_get_coords(lv_obj_find_by_name(slot, "spool_container"), &spool_a);
    CHECK(name_a.y2 <= mat_a.y1);
    CHECK(mat_a.y2 <= spool_a.y1);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: no name reads exactly as the material-only label",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "", "PLA");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    CHECK_FALSE(shown(name_label(slot)));
    CHECK(text_of(material_label(slot)) == "PLA");
    // A present lane draws its material at full strength; only a ghost dims it.
    CHECK(lv_obj_get_style_text_opa(material_label(slot), LV_PART_MAIN) == LV_OPA_COVER);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a name stating its material keeps the material line",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "PLA+ Bone White", "PLA+");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    CHECK(shown(name_label(slot)));
    CHECK(text_of(name_label(slot)) == "PLA+ Bone White");
    CHECK(text_of(material_label(slot)) == "PLA+");

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: five or more slots keep the material-only label",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Bone White", "PETG-CF");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    REQUIRE(shown(name_label(slot)));

    // The containers learn the count after the slot is built; the label must
    // follow it on that call, not on the next subject change.
    ui_ams_slot_set_layout_info(slot, 0, 8);
    CHECK_FALSE(shown(name_label(slot)));
    CHECK(text_of(material_label(slot)) == "PETG");

    ui_ams_slot_set_layout_info(slot, 0, 4);
    CHECK(shown(name_label(slot)));
    CHECK(text_of(material_label(slot)) == "PETG-CF");

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: an Empty lane keeps its placeholder alone",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Bone White", "", helix::ui::LaneState::Empty);

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    CHECK_FALSE(shown(name_label(slot)));
    CHECK(text_of(material_label(slot)) == std::string(lv_tr("Empty")));

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a ghosted lane dims its name with its material",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Bone White", "PLA", helix::ui::LaneState::Ghosted);

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);

    CHECK(shown(name_label(slot)));
    CHECK(lv_obj_get_style_text_opa(name_label(slot), LV_PART_MAIN) ==
          lv_obj_get_style_text_opa(material_label(slot), LV_PART_MAIN));
    CHECK(lv_obj_get_style_text_opa(name_label(slot), LV_PART_MAIN) < LV_OPA_COVER);

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a new name with the same material repaints",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Bone White", "PLA");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    REQUIRE(text_of(name_label(slot)) == "Bone White");

    lv_subject_copy_string(AmsState::instance().get_slot_name_subject(0), "Jet Black");
    process_lvgl(50);
    CHECK(text_of(name_label(slot)) == "Jet Black");
    CHECK(text_of(material_label(slot)) == "PLA");

    // Losing the name falls back to the material line alone.
    lv_subject_copy_string(AmsState::instance().get_slot_name_subject(0), "");
    process_lvgl(50);
    CHECK_FALSE(shown(name_label(slot)));
    CHECK(text_of(material_label(slot)) == "PLA");

    lv_obj_delete(slot);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: swapping two PLA spools repaints the name",
                 "[ams][slot_name][ui]") {
    ui_ams_slot_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* mock = owned.get();
    ams.set_backend(std::move(owned));

    SlotInfo info = mock->get_slot_info(0);
    info.spool_name = "Bone White";
    info.material = "PLA";
    REQUIRE(helix::test::apply_edit(*mock, 0, info).success());
    mock->force_slot_status(0, SlotStatus::AVAILABLE);
    ams.sync_from_backend();

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    REQUIRE(text_of(name_label(slot)) == "Bone White");

    info = mock->get_slot_info(0);
    info.spool_name = "Jet Black";
    REQUIRE(helix::test::apply_edit(*mock, 0, info).success());
    ams.sync_from_backend();
    process_lvgl(50);

    CHECK(text_of(name_label(slot)) == "Jet Black");
    CHECK(text_of(material_label(slot)) == "PLA");

    lv_obj_delete(slot);
    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a secondary backend's slot shows its own name",
                 "[ams][slot_name][ui][multi_backend]") {
    ui_ams_slot_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ams.set_backend(std::make_unique<AmsBackendMock>(4));
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);

    seed_slot(0, "Primary Name", "PETG");
    SubjectLifetime lt;
    lv_subject_t* sec_name = ams.get_slot_name_subject(second, 0, lt);
    REQUIRE(sec_name != nullptr);
    lv_subject_copy_string(sec_name, "Secondary Name");
    lv_subject_copy_string(ams.get_slot_material_subject(second, 0, lt), "PLA");
    lv_subject_set_int(ams.get_slot_lane_state_subject(second, 0, lt),
                       static_cast<int>(helix::ui::LaneState::Present));

    ams.set_active_backend(second);
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    CHECK(text_of(name_label(slot)) == "Secondary Name");

    lv_subject_copy_string(sec_name, "Secondary Renamed");
    process_lvgl(50);
    CHECK(text_of(name_label(slot)) == "Secondary Renamed");

    lv_obj_delete(slot);
    ams.set_active_backend(0);
    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a long name stays one line inside the slot",
                 "[ams][slot_name][ui][layout]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    seed_slot(0, "Polymaker PolyTerra Ambrosia Pink Matte", "PLA");

    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    lv_obj_update_layout(test_screen());

    lv_obj_t* name = name_label(slot);
    const lv_font_t* font = lv_obj_get_style_text_font(name, LV_PART_MAIN);
    CHECK(lv_obj_get_height(name) == lv_font_get_line_height(font));
    CHECK(lv_obj_get_width(name) <= lv_obj_get_content_width(slot));
    CHECK(lv_label_get_long_mode(name) == LV_LABEL_LONG_MODE_DOTS);

    lv_obj_delete(slot);
}

// ============================================================================
// An empty lane's label through the real status and material observers
// ============================================================================

namespace {

lv_opa_t material_opa(lv_obj_t* slot) {
    return lv_obj_get_style_text_opa(material_label(slot), LV_PART_MAIN);
}

/// The dashed "add filament here" circle that replaces the spool on a lane with
/// no identity. Every spool style creates it.
bool placeholder_visible(lv_obj_t* slot) {
    lv_obj_t* ph = lv_obj_find_by_name(slot, "empty_placeholder");
    REQUIRE(ph != nullptr);
    return shown(ph);
}

/// A started mock in AmsState, with lane @p slot_index stripped to "never
/// assigned" (unassigned) or ejected keeping @p material (assigned). Status goes
/// through force_slot_status(): no backend's apply_user_edit() accepts it.
AmsBackendMock* install_empty_lane(int slot_index, const char* material) {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_operation_delay(0);
    AmsBackendMock* mock = owned.get();
    ams.set_backend(std::move(owned));

    SlotInfo info = mock->get_slot_info(slot_index);
    info.material = material;
    if (material[0] == '\0') {
        info.brand.clear();
        info.spool_name.clear();
        info.color_name.clear();
        info.spoolman_id = 0;
    }
    REQUIRE(helix::test::apply_edit(*mock, slot_index, info).success());
    mock->force_slot_status(slot_index, SlotStatus::EMPTY);
    ams.sync_from_backend();
    ui_spool_canvas_register();
    ui_ams_slot_register();
    return mock;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: unassigned empty lane reads Empty on first paint",
                 "[ams][slot]") {
    install_empty_lane(3, "");
    lv_obj_t* slot = create_ams_slot(test_screen(), 3);
    REQUIRE(slot != nullptr);

    // First paint, before any further tick: status then material are applied,
    // and the label already reads "Empty", not the material pass's "--".
    CHECK(text_of(material_label(slot)) == std::string(lv_tr("Empty")));
    // Full strength: an unassigned lane is a named blank, not a ghost.
    CHECK(material_opa(slot) == LV_OPA_COVER);
    CHECK(placeholder_visible(slot));

    lv_obj_delete(slot);
    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: assigned ejected lane keeps its material, ghosted",
                 "[ams][slot]") {
    install_empty_lane(2, "PETG");
    lv_obj_t* slot = create_ams_slot(test_screen(), 2);
    REQUIRE(slot != nullptr);

    CHECK(text_of(material_label(slot)) == "PETG");
    CHECK(material_opa(slot) == LV_OPA_30);
    // A retained identity ghosts the real spool rather than swapping in the
    // placeholder.
    CHECK_FALSE(placeholder_visible(slot));

    lv_obj_delete(slot);
    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: a later material notify does not undo Empty",
                 "[ams][slot]") {
    // Status and material are separate subjects with separate observers; the
    // material one notifying after the lane is painted "Empty" must not move it.
    install_empty_lane(3, "");
    lv_obj_t* slot = create_ams_slot(test_screen(), 3);
    REQUIRE(slot != nullptr);
    REQUIRE(text_of(material_label(slot)) == std::string(lv_tr("Empty")));

    lv_subject_t* material_subject = AmsState::instance().get_slot_material_subject(3);
    REQUIRE(material_subject != nullptr);
    // Round-trip through a non-empty value so the subject genuinely notifies twice.
    lv_subject_copy_string(material_subject, "PLA");
    process_lvgl(20);
    lv_subject_copy_string(material_subject, "");
    process_lvgl(20);

    CHECK(text_of(material_label(slot)) == std::string(lv_tr("Empty")));

    lv_obj_delete(slot);
    AmsState::instance().clear_backends();
}
