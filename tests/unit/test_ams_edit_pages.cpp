// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lane spool editor's pages: how each one is reached and left, and the
// jobs that finish on them (Clear Lane and its archive, Same Filament, the Add
// to Spoolman box, Use moving a spool between lanes).

#include "ui_ams_edit_overlay.h"
#include "ui_ams_lane_settings.h"
#include "ui_color_picker_page.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"

#include "../test_helpers/ams_edit_overlay_view_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/toast_log.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "config.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "filament_catalog.h"
#include "filament_favorites.h"
#include "filament_variants.h"
#include "lane_translation.h"
#include "moonraker_client_mock_spoolman.h"
#include "theme_manager.h"

#include <cstdlib>
#include <optional>

#include "../catch_amalgamated.hpp"

namespace {

/// A lane holding a described spool, as a user edit leaves it.
SlotInfo describe_lane(AmsBackendMock& backend, int slot, const char* material) {
    SlotInfo info = backend.get_slot_info(slot);
    info.material = material;
    info.brand = "eSUN";
    info.color_rgb = 0x2050C0;
    info.status = SlotStatus::AVAILABLE;
    REQUIRE(helix::test::apply_edit(backend, slot, info).success());
    return backend.get_slot_info(slot);
}

struct PagesFixture : OverlayConsumerCommitFixture {
    AmsEditOverlay& overlay = get_ams_edit_overlay();
    AmsEditOverlayViewTestAccess access{overlay};
    std::optional<AmsEditOverlay::EditResult> result;

    ~PagesFixture() override {
        ModalStack::instance().clear();
        set_spoolman_available(false);
    }

    void open(int slot, const SlotInfo& info, bool open_on_picker = false) {
        REQUIRE(overlay.show_for_slot(
            test_screen(), slot, info, &api,
            [this](const AmsEditOverlay::EditResult& r) {
                result = r;
                commit_like_consumer(r);
            },
            open_on_picker));
        settle();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(10);
        UpdateQueue::instance().drain();
    }

    void back() {
        access.call_handle_back();
        settle();
    }
};

} // namespace

TEST_CASE_METHOD(PagesFixture, "each page's Back returns to the page that opened it",
                 "[ams_edit_overlay][pages]") {
    const SlotInfo lane = describe_lane(*backend, 1, "PLA");
    open(1, lane);
    REQUIRE(access.page() == AmsEditOverlay::kPageSummary);

    // Filament: the pick is held for the summary's Save.
    access.click("summary_card");
    REQUIRE(access.page() == AmsEditOverlay::kPageFilament);
    REQUIRE(access.pick_dropdown("dropdown_material", "PETG"));
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.subject_text("ams_edit_material_line").rfind("PETG", 0) == 0);
    CHECK(access.is_dirty());

    // Lane Settings.
    access.click("lane_line");
    REQUIRE(access.page() == AmsEditOverlay::kPageLane);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);

    // New Spool, and Back to the lane's own spool with the held pick.
    access.click("btn_hdr_new_spool");
    REQUIRE(access.page() == AmsEditOverlay::kPageList);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.mode() == AmsEditOverlay::kModeEdit);
    CHECK(access.subject_text("ams_edit_material_line").rfind("PETG", 0) == 0);

    // The summary's Back asks about the held edits; Discard closes unsaved.
    back();
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    helix::test::answer_top_modal("btn_secondary");
    settle();
    REQUIRE(result.has_value());
    CHECK_FALSE(result->saved);
}

