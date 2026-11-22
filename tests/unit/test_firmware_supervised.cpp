// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_firmware_supervised.cpp
 * @brief Firmware that supervises HelixScreen owns its software and the printer's
 *        configuration (prestonbrown/helixscreen#2154): what decides it, and the choke
 *        points that stop HelixScreen writing there.
 */

#include "../../src/api/moonraker_gcode_guards.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/firmware_supervised_override.h"
#include "../test_helpers/scoped_env.h"
#include "helix_plugin_features.h"
#include "klipper_config_editor.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_validation.h"
#include "platform_info.h"
#include "printer_state.h"
#include "system/update_checker.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::system::ConfigEdit;
using helix::system::KlipperConfigEditor;

namespace {

using SupervisedFor = helix::test::FirmwareSupervisedFor;

class SupervisedApiFixture : public LVGLTestFixture {
  public:
    SupervisedApiFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }
    ~SupervisedApiFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// The error type an upload reports, or nullopt when it reports none.
    std::optional<MoonrakerErrorType> upload_error(const std::string& root,
                                                   const std::string& path) {
        std::optional<MoonrakerErrorType> type;
        api->transfers().upload_file(
            root, path, "x", []() {}, [&](const MoonrakerError& e) { type = e.type; });
        return type;
    }

    std::optional<MoonrakerErrorType> delete_error(const std::string& path) {
        std::optional<MoonrakerErrorType> type;
        api->files().delete_file(path, []() {}, [&](const MoonrakerError& e) { type = e.type; });
        return type;
    }

    bool plugin_call_refused(const std::string& method, const nlohmann::json& params) {
        return api::reject_plugin_rpc(state, method, params, [](const MoonrakerError&) {});
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE("firmware supervision: the firmware's variable alone decides it",
          "[firmware_supervised]") {
    {
        helix::ScopedEnv on("HELIX_FIRMWARE_MANAGED", "1");
        CHECK(firmware_supervised());
    }
    {
        helix::ScopedEnv off("HELIX_FIRMWARE_MANAGED", "0");
        CHECK_FALSE(firmware_supervised());
    }
    helix::ScopedEnv unset("HELIX_FIRMWARE_MANAGED", nullptr);
    CHECK_FALSE(firmware_supervised());
}

TEST_CASE_METHOD(SupervisedApiFixture,
                 "firmware supervision: no upload or file change reaches the config root",
                 "[firmware_supervised]") {
    {
        SupervisedFor on(true);
        CHECK(upload_error("config", "printer.cfg") == MoonrakerErrorType::PERMISSION_DENIED);
        CHECK(upload_error("config", "") == MoonrakerErrorType::PERMISSION_DENIED);
        CHECK(delete_error("config/helix_skips.cfg") == MoonrakerErrorType::PERMISSION_DENIED);
        // Print files stay HelixScreen's to manage.
        CHECK(upload_error("gcodes", "part.gcode") != MoonrakerErrorType::PERMISSION_DENIED);
        CHECK(delete_error("gcodes/part.gcode") != MoonrakerErrorType::PERMISSION_DENIED);
    }
    SupervisedFor off(false);
    CHECK(upload_error("config", "printer.cfg") != MoonrakerErrorType::PERMISSION_DENIED);
    CHECK(delete_error("config/helix_skips.cfg") != MoonrakerErrorType::PERMISSION_DENIED);
}

TEST_CASE_METHOD(SupervisedApiFixture,
                 "firmware supervision: the config editor refuses on either route",
                 "[firmware_supervised]") {
    SupervisedFor on(true);
    const bool plugin_route = GENERATE(false, true);
    const bool one_value = GENERATE(false, true);
    CAPTURE(plugin_route, one_value);
    KlipperConfigEditor editor;
    editor.set_feature_state_source([plugin_route]() {
        return plugin_route ? PluginFeatureState::Ready : PluginFeatureState::Absent;
    });
    bool succeeded = false;
    std::string error;
    auto ok = [&]() { succeeded = true; };
    auto fail = [&](const std::string& e) { error = e; };
    if (one_value) {
        editor.safe_edit_value(*api, "probe", "x_offset", "5", ok, fail);
    } else {
        editor.safe_multi_edit(*api, "input_shaper",
                               {{ConfigEdit::Type::SET_VALUE, "shaper_freq_x", "50"}}, ok, fail);
    }
    CHECK_FALSE(succeeded);
    CHECK(error == kFirmwareSupervisedConfigRefusal);
}

TEST_CASE("firmware supervision: a folder named config under another root is not the config root",
          "[firmware_supervised]") {
    CHECK(moonraker_internal::names_config_root("config", "printer.cfg"));
    CHECK(moonraker_internal::names_config_root("", "config/printer.cfg"));
    CHECK_FALSE(moonraker_internal::names_config_root("gcodes", "config/part.gcode"));
    CHECK_FALSE(moonraker_internal::names_config_root("", "config_backup/printer.cfg"));
}

#if HELIX_HAS_PLUGINS
TEST_CASE_METHOD(SupervisedApiFixture,
                 "firmware supervision: plugins cannot update components or edit the config root",
                 "[firmware_supervised]") {
    {
        SupervisedFor on(true);
        CHECK(plugin_call_refused("machine.update.upgrade", {{"name", "moonraker"}}));
        CHECK_FALSE(plugin_call_refused("machine.update.status", nlohmann::json::object()));
        CHECK(plugin_call_refused("server.files.delete_file", {{"path", "config/printer.cfg"}}));
        CHECK(plugin_call_refused("server.files.move",
                                  {{"source", "gcodes/a.cfg"}, {"dest", "config/a.cfg"}}));
        CHECK_FALSE(plugin_call_refused("server.files.delete_file", {{"path", "gcodes/a.gcode"}}));
        CHECK(plugin_call_refused("server.files.zip", {{"dest", "config/all.zip"}}));
    }
    SupervisedFor off(false);
    CHECK_FALSE(plugin_call_refused("machine.update.upgrade", {{"name", "moonraker"}}));
}
#endif

TEST_CASE("firmware supervision: the boot never runs the config restore script",
          "[firmware_supervised]") {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("helix-supervised-restore-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "bin");
    { std::ofstream bin(root / "bin" / "helix-screen"); }
    const std::string deployed = root.string();
    {
        SupervisedFor off(false);
        REQUIRE(UpdateChecker::startup_runs_restore(nullptr, deployed)); // a layout it would run on
    }
    SupervisedFor on(true);
    CHECK_FALSE(UpdateChecker::startup_runs_restore(nullptr, deployed));
    fs::remove_all(root);
}
