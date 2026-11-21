// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lane spool editor's pages: how each one is reached and left, and the
// jobs that finish on them (Clear Lane and its archive, Same Filament, the Add
// to Spoolman box, Use moving a spool between lanes).

#include "ui_ams_edit_overlay.h"
#include "ui_modal.h"

#include "../test_helpers/ams_edit_overlay_view_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/modal_answer.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
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

    // New Spool, and Back to the lane's own spool.
    access.click("btn_hdr_new_spool");
    REQUIRE(access.page() == AmsEditOverlay::kPageList);
    back();
    CHECK(access.page() == AmsEditOverlay::kPageSummary);
    CHECK(access.mode() == AmsEditOverlay::kModeEdit);
    CHECK(access.working_info().material == "PLA");

    // The summary's Back closes the editor, unsaved.
    back();
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
    if (lane.status == SlotStatus::EMPTY) {
        // A ran-out lane opens on its empty page; its summary is where Clear is.
        access.open_page(AmsEditOverlay::kPageSummary);
    }
    REQUIRE(access.clear_visible() == 1);
    access.click("btn_clear_spool");
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
