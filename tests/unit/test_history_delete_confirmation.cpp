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
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HistoryListPanelTestAccess;

namespace {

/// Records every JSON-RPC method sent, so "was the record deleted?" is read
/// off the wire rather than off a spy on our own code.
class RecordingClient : public MoonrakerClientMock {
  public:
    RecordingClient() : MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24) {}

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        methods.push_back(method);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] int count(const std::string& method) const {
        int n = 0;
        for (const auto& m : methods) {
            if (m == method) {
                ++n;
            }
        }
        return n;
    }

    std::vector<std::string> methods;
};

constexpr const char* DELETE_JOB = "server.history.delete_job";

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
        client.connect("ws://mock/websocket", []() {}, []() {});
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
        client.stop_temperature_simulation();
        client.disconnect();
    }

    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static void press(lv_obj_t* dialog, const char* name) {
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    }

    RecordingClient client;
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
    CHECK(client.count(DELETE_JOB) == 0);

    // A second tap while the dialog is up must not stack another one.
    HistoryListPanelTestAccess::handle_delete(panel);
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) == dialog);

    press(dialog, "btn_primary");
    settle();

    CHECK(client.count(DELETE_JOB) == 1);
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

    CHECK(client.count(DELETE_JOB) == 0);
    // The guard is released, so the button works again.
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) == nullptr);
    HistoryListPanelTestAccess::handle_delete(panel);
    settle();
    CHECK(HistoryListPanelTestAccess::delete_dialog(panel) != nullptr);
}

namespace {

/// Holds the reply to `server.history.list` instead of delivering it, so a test
/// can choose the moment it lands. Moonraker replies arrive on the WebSocket
/// thread, so a panel that has left the screen can still be handed one.
class ParkingClient : public MoonrakerClientMock {
  public:
    ParkingClient() : MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24) {}

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "server.history.list") {
            parked = std::move(success_cb);
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    std::function<void(const json&)> parked;
};

class HistoryFetchLifetimeFixture : public LVGLUITestFixture {
  public:
    HistoryFetchLifetimeFixture() {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        panel.init_subjects();
    }

    ~HistoryFetchLifetimeFixture() override {
        settle();
        set_moonraker_api(previous_api_);
        api.reset();
        client.stop_temperature_simulation();
        client.disconnect();
    }

    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
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

    ParkingClient client;
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
    REQUIRE(client.parked != nullptr);
    REQUIRE(HistoryListPanelTestAccess::jobs(panel).empty());

    panel.on_deactivate(DeactivateReason::NavigateAway);

    client.parked(one_job_reply());
    settle();

    CHECK(HistoryListPanelTestAccess::jobs(panel).empty());
}

TEST_CASE_METHOD(HistoryFetchLifetimeFixture, "A history reply arriving on screen still populates",
                 "[history][lifetime][1578]") {
    panel.refresh_from_api();
    REQUIRE(client.parked != nullptr);

    client.parked(one_job_reply());
    settle();

    CHECK(HistoryListPanelTestAccess::jobs(panel).size() == 1);
}
