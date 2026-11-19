// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_edit_tool_refusal.cpp
 * @brief A lane whose tool mapping the backend refuses shows Assigned Tool
 *        greyed, with the backend's reason under it.
 */

#include "ui_ams_edit_overlay.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// A backend that takes every edit but refuses to remap a tool, as AFC does
/// with multiple_tool_mapping on.
class RemapRefusingMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    AmsError can_set_tool_mapping(int, int) const override {
        return AmsError(AmsResult::NOT_SUPPORTED, "remap refused", "Tools map in the firmware");
    }
};

struct ToolRefusalFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api{client, get_printer_state()};

    ToolRefusalFixture() {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
        SpoolmanManager::instance().init_subjects();
        ams.set_moonraker_api(&api);
    }
    ~ToolRefusalFixture() override {
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::instance().set_api(nullptr);
    }

    /// Opens the editor on lane 1 of @p backend and reports the tool dropdown's
    /// disabled state and the refusal line shown under it ("" when hidden).
    std::pair<bool, std::string> tool_row(std::unique_ptr<AmsBackendMock> backend) {
        AmsBackendMock* raw = backend.get();
        AmsState::instance().set_backend(std::move(backend));
        auto& overlay = get_ams_edit_overlay();
        REQUIRE(overlay.show_for_slot(test_screen(), 1, raw->get_slot_info(1), nullptr,
                                      [](const AmsEditOverlay::EditResult&) {}));
        UpdateQueue::instance().drain();
        process_lvgl(10);
        lv_obj_t* dd = lv_obj_find_by_name(test_screen(), "tool_dropdown");
        lv_obj_t* why = lv_obj_find_by_name(test_screen(), "tool_refusal");
        REQUIRE(dd != nullptr);
        REQUIRE(why != nullptr);
        std::pair<bool, std::string> out{
            lv_obj_has_state(dd, LV_STATE_DISABLED),
            lv_obj_is_hidden(why) ? std::string() : std::string(lv_label_get_text(why))};
        NavigationManager::instance().go_back();
        UpdateQueue::instance().drain();
        lv_timer_handler();
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(ToolRefusalFixture, "a refused tool remap greys Assigned Tool and says why",
                 "[ams_edit_overlay][lane_settings]") {
    const auto refused = tool_row(std::make_unique<RemapRefusingMock>(4));
    CHECK(refused.first);
    CHECK(refused.second == "Tools map in the firmware");

    const auto allowed = tool_row(std::make_unique<AmsBackendMock>(4));
    CHECK_FALSE(allowed.first);
    CHECK(allowed.second.empty());
}
