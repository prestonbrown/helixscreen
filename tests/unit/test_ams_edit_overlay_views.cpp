// SPDX-License-Identifier: GPL-3.0-or-later
//
// View-level tests for the AMS slot editor overlay redesign: identity chip
// contents (tracked vs untracked), managed-state subject, pre-selection, and
// view transitions. Uses the full-UI fixture (real XML tree) plus the
// AmsEditOverlayViewTestAccess friend shim.

#include "ui_ams_edit_overlay.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_slot_saver.h"
#include "src/ui/panel_widgets/active_spool_widget.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::ui;

class AmsEditOverlayViewTestAccess {
  public:
    explicit AmsEditOverlayViewTestAccess(AmsEditOverlay& overlay) : overlay_(overlay) {}

    lv_obj_t* widget(const char* name) {
        return overlay_.find_widget(name);
    }
    void set_working_info(const SlotInfo& info) {
        overlay_.working_info_ = info;
    }
    void call_update_ui() {
        overlay_.update_ui();
    }
    int view() {
        return lv_subject_get_int(&overlay_.view_mode_subject_);
    }
    void call_set_view(int v) {
        overlay_.set_view(v);
    }
    int is_managed() {
        return lv_subject_get_int(&overlay_.is_managed_subject_);
    }
    void set_cached_spools(std::vector<SpoolInfo> spools) {
        overlay_.cached_spools_ = std::move(spools);
    }
    void call_render_spool_list(const std::string& filter) {
        overlay_.render_spool_list(filter);
    }
    void call_enter_spool_edit() {
        overlay_.enter_spool_edit();
    }
    void call_handle_spool_edit_save() {
        overlay_.handle_spool_edit_save();
    }
    void call_handle_save() {
        overlay_.handle_save();
    }
    void call_switch_to_picker() {
        overlay_.switch_to_picker();
    }
    void call_handle_spool_selected(int spool_id) {
        overlay_.handle_spool_selected(spool_id);
    }
    void call_switch_to_form() {
        overlay_.switch_to_form();
    }
    void set_details_color(uint32_t rgb) {
        overlay_.details_color_ = rgb;
        overlay_.details_color_set_ = true;
    }
    SlotInfo working_info() {
        return overlay_.working_info_;
    }
    helix::ui::FilamentCatalogSelector& details_selector() {
        return overlay_.details_selector_;
    }
    // Simulate the async Spoolman fetch in enter_spool_edit() that overwrites
    // detail_original_/detail_working_ wholesale with the fetched record
    // (spool_weight_g = empty-spool CORE weight, not the filament total) and
    // repopulates the on-screen fields — exactly what the fetch callback does.
    void seed_detail_fetch(const SpoolInfo& spool) {
        overlay_.detail_original_ = spool;
        overlay_.detail_working_ = spool;
        overlay_.populate_detail_fields();
    }
    bool is_dirty() {
        return overlay_.is_dirty();
    }
    bool save_opt_in() {
        return overlay_.save_to_spoolman_opt_in_;
    }
    void call_open_color_view() {
        overlay_.open_color_view();
    }
    void call_apply_color(uint32_t rgb) {
        overlay_.apply_color(rgb);
    }
    uint32_t details_color() {
        return overlay_.details_color_;
    }
    bool details_color_set() {
        return overlay_.details_color_set_;
    }
    static void build_spool_patches(const SpoolInfo& original, const SpoolInfo& edited,
                                    nlohmann::json& spool_patch, nlohmann::json& filament_patch) {
        SpoolmanSlotSaver::build_spool_patches(original, edited, spool_patch, filament_patch);
    }

  private:
    AmsEditOverlay& overlay_;
};

namespace {

void close_editor_overlay() {
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
}

SlotInfo untracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 0;
    info.brand = "Generic";
    info.material = "PETG";
    info.color_rgb = 0xFF6600;
    info.color_name = "Orange";
    return info;
}

SlotInfo tracked_slot() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 7;
    info.brand = "Bambu Lab";
    info.material = "ASA";
    info.spool_name = "Bambu Lab ASA";
    info.color_rgb = 0x8A949E;
    info.color_name = "Gray ASA";
    return info;
}

// A slot as apply_spool_to_slot() leaves it: spool_name is the Spoolman
// filament name alone, with brand and material in their own fields. Nothing
// about the name repeats the other two, so the chip has to join all three.
SlotInfo tracked_slot_spoolman_named() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 42;
    info.brand = "Polymaker";
    info.material = "PLA";
    info.spool_name = "Ambrosia Pink";
    info.color_rgb = 0xFFB6C1;
    return info;
}

// A Spoolman-linked lane on AFC older than v1.2.0: the firmware publishes
// spool_id but no filament_name, so the slot carries the link and nothing to
// name it with. Seen live on a BoxTurtle at 192.168.1.112 (spool #106).
SlotInfo tracked_slot_named_only_in_cache() {
    SlotInfo info;
    info.slot_index = 0;
    info.spoolman_id = 106;
    info.material = "PLA";
    info.color_rgb = 0x333333;
    // brand and spool_name deliberately empty — only the cache knows them.
    return info;
}

// Copy of untracked_slot() with weights explicitly marked unknown (-1
// sentinel, also SlotInfo's default) — models a slot with no Spoolman/manual
// weight data on record.
SlotInfo untracked_slot_without_weights() {
    SlotInfo info = untracked_slot();
    info.total_weight_g = -1.0f;
    info.remaining_weight_g = -1.0f;
    return info;
}