TEST_CASE_METHOD(PagesFixture, "an empty lane's New Spool pages lead back to it",
                 "[ams_edit_overlay][pages]") {
    SlotInfo ran_out = describe_lane(*backend, 1, "PLA");
    ran_out.status = SlotStatus::EMPTY;
    open(1, ran_out);
    REQUIRE(access.page() == AmsEditOverlay::kPageEmpty);
    CHECK(access.subject_text("ams_edit_empty_title") == "Gate 2 is empty");

    access.click("btn_choose_spool");
    REQUIRE(access.page() == AmsEditOverlay::kPageList);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageEmpty);

    // Not Listed? Add New Spool: a full 1 kg Generic spool, held on the summary.
    access.click("btn_choose_spool");
    access.click("btn_add_new");
    REQUIRE(access.page() == AmsEditOverlay::kPageFilament);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.adding() == 1);
    CHECK(access.subject_text("ams_edit_weight_text") == "1000 g of 1 kg · 100%");
    CHECK(access.subject_text("ams_edit_product_line") == "Generic");
    CHECK(access.subject_text("ams_edit_material_line").rfind("PLA", 0) == 0);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageEmpty);
    CHECK(access.adding() == 0);
    CHECK_FALSE(result.has_value());
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Clear Lane asks first and archives the spool only when ticked",
                 "[ams_edit_overlay][pages][clear_spool]") {
    set_spoolman_available(true);
    auto& spool = client.spoolman_mock().get_mock_spools();
    spool.clear();
    SpoolInfo record;
    record.id = 40;
    record.vendor = "eSUN";
    record.material = "PLA";
    record.color_hex = "2050C0";
    spool.push_back(record);

    SlotInfo lane = describe_lane(*backend, 1, "PLA");
    lane.spoolman_id = 40;
    lane.total_weight_g = 1000;
    bool ticked = false;
    bool archive = false;
    SECTION("all but gone: ticked, and archived") {
        lane.remaining_weight_g = 15;
        ticked = archive = true;
    }
    SECTION("ran out while linked: ticked, and archived") {
        lane.remaining_weight_g = 300;
        lane.status = SlotStatus::EMPTY;
        ticked = archive = true;
    }
    SECTION("plenty left: unticked, and kept") {
        lane.remaining_weight_g = 600;
    }
    SECTION("plenty left, ticked by hand: archived") {
        lane.remaining_weight_g = 600;
        archive = true;
    }
    REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
    open(1, lane);
    REQUIRE(access.clear_visible() == 1);
    // A ran-out lane opens on its empty page, which has its own Clear.
    const bool ran_out = lane.status == SlotStatus::EMPTY;
    CHECK(access.page() == (ran_out ? AmsEditOverlay::kPageEmpty : AmsEditOverlay::kPageSummary));
    const char* clear = ran_out ? "btn_clear_empty" : "btn_clear_spool";
    REQUIRE(access.shown(clear));
    access.click(clear);
    settle();
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    CHECK(helix::test::top_modal_text().find("Clear Gate 2?") != std::string::npos);
    lv_obj_t* check =
        lv_obj_find_by_name(ModalStack::instance().top_dialog(), "clear_archive_check");
    REQUIRE(check != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(check));
    CHECK(lv_obj_has_state(check, LV_STATE_CHECKED) == ticked);
    if (archive != ticked) {
        lv_obj_add_state(check, LV_STATE_CHECKED);
        lv_obj_send_event(check, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    helix::test::answer_top_modal("btn_primary");
    settle();

    CHECK(backend->get_slot_info(1).spoolman_id == 0);
    CHECK(backend->get_slot_info(1).material.empty());
    CHECK(access.page() == AmsEditOverlay::kPageEmpty);
    CHECK(access.subject_text("ams_edit_last_material").rfind("PLA", 0) == 0);
    bool archived = false;
    for (const auto& update : client.spoolman_mock().spool_updates) {
        if (update.spool_id == 40 && update.patch.value("archived", false)) {
            archived = true;
        }
    }
    CHECK(archived == archive);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Cancel and an untracked spool's Clear leave Spoolman alone",
                 "[ams_edit_overlay][pages][clear_spool]") {
    const SlotInfo lane = describe_lane(*backend, 1, "PLA");
    open(1, lane);
    access.click("btn_clear_spool");
    settle();
    lv_obj_t* check =
        lv_obj_find_by_name(ModalStack::instance().top_dialog(), "clear_archive_check");
    REQUIRE(check != nullptr);
    CHECK(lv_obj_is_hidden(check));

    helix::test::answer_top_modal("btn_secondary");
    settle();
    CHECK(ModalStack::instance().stack_empty());
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(backend->get_slot_info(1).material == "PLA");

    access.click("btn_clear_spool");
    settle();
    helix::test::answer_top_modal("btn_primary");
    settle();
    CHECK(backend->get_slot_info(1).material.empty());
    CHECK(client.spoolman_mock().spool_updates.empty());
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "Same Filament, New Spool assigns a full spool of what the lane held",
                 "[ams_edit_overlay][pages][same_filament]") {
    SlotInfo ran_out = describe_lane(*backend, 1, "PETG");
    ran_out.remaining_weight_g = 0;
    ran_out.total_weight_g = 750;
    ran_out.status = SlotStatus::EMPTY;

    int created = 0;
    SECTION("without Spoolman: the lane alone") {
        set_spoolman_available(false);
    }
    SECTION("with Spoolman: a new Spoolman spool") {
        set_spoolman_available(true);
        client.spoolman_mock().get_mock_spools().clear();
        client.spoolman_mock().next_created_filament_id = 117;
        client.spoolman_mock().next_created_spool_id = 152;
        created = 152;
    }
    open(1, ran_out);
    REQUIRE(access.page() == AmsEditOverlay::kPageEmpty);
    REQUIRE(access.shown("btn_same_filament"));
    access.click("btn_same_filament");
    settle();
    settle();

    REQUIRE(result.has_value());
    CHECK(result->saved);
    const SlotInfo now = backend->get_slot_info(1);
    CHECK(now.material == "PETG");
    CHECK(now.brand == "eSUN");
    CHECK(now.color_rgb == 0x2050C0U);
    CHECK(now.remaining_weight_g == Catch::Approx(1000.0f));
    CHECK(now.total_weight_g == Catch::Approx(1000.0f));
    CHECK(now.spoolman_id == created);
    CHECK(client.spoolman_mock().created_spools.size() == (created ? 1U : 0U));
}

TEST_CASE_METHOD(PagesFixture, "Save on a new spool follows the Add to Spoolman box",
                 "[ams_edit_overlay][pages][add_to_spoolman]") {
    set_spoolman_available(true);
    client.spoolman_mock().get_mock_spools().clear();
    client.spoolman_mock().next_created_filament_id = 117;
    client.spoolman_mock().next_created_spool_id = 152;
    open(1, SlotInfo{});
    access.click("btn_choose_spool");
    access.click("btn_add_new");
    REQUIRE(access.shown("add_to_spoolman_check"));
    CHECK(lv_obj_has_state(access.widget("add_to_spoolman_check"), LV_STATE_CHECKED));
    REQUIRE(access.pick_dropdown("dropdown_material", "ABS"));
    access.call_apply_color(0xFFFFFF);

    bool ticked = true;
    SECTION("ticked: filed in Spoolman") {}
    SECTION("unticked: the lane alone") {
        ticked = false;
        lv_obj_t* box = access.widget("add_to_spoolman_check");
        lv_obj_remove_state(box, LV_STATE_CHECKED);
        lv_obj_send_event(box, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    back();
    access.click("btn_edit_save");
    settle();
    settle();

    REQUIRE(result.has_value());
    CHECK(result->saved);
    CHECK(backend->get_slot_info(1).material == "ABS");
    CHECK(backend->get_slot_info(1).spoolman_id == (ticked ? 152 : 0));
    CHECK(client.spoolman_mock().created_spools.size() == (ticked ? 1U : 0U));
}

TEST_CASE_METHOD(PagesFixture, "Use moves a spool another lane holds",
                 "[ams_edit_overlay][pages][picker]") {
    SlotInfo lane3 = describe_lane(*backend, 3, "PETG");
    lane3.spoolman_id = 22;
    REQUIRE(helix::test::apply_edit(*backend, 3, lane3).success());
    REQUIRE(backend->get_slot_info(3).spoolman_id == 22);

    open(0, backend->get_slot_info(0), /*open_on_picker=*/true);
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
    access.set_cached_spools({a, b});
    access.use_spool(22);
    settle();

    REQUIRE(result.has_value());
    CHECK(result->saved);
    CHECK(backend->get_slot_info(0).spoolman_id == 22);
    CHECK(backend->get_slot_info(3).spoolman_id == 0);
}

TEST_CASE_METHOD(PagesFixture,
                 "the Filament row's product says the material; Material shows only without one",
                 "[ams_edit_overlay][pages][filament_rows]") {
    // A brand with products of two different materials, from the catalog itself.
    const auto catalog = helix::printer::FilamentCatalog::load_full();
    std::string brand;
    const helix::printer::EffectiveFilament* first = nullptr;
    const helix::printer::EffectiveFilament* other = nullptr;
    for (const auto& b : catalog.all_brands()) {
        if (b == "Generic") {
            continue;
        }
        const auto lines = catalog.products_for_brand(b);
        for (const auto* ef : lines) {
            if (lines.front()->type != ef->type) {
                brand = b;
                first = lines.front();
                other = ef;
                break;
            }
        }
        if (other) {
            break;
        }
    }
    REQUIRE(other != nullptr);

    open(1, describe_lane(*backend, 1, "PLA"));
    access.open_page(AmsEditOverlay::kPageFilament);
    settle();
    REQUIRE(access.pick_dropdown("dropdown_brand", brand));
    settle();
    // A brand with no product picked yet: Material says what the spool is.
    CHECK(access.shown("dropdown_product"));
    CHECK(access.shown("dropdown_material"));

    // A product carries its material and type, and Material steps aside.
    REQUIRE(access.pick_dropdown("dropdown_product", other->name));
    settle();
    CHECK_FALSE(access.shown("dropdown_material"));
    CHECK(access.sel().family == filament::display_family(other->type));
    CHECK(access.sel().product_id == other->id);
    REQUIRE(access.pick_dropdown("dropdown_product", first->name));
    settle();
    CHECK(access.sel().family == filament::display_family(first->type));

    // "Other…": described by Material again.
    REQUIRE(access.pick_dropdown("dropdown_product", "Other…"));
    settle();
    CHECK(access.sel().product_id.empty());
    CHECK(access.shown("dropdown_material"));

    // Generic: no Product; one Material list of precise types beside Brand,
    // each family's types in the catalog's order.
    REQUIRE(access.pick_dropdown("dropdown_brand", "Generic"));
    settle();
    CHECK_FALSE(access.shown("dropdown_product"));
    CHECK(access.shown("dropdown_material"));
    CHECK(access.widget("dropdown_variant") == nullptr);
    std::vector<std::string> expected;
    for (const auto& v : catalog.variants_in_family("ASA")) {
        expected.push_back(filament::compose_family_type("ASA", v.label));
    }
    REQUIRE(expected.size() > 1);
    const std::string options = "\n" + access.dropdown_options("dropdown_material") + "\n";
    size_t at = 0;
    for (const auto& type : expected) {
        INFO(type);
        at = options.find("\n" + type + "\n", at);
        REQUIRE(at != std::string::npos);
    }

    // One pick says the precise type; the lane keeps it, and the material a
    // slicer matches reduces from it.
    const std::string precise = expected.back();
    REQUIRE(access.pick_dropdown("dropdown_material", precise));
    settle();
    CHECK(access.sel().family == "ASA");
    back();
    CHECK(access.subject_text("ams_edit_material_line").rfind(precise + " · ", 0) == 0);
    // Save stages the pick into the lane record it writes.
    access.call_handle_save();
    settle();
    CHECK(access.working_info().material == precise);
    CHECK(filament::slicer_material(precise) == "ASA");
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "the Filament row's reset shows only while the lane differs from its tag",
                 "[ams_edit_overlay][pages][filament_rows]") {
    const SlotInfo lane = describe_lane(*backend, 1, "PLA");
    open(1, lane);
    access.open_page(AmsEditOverlay::kPageFilament);
    helix::ams::TagReading tag;
    tag.material = "PLA";
    tag.brand = "eSUN";
    tag.color_rgb = lane.color_rgb;
    access.set_tag(tag);
    access.call_update_ui();
    settle();
    CHECK_FALSE(access.shown("btn_reset_tag"));

    tag.color_rgb = 0xFF0000;
    access.set_tag(tag);
    access.call_update_ui();
    settle();
    REQUIRE(access.shown("btn_reset_tag"));
    const lv_area_t hit = access.click_area("btn_reset_tag");
    CHECK(lv_area_get_width(&hit) >= 44);
    CHECK(lv_area_get_height(&hit) >= 44);
    // Its tap puts the tag's color back.
    access.click("btn_reset_tag");
    settle();
    CHECK(access.working_info().color_rgb == 0xFF0000U);
    close_editor_overlay();
}

namespace {
/// The machine runs a job for one case, and is free again after it.
struct ScopedPrinting {
    ScopedPrinting() {
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
    }
    ~ScopedPrinting() {
        helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::STANDBY);
    }
};

/// Spoolman knows spool @p id.
void seed_spool(MoonrakerClientMock& client, int id) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    spools.clear();
    SpoolInfo record;
    record.id = id;
    record.vendor = "eSUN";
    record.material = "PLA";
    record.color_hex = "2050C0";
    spools.push_back(record);
}

bool archived(MoonrakerClientMock& client, int id) {
    for (const auto& update : client.spoolman_mock().spool_updates) {
        if (update.spool_id == id && update.patch.value("archived", false)) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE_METHOD(
    PagesFixture,
    "Clear on the lane the print draws from is greyed, and a stale confirm archives nothing",
    "[ams_edit_overlay][pages][clear_spool][review]") {
    set_spoolman_available(true);
    seed_spool(client, 40);
    REQUIRE(backend->slot_is_actively_loaded(0));
    SlotInfo lane = describe_lane(*backend, 0, "PLA");
    lane.spoolman_id = 40;
    lane.remaining_weight_g = 15; // all but gone: the archive box opens ticked
    lane.total_weight_g = 1000;
    REQUIRE(helix::test::apply_edit(*backend, 0, lane).success());

    SECTION("mid-print: greyed with the reason") {
        ScopedPrinting printing;
        open(0, backend->get_slot_info(0));
        REQUIRE(access.shown("btn_clear_spool"));
        CHECK(lv_obj_has_state(access.widget("btn_clear_spool"), LV_STATE_DISABLED));
        CHECK(access.shown("clear_hint"));
        CHECK(access.subject_text("ams_edit_clear_hint").find("feeding the current print") !=
              std::string::npos);
    }
    SECTION("a confirmation opened before the print started") {
        open(0, backend->get_slot_info(0));
        CHECK_FALSE(lv_obj_has_state(access.widget("btn_clear_spool"), LV_STATE_DISABLED));
        access.click("btn_clear_spool");
        settle();
        REQUIRE_FALSE(ModalStack::instance().stack_empty());
        ScopedPrinting printing;
        helix::test::answer_top_modal("btn_primary");
        settle();
        // Refused: the lane keeps its spool, the editor its summary, Spoolman the spool.
        CHECK(backend->get_slot_info(0).spoolman_id == 40);
        CHECK(access.page() == AmsEditOverlay::kPageSummary);
        CHECK_FALSE(archived(client, 40));
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(
    PagesFixture,
    "the archive box follows the lane's own spool and Spoolman, and archives only when seen",
    "[ams_edit_overlay][pages][clear_spool][review]") {
    seed_spool(client, 40);
    SlotInfo lane = describe_lane(*backend, 1, "PLA");
    lane.spoolman_id = 40;
    lane.remaining_weight_g = 15;
    lane.total_weight_g = 1000;
    REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
    bool spoolman = true;
    SECTION("Spoolman there: the box shows, ticked") {
        set_spoolman_available(true);
    }
    SECTION("Spoolman gone: no box, and no archive") {
        spoolman = false;
        set_spoolman_available(false);
    }
    open(1, backend->get_slot_info(1));
    // The working spool moving off the link does not hide the lane's box.
    access.set_working_info(SlotInfo{});
    access.click("btn_clear_spool");
    settle();
    lv_obj_t* check =
        lv_obj_find_by_name(ModalStack::instance().top_dialog(), "clear_archive_check");
    REQUIRE(check != nullptr);
    CHECK(lv_obj_is_hidden(check) == !spoolman);
    CHECK(lv_obj_has_state(check, LV_STATE_CHECKED) == spoolman);
    helix::test::answer_top_modal("btn_primary");
    settle();
    CHECK(backend->get_slot_info(1).spoolman_id == 0);
    CHECK(archived(client, 40) == spoolman);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Use never moves a spool off the lane the print draws from",
                 "[ams_edit_overlay][pages][picker][review]") {
    REQUIRE(backend->slot_is_actively_loaded(0));
    SlotInfo lane0 = describe_lane(*backend, 0, "PETG");
    lane0.spoolman_id = 22;
    REQUIRE(helix::test::apply_edit(*backend, 0, lane0).success());
    ScopedPrinting printing;
    open(1, backend->get_slot_info(1), /*open_on_picker=*/true);
    SpoolInfo b;
    b.id = 22;
    b.vendor = "eSUN";
    b.material = "PETG";
    access.set_cached_spools({b});
    access.call_render_spool_list("");
    settle();
    lv_obj_t* row = lv_obj_get_child(access.widget("picker_spool_list"), 0);
    REQUIRE(row != nullptr);
    CHECK(lv_obj_has_state(lv_obj_find_by_name(row, "row_use"), LV_STATE_DISABLED));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(row, "row_detail")))
              .find("feeding the print") != std::string::npos);
    // Even a tap that gets through leaves both lanes as they were.
    access.use_spool(22);
    settle();
    CHECK(backend->get_slot_info(0).spoolman_id == 22);
    CHECK(backend->get_slot_info(1).spoolman_id != 22);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "the weight steps, drags and types from the value the summary shows",
                 "[ams_edit_overlay][pages][weight][review]") {
    SlotInfo lane = describe_lane(*backend, 1, "PLA");
    lane.remaining_weight_g = -1; // unknown: shown as a full spool of the default size
    lane.total_weight_g = -1;
    REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
    open(1, backend->get_slot_info(1));
    REQUIRE(access.subject_text("ams_edit_weight_text") == "1000 g of 1 kg · 100%");
    SECTION("minus steps down from full") {
        access.click("btn_remaining_minus");
        CHECK(access.working_info().remaining_weight_g == Catch::Approx(990.0f));
        CHECK(access.working_info().total_weight_g == Catch::Approx(1000.0f));
    }
    SECTION("the drag bar is a share of the default size") {
        lv_obj_t* bar = access.widget("weight_slider");
        lv_slider_set_value(bar, 50, LV_ANIM_OFF);
        lv_obj_send_event(bar, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(access.working_info().remaining_weight_g == Catch::Approx(500.0f));
        CHECK(access.working_info().total_weight_g == Catch::Approx(1000.0f));
    }
    SECTION("a new spool steps from its full 1 kg") {
        access.click("btn_hdr_new_spool");
        access.click("btn_add_new");
        access.call_handle_back();
        access.click("btn_remaining_minus");
        CHECK(access.subject_text("ams_edit_weight_text") == "990 g of 1 kg · 99%");
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "New Spool and Back return to the summary with its held edits",
                 "[ams_edit_overlay][pages][review]") {
    open(1, describe_lane(*backend, 1, "PLA"));
    access.click("summary_card");
    REQUIRE(access.pick_dropdown("dropdown_brand", "Generic"));
    REQUIRE(access.pick_dropdown("dropdown_material", "PETG"));
    back();
    access.call_apply_weight(321);
    REQUIRE(access.is_dirty());
    access.click("btn_hdr_new_spool");
    REQUIRE(access.page() == AmsEditOverlay::kPageList);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.mode() == AmsEditOverlay::kModeEdit);
    CHECK(access.sel().family == "PETG");
    CHECK(access.working_info().remaining_weight_g == Catch::Approx(321.0f));
    CHECK(access.is_dirty());
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "None is offered as the tool only to a lane that has no tool",
                 "[ams_edit_overlay][pages][lane_settings][review]") {
    open(1, describe_lane(*backend, 1, "PLA"));
    REQUIRE(helix::ui::lane_settings::assigned_tool(*backend, 1) >= 0);
    access.open_page(AmsEditOverlay::kPageLane);
    const std::string options = access.dropdown_options("tool_dropdown");
    CHECK(options.rfind("None", 0) != 0);
    // The selection still names the lane's own tool.
    CHECK(access.dropdown_selected("tool_dropdown") ==
          helix::ui::tool_label(helix::ui::lane_settings::assigned_tool(*backend, 1)));
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "the color row builds the swatch cap the XML names and shows no more",
                 "[ams_edit_overlay][pages][color][review]") {
    open(1, describe_lane(*backend, 1, "PLA"));
    access.open_page(AmsEditOverlay::kPageFilament);
    settle();
    const char* cap_text =
        lv_xml_get_const(lv_xml_component_get_scope("ams_edit_overlay"), "ams_edit_swatch_cap");
    REQUIRE(cap_text != nullptr);
    const int cap = std::atoi(cap_text);
    lv_obj_t* row = access.widget("color_row");
    REQUIRE(row != nullptr);
    // The dots, the cap's swatches, the rainbow tile.
    CHECK(static_cast<int>(lv_obj_get_child_count(row)) == cap + 2);
    CHECK(static_cast<int>(access.color_row().size()) <= cap);
    CHECK(access.row_swatches().size() == access.color_row().size());
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "the summary's Save sits at the right whether Clear Lane shows or not",
                 "[ams_edit_overlay][pages][clear_spool]") {
    lv_display_t* disp = lv_display_get_default();
    const SlotInfo lane = describe_lane(*backend, 1, "PLA");
    for (const auto& [w, h] : {std::pair{480, 272}, std::pair{800, 480}}) {
        CAPTURE(w, h);
        ScopedResolution res(disp, w, h);
        theme_manager_refresh_layout_constants(disp);
        open(1, lane);
        REQUIRE(access.page() == AmsEditOverlay::kPageSummary);
        lv_obj_t* page = access.widget("summary_page");
        lv_obj_t* save = access.widget("btn_edit_save");
        REQUIRE(page != nullptr);
        REQUIRE(save != nullptr);
        const auto save_right_gap = [&] {
            lv_obj_update_layout(page);
            lv_area_t page_box;
            lv_area_t save_box;
            lv_obj_get_content_coords(page, &page_box);
            lv_obj_get_coords(save, &save_box);
            return page_box.x2 - save_box.x2;
        };

        // Clear Lane shown, at the left.
        REQUIRE(access.shown("btn_clear_spool"));
        CHECK(save_right_gap() == 0);

        // Clear Lane hidden: Save alone in the row.
        lv_subject_t* visible = lv_xml_get_subject(nullptr, "ams_edit_clear_visible");
        REQUIRE(visible != nullptr);
        lv_subject_set_int(visible, 0);
        settle();
        REQUIRE_FALSE(access.shown("btn_clear_spool"));
        REQUIRE_FALSE(access.shown("clear_hint"));
        CHECK(save_right_gap() == 0);
        close_editor_overlay();
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(PagesFixture,
                 "a Clear confirmation left open over another lane's editor does nothing",
                 "[ams_edit_overlay][pages][clear_spool][review]") {
    const SlotInfo lane1 = describe_lane(*backend, 1, "PLA");
    const SlotInfo lane2 = describe_lane(*backend, 2, "PETG");
    open(1, lane1);
    access.click("btn_clear_spool");
    settle();
    REQUIRE_FALSE(ModalStack::instance().stack_empty());
    // The editor moves to another lane under the open confirmation.
    open(2, lane2);
    helix::test::answer_top_modal("btn_primary");
    settle();
    CHECK(backend->get_slot_info(1).material == "PLA");
    CHECK(backend->get_slot_info(2).material == "PETG");
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "Multi-Color lives on the color page the editor opens, and only there",
                 "[ams_edit_overlay][pages][multi_color]") {
    open(0, describe_lane(*backend, 0, "PLA"));
    REQUIRE(access.subject_text("ams_edit_material_line").rfind("PLA", 0) == 0);
    access.open_page(AmsEditOverlay::kPageFilament);
    settle();
    // The Filament strip has no "+": adding colors is the color page's.
    CHECK_FALSE(access.shown("btn_add_color"));

    access.call_open_color_page();
    settle();
    auto& page = helix::ui::get_color_picker_page();
    REQUIRE(page.view().attached());
    lv_subject_t* host = lv_xml_get_subject(nullptr, "color_picker_host_controls");
    REQUIRE(host != nullptr);
    CHECK(lv_subject_get_int(host) == 1);
    lv_obj_t* sw = lv_obj_find_by_name(page.get_root(), "multi_color_switch");
    REQUIRE(sw != nullptr);
    CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));

    // On: a second color, and the picker edits it.
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    std::vector<uint32_t> colors = access.draft_colors();
    REQUIRE(colors.size() == 2);
    CHECK(page.view().color() == colors[1]);
    // Tapping the first dot points the picker at it.
    lv_obj_t* dots = lv_obj_find_by_name(page.get_root(), "dots");
    REQUIRE(dots != nullptr);
    lv_obj_send_event(lv_obj_get_child(dots, 0), LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(page.view().color() == colors[0]);
    // Off: back to one color, the one being edited.
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(access.draft_colors() == std::vector<uint32_t>{colors[0]});

    NavigationManager::instance().go_back();
    settle();
    process_lvgl(400);
    close_editor_overlay();

    // Any other host (LEDs, themes, Spoolman) opens the bare picker.
    helix::ui::ColorPickerConfig cfg;
    cfg.palette = helix::ColorPalette::Led;
    cfg.strip = helix::ui::ColorStrip::LedRecents;
    page.open(test_screen(), 0xFF0000, cfg, nullptr, nullptr);
    settle();
    CHECK(lv_subject_get_int(host) == 0);
    CHECK(lv_obj_find_by_name(page.get_root(), "multi_color_switch") == nullptr);
    NavigationManager::instance().go_back();
    settle();
    process_lvgl(400);
}

namespace {
/// What a finger can reach of @p w: its click area, cut by every ancestor's
/// box, since LVGL hit-tests a child only inside its parent.
lv_area_t reachable_area(lv_obj_t* w) {
    lv_obj_update_layout(w);
    lv_area_t a;
    lv_obj_get_click_area(w, &a);
    for (lv_obj_t* p = lv_obj_get_parent(w); p != nullptr; p = lv_obj_get_parent(p)) {
        lv_area_t box;
        lv_obj_get_coords(p, &box);
        a = {LV_MAX(a.x1, box.x1), LV_MAX(a.y1, box.y1), LV_MIN(a.x2, box.x2),
             LV_MIN(a.y2, box.y2)};
    }
    return a;
}
} // namespace

TEST_CASE_METHOD(PagesFixture,
                 "the Multi-Color row's + and mode reach 44px on one line, none of it spilling",
                 "[ams_edit_overlay][pages][multi_color][touch]") {
    lv_display_t* disp = lv_display_get_default();
    for (const auto& [w, h] : {std::pair{480, 272}, std::pair{480, 320}, std::pair{800, 480}}) {
        CAPTURE(w, h);
        ScopedResolution res(disp, w, h);
        theme_manager_refresh_layout_constants(disp);
        open(0, describe_lane(*backend, 0, "PLA"));
        access.open_page(AmsEditOverlay::kPageFilament);
        access.call_open_color_page();
        settle();
        auto& page = helix::ui::get_color_picker_page();
        lv_obj_t* sw = lv_obj_find_by_name(page.get_root(), "multi_color_switch");
        REQUIRE(sw != nullptr);
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
        // Three colors: the widest the row gets, four circles with the "+".
        access.call_add_color();
        settle();
        REQUIRE(access.draft_colors().size() == 3);

        for (const char* name : {"btn_add_color", "multi_color_mode"}) {
            INFO(name);
            lv_obj_t* target = lv_obj_find_by_name(page.get_root(), name);
            REQUIRE(target != nullptr);
            REQUIRE_FALSE(lv_obj_is_hidden(target));
            const lv_area_t reach = reachable_area(target);
            CHECK(lv_area_get_width(&reach) >= 44);
            CHECK(lv_area_get_height(&reach) >= 44);
            // A real press at the reach's corners lands on the target.
            for (lv_point_t pt : {lv_point_t{reach.x1, reach.y1}, lv_point_t{reach.x2, reach.y2}}) {
                CHECK(lv_indev_search_obj(lv_obj_get_screen(target), &pt) == target);
            }
            // Its content stays inside its box: nothing to clip or scroll.
            lv_obj_update_layout(target);
            CHECK(lv_obj_get_scroll_right(target) <= 0);
            CHECK(lv_obj_get_scroll_bottom(target) <= 0);
        }
        // One line: the dropdown sits level with the switch.
        lv_obj_t* mode = lv_obj_find_by_name(page.get_root(), "multi_color_mode");
        lv_area_t a;
        lv_area_t b;
        lv_obj_get_coords(sw, &a);
        lv_obj_get_coords(mode, &b);
        CHECK(std::abs((a.y1 + a.y2) - (b.y1 + b.y2)) <= 2);

        NavigationManager::instance().go_back();
        settle();
        process_lvgl(400);
        close_editor_overlay();
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(PagesFixture,
                 "the color page's Translucent checkbox is staged, and Save commits it",
                 "[ams_edit_overlay][pages][translucent]") {
    open(0, describe_lane(*backend, 0, "PETG"));
    REQUIRE_FALSE(backend->get_slot_info(0).translucent);
    access.call_open_color_page();
    settle();
    auto& page = helix::ui::get_color_picker_page();
    REQUIRE(page.view().attached());
    lv_obj_t* box = lv_obj_find_by_name(page.get_root(), "translucent_check");
    REQUIRE(box != nullptr);
    CHECK_FALSE(lv_obj_has_state(box, LV_STATE_CHECKED));

    lv_obj_add_state(box, LV_STATE_CHECKED);
    lv_obj_send_event(box, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    // Staged: Save lights, the lane waits.
    CHECK(access.is_dirty());
    CHECK_FALSE(backend->get_slot_info(0).translucent);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_edit_translucent")) == 1);

    NavigationManager::instance().go_back();
    settle();
    process_lvgl(400);
    access.call_handle_save();
    settle();
    CHECK(backend->get_slot_info(0).translucent);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "a translucent lane's color dots and the color page's readout draw translucent",
                 "[ams_edit_overlay][pages][translucent]") {
    SlotInfo lane = describe_lane(*backend, 0, "PETG");
    lane.translucent = true;
    REQUIRE(helix::test::apply_edit(*backend, 0, lane).success());
    open(0, backend->get_slot_info(0));
    access.open_page(AmsEditOverlay::kPageFilament);
    settle();
    const auto first_dot = [](lv_obj_t* root) {
        lv_obj_t* dots = lv_obj_find_by_name(root, "dots");
        REQUIRE(dots != nullptr);
        REQUIRE(lv_obj_get_child_count(dots) > 0);
        return lv_obj_find_by_name(lv_obj_get_child(dots, 0), "color_swatch_disc");
    };
    lv_obj_t* filament_dot = first_dot(lv_screen_active());
    REQUIRE(filament_dot != nullptr);
    CHECK(helix::ui::swatch_paint_translucent(filament_dot));

    access.call_open_color_page();
    settle();
    auto& page = helix::ui::get_color_picker_page();
    REQUIRE(page.view().attached());
    lv_obj_t* readout = lv_obj_find_by_name(page.get_root(), "readout_swatch");
    REQUIRE(readout != nullptr);
    lv_obj_t* readout_disc = lv_obj_find_by_name(readout, "color_swatch_disc");
    REQUIRE(readout_disc != nullptr);
    CHECK(helix::ui::swatch_paint_translucent(readout_disc));

    // Unchecking shows opaque on both at once.
    lv_obj_t* box = lv_obj_find_by_name(page.get_root(), "translucent_check");
    REQUIRE(box != nullptr);
    lv_obj_remove_state(box, LV_STATE_CHECKED);
    lv_obj_send_event(box, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK_FALSE(helix::ui::swatch_paint_translucent(readout_disc));
    CHECK_FALSE(helix::ui::swatch_paint_translucent(filament_dot));

    NavigationManager::instance().go_back();
    settle();
    process_lvgl(400);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture,
                 "the card says the material once: not again under a product that names it",
                 "[ams_edit_overlay][pages][card]") {
    SlotInfo lane = describe_lane(*backend, 1, "ASA");
    const std::string color = helix::ams::slot_color_name(lane.color_rgb);
    lane.spool_name.clear(); // the product line then reads brand and product
    SECTION("PolyLite ASA: the color alone") {
        lane.brand = "Polymaker";
        lane.product_name = "PolyLite ASA";
        REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
        open(1, backend->get_slot_info(1));
        CHECK(access.subject_text("ams_edit_product_line") == "Polymaker PolyLite ASA");
        CHECK(access.subject_text("ams_edit_material_line") == color);
    }
    SECTION("a product that does not name it keeps the material") {
        lane.brand = "Polymaker";
        lane.product_name = "Basic";
        REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
        open(1, backend->get_slot_info(1));
        CHECK(access.subject_text("ams_edit_material_line") == "ASA · " + color);
    }
    SECTION("the empty page's last-held lines follow the same rule") {
        lane.brand = "Polymaker";
        lane.product_name = "PolyLite ASA";
        lane.status = SlotStatus::EMPTY;
        open(1, lane);
        REQUIRE(access.page() == AmsEditOverlay::kPageEmpty);
        CHECK(access.subject_text("ams_edit_last_material") == color);
    }
    close_editor_overlay();
}

namespace {
/// A lane holding a described spool that Spoolman does not track.
SlotInfo untracked_lane(AmsBackendMock& backend, int slot, const char* material) {
    SlotInfo info = describe_lane(backend, slot, material);
    info.spoolman_id = 0;
    REQUIRE(helix::test::apply_edit(backend, slot, info).success());
    return backend.get_slot_info(slot);
}
} // namespace

TEST_CASE_METHOD(PagesFixture, "the summary offers Add to Spoolman only for an untracked spool",
                 "[ams_edit_overlay][pages][link_spoolman]") {
    SlotInfo lane = untracked_lane(*backend, 1, "ASA");
    SECTION("untracked, Spoolman set up: the row") {
        set_spoolman_available(true);
        open(1, lane);
        CHECK(access.shown("add_spoolman_row"));
    }
    SECTION("linked: no row") {
        set_spoolman_available(true);
        lane.spoolman_id = 22;
        open(1, lane);
        CHECK_FALSE(access.shown("add_spoolman_row"));
    }
    SECTION("no Spoolman: no row") {
        set_spoolman_available(false);
        open(1, lane);
        CHECK_FALSE(access.shown("add_spoolman_row"));
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Add to Spoolman files the lane's spool as it stands and links it",
                 "[ams_edit_overlay][pages][link_spoolman]") {
    set_spoolman_available(true);
    client.spoolman_mock().get_mock_spools().clear();
    client.spoolman_mock().next_created_filament_id = 117;
    client.spoolman_mock().next_created_spool_id = 152;
    SlotInfo lane = untracked_lane(*backend, 1, "ASA");
    lane.color_rgb = 0x101010;
    lane.remaining_weight_g = 640.0F;
    lane.total_weight_g = 750.0F;
    REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
    open(1, backend->get_slot_info(1));
    REQUIRE(access.shown("add_spoolman_row"));
    helix::test::ToastLog toasts;

    access.click("add_spoolman_row");
    settle();
    settle();
    REQUIRE_FALSE(toasts.shown.empty());
    CHECK(toasts.shown.back().message.find("#152") != std::string::npos);

    REQUIRE(client.spoolman_mock().created_spools.size() == 1U);
    const auto& spool = client.spoolman_mock().created_spools[0];
    CHECK(spool["remaining_weight"].get<double>() == Catch::Approx(640.0));
    CHECK(spool["initial_weight"].get<double>() == Catch::Approx(750.0));
    REQUIRE(client.spoolman_mock().created_filaments.size() == 1U);
    const auto& filament = client.spoolman_mock().created_filaments[0];
    CHECK(filament["material"] == "ASA");
    CHECK(helix::text_io::to_lower(filament["color_hex"].get<std::string>()) == "101010");

    // The lane is linked, and the editor stays on it, showing the link.
    const SlotInfo now = backend->get_slot_info(1);
    CHECK(now.spoolman_id == 152);
    CHECK(now.remaining_weight_g == Catch::Approx(640.0F));
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.working_info().spoolman_id == 152);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_edit_is_managed")) == 1);
    CHECK_FALSE(access.shown("add_spoolman_row"));
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Add to Spoolman on a lane with no brand says so in a toast",
                 "[ams_edit_overlay][pages][link_spoolman]") {
    set_spoolman_available(true);
    SlotInfo lane = untracked_lane(*backend, 1, "PLA");
    lane.brand.clear();
    REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
    open(1, backend->get_slot_info(1));
    helix::test::ToastLog toasts;

    access.click("add_spoolman_row");
    settle();

    CHECK(toasts.shown.size() == 1U);
    CHECK(access.subject_text("ams_edit_save_error").empty());
    CHECK(backend->get_slot_info(1).spoolman_id == 0);
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "a failed Add to Spoolman leaves the lane as it was",
                 "[ams_edit_overlay][pages][link_spoolman]") {
    set_spoolman_available(true);
    const SlotInfo lane = untracked_lane(*backend, 1, "ASA");
    REQUIRE(lane.spoolman_id == 0);
    open(1, lane);
    client.set_mock_spoolman_enabled(false);
    helix::test::ToastLog toasts;

    access.click("add_spoolman_row");
    settle();
    settle();
    CHECK(toasts.shown.size() == 1U);

    const SlotInfo now = backend->get_slot_info(1);
    CHECK(now.spoolman_id == 0);
    CHECK(now.material == lane.material);
    CHECK(now.color_rgb == lane.color_rgb);
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.shown("add_spoolman_row"));
    close_editor_overlay();
}

namespace {
/// Stars @p ids in the app-wide favorites for one test.
struct StarredFavorites {
    explicit StarredFavorites(const std::vector<std::string>& ids) {
        helix::Config::get_instance()->set(helix::filament_favorites::kFavoriteIdsPath, ids);
    }
    ~StarredFavorites() {
        helix::Config::get_instance()->set(helix::filament_favorites::kFavoriteIdsPath,
                                           std::vector<std::string>{});
    }
};

/// The first @p n catalog products of named makers, in catalog order.
std::vector<const helix::printer::EffectiveFilament*>
catalog_products(const helix::printer::FilamentCatalog& catalog, size_t n) {
    std::vector<const helix::printer::EffectiveFilament*> out;
    for (const auto& brand : catalog.all_brands()) {
        if (helix::text_io::equals_ignore_case(brand, "Generic")) {
            continue;
        }
        for (const auto* ef : catalog.products_for_brand(brand)) {
            if (out.size() < n) {
                out.push_back(ef);
            }
        }
    }
    return out;
}
} // namespace

TEST_CASE_METHOD(PagesFixture,
                 "a new spool starts from the user's favorites, then the common spools",
                 "[ams_edit_overlay][pages][favorites]") {
    const auto catalog = helix::printer::FilamentCatalog::load_full();
    const auto favs = catalog_products(catalog, 3);
    REQUIRE(favs.size() == 3);
    StarredFavorites starred({favs[0]->id, favs[1]->id, favs[2]->id});

    open(1, SlotInfo{});
    access.click("btn_choose_spool");
    access.click("btn_add_new");
    settle();
    REQUIRE(access.page() == AmsEditOverlay::kPageFilament);
    REQUIRE(access.shown("common_spools"));

    // Star order, each named; the common spools after them.
    for (int i = 0; i < 3; ++i) {
        INFO(i);
        CHECK(access.subject_text(("ams_edit_tile_label_" + std::to_string(i)).c_str()) ==
              helix::spool_assign::spool_tile_label(favs[static_cast<size_t>(i)]->brand,
                                                    favs[static_cast<size_t>(i)]->name));
        CHECK(lv_subject_get_int(lv_xml_get_subject(
                  nullptr, ("ams_edit_tile_fav_" + std::to_string(i)).c_str())) == 1);
    }
    CHECK(access.subject_text("ams_edit_tile_label_3") == "Generic PLA");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_edit_tile_fav_3")) == 0);

    // A favorite fills the form as a catalog pick does.
    lv_obj_t* tile = access.widget("spool_tile_1");
    REQUIRE(tile != nullptr);
    lv_obj_send_event(tile, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(access.sel().product_id == favs[1]->id);
    CHECK(access.sel().brand == favs[1]->brand);
    CHECK(access.sel().family == filament::display_family(favs[1]->type));
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "Brand leads with Favorites only while there are favorites",
                 "[ams_edit_overlay][pages][favorites]") {
    const auto first_brand = [this] {
        const std::string all = access.dropdown_options("dropdown_brand");
        return all.substr(0, all.find('\n'));
    };
    SECTION("none starred: Generic leads") {
        open(1, describe_lane(*backend, 1, "PLA"));
        access.open_page(AmsEditOverlay::kPageFilament);
        settle();
        CHECK(first_brand() == "Generic");
    }
    SECTION("starred: Favorites leads, and opens the picker on its Favorites view") {
        const auto catalog = helix::printer::FilamentCatalog::load_full();
        StarredFavorites starred({catalog_products(catalog, 1).front()->id});
        open(1, describe_lane(*backend, 1, "PLA"));
        access.open_page(AmsEditOverlay::kPageFilament);
        settle();
        REQUIRE(first_brand() == "Favorites \xE2\x98\x85");
        REQUIRE(access.pick_dropdown("dropdown_brand", "Favorites \xE2\x98\x85"));
        settle();
        lv_obj_t* picker = ModalStack::instance().top_dialog();
        REQUIRE(picker != nullptr);
        lv_obj_t* vendor = lv_obj_find_by_name(picker, "vendor_dropdown");
        REQUIRE(vendor != nullptr);
        char buf[64] = {};
        lv_dropdown_get_selected_str(vendor, buf, sizeof(buf));
        CHECK(std::string(buf) == "Favorites");
        ModalStack::instance().clear();
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "the Filament row's star adds the lane's product to the favorites",
                 "[ams_edit_overlay][pages][favorites]") {
    StarredFavorites starred({});
    const auto star_state = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_edit_star_state"));
    };
    SECTION("Generic: no star") {
        open(1, describe_lane(*backend, 1, "PLA"));
        access.open_page(AmsEditOverlay::kPageFilament);
        REQUIRE(access.pick_dropdown("dropdown_brand", "Generic"));
        settle();
        CHECK_FALSE(access.shown("btn_favorite"));
        CHECK(star_state() == 0);
    }
    SECTION("a catalog product: outlined, and a tap stars it") {
        const auto catalog = helix::printer::FilamentCatalog::load_full();
        const auto* product = catalog_products(catalog, 1).front();
        SlotInfo lane = describe_lane(*backend, 1, "PLA");
        lane.brand = product->brand;
        lane.catalog_id = product->id;
        lane.material = product->type;
        REQUIRE(helix::test::apply_edit(*backend, 1, lane).success());
        open(1, backend->get_slot_info(1));
        access.open_page(AmsEditOverlay::kPageFilament);
        settle();
        REQUIRE(access.sel().product_id == product->id);
        REQUIRE(access.shown("btn_favorite"));
        CHECK(access.shown("favorite_star_outline"));
        CHECK_FALSE(access.shown("favorite_star"));

        access.click("btn_favorite");
        settle();
        const auto ids = helix::filament_favorites::load_favorite_ids();
        CHECK(std::find(ids.begin(), ids.end(), product->id) != ids.end());
        CHECK(star_state() == 2);
        CHECK(access.shown("favorite_star"));
        CHECK_FALSE(access.shown("favorite_star_outline"));
        // It leads the spool tiles now.
        CHECK(access.subject_text("ams_edit_tile_label_0") ==
              helix::spool_assign::spool_tile_label(product->brand, product->name));

        // A second tap takes it off again.
        access.click("btn_favorite");
        settle();
        CHECK(helix::filament_favorites::load_favorite_ids().empty());
        CHECK(star_state() == 1);
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "the small classes give favorites one row, taken from the swatches",
                 "[ams_edit_overlay][pages][favorites]") {
    lv_display_t* disp = lv_display_get_default();
    const auto catalog = helix::printer::FilamentCatalog::load_full();
    const auto favs = catalog_products(catalog, 3);
    REQUIRE(favs.size() == 3);
    const auto add_spool = [this] {
        open(1, SlotInfo{});
        access.click("btn_choose_spool");
        access.click("btn_add_new");
        settle();
        REQUIRE(access.page() == AmsEditOverlay::kPageFilament);
    };
    for (const auto& [w, h] : {std::pair{480, 320}, std::pair{480, 272}}) {
        CAPTURE(w, h);
        ScopedResolution res(disp, w, h);
        theme_manager_refresh_layout_constants(disp);

        // No favorites: as before, no tiles at all.
        add_spool();
        CHECK_FALSE(access.shown("favorite_spools"));
        CHECK_FALSE(access.shown("common_spools"));
        const size_t swatches_without = access.color_row().size();
        close_editor_overlay();

        {
            StarredFavorites starred({favs[0]->id, favs[1]->id, favs[2]->id});
            add_spool();
            REQUIRE(access.shown("favorite_spools"));
            CHECK_FALSE(access.shown("common_spools"));
            // The favorites alone: the fourth slot is a common spool and stays hidden.
            CHECK(access.shown("favorite_tile_0"));
            CHECK(access.shown("favorite_tile_2"));
            CHECK_FALSE(access.shown("favorite_tile_3"));
            // The swatches gave up room for the row.
            CHECK(access.color_row().size() < swatches_without);
            close_editor_overlay();
        }
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(PagesFixture, "a lane holding an unrecorded spool opens on its summary",
                 "[ams_edit_overlay][pages]") {
    SlotInfo lane = backend->get_slot_info(1);
    lane.material.clear();
    lane.brand.clear();
    lane.spoolman_id = 0;
    lane.status = SlotStatus::AVAILABLE;
    open(1, lane);
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    // Its unknown state: no material, a full spool of the lane's size.
    CHECK(access.working_info().material.empty());
    CHECK(access.shown("btn_hdr_new_spool"));
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "a label scanned on the empty page or the list opens the summary",
                 "[ams_edit_overlay][pages][scan]") {
    SpoolInfo spool;
    spool.id = 77;
    spool.vendor = "Polymaker";
    spool.material = "PLA";
    spool.color_hex = "202020";
    spool.remaining_weight_g = 700;
    spool.initial_weight_g = 1000;
    SlotInfo empty;
    empty.status = SlotStatus::EMPTY;
    SECTION("from the empty page") {
        open(1, empty);
        REQUIRE(access.page() == AmsEditOverlay::kPageEmpty);
    }
    SECTION("from the New Spool list") {
        open(1, empty);
        access.click("btn_choose_spool");
        settle();
        REQUIRE(access.page() == AmsEditOverlay::kPageList);
    }
    access.scan_spool(spool);
    settle();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.working_info().spoolman_id == 77);
    CHECK(access.save_disabled() == 0);
    close_editor_overlay();
}

namespace {
/// A filament system whose firmware keeps the runout backup.
struct FirmwareRunoutMock : AmsBackendMock {
    using AmsBackendMock::AmsBackendMock;
    helix::printer::EndlessSpoolCapabilities caps;
    std::optional<int> backup;
    helix::printer::EndlessSpoolCapabilities get_endless_spool_capabilities() const override {
        return caps;
    }
    std::optional<int> runout_backup_for(int /*slot*/) const override {
        return backup;
    }
};
} // namespace

TEST_CASE_METHOD(PagesFixture, "a firmware-kept runout reads on the summary, read-only",
                 "[ams_edit_overlay][pages][lane_runout]") {
    using helix::printer::EndlessSpoolAvailability;
    using helix::printer::EndlessSpoolEditability;
    using helix::printer::EndlessSpoolEnabled;
    auto owned = std::make_unique<FirmwareRunoutMock>(4);
    FirmwareRunoutMock* mock = owned.get();
    mock->set_remap_strategy(AmsBackend::RemapStrategy::PrePrintSend);
    mock->caps.availability = EndlessSpoolAvailability::Available;
    mock->caps.editability = EndlessSpoolEditability::ReadOnly;
    mock->caps.enabled = EndlessSpoolEnabled::On;
    REQUIRE(mock->start().success());
    AmsState::instance().set_backend(std::move(owned));
    const auto lane_line = [this] { return access.subject_text("ams_edit_lane_line"); };
    const std::string third = helix::ui::lane_label(mock->lane_noun(), 2);

    SECTION("on, with a matching spool: continues from it") {
        mock->backup = 2;
        open(1, describe_lane(*mock, 1, "PLA"));
        REQUIRE(access.shown("lane_line"));
        CHECK(lane_line() == "Continues from " + third + " on runout");
        CHECK_FALSE(access.shown("lane_line_chevron"));
    }
    SECTION("on, without one: pauses") {
        mock->backup = -1;
        open(1, describe_lane(*mock, 1, "PLA"));
        REQUIRE(access.shown("lane_line"));
        CHECK(lane_line() == "Pauses on runout (no matching spool)");
    }
    SECTION("off, and switchable: pauses, and the row opens the switch") {
        mock->caps.enabled = EndlessSpoolEnabled::Off;
        mock->caps.switchable = true;
        open(1, describe_lane(*mock, 1, "PLA"));
        REQUIRE(access.shown("lane_line"));
        CHECK(lane_line() == "Pauses on runout (automatic refill off)");
        CHECK(access.shown("lane_line_chevron"));
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_edit_lane_tap")) ==
              static_cast<int>(helix::spool_assign::LaneTap::RefillToggle));
    }
    SECTION("unsupported: no row") {
        mock->caps.availability = EndlessSpoolAvailability::Unsupported;
        open(1, describe_lane(*mock, 1, "PLA"));
        CHECK_FALSE(access.shown("lane_line"));
    }
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "the weight bar's knob stays whole at both ends",
                 "[ams_edit_overlay][pages][weight]") {
    lv_display_t* disp = lv_display_get_default();
    for (const auto& [w, h] : {std::pair{480, 272}, std::pair{480, 320}, std::pair{800, 480}}) {
        CAPTURE(w, h);
        ScopedResolution res(disp, w, h);
        theme_manager_refresh_layout_constants(disp);
        open(1, describe_lane(*backend, 1, "PLA"));
        lv_obj_t* slider = access.widget("weight_slider");
        lv_obj_t* row = access.widget("weight_slider_row");
        REQUIRE(slider != nullptr);
        REQUIRE(row != nullptr);
        for (int pct : {0, 100}) {
            CAPTURE(pct);
            lv_slider_set_value(slider, pct, LV_ANIM_OFF);
            lv_obj_update_layout(row);
            // The knob is centered on the track's end, as tall as the track
            // plus its padding: half of that reaches past either end, and the
            // row's own area must hold it so no edge clips it.
            const int32_t reach = lv_obj_get_height(slider) / 2 +
                                  LV_MAX(lv_obj_get_style_pad_left(slider, LV_PART_KNOB),
                                         lv_obj_get_style_pad_right(slider, LV_PART_KNOB));
            lv_area_t track;
            lv_area_t room;
            lv_obj_get_coords(slider, &track);
            lv_obj_get_coords(row, &room);
            CHECK(reach > 0);
            CHECK(track.x1 - reach >= room.x1);
            CHECK(track.x2 + reach <= room.x2);
        }
        close_editor_overlay();
    }
    theme_manager_refresh_layout_constants(disp);
}

namespace {
/// The lane's sensor now reads @p status, as the backend's next sync reports it.
void lane_reads(AmsBackendMock& backend, int slot, SlotStatus status) {
    backend.force_slot_status(slot, status);
    AmsState::instance().sync_from_backend();
}
} // namespace

TEST_CASE_METHOD(PagesFixture, "the editor follows its lane as a spool goes in and runs out",
                 "[ams_edit_overlay][pages]") {
    backend->force_slot_cleared(1);
    open(1, backend->get_slot_info(1));
    REQUIRE(access.page() == AmsEditOverlay::kPageEmpty);

    lane_reads(*backend, 1, SlotStatus::AVAILABLE);
    settle();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.mode() == AmsEditOverlay::kModeEdit);
    CHECK_FALSE(access.is_dirty());

    lane_reads(*backend, 1, SlotStatus::EMPTY);
    settle();
    CHECK(access.page() == AmsEditOverlay::kPageEmpty);
    CHECK(access.subject_text("ams_edit_empty_title") == "Gate 2 is empty");
    close_editor_overlay();
}

TEST_CASE_METHOD(PagesFixture, "a lane change never moves an edit in progress or the spools list",
                 "[ams_edit_overlay][pages]") {
    SECTION("an edit held on the summary stays through a runout") {
        const SlotInfo lane = describe_lane(*backend, 1, "PLA");
        open(1, lane);
        REQUIRE(access.page() == AmsEditOverlay::kPageSummary);
        access.click("summary_card");
        REQUIRE(access.pick_dropdown("dropdown_material", "PETG"));
        back();
        REQUIRE(access.page() == AmsEditOverlay::kPageSummary);
        REQUIRE(access.is_dirty());

        lane_reads(*backend, 1, SlotStatus::EMPTY);
        settle();
        CHECK(access.page() == AmsEditOverlay::kPageSummary);
        CHECK(access.is_dirty());
        CHECK(access.subject_text("ams_edit_material_line").rfind("PETG", 0) == 0);
    }
    SECTION("a staged lane setting holds the summary through a runout") {
        const SlotInfo lane = describe_lane(*backend, 1, "PLA");
        open(1, lane);
        access.open_page(AmsEditOverlay::kPageLane);
        REQUIRE(access.pick_dropdown("tool_dropdown", "T0"));
        back();
        REQUIRE(access.page() == AmsEditOverlay::kPageSummary);
        REQUIRE(access.is_dirty());

        lane_reads(*backend, 1, SlotStatus::EMPTY);
        settle();
        CHECK(access.page() == AmsEditOverlay::kPageSummary);
        CHECK(access.is_dirty());
    }
    SECTION("the list stays open, and Back from it lands on the lane's new page") {
        backend->force_slot_cleared(1);
        open(1, backend->get_slot_info(1));
        access.click("btn_choose_spool");
        REQUIRE(access.page() == AmsEditOverlay::kPageList);

        lane_reads(*backend, 1, SlotStatus::AVAILABLE);
        settle();
        CHECK(access.page() == AmsEditOverlay::kPageList);
        back();
        CHECK(access.page() == AmsEditOverlay::kPageSummary);
    }
    ModalStack::instance().clear();
    close_editor_overlay();
}
