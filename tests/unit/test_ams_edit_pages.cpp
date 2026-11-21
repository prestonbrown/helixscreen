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

#include "../test_helpers/ams_edit_overlay_view_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "filament_catalog.h"
#include "filament_variants.h"
#include "lane_translation.h"
#include "moonraker_client_mock_spoolman.h"

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
    CHECK(access.shown("material_row"));

    // A product carries its material and type, and Material steps aside.
    REQUIRE(access.pick_dropdown("dropdown_product", other->name));
    settle();
    CHECK_FALSE(access.shown("material_row"));
    CHECK(access.sel().family == filament::display_family(other->type));
    CHECK(access.sel().product_id == other->id);
    REQUIRE(access.pick_dropdown("dropdown_product", first->name));
    settle();
    CHECK(access.sel().family == filament::display_family(first->type));

    // "Other…": described by Material again.
    REQUIRE(access.pick_dropdown("dropdown_product", "Other…"));
    settle();
    CHECK(access.sel().product_id.empty());
    CHECK(access.shown("material_row"));

    // Generic: no Product, the material row instead.
    REQUIRE(access.pick_dropdown("dropdown_brand", "Generic"));
    settle();
    CHECK_FALSE(access.shown("dropdown_product"));
    CHECK(access.shown("material_row"));
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
