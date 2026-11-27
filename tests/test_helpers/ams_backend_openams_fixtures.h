// tests/test_helpers/ams_backend_openams_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_openams.cpp, test_ams_backend_openams_2.cpp.
// Generated from the first of those files by split_v3.py, which keeps every
// case's text and order and moves everything else here.
#pragma once

#include "ui_ams_detail.h"
#include "ui_test_utils.h"

#include "../helix_test_fixture.h"
#include "ams_backend_openams.h"
#include "ams_error.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "core/update_queue.h"
#include "filament_chip_model.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/filament_slot_override_store_test_access.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/openams_status_json.h"
#include "test_helpers/openams_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

using helix::AmsAction;
using helix::AmsBackendOpenAms;
using helix::AmsResult;
using helix::AmsType;
using helix::PathTopology;
using helix::SlotStatus;
using json = nlohmann::json;
using namespace openams_json;

class OpenAmsHarness : public AmsBackendOpenAms {
  public:
    explicit OpenAmsHarness(IMoonrakerAPI* api = nullptr, helix::IMoonrakerClient* client = nullptr)
        : AmsBackendOpenAms(api, client) {
        running_ = true;
        set_event_callback(
            [this](const std::string& event, const std::string&) { events.push_back(event); });
    }

    ~OpenAmsHarness() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed(const json& manager) {
        handle_status_update(json{{"method", "notify_status_update"},
                                  {"params", json::array({json{{"oams_manager", manager}}, 0.0})}});
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        commands.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    void complete_operation() {
        REQUIRE(on_complete);
        on_complete();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail_operation(const std::string& message) {
        REQUIRE(on_error);
        MoonrakerError error;
        error.message = message;
        on_error(error);
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> operations;
    std::vector<std::string> commands;
    std::vector<std::string> events;
    bool send_succeeds = true;

  protected:
    helix::AmsError
    send_operation_gcode(const std::string& gcode, helix::SharedCallback<void()> complete,
                         helix::SharedCallback<void(const MoonrakerError&)> error) override {
        operations.push_back(gcode);
        on_complete = std::move(complete);
        on_error = std::move(error);
        return send_succeeds ? helix::AmsErrorHelper::success()
                             : helix::AmsErrorHelper::not_connected("fixture send failure");
    }

  private:
    helix::SharedCallback<void()> on_complete;
    helix::SharedCallback<void(const MoonrakerError&)> on_error;
};

struct TmpCacheDir {
    std::filesystem::path path;
    explicit TmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("openams_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

inline json component_status(int spool_id, const char* material, const char* color_hex) {
    return json{{"schema_version", 1},
                {"spoolman_online", true},
                {"bays",
                 {{"ams2-1",
                   {{"state", "linked"},
                    {"spool_id", spool_id},
                    {"source", "manual"},
                    {"spool",
                     {{"material", material},
                      {"color_hex", color_hex},
                      {"vendor", "Sunlu"},
                      {"remaining_g", 500},
                      {"initial_g", 1000}}}}}}}};
}

struct ComponentRig {
    MockPrinter printer;
    helix::test::RegisteredBackend<OpenAmsHarness> reg;
    ComponentRig() : reg(&printer.api, &printer.client) {
        printer.client.set_openams_spoolman(true);
        reg->feed(shared_manager(0));
        helix::OpenAmsTestAccess::start(*reg);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    helix::AmsError edit(int slot, const std::function<void(SlotInfo&)>& change) {
        const SlotInfo original = reg->get_slot_info(slot);
        SlotInfo edited = original;
        change(edited);
        return reg->commit_user_edit(slot, original, edited);
    }
};

const std::string kClearHt = "OAMS_CLEAR_FAULT OAMS=1\nOAMSM_CLEAR_ERRORS";

inline json tool_group(const std::string& name, const json& slots) {
    return json{{"name", name}, {"lane", "fps"}, {"slots", slots}};
}

/// manager() with slots 1 and 2 forming T0 (a runout backup pair), slot 3 as
/// T1 and slot 0 as the single-slot T2. Slot 0 is empty, so slot 1 serves T0.
inline json backup_pair_manager() {
    json m = manager();
    m["groups"] =
        json::array({tool_group("T0", json::array({1, 2})), tool_group("T1", json::array({3})),
                     tool_group("T2", json::array({0}))});
    return m;
}

} // namespace helix::test
