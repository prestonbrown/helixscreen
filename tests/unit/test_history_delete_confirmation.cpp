// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_delete_confirmation.cpp
 * @brief The history detail Delete button asks before it destroys the record.
 *
 * Run with: ./build/bin/helix-tests "[history][modal]"
 *
 * A history record is not recoverable once server.history.delete_job runs, so
 * the button raises a confirmation and only its Delete side reaches the wire.
 * Cancel and a backdrop dismissal must also release the re-entry guard, or the
 * next tap on the button does nothing (prestonbrown/helixscreen#1373).
 */

#include "ui_modal.h"
#include "ui_panel_history_list.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HistoryListPanelTestAccess;

namespace {

// "Was the record deleted?" is read off the wire rather than off a spy on our own code.
constexpr const char* kDeleteJob = "server.history.delete_job";

PrintHistoryJob job(const std::string& filename) {
    PrintHistoryJob j;
    j.job_id = "id-" + filename;
    j.filename = filename;
    j.status = PrintJobStatus::COMPLETED;
    return j;
}

class HistoryDeleteFixture : public LVGLUITestFixture {
  public:
    HistoryDeleteFixture() {
        helix::ui::modal_init_subjects();
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        // The delete path reaches apply_filters_and_sort(), which notifies
        // subject_filter_active_. A panel whose subjects were never initialized
        // notifies through an uninitialized observer list.
        panel.init_subjects();
        HistoryListPanelTestAccess::select_job(panel, {job("benchy.gcode")}, 0);
    }

    ~HistoryDeleteFixture() override {
        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        settle();
        set_moonraker_api(previous_api_);
        api.reset();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void press(lv_obj_t* dialog, const char* name) {
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    }

    helix::test::FakeMoonrakerClient client;
    std::unique_ptr<MoonrakerAPI> api;
    HistoryListPanel panel;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(HistoryDeleteFixture, "History delete asks before touching the record",
                 "[history][modal][1373]") {
    HistoryListPanelTestAccess::handle_delete(panel);
    settle();

    lv_obj_t* dialog = HistoryListPanelTestAccess::delete_dialog(panel);
    REQUIRE(dialog != nullptr);
    CHECK(Modal::get_top() == dialog);
    CHECK(client.rpc_count(kDeleteJob) == 0);

    // A second tap while the dialog is up must not stack another one.
    HistoryListPanelTestAccess::handle_delete(panel);
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) == dialog);

    press(dialog, "btn_primary");
    settle();

    CHECK(client.rpc_count(kDeleteJob) == 1);
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) == nullptr);
}

TEST_CASE_METHOD(HistoryDeleteFixture,
                 "Cancelling or dismissing the delete dialog keeps the record",
                 "[history][modal][1373]") {
    HistoryListPanelTestAccess::handle_delete(panel);
    settle();
    lv_obj_t* dialog = HistoryListPanelTestAccess::delete_dialog(panel);
    REQUIRE(dialog != nullptr);

    SECTION("cancel button") {
        press(dialog, "btn_secondary");
    }
    SECTION("dismissed by a backdrop tap") {
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);
        lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    }
    process_lvgl(50);
    settle();

    CHECK(client.rpc_count(kDeleteJob) == 0);
    // The guard is released, so the button works again.
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) == nullptr);
    HistoryListPanelTestAccess::handle_delete(panel);
    settle();
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) != nullptr);
}

namespace {

// Moonraker replies arrive on the WebSocket thread, so a panel that has left the
// screen can still be handed one: a test holds `server.history.list` unanswered and
// chooses the moment it lands.
constexpr const char* kHistoryList = "server.history.list";

class HistoryFetchLifetimeFixture : public LVGLUITestFixture {
  public:
    HistoryFetchLifetimeFixture() {
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        panel.init_subjects();
    }

    ~HistoryFetchLifetimeFixture() override {
        settle();
        set_moonraker_api(previous_api_);
        api.reset();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// One completed job, in the shape `server.history.list` returns.
    static json one_job_reply() {
        json entry = json::object();
        entry["job_id"] = "000001";
        entry["filename"] = "benchy.gcode";
        entry["status"] = "completed";
        json result = json::object();
        result["count"] = 1;
        result["jobs"] = json::array({entry});
        json reply = json::object();
        reply["result"] = result;
        return reply;
    }

    helix::test::FakeMoonrakerClient client;
    std::unique_ptr<MoonrakerAPI> api;
    HistoryListPanel panel;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(HistoryFetchLifetimeFixture,
                 "A history reply landing after the panel leaves the screen is dropped",
                 "[history][lifetime][1578]") {
    panel.refresh_from_api();
    REQUIRE(HistoryListPanelTestAccess::jobs(panel).empty());

    panel.on_deactivate(DeactivateReason::NavigateAway);

    REQUIRE(client.reply_to_last(kHistoryList, one_job_reply()));
    settle();

    CHECK(HistoryListPanelTestAccess::jobs(panel).empty());
}

TEST_CASE_METHOD(HistoryFetchLifetimeFixture, "A history reply arriving on screen still populates",
                 "[history][lifetime][1578]") {
    panel.refresh_from_api();

    REQUIRE(client.reply_to_last(kHistoryList, one_job_reply()));
    settle();

    CHECK(HistoryListPanelTestAccess::jobs(panel).size() == 1);
}