// Mirrors the show_for_slot() + drain + process_lvgl sequence used by every
// test in this file, seeded with a weightless slot. Takes the fixture so it
// can reach test_screen()/process_lvgl() (both LVGLUITestFixture members).
void show_overlay_for_mock_slot_without_weights(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot_without_weights(),
                                  nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_tracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    // api=nullptr keeps the async Spoolman re-fetch out of the picture.
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

void show_overlay_for_mock_untracked_slot(LVGLUITestFixture& fixture) {
    auto& overlay = get_ams_edit_overlay();
    REQUIRE(overlay.show_for_slot(fixture.test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "card tap opens spool-edit; change-filament row opens picker",
                 "[ams_edit_overlay][card]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // Change-filament routes to the picker only when Spoolman is connected.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    show_overlay_for_mock_tracked_slot(*this);

    lv_obj_t* card = access.widget("spool_card");
    REQUIRE(card != nullptr);
    lv_obj_send_event(card, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    auto* view_subj = lv_xml_get_subject(nullptr, "ams_edit_view");
    REQUIRE(view_subj != nullptr);
    CHECK(lv_subject_get_int(view_subj) == AmsEditOverlay::VIEW_SPOOL_EDIT);

    access.call_switch_to_form();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    // Change Filament is now a button in the paired action row (design Option 1);
    // the old link-style change_filament_row was retired.
    CHECK(access.widget("change_filament_row") == nullptr);
    lv_obj_t* row = access.widget("btn_change_filament");
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(lv_subject_get_int(view_subj) == AmsEditOverlay::VIEW_SPOOL_PICKER);

    // Retired widgets: chip, details row, inline remaining slider.
    CHECK(access.widget("identity_chip") == nullptr);
    CHECK(access.widget("spool_details_row") == nullptr);
    CHECK(access.widget("remaining_slider") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool card shows Brand · Material for untracked slots",
                 "[ams_edit_overlay][card]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* label = access.widget("card_identity_label");
    REQUIRE(label != nullptr);
    // Locked naming (spec §3.8): "Generic · PETG" — brand · material.
    CHECK(std::string(lv_label_get_text(label)) == "Generic \xC2\xB7 PETG");
    CHECK(access.is_managed() == 0);

    lv_obj_t* mark = access.widget("chip_spoolman_mark");
    REQUIRE(mark != nullptr);
    CHECK(lv_obj_has_flag(mark, LV_OBJ_FLAG_HIDDEN));

    // Chip + details row retired in favor of the card.
    CHECK(access.widget("identity_chip") == nullptr);
    CHECK(access.widget("spool_details_row") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool card shows spool name + mark for tracked slots",
                 "[ams_edit_overlay][card]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // Managed state requires Spoolman availability, not just spoolman_id > 0.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    // api=nullptr keeps the async Spoolman re-fetch out of the picture.
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* label = access.widget("card_identity_label");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Bambu Lab ASA");
    CHECK(access.is_managed() == 1);

    lv_obj_t* mark = access.widget("chip_spoolman_mark");
    REQUIRE(mark != nullptr);
    CHECK_FALSE(lv_obj_has_flag(mark, LV_OBJ_FLAG_HIDDEN));

    CHECK(access.widget("spool_details_row") == nullptr);

    // No "(Spoolman #N)" label anywhere anymore.
    CHECK(access.widget("spoolman_id_label") == nullptr);
    // Overview dropdowns are gone (the details view's embedded catalog
    // selector still has its own vendor/type dropdowns — scope to form_view).
    lv_obj_t* form_view = access.widget("form_view");
    REQUIRE(form_view != nullptr);
    CHECK(lv_obj_find_by_name(form_view, "vendor_dropdown") == nullptr);
    CHECK(lv_obj_find_by_name(form_view, "material_dropdown") == nullptr);
    CHECK(access.widget("btn_change_spool") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool card keeps brand and material around a bare Spoolman filament name",
                 "[ams_edit_overlay][card][spoolman][regression]") {
    // apply_spool_to_slot() writes the Spoolman filament name into spool_name
    // rather than a synthesized "vendor material". Printing that field verbatim
    // would reduce the chip to "Ambrosia Pink" — a colour with no vendor and no
    // material, strictly less than the "Generic · PETG" the untracked branch
    // shows. The chip joins the three fields through the same dedup helper the
    // AMS card uses, so a name that already contains the brand or the material
    // still prints once.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    REQUIRE(
        overlay.show_for_slot(test_screen(), 0, tracked_slot_spoolman_named(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* label = access.widget("card_identity_label");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Polymaker Ambrosia Pink PLA");
    CHECK(access.is_managed() == 1);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool card names a linked slot from the identity cache when AFC cannot",
                 "[ams_edit_overlay][card][spoolman][regression]") {
    // AFC below v1.2.0 publishes spool_id but no filament_name, so the slot
    // carries the Spoolman link with nothing to name it. The loaded card
    // resolves that through the identity cache; this card must too, or it
    // silently degrades to the untracked "Elegoo · PLA" for a spool we can
    // name. Reproduced live on a BoxTurtle (spool #106) before this fix.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    // Seed through the real entry point the weight poll uses.
    SpoolInfo spool;
    spool.id = 106;
    spool.vendor = "Elegoo";
    spool.filament_name = "Black Rapid PLA+";
    spool.material = "PLA";
    SpoolmanManager::invalidate_identity(106); // insert-if-absent: start clean
    SpoolmanManager::cache_identity(spool);
    REQUIRE(SpoolmanManager::find_identity(106).has_value());

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot_named_only_in_cache(), nullptr,
                                  nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* label = access.widget("card_identity_label");
    REQUIRE(label != nullptr);
    // "PLA" is dropped because "PLA+" already states it.
    CHECK(std::string(lv_label_get_text(label)) == "Elegoo Black Rapid PLA+");

    close_editor_overlay();
    SpoolmanManager::invalidate_identity(106);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "managed state requires Spoolman availability, not just a stale spoolman_id",
                 "[ams_edit_overlay][card][spoolman]") {
    // Regression (HELIX_MOCK_SPOOLMAN=0): a slot can carry a stale spoolman_id
    // from a session where Spoolman was available. Once Spoolman itself is
    // unavailable, the card mark and spool-edit logistics section must hide —
    // update_ui() must gate on printer_has_spoolman, not spoolman_id alone.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);

    SECTION("Spoolman unavailable -> not managed despite spoolman_id > 0") {
        lv_subject_set_int(spoolman_subj, 0);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.is_managed() == 0);
        lv_obj_t* mark = access.widget("chip_spoolman_mark");
        REQUIRE(mark != nullptr);
        CHECK(lv_obj_has_flag(mark, LV_OBJ_FLAG_HIDDEN));

        close_editor_overlay();
    }

    SECTION("Spoolman available -> managed with spoolman_id > 0") {
        lv_subject_set_int(spoolman_subj, 1);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.is_managed() == 1);
        lv_obj_t* mark = access.widget("chip_spoolman_mark");
        REQUIRE(mark != nullptr);
        CHECK_FALSE(lv_obj_has_flag(mark, LV_OBJ_FLAG_HIDDEN));

        close_editor_overlay();
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot skips the identity-sync fetch when Spoolman is unavailable",
                 "[ams_edit_overlay][card][spoolman]") {
    // A Spoolman-less printer must not fire a doomed identity-sync fetch (one
    // RPC + warn log) every time the slot editor opens. show_for_slot() gates
    // the open-time re-fetch on printer_has_spoolman, matching enter_spool_edit.
    //
    // The mock is fully ENABLED here, so if the fetch fired it would return
    // authoritative data and overwrite working_info_ (brand/material). The gate
    // under test is the UI-side availability subject, not the mock's own flag —
    // so an untouched working_info_ proves the fetch never ran.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().set_mock_spoolman_enabled(true);

    // Authoritative record whose vendor/material differ from the slot's initial
    // values, so a fired fetch is observable as a working_info_ change.
    api.spoolman_mock().get_mock_spools().clear();
    SpoolInfo authoritative;
    authoritative.id = 7;
    authoritative.vendor = "SpoolmanVendor";
    authoritative.material = "SpoolmanPETG";
    api.spoolman_mock().get_mock_spools().push_back(authoritative);

    SlotInfo slot = tracked_slot(); // spoolman_id = 7
    slot.brand = "SlotBrand";
    slot.material = "SlotPLA";

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SECTION("Spoolman unavailable -> no fetch, working_info_ untouched") {
        lv_subject_set_int(spoolman_subj, 0);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SlotBrand");
        CHECK(access.working_info().material == "SlotPLA");

        close_editor_overlay();
    }

    SECTION("Spoolman available -> fetch fires, working_info_ synced") {
        lv_subject_set_int(spoolman_subj, 1);

        REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
        UpdateQueue::instance().drain();
        process_lvgl(10);

        CHECK(access.working_info().brand == "SpoolmanVendor");
        CHECK(access.working_info().material == "SpoolmanPETG");

        close_editor_overlay();
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "filament-details view: toggle hidden without Spoolman",
                 "[ams_edit_overlay][details][toggle]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);

    lv_obj_t* toggle_row = access.widget("save_to_spoolman_row");
    REQUIRE(toggle_row != nullptr);
    CHECK(lv_obj_has_flag(toggle_row, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(spoolman_subj, 1);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK_FALSE(lv_obj_has_flag(toggle_row, LV_OBJ_FLAG_HIDDEN));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool-edit Save applies color locally with toggle off",
                 "[ams_edit_overlay][spool_edit][toggle]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Untracked slot -> toggle defaults OFF (ams_edit_is_managed drives it).
    lv_obj_t* toggle = access.widget("save_to_spoolman_switch");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    access.set_details_color(0xE53935);
    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK(access.working_info().color_rgb == 0xE53935);
    CHECK(access.working_info().spoolman_id == 0); // stays untracked
    CHECK_FALSE(access.save_opt_in());             // Save will NOT write Spoolman

    close_editor_overlay();
}

// Bug A — the spool-edit round-trip drops the user's existing brand.
//
// A slot already carries a non-Generic vendor (e.g. "Sunlu"). The user opens
// spool-edit to tweak something unrelated and taps Save without ever touching
// the vendor dropdown. setup_details_selector() seeds the catalog selector
// with the MATERIAL only (no vendor), and populate_vendor_dropdown() forces the
// vendor to index 0 = "Generic". preselect_first() then highlights the Generic
// product, so handle_spool_edit_save() reads details_selector_.highlighted()
// and overwrites working_info_.brand with that product's brand ("Generic").
// The user's "Sunlu" is silently lost even though they never edited the vendor.
//
// The fix will seed the selector's vendor from the slot's existing brand so an
// untouched Save round-trips it. This test opens the editor on a Sunlu slot,
// enters spool-edit, saves without changing the dropdown, and asserts the brand
// survives.
TEST_CASE_METHOD(LVGLUITestFixture, "spool-edit Save preserves an existing non-Generic brand",
                 "[ams_edit_overlay][spool_edit][brand]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // Untracked slot whose vendor is a real non-Generic brand the user chose.
    SlotInfo sunlu;
    sunlu.slot_index = 0;
    sunlu.spoolman_id = 0;
    sunlu.brand = "Sunlu";
    sunlu.material = "PLA";
    sunlu.color_rgb = 0xFEF043;
    sunlu.color_name = "Yellow";

    REQUIRE(overlay.show_for_slot(test_screen(), 0, sunlu, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Save WITHOUT touching the vendor dropdown — the user only meant to confirm.
    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // REGRESSION: the brand must round-trip. Currently the selector forced
    // vendor=Generic on open, so the highlighted Generic product clobbers it.
    CHECK(access.working_info().brand == "Sunlu");
    CHECK(access.working_info().material == "PLA"); // material unchanged

    close_editor_overlay();
}

// Regression — a Spoolman-only vendor no longer reaches the vendor dropdown.
//
// A vendor can live on the Spoolman server without a matching entry in the
// bundled assets/filaments.json catalog (e.g. "PolyTerra"). The branded rework
// dropped the live vendor fetch, so populate_vendor_dropdown() built the vendor
// list from the catalog ALONE: such a vendor never appeared, the dropdown
// snapped it to "Generic" on open, and an untouched Save baked "Generic" in —
// silently overwriting the user's saved vendor.
//
// The fix: when Spoolman is connected, the host fetches the live vendor list and
// merges it into the selector (which stays Spoolman-agnostic — it just receives
// the names). The seed vendor then resolves and the brand string round-trips.
TEST_CASE_METHOD(LVGLUITestFixture, "spool-edit surfaces a Spoolman-only vendor and Save keeps it",
                 "[ams_edit_overlay][spool_edit][brand][spoolman]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().set_mock_spoolman_enabled(true);
    // A vendor that exists on the Spoolman server but NOT in the bundled catalog.
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_vendor(42, "PolyTerra");

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    // Untracked slot (spoolman_id = 0 keeps the open-time id-sync fetch out of
    // it) whose brand is the Spoolman-only vendor.
    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 0;
    slot.brand = "PolyTerra";
    slot.material = "PLA";
    slot.color_rgb = 0x66CC33;
    slot.color_name = "Green";

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain(); // resolve the async vendor fetch's tok.defer
    process_lvgl(10);

    // The Spoolman-only vendor is now in the dropdown AND selected (seed honored).
    CHECK(access.details_selector().current_vendor() == "PolyTerra");

    // Save WITHOUT touching the dropdown -> the brand string round-trips.
    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.working_info().brand == "PolyTerra");

    close_editor_overlay();
}

// Regression guard — with no Spoolman, a catalog-absent vendor stays Generic.
//
// This is the ACCEPTED behavior for a Spoolman-less printer: the dropdown is
// catalog-only, so an unknown brand has nowhere to resolve and falls to Generic.
// The fix must NOT start a doomed vendor fetch when Spoolman is not connected.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool-edit without Spoolman leaves a catalog-absent vendor at Generic",
                 "[ams_edit_overlay][spool_edit][brand]") {
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0); // no Spoolman -> no vendor fetch

    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 0;
    slot.brand = "PolyTerra"; // absent from the bundled catalog
    slot.material = "PLA";
    slot.color_rgb = 0x66CC33;

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // No fetch fired -> the dropdown stayed catalog-only -> Generic.
    CHECK(access.details_selector().current_vendor() == "Generic");

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool-edit Save with toggle off unlinks a managed slot",
                 "[ams_edit_overlay][spool_edit][toggle]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Managed slot -> toggle defaults ON.
    lv_obj_t* toggle = access.widget("save_to_spoolman_switch");
    REQUIRE(toggle != nullptr);
    CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    // User switches it off -> unlink on Save (resolution §2.1): id -> 0,
    // identity kept locally, no Spoolman write. This is the SOLE unlink path
    // now that the picker's unlink entry is retired.
    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.working_info().spoolman_id == 0);
    CHECK(access.working_info().material == "ASA"); // identity preserved
    CHECK_FALSE(access.save_opt_in());

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool-edit Save unlinking a tracked slot keeps the local total weight",
                 "[ams_edit_overlay][spool_edit][toggle]") {
    // Regression (Finding 1): unlinking a tracked slot must NOT stage the
    // fetched empty-spool core weight over the real total. Before the fix the
    // unlink zeroed spoolman_id, control fell into the untracked weight-commit
    // branch, and detail_working_.spool_weight_g (the 216g core weight from the
    // async fetch) clobbered total_weight_g (1000).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    SlotInfo slot = tracked_slot();
    slot.total_weight_g = 1000.0f;
    slot.remaining_weight_g = 1000.0f;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Simulate the async fetch that overwrote the detail buffers with the
    // Spoolman record: spool_weight_g here is the empty-spool CORE weight.
    SpoolInfo fetched;
    fetched.id = 7;
    fetched.filament_id = 3;
    fetched.spool_weight_g = 216.0; // core weight, NOT the filament total
    fetched.remaining_weight_g = 1000.0;
    fetched.initial_weight_g = 1000.0;
    access.seed_detail_fetch(fetched);

    // User turns the toggle off -> unlink on Save.
    lv_obj_t* toggle = access.widget("save_to_spoolman_switch");
    REQUIRE(toggle != nullptr);
    lv_obj_remove_state(toggle, LV_STATE_CHECKED);

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK(access.working_info().spoolman_id == 0); // unlink still happened
    // The core weight (216) must NOT have overwritten the real total (1000).
    CHECK(access.working_info().total_weight_g == Catch::Approx(1000.0f));
    CHECK(access.working_info().remaining_weight_g == Catch::Approx(1000.0f));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker no longer offers a standalone unlink entry",
                 "[ams_edit_overlay][spool_edit][picker]") {
    // The picker_unlink_entry is retired — the single unlink path is spool-edit
    // Save with the toggle off (covered above).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_tracked_slot(*this);
    access.call_switch_to_picker();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.widget("picker_unlink_entry") == nullptr);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "color view from spool-edit stages the pending color and returns there",
                 "[ams_edit_overlay][color_view]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_open_color_view();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::VIEW_COLOR);

    uint32_t before = access.working_info().color_rgb;
    access.call_apply_color(0x1E88E5);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);
    CHECK(access.details_color() == 0x1E88E5);
    CHECK(access.details_color_set());
    CHECK(access.working_info().color_rgb == before); // slot untouched until Save

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "unified spool-edit view shows logistics only when tracked",
                 "[ams_edit_overlay][spool_edit]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // Managed state requires Spoolman availability, not just spoolman_id > 0.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    // Tracked slot: is_managed==1 -> logistics section visible.
    show_overlay_for_mock_tracked_slot(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);
    lv_obj_t* logistics = access.widget("spool_edit_logistics");
    REQUIRE(logistics != nullptr);
    CHECK_FALSE(lv_obj_has_flag(logistics, LV_OBJ_FLAG_HIDDEN));
    // The in-content Save button was retired — Save lives in the header bar now.
    CHECK(access.widget("btn_spool_edit_save") == nullptr);
    close_editor_overlay();

    // Untracked slot: is_managed==0 -> logistics section hidden.
    show_overlay_for_mock_untracked_slot(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    logistics = access.widget("spool_edit_logistics");
    REQUIRE(logistics != nullptr);
    CHECK(lv_obj_has_flag(logistics, LV_OBJ_FLAG_HIDDEN));
    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "untracked spool-edit Save commits Remaining/Spool-weight into working_info_",
                 "[ams_edit_overlay][spool_edit]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_untracked_slot(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* remaining = access.widget("detail_field_remaining");
    lv_obj_t* spool_weight = access.widget("detail_field_spool_weight");
    REQUIRE(remaining != nullptr);
    REQUIRE(spool_weight != nullptr);

    lv_textarea_set_text(remaining, "321");
    lv_textarea_set_text(spool_weight, "987");

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK(access.working_info().spoolman_id == 0); // still untracked, no PATCH
    CHECK(access.working_info().remaining_weight_g == Catch::Approx(321.0f));
    CHECK(access.working_info().total_weight_g == Catch::Approx(987.0f));

    // The commit path must be LIVE: staging into working_info_ without
    // re-syncing original_info_ leaves is_dirty() true, so the overview
    // header Save (the ONLY commit path) is enabled. A prior fix synced
    // original_info_ here and blinded is_dirty() -> Save disabled -> the
    // edit staged but could never commit.
    CHECK(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    CHECK(lv_subject_get_int(save_dis) == 0); // Save enabled

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "untracked spool-edit Save with a spool-weight-only edit lights the header Save",
                 "[ams_edit_overlay][spool_edit][dirty]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_untracked_slot(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* spool_weight = access.widget("detail_field_spool_weight");
    REQUIRE(spool_weight != nullptr);
    // Edit ONLY the spool weight — remaining stays blank (unchanged).
    lv_textarea_set_text(spool_weight, "144");

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK(access.working_info().total_weight_g == Catch::Approx(144.0f));
    // The new total_weight_g term in is_dirty() must light Save even when
    // remaining is untouched.
    CHECK(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    CHECK(lv_subject_get_int(save_dis) == 0); // Save enabled

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "untracked spool-edit Save with blank fields preserves the unknown sentinel",
                 "[ams_edit_overlay][spool_edit][dirty]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // Unknown-weight untracked slot: both weights are the -1 sentinel, so
    // populate_detail_fields() renders both quantity fields blank.
    show_overlay_for_mock_slot_without_weights(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* remaining = access.widget("detail_field_remaining");
    lv_obj_t* spool_weight = access.widget("detail_field_spool_weight");
    REQUIRE(remaining != nullptr);
    REQUIRE(spool_weight != nullptr);
    CHECK(std::string(lv_textarea_get_text(remaining)).empty());
    CHECK(std::string(lv_textarea_get_text(spool_weight)).empty());

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Blank == unchanged: the -1 sentinel is preserved (NOT overwritten with
    // 0), so the slot stays clean and Save does not light up.
    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK(access.working_info().remaining_weight_g <= 0);
    CHECK(access.working_info().total_weight_g <= 0);
    CHECK_FALSE(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    CHECK(lv_subject_get_int(save_dis) == 1); // Save stays disabled

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "untracked spool-edit Save rejects a negative weight and leaves working_info_ "
                 "untouched",
                 "[ams_edit_overlay][spool_edit]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_untracked_slot(*this);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* remaining = access.widget("detail_field_remaining");
    REQUIRE(remaining != nullptr);
    lv_textarea_set_text(remaining, "-50");

    float before_remaining = access.working_info().remaining_weight_g;
    float before_total = access.working_info().total_weight_g;

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Validation toast fired and returned early -> still on spool-edit view,
    // working_info_ untouched, and the catalog selector stays attached (the
    // early return happens before detach()/clear_catalog()).
    CHECK(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);
    CHECK(access.working_info().remaining_weight_g == before_remaining);
    CHECK(access.working_info().total_weight_g == before_total);

    close_editor_overlay();
}

namespace {
std::vector<SpoolInfo> two_spools() {
    SpoolInfo a;
    a.id = 11;
    a.vendor = "Polymaker";
    a.material = "PLA";
    a.color_hex = "1A1A2E";
    SpoolInfo b;
    b.id = 22;
    b.vendor = "eSUN";
    b.material = "PETG";
    b.color_hex = "00FF00";
    return {a, b};
}
} // namespace

// ============================================================================
// OverlayConsumerCommitFixture — for tests whose completion callback must
// mirror the production consumers (AmsPanel / AmsOverviewPanel): the overlay
// no longer pre-fires the server active-spool sync, so the consumer's
// commit_slot_edit() owns it, and the wiring that commit needs (a backend +
// the API registered on AmsState) has to exist in the test too. Wiring shape
// mirrors OverlayCommitFixture (test_ams_edit_overlay_active_spool.cpp) /
// CommitFixture (test_ams_state_commit_slot.cpp); no Config isolation —
// commit_slot_edit persists nothing (AmsBackendMock ignores the persist flag).
// ============================================================================

struct OverlayConsumerCommitFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    OverlayConsumerCommitFixture() : api(client, get_printer_state()) {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // A previous test file's SpoolmanManager::deinit_subjects() may have
        // latched its shutdown flag — unlatch it (see CommitFixture).
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);
    }

    ~OverlayConsumerCommitFixture() override {
        // Detach the mocks BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; queued backend-event
        // syncs from this test must not leak into the next one.
        UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    /// The production backend-slot completion-consumer body (AmsPanel /
    /// AmsOverviewPanel): capture the pre-edit slot BEFORE the commit — its
    /// unlink arm (clear the server active spool) needs the old link.
    void commit_like_consumer(const AmsEditOverlay::EditResult& r) {
        if (!r.saved || r.slot_index < 0)
            return;
        AmsBackend* commit_backend = AmsState::instance().get_backend();
        REQUIRE(commit_backend);
        SlotInfo original = commit_backend->get_slot_info(r.slot_index);
        AmsError err = AmsState::instance().commit_slot_edit(r.slot_index, original, r.slot_info);
        REQUIRE(err.success());
    }
};

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the first row for unlinked slots",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker-entry spool selection commits and closes the editor",
                 "[ams_edit_overlay][picker][header_save]") {
    // Task #13: when the editor is opened directly on the picker (context-menu
    // "Select spool"), choosing a spool is a one-tap commit — apply + close the
    // whole overlay via the header-Save commit path, firing completion with the
    // applied spool. Spoolman is left unavailable so commit_and_close takes the
    // synchronous local-close branch (no async PATCH seam needed here).
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, untracked_slot(), nullptr,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Select spool #22 (eSUN PETG) from the picker.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Committed + closed in one step, completion fired with the applied spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");

    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker-entry relink to a different spool never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16 regression: a slot linked to spool A, opened directly on the
    // picker, then switched to a DIFFERENT spool B is a pure RELINK — not an
    // edit of A's identity. The old "Different filament?" confirm + identity
    // PATCH would clobber the wrong Spoolman record. Assert: no confirm modal,
    // completion fires once with spool B, and NO spool/filament PATCH is sent.

    // Seed the currently-linked spool A (id 7) so the open-time re-fetch has an
    // authoritative record and leaves original_info_ as A's identity.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    api.spoolman_mock().get_mock_spools().push_back(linked_a);
    // The backend mirrors the overlay's initial info, the way a live backend
    // holds the tracked slot the editor was opened on.
    backend->set_slot_info(0, tracked_slot(), /*persist=*/false);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1); // is_spoolman_available() -> true
    get_printer_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, tracked_slot(), &api,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
            commit_like_consumer(r); // the consumer commit owns the server sync
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Switch the link to spool #22 (eSUN PETG) — a different material/color.
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // No "Different filament?" confirm modal interposed.
    CHECK(ModalStack::instance().stack_empty());
    // Committed + closed in one step with the newly linked spool.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");
    // The old spool A was never touched: no identity/weight PATCH, no filament
    // repoint, no new filament created.
    CHECK(api.spoolman_mock().spool_updates.empty());
    CHECK(api.spoolman_mock().filament_updates.empty());
    // The new spool is registered as active on the server.
    CHECK(api.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "two-step relink header Save never prompts or PATCHes the old spool",
                 "[ams_edit_overlay][filament_picker][picker][header_save]") {
    // Task #16, two-step variant: reach the picker via Change Filament (not the
    // one-tap entry), pick a different spool, return to the overview, then tap
    // header Save. Same relink semantics: no confirm, no PATCH of the old spool.

    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    api.spoolman_mock().get_mock_spools().push_back(linked_a);
    backend->set_slot_info(0, tracked_slot(), /*persist=*/false);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().set_spoolman_available(true);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                      commit_like_consumer(r);
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22); // stages the relink, returns to overview
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    REQUIRE_FALSE(fired); // not committed yet
    REQUIRE(access.working_info().spoolman_id == 22);

    access.call_handle_save(); // header Save on the overview
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(ModalStack::instance().stack_empty()); // no identity confirm
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(api.spoolman_mock().spool_updates.empty());
    CHECK(api.spoolman_mock().filament_updates.empty());
    CHECK(api.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "active_spool widget completion routes the edit through commit_slot_edit",
                 "[ams_edit_overlay][active_spool][commit]") {
    // Final-review find: ActiveSpoolWidget is the sixth completion consumer of
    // the shared edit overlay. Its backend-slot arm used to write the backend
    // directly (set_slot_info) — no server active-spool sync, no identity
    // invalidation. The consumer commit owns both; this drives the widget's
    // own click → editor → Save path and asserts the server sync fired.

    // The mock backend starts with slot 0 loaded and current by construction;
    // give that lane a Spoolman link the way a live one carries it.
    SlotInfo seeded = backend->get_slot_info(0);
    seeded.spoolman_id = 169;
    seeded.material = "PLA";
    backend->set_slot_info(0, seeded, /*persist=*/false);

    // Spoolman "unavailable" so the editor's Save takes the synchronous
    // local-close branch (no async PATCH seam).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);
    get_printer_state().set_spoolman_available(false);

    // Build the home-panel component + controller, then tap it the way a user
    // does — the widget's own completion wiring is the code under test.
    lv_obj_t* comp =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_active_spool", nullptr));
    REQUIRE(comp != nullptr);
    ActiveSpoolWidget widget(&api);
    widget.attach(comp, test_screen());

    lv_obj_t* btn = lv_obj_find_by_name(comp, "spoolman_btn");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The editor opened on the loaded slot; stage an edit and header-Save the
    // overlay the widget opened.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);
    SlotInfo edited = seeded;
    edited.material = "PETG";
    access.set_working_info(edited);
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // REQUIRED: the edit reached the backend slot through commit_slot_edit...
    const SlotInfo after = backend->get_slot_info(0);
    REQUIRE(after.material == "PETG");
    REQUIRE(after.spoolman_id == 169);
    // ...AND the server active-spool sync fired — the old direct-write arm
    // never did, which is exactly the branch regression this pins.
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 169);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "editing the linked spool's identity (same spool) still prompts to confirm",
                 "[ams_edit_overlay][filament_picker]") {
    // Contrast with the relink cases: when spoolman_id is UNCHANGED and the
    // material identity changes, this is a genuine edit of the linked spool —
    // the "Different filament?" confirmation MUST still interpose (task #16 must
    // not swallow it). Completion does not fire until the user answers.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Seed spool 7 so the open-time re-fetch leaves original_info_ as ASA.
    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    api.spoolman_mock().get_mock_spools().push_back(linked_a);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().set_spoolman_available(true);
    UpdateQueue::instance().drain(); // flush the queued availability update
    REQUIRE(get_printer_state().is_spoolman_available());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Same linked spool (id 7), but change the material identity in place.
    SlotInfo edited = access.working_info();
    edited.material = "PLA"; // ASA -> PLA on the SAME spool 7
    access.set_working_info(edited);

    access.call_handle_save(); // header Save on the overview
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The relink short-circuit must NOT swallow a same-spool identity edit: it
    // still routes into the confirm path (no early close). Either the confirm
    // modal is up (completion pending) or the fallback save fired against the
    // linked spool — but never a silent no-op relink close.
    const bool confirm_pending = !ModalStack::instance().stack_empty() && !fired;
    const bool save_ran =
        !api.spoolman_mock().spool_updates.empty() || !api.spoolman_mock().filament_updates.empty();
    CHECK((confirm_pending || save_ran));

    // Dismiss any modal so teardown is clean.
    if (!ModalStack::instance().stack_empty()) {
        Modal::hide(Modal::get_top());
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }
    if (!fired) {
        close_editor_overlay();
    }
    get_printer_state().set_spoolman_available(false); // restore clean slate
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "identity-confirm Cancel aborts entirely — no silent local commit, selector stays alive",
    "[ams_edit_overlay][filament_picker]") {
    // Regression: Cancel on the "Different filament?" dialog used to hide the
    // modal and then close_editor(true) — silently committing the staged
    // identity change to the AMS panel (backend->set_slot_info() +
    // sync_from_backend() in the panel's completion handler) while leaving
    // Spoolman untouched. Cancel must be a TRUE ABORT: no completion, no
    // PATCH, user stays on the spool-edit view with the edit still staged so
    // they can re-Save (dialog reappears) or Back out.
    //
    // Reached via the spool-edit view's header Save (not a direct
    // working_info_ mutation) so this also exercises the catalog-selector
    // detach/reattach seam: handle_spool_edit_save()'s finish=true path
    // unconditionally detaches + clears details_selector_ before reaching
    // commit_and_close(), which is safe for every OTHER path (close, or
    // async-error back to the overview) but would strand a dead selector on
    // Cancel-abort if nothing re-attached it.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    api.spoolman_mock().get_mock_spools().push_back(linked_a);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(get_printer_state().is_spoolman_available());

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    int fire_count = 0;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fire_count++;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain(); // applies the async Spoolman logistics fetch
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);

    // Stage a color-only identity change (material stays ASA via the
    // preselected catalog product) — same-spool edit, no logistics diff, so
    // Save routes straight into the identity-confirm gate synchronously.
    access.set_details_color(0x112233);

    access.call_handle_save(); // header Save on spool-edit -> finish=true
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // "Different filament?" is up
    REQUIRE(fire_count == 0);

    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* cancel_btn = lv_obj_find_by_name(dlg, "btn_secondary");
    REQUIRE(cancel_btn != nullptr);
    lv_obj_send_event(cancel_btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // TRUE ABORT: no completion, still on the spool-edit view, nothing sent.
    CHECK(fire_count == 0);
    CHECK(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(api.spoolman_mock().spool_updates.empty());
    CHECK(api.spoolman_mock().filament_updates.empty());
    // The staged edit is still there (Cancel didn't discard it).
    CHECK(access.working_info().color_rgb == 0x112233u);
    // The catalog selector must still be functional — not stranded inert by
    // the earlier detach() (the same stranded-selector bug class fixed
    // twice before this one).
    CHECK(access.details_selector().highlighted() != nullptr);

    // Save again: same diff, dialog must reappear (state intact).
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    REQUIRE(fire_count == 0);

    // This time, Confirm: PATCH lands and completion fires exactly once.
    lv_obj_t* dlg2 = ModalStack::instance().top_dialog();
    REQUIRE(dlg2 != nullptr);
    lv_obj_t* confirm_btn = lv_obj_find_by_name(dlg2, "btn_primary");
    REQUIRE(confirm_btn != nullptr);
    lv_obj_send_event(confirm_btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(fire_count == 1);
    CHECK(captured.saved);
    // The primary action is now "It's a new spool": it CREATES and rebinds
    // rather than patching the linked spool, so the previously linked spool
    // must come through untouched. Reaching this outcome at all was the point
    // of the change — before it, a different physical spool in a linked lane
    // could only overwrite the old spool's identity.
    CHECK(captured.slot_info.spoolman_id != 0);
    for (const auto& rec : api.spoolman_mock().spool_updates) {
        CHECK(rec.spool_id != 7); // linked_a.id — never patched
    }

    get_printer_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "combined identity+logistics save issues exactly one weight PATCH",
                 "[ams_edit_overlay][spoolman][slot_saver]") {
    // A header Save that changes BOTH the filament identity AND the remaining
    // weight in one go must PATCH the weight exactly once. The overlay's
    // logistics PATCH (build_spool_patches -> update_spoolman_spool carrying
    // remaining_weight) covers it; on_all_saved then syncs the SlotInfo weight
    // baseline so the follow-on SpoolmanSlotSaver::save() sees no spool-level
    // delta and does NOT re-PATCH the weight. This locks that single-PATCH
    // invariant against a redundant idempotent weight re-PATCH regressing.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolInfo linked_a;
    linked_a.id = 7;
    linked_a.filament_id = 3;
    linked_a.vendor = "Bambu Lab";
    linked_a.material = "ASA";
    linked_a.color_hex = "8A949E";
    linked_a.remaining_weight_g = 1000.0;
    api.spoolman_mock().get_mock_spools().push_back(linked_a);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    int fire_count = 0;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api,
                                  [&](const AmsEditOverlay::EditResult&) { fire_count++; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain(); // applies the async Spoolman logistics fetch
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);

    // Change logistics: remaining weight 1000 -> 750.
    lv_obj_t* remaining = access.widget("detail_field_remaining");
    REQUIRE(remaining != nullptr);
    lv_textarea_set_text(remaining, "750");
    // Change identity: a color different from the linked spool, so Save routes
    // through the identity-confirm gate and then SpoolmanSlotSaver.
    access.set_details_color(0x112233);

    access.call_handle_save(); // header Save on spool-edit -> finish=true
    UpdateQueue::instance().drain();
    process_lvgl(10);

    REQUIRE_FALSE(ModalStack::instance().stack_empty()); // "Different filament?"
    lv_obj_t* dlg = ModalStack::instance().top_dialog();
    REQUIRE(dlg != nullptr);
    lv_obj_t* confirm_btn = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(confirm_btn != nullptr);
    lv_obj_send_event(confirm_btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(fire_count == 1);

    // Count every PATCH that set the remaining weight, across BOTH paths: the
    // combined update_spoolman_spool() body and the dedicated
    // update_spoolman_spool_weight() path.
    // Confirming now creates a NEW spool instead of patching the linked one, so
    // the invariant this test guards changes shape: the linked spool must
    // receive NO weight PATCH at all. (The single-PATCH rule still applies to
    // the update path, which the LinkIntent tests in
    // test_spoolman_slot_saver.cpp cover directly.)
    int linked_weight_patches = 0;
    for (const auto& rec : api.spoolman_mock().weight_updates) {
        if (rec.spool_id == 7)
            linked_weight_patches++;
    }
    for (const auto& rec : api.spoolman_mock().spool_updates) {
        if (rec.spool_id == 7 && rec.patch.contains("remaining_weight"))
            linked_weight_patches++;
    }
    CHECK(linked_weight_patches == 0);

    get_printer_state().set_spoolman_available(false); // restore clean slate
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Change-Filament picker selection returns to overview without closing",
                 "[ams_edit_overlay][picker]") {
    // Contrast with the picker-entry shortcut: reaching the picker via Change
    // Filament (switch_to_picker clears opened_on_picker_) keeps the two-step
    // flow — a selection returns to the overview for review, no completion.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    bool fired = false;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult&) { fired = true; }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_switch_to_picker(); // Change-Filament entry: clears the shortcut
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(access.view() == AmsEditOverlay::VIEW_OVERVIEW);
    CHECK_FALSE(fired); // no commit — the overview header Save commits later
    CHECK(access.working_info().spoolman_id == 22); // staged, awaiting review

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker always offers the setup entry",
                 "[ams_edit_overlay][picker][setup_entry]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* entry = access.widget("picker_setup_entry");
    REQUIRE(entry != nullptr);
    // Present regardless of picker fetch state (loading/empty/content) —
    // it is the only path forward when the spool list is empty.
    CHECK_FALSE(lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(entry, LV_OBJ_FLAG_CLICKABLE));

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture, "picker pre-selects the current spool when linked",
                 "[ams_edit_overlay][picker][preselect]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo linked = tracked_slot();
    linked.spoolman_id = 22;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, linked, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* list = access.widget("picker_spool_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    CHECK_FALSE(lv_obj_has_state(lv_obj_get_child(list, 0), LV_STATE_CHECKED));
    CHECK(lv_obj_has_state(lv_obj_get_child(list, 1), LV_STATE_CHECKED));

    close_editor_overlay();
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "spool-edit Save after a type change stages the checked product, not the old identity",
    "[ams_edit_overlay][catalog_selector]") {
    // Regression for the silent-drop bug: user changes the Type dropdown, the
    // product list rebuilds, then taps header Save. Before the fix the rebuilt
    // list had nothing highlighted and Save skipped the identity entirely,
    // saving the OLD brand/material. Now the selector always leaves a product
    // checked, so Save stages the new identity.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo slot = untracked_slot(); // Generic PETG
    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto& sel = access.details_selector();
    // Constrain the type dropdown for deterministic indices (index 0 = PETG,
    // 1 = PLA); preselect-on-change was already enabled by enter_spool_edit.
    sel.set_preselect_on_change(true);
    sel.configure(std::string("PETG"), std::vector<std::string>{"PETG", "PLA"});
    sel.populate();
    sel.preselect_first();
    REQUIRE(sel.current_type() == "PETG");
    REQUIRE(sel.highlighted() != nullptr);

    // Change type to PLA — the list must auto-highlight a PLA product.
    sel.change_type_for_test(1);
    REQUIRE(sel.current_type() == "PLA");
    const helix::printer::EffectiveFilament* pla = sel.highlighted();
    REQUIRE(pla != nullptr);
    std::string expect_brand = pla->brand;

    access.call_handle_spool_edit_save();
    CHECK(access.working_info().material == "PLA");
    CHECK(access.working_info().brand == expect_brand);

    close_editor_overlay();
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "spool-edit Save applies Generic identity when a whitelisted type has no catalog product",
    "[ams_edit_overlay][catalog_selector]") {
    // A firmware-whitelisted material with no seeded catalog product yields an
    // empty (all-unchecked) product list. Save must not silently no-op the
    // identity change: apply vendor Generic + the selected type string.
    //
    // PEEK is the example because Generic stocks no PEEK product at all, so it
    // stands as its own appended heading. A stocked material would instead fold
    // into its family heading (SILK -> "Silk PLA" -> the PLA heading) and would
    // no longer exercise the empty-product-list path this test covers.
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo slot = untracked_slot();
    slot.brand = "eSUN";    // prove the brand gets forced to Generic
    slot.material = "PETG"; // differs from the selected PEEK
    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto& sel = access.details_selector();
    sel.configure(std::nullopt, std::vector<std::string>{"PEEK"});
    sel.populate();
    REQUIRE(sel.current_type() == "PEEK");
    REQUIRE(sel.highlighted() == nullptr);

    access.call_handle_spool_edit_save();
    CHECK(access.working_info().material == "PEEK");
    CHECK(access.working_info().brand == "Generic");

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "header Save shows on overview + spool-edit, hides on picker/color",
                 "[ams_edit_overlay][views]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto* hide_subj = lv_xml_get_subject(nullptr, "ams_edit_save_hidden");
    REQUIRE(hide_subj != nullptr);
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(hide_subj) == 0); // overview: visible

    access.call_set_view(AmsEditOverlay::VIEW_SPOOL_PICKER);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // picker: hidden

    // Spool-edit: Save is VISIBLE and ENABLED even though nothing is dirty
    // (edits live in widgets, not staged into working_info_).
    access.call_set_view(AmsEditOverlay::VIEW_SPOOL_EDIT);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    REQUIRE_FALSE(access.is_dirty());
    CHECK(lv_subject_get_int(save_dis) == 0);

    access.call_set_view(AmsEditOverlay::VIEW_COLOR);
    REQUIRE(lv_subject_get_int(hide_subj) == 1); // color: hidden

    access.call_set_view(AmsEditOverlay::VIEW_OVERVIEW);
    REQUIRE(lv_subject_get_int(hide_subj) == 0);
    // Overview: dirty-gated — a clean slot keeps Save disabled.
    CHECK(lv_subject_get_int(save_dis) == 1);

    close_editor_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "header Save on spool-edit finishes and closes for an untracked slot",
                 "[ams_edit_overlay][spool_edit][header_save]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    // No Spoolman -> the commit path skips any remote save and closes locally.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 0);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, untracked_slot(), nullptr,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fired = true;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);

    // Stage a color + weight edit in the spool-edit widgets.
    access.set_details_color(0x123456);
    lv_obj_t* remaining = access.widget("detail_field_remaining");
    lv_obj_t* spool_weight = access.widget("detail_field_spool_weight");
    REQUIRE(remaining != nullptr);
    REQUIRE(spool_weight != nullptr);
    lv_textarea_set_text(remaining, "654");
    lv_textarea_set_text(spool_weight, "987");

    // Header Save on spool-edit: applies the staged edits, then finishes + closes.
    access.call_handle_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.color_rgb == 0x123456);
    CHECK(captured.slot_info.remaining_weight_g == Catch::Approx(654.0f));
    CHECK(captured.slot_info.total_weight_g == Catch::Approx(987.0f));
    CHECK(captured.slot_info.spoolman_id == 0); // stayed untracked

    // Editor closed itself (go_back inside close_editor) — just settle the queue.
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tracked spool-edit header Save PATCHes logistics then commits and closes once",
                 "[ams_edit_overlay][spool_edit][header_save]") {
    // Task #12 seam: a tracked slot's header Save on the spool-edit view runs
    // the logistics PATCH (via the MoonrakerAPIMock, whose update callbacks fire
    // synchronously), then finishes through commit_and_close(). Assert the PATCH
    // landed and completion fired EXACTLY once — a blocking identity-confirm
    // modal or a double commit would leave fire_count != 1.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Seed the tracked spool so enter_spool_edit's fetch populates the logistics
    // fields from a known baseline (price 19.99).
    SpoolInfo seed;
    seed.id = 7;
    seed.filament_id = 3;
    seed.vendor = "Generic";
    seed.material = "PLA";
    seed.filament_name = "Navy";
    seed.color_hex = "#112233";
    seed.price = 19.99;
    seed.remaining_weight_g = 500.0;
    seed.spool_weight_g = 200.0;
    seed.initial_weight_g = 1000.0;
    api.spoolman_mock().get_mock_spools().push_back(seed);

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);

    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 7;
    slot.spoolman_filament_id = 3;
    slot.brand = "Generic";
    slot.material = "PLA";
    slot.spool_name = "Generic PLA";
    slot.color_rgb = 0x112233;
    slot.color_name = "Navy";
    slot.total_weight_g = 1000.0f;
    slot.remaining_weight_g = 500.0f;

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    int fire_count = 0;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, &api,
                                  [&](const AmsEditOverlay::EditResult& r) {
                                      fire_count++;
                                      captured = r;
                                  }));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain(); // applies the async Spoolman logistics fetch
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_EDIT);

    // Managed slot -> Save-to-Spoolman toggle defaults ON (stays tracked).
    lv_obj_t* toggle = access.widget("save_to_spoolman_switch");
    REQUIRE(toggle != nullptr);
    REQUIRE(lv_obj_has_state(toggle, LV_STATE_CHECKED));

    // Logistics-only edit: bump the price. Identity (material/color) untouched,
    // so no identity-confirm modal should interpose.
    lv_obj_t* price = access.widget("detail_field_price");
    REQUIRE(price != nullptr);
    lv_textarea_set_text(price, "29.99");

    access.call_handle_save(); // header Save on spool-edit -> finish=true
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Single completion fire, marked saved.
    CHECK(fire_count == 1);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 7); // stayed tracked

    // The logistics PATCH reached Spoolman with the new price.
    const auto& updates = api.spoolman_mock().spool_updates;
    bool price_patched = false;
    for (const auto& u : updates) {
        if (u.patch.contains("price") && std::abs(u.patch["price"].get<double>() - 29.99) < 0.001) {
            price_patched = true;
        }
    }
    CHECK(price_patched);

    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture, "weightless slot opens clean — no fabricated weights",
                 "[ams_edit_overlay][dirty]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    show_overlay_for_mock_slot_without_weights(*this); // SlotInfo with total/remaining = -1
    access.call_update_ui();

    REQUIRE_FALSE(access.is_dirty());
    auto* save_dis = lv_xml_get_subject(nullptr, "ams_edit_save_disabled");
    REQUIRE(save_dis != nullptr);
    REQUIRE(lv_subject_get_int(save_dis) == 1);             // Save stays disabled
    REQUIRE(access.working_info().total_weight_g <= 0);     // untouched
    REQUIRE(access.working_info().remaining_weight_g <= 0); // untouched

    close_editor_overlay();
}

TEST_CASE("build_spool_patches splits spool-level vs filament-level fields",
          "[ams_edit_overlay][spool_details]") {
    SpoolInfo original;
    original.id = 42;
    original.filament_id = 7;
    original.remaining_weight_g = 500.0;
    original.spool_weight_g = 140.0;
    original.price = 19.99;
    original.lot_nr = "LOT-A";
    original.location = "Shelf A";
    original.comment = "";
    original.color_hex = "#FF0000";

    SpoolInfo edited = original;
    edited.remaining_weight_g = 450.0;
    edited.spool_weight_g = 200.0;
    edited.price = 24.99;
    edited.lot_nr = "LOT-B";
    edited.location = "Shelf B";
    edited.comment = "dried 4h";
    edited.color_hex = "#00FF00";

    nlohmann::json spool_patch;
    nlohmann::json filament_patch;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, edited, spool_patch,
                                                      filament_patch);

    CHECK(spool_patch["remaining_weight"] == Catch::Approx(450.0));
    CHECK(spool_patch["price"] == Catch::Approx(24.99));
    CHECK(spool_patch["lot_nr"] == "LOT-B");
    CHECK(spool_patch["location"] == "Shelf B");
    CHECK(spool_patch["comment"] == "dried 4h");
    CHECK(spool_patch.count("spool_weight") == 0);

    CHECK(filament_patch["spool_weight"] == Catch::Approx(200.0));
    CHECK(filament_patch["color_hex"] == "#00FF00");
    CHECK(filament_patch.count("remaining_weight") == 0);

    // No changes -> both empty
    nlohmann::json empty_spool;
    nlohmann::json empty_filament;
    AmsEditOverlayViewTestAccess::build_spool_patches(original, original, empty_spool,
                                                      empty_filament);
    CHECK(empty_spool.empty());
    CHECK(empty_filament.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams edit overlay reclaims its widget tree on close and rebuilds on reopen",
                 "[ams_edit_overlay][lifecycle]") {
    // destroy-on-close: the overlay is large (~180 widgets) and opened only to
    // edit a spool, so its tree is torn down on close and transparently rebuilt
    // on the next open (memory reclaim for 111MB devices). Exercises a double
    // open/close cycle and the picker-entry path. Animations off so the close
    // callback runs synchronously and teardown is deterministic.
    DisplaySettingsManager::instance().set_animations_enabled(false);

    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolInfo linked;
    linked.id = 7;
    linked.filament_id = 3;
    linked.vendor = "Bambu Lab";
    linked.material = "ASA";
    linked.color_hex = "8A949E";
    api.spoolman_mock().get_mock_spools().push_back(linked);

    auto* subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(subj != nullptr);
    lv_subject_set_int(subj, 1);
    get_printer_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto& overlay = get_ams_edit_overlay();

    // --- Cycle 1: open ---
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* root1 = overlay.get_root();
    REQUIRE(root1 != nullptr);
    REQUIRE(lv_obj_is_valid(root1));

    // --- Cycle 1: close → the widget tree is reclaimed (deferred delete) ---
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(60);
    CHECK_FALSE(lv_obj_is_valid(root1));

    // --- Cycle 2: reopen → a FRESH tree (different root), fully functional ---
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* root2 = overlay.get_root();
    REQUIRE(root2 != nullptr);
    REQUIRE(lv_obj_is_valid(root2));
    // Deliberately NOT CHECK(root2 != root1): the allocator reuses the block the
    // first tree just released, so that comparison fails ~7 runs in 8 while the
    // teardown is working perfectly. Freshness is already established above by
    // CHECK_FALSE(lv_obj_is_valid(root1)) — the old tree really was reclaimed —
    // and by the functional assertions below, neither of which depends on where
    // the allocator happens to place the new root.
    AmsEditOverlayViewTestAccess access(overlay);
    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.widget("details_catalog_selector") != nullptr);

    // --- Cycle 2: close ---
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(60);
    CHECK_FALSE(lv_obj_is_valid(root2));

    // --- Picker-entry path (opened_on_picker_) opens + reclaims cleanly ---
    REQUIRE(overlay.show_for_slot(test_screen(), 0, tracked_slot(), &api, nullptr,
                                  /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    lv_obj_t* root3 = overlay.get_root();
    REQUIRE(root3 != nullptr);
    REQUIRE(lv_obj_is_valid(root3));
    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(60);
    CHECK_FALSE(lv_obj_is_valid(root3));

    DisplaySettingsManager::instance().set_animations_enabled(true);
    get_printer_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(OverlayConsumerCommitFixture,
                 "picker-entry link on an untracked slot with Spoolman available commits cleanly",
                 "[ams_edit_overlay][filament_picker][picker][spoolman][header_save]") {
    // Companion to the tracked-relink picker-entry test (8e23fbc23 covers
    // A>0 -> B>0). This exercises the OTHER relink branch — 0 -> B>0 — with
    // Spoolman AVAILABLE. The active-spool registration is the completion
    // consumer's job now (commit_slot_edit), so the callback below mirrors the
    // production consumer the way the sibling active-spool tests do. Assert:
    // one-tap commit + close, slot linked to B, active spool set on the server,
    // NO identity dialog, and no spurious identity PATCH (a fresh link is not
    // an edit).

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    REQUIRE(spoolman_subj != nullptr);
    lv_subject_set_int(spoolman_subj, 1);
    get_printer_state().set_spoolman_available(true);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    bool fired = false;
    AmsEditOverlay::EditResult captured;
    REQUIRE(overlay.show_for_slot(
        test_screen(), 0, untracked_slot(), &api,
        [&](const AmsEditOverlay::EditResult& r) {
            fired = true;
            captured = r;
            commit_like_consumer(r); // the consumer commit owns the server sync
        },
        /*open_on_picker=*/true));
    UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(access.view() == AmsEditOverlay::VIEW_SPOOL_PICKER);

    access.set_cached_spools(two_spools());
    access.call_render_spool_list("");
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Link the untracked slot to spool #22 (eSUN PETG).
    access.call_handle_spool_selected(22);
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // No "Different filament?" confirm — a fresh link is a pure link switch.
    CHECK(ModalStack::instance().stack_empty());
    // Committed + closed in one step, linked to B.
    CHECK(fired);
    CHECK(captured.saved);
    CHECK(captured.slot_info.spoolman_id == 22);
    CHECK(captured.slot_info.material == "PETG");
    // No identity/weight PATCH — linking is not editing.
    CHECK(api.spoolman_mock().spool_updates.empty());
    CHECK(api.spoolman_mock().filament_updates.empty());
    // The newly linked spool is registered active on the server.
    CHECK(api.spoolman_mock().get_mock_active_spool_id() == 22);

    get_printer_state().set_spoolman_available(false);
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

// =============================================================================
// Catalog product identity round-trip (bundle TDQCCQB3, AD5X v0.99.107)
// =============================================================================
//
// The user edits an AMS slot, picks SUNLU "PLA+ 2.0", and Save reports success.
// Reopening the editor shows "PLA Marble". Brand and material family DO persist,
// which is what made it look like a reload bug — it is not. The catalog product
// identity was never captured at SAVE:
//
//   - handle_spool_edit_save() read the highlighted EffectiveFilament but copied
//     only type / brand / the three temps into working_info_. ef->name and
//     ef->id were read nowhere.
//   - clear_catalog() then wiped the selector-local highlighted_id_.
//   - On reopen, setup_details_selector() seeded vendor + material family only,
//     and preselect_first() took ordered_products_for().front(). All six SUNLU
//     PLA products share variant_key "" and rank 1, so the tiebreak is
//     lowercased-name alphabetical and "pla marble" always sorts first.
//
// Deterministic, which is why this test can assert an exact product id.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool-edit Save persists the picked catalog product across a reopen",
                 "[ams_edit_overlay][spool_edit][catalog_identity]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 0;
    slot.brand = "SUNLU";
    slot.material = "PLA";
    slot.color_rgb = 0xFEF043;
    slot.color_name = "Yellow";

    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Sanity: the vendor seed resolved, and the list really does put the wrong
    // product first — so a passing assertion below cannot be an accident of
    // ordering.
    REQUIRE(access.details_selector().current_vendor() == "SUNLU");
    REQUIRE(access.details_selector().current_type() == "PLA");
    REQUIRE(access.details_selector().product_names_for_test().front() == "PLA Marble");

    // The user taps the "PLA+ 2.0" row.
    access.details_selector().select_product_for_test("sunlu-pla-plus-2-0");
    REQUIRE(access.details_selector().highlighted() != nullptr);
    REQUIRE(access.details_selector().highlighted()->name == "PLA+ 2.0");

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Save must capture the product identity, not just the material family.
    const SlotInfo saved = access.working_info();
    CHECK(saved.catalog_id == "sunlu-pla-plus-2-0");
    CHECK(saved.product_name == "PLA+ 2.0");
    CHECK(saved.material == "PLA"); // family still the firmware-facing string
    CHECK(saved.brand == "SUNLU");

    close_editor_overlay();

    // --- Reopen with exactly what was persisted -------------------------------
    REQUIRE(overlay.show_for_slot(test_screen(), 0, saved, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // THE regression assertion: the same product is checked, not the
    // alphabetically-first one.
    REQUIRE(access.details_selector().highlighted() != nullptr);
    CHECK(access.details_selector().highlighted()->id == "sunlu-pla-plus-2-0");
    CHECK(access.details_selector().highlighted()->name == "PLA+ 2.0");
    CHECK(access.details_selector().current_vendor() == "SUNLU");

    // And an untouched Save round-trips it rather than re-collapsing to first.
    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(access.working_info().catalog_id == "sunlu-pla-plus-2-0");
    CHECK(access.working_info().product_name == "PLA+ 2.0");

    close_editor_overlay();
}

// A stored id that no longer resolves (a custom overlay product the user
// deleted, or an id retired by an app update) must not strand the editor: the
// selector falls back to preselect_first() so the list still shows a checked
// row, and the SAVE overwrites the dead id with whatever the user confirms.
// The old product_name is not preserved through such a save — the user is
// looking at, and confirming, a different product.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool-edit tolerates a stored catalog id that no longer resolves",
                 "[ams_edit_overlay][spool_edit][catalog_identity]") {
    auto& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access(overlay);

    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 0;
    slot.brand = "SUNLU";
    slot.material = "PLA";
    slot.color_rgb = 0xFEF043;
    slot.catalog_id = "sunlu-pla-plus-9-9-retired";
    slot.product_name = "PLA+ 9.9";

    REQUIRE(overlay.show_for_slot(test_screen(), 0, slot, nullptr, nullptr));
    UpdateQueue::instance().drain();
    process_lvgl(10);

    access.call_enter_spool_edit();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // Fallback path: a checked row (the preselect_on_change invariant) on the
    // right vendor+family, just not the dead id.
    REQUIRE(access.details_selector().highlighted() != nullptr);
    CHECK(access.details_selector().current_vendor() == "SUNLU");
    CHECK(access.details_selector().highlighted()->id != "sunlu-pla-plus-9-9-retired");

    access.call_handle_spool_edit_save();
    UpdateQueue::instance().drain();
    process_lvgl(10);

    // The dead id is replaced by the product the user actually confirmed —
    // never carried forward, or the lane would advertise an id nothing resolves
    // for the rest of its life.
    CHECK(access.working_info().catalog_id != "sunlu-pla-plus-9-9-retired");
    CHECK_FALSE(access.working_info().catalog_id.empty());
    CHECK(access.working_info().product_name != "PLA+ 9.9");

    close_editor_overlay();
}
