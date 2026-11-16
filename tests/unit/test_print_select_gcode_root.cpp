// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_gcode_root.cpp
 * @brief The local G-code root is asked for only over a live connection.
 *
 * Print select builds its detail view's analysis at setup, while the websocket
 * is usually still coming up. A server.files.roots request then fails before
 * reaching Moonraker, and latching that answer would send every file over HTTP
 * for the session (#1233). The client here fails that request the way the real
 * one does when not connected, and answers it once connected.
 */

#include "ui_panel_print_select.h"
#include "ui_print_select_detail_view.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ConnectionState;
using helix::ui::PrintSelectDetailView;
using helix::ui::UpdateQueue;

namespace {

class RootsCountingClient : public MoonrakerClientMock {
  public:
    std::atomic<int> roots_requests{0};

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        if (method != "server.files.roots") {
            return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                     std::move(error_cb), timeout_ms, silent,
                                                     intent);
        }
        ++roots_requests;
        if (get_connection_state() != ConnectionState::CONNECTED) {
            if (error_cb) {
                error_cb(MoonrakerError::connection_lost("server.files.roots"));
            }
            return 0;
        }
        if (success_cb) {
            const std::string dir = std::filesystem::temp_directory_path().string();
            success_cb(json::array({{{"name", "gcodes"}, {"path", dir}, {"permissions", "rw"}}}));
        }
        return 0;
    }
};

class GcodeRootFixture : public LVGLUITestFixture {
  public:
    RootsCountingClient client_;
    MoonrakerAPIMock api_{client_, state()};

    GcodeRootFixture() {
        state().init_subjects(false);
    }

    ~GcodeRootFixture() override {
        settle();
        client_.disconnect();
    }

    void connect() {
        client_.connect("ws://localhost:7125/websocket", []() {}, []() {});
        settle();
    }

    void settle() {
        UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE_METHOD(GcodeRootFixture, "print select setup asks for no file roots",
                 "[print_select][1233]") {
    REQUIRE(client_.get_connection_state() != ConnectionState::CONNECTED);

    auto panel = std::make_unique<PrintSelectPanel>(state(), &api_);
    panel->init_subjects();
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
    REQUIRE(root != nullptr);
    panel->setup(root, test_screen());
    panel->set_api(&api_);
    settle();
    REQUIRE(PrintSelectPanelTestAccess::prep_manager(*panel) != nullptr);
    CHECK(client_.roots_requests == 0);

    // The first open, once connected, is where the root is resolved, and once.
    connect();
    PrintSelectPanelTestAccess::build_detail_view(*panel);
    settle();
    CHECK(client_.roots_requests == 1);
    panel->set_api(&api_);
    settle();
    CHECK(client_.roots_requests == 1);

    lv_obj_delete(root);
    settle();
    panel.reset();
    settle();
}

TEST_CASE_METHOD(GcodeRootFixture, "a file-roots lookup before connect latches nothing",
                 "[print_select][1233]") {
    PrintSelectDetailView view;
    view.set_dependencies(&api_, &state());
    settle();
    REQUIRE(client_.get_connection_state() != ConnectionState::CONNECTED);
    CHECK(client_.roots_requests == 0);

    connect();
    view.set_dependencies(&api_, &state());
    settle();
    CHECK(client_.roots_requests == 1);

    view.set_dependencies(&api_, &state());
    settle();
    CHECK(client_.roots_requests == 1);
}
