// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_discovery_attach_detach.cpp
 * @brief Notification subscriptions the discovery-complete reaction owns.
 *
 * Discovery runs on every connect and reconnect, so each feature it wires up has
 * to be safe to attach again: a second attach must not double-deliver. A printer
 * switch has to take every one of them off the old client, or a handler outlives
 * the objects it reaches into.
 */

#include "ui_settings_about.h"
#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../ui_test_utils.h"
#include "app_constants.h"
#include "system/update_checker.h"
#include "timelapse_state.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::test::FakeMoonrakerClient;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* TIMELAPSE = "notify_timelapse_event";
constexpr const char* kTimelapseHandler = "timelapse_state";
constexpr const char* kHistory = "notify_history_changed";
constexpr const char* kHistoryHandler = "AboutOverlay_print_hours";
constexpr const char* kUpdate = "notify_update_response";
constexpr const char* kUpdateHandler = "external_update_restart";

void attach_connection_features(FakeMoonrakerClient& client) {
    helix::TimelapseState::instance().attach(client);
    helix::settings::get_about_settings_overlay().attach_print_hours(client);
    UpdateChecker::instance().on_connected(client);
}

void detach_connection_features(FakeMoonrakerClient& client) {
    helix::TimelapseState::instance().detach(client);
    helix::settings::get_about_settings_overlay().detach_print_hours(client);
    UpdateChecker::instance().detach(client);
}

/// Points the self-restart sentinel at a scratch dir for one test.
struct ScratchSentinelDir {
    std::string previous = helix::app_constants::update::detail::backup_fallback_dir_ref();
    fs::path root = fs::temp_directory_path() / ("helix-attach-" + std::to_string(getpid()));
    ScratchSentinelDir() {
        fs::remove_all(root);
        fs::create_directories(root);
        helix::app_constants::update::detail::backup_fallback_dir_ref() = root.string();
    }
    ~ScratchSentinelDir() {
        helix::app_constants::update::detail::backup_fallback_dir_ref() = previous;
        fs::remove_all(root);
    }
    fs::path sentinel() const {
        return root / "self_restart_sentinel";
    }
};

} // namespace

TEST_CASE("connection features subscribe, and a second attach does not double up",
          "[discovery_attach][threading]") {
    lv_init_safe();
    ScratchSentinelDir scratch;
    FakeMoonrakerClient client;

    attach_connection_features(client);
    CHECK(client.handler_count(TIMELAPSE, kTimelapseHandler) == 1);
    CHECK(client.handler_count(kHistory, kHistoryHandler) == 1);
    CHECK(client.handler_count(kUpdate, kUpdateHandler) == 1);
    const auto once = client.live_handlers();

    attach_connection_features(client);
    CHECK(client.live_handlers() == once);

    detach_connection_features(client);
}

TEST_CASE("a repeat attach delivers a timelapse event once",
          "[discovery_attach][timelapse_state]") {
    lv_init_safe();
    auto& state = helix::TimelapseState::instance();
    state.deinit_subjects();
    state.init_subjects(false);
    FakeMoonrakerClient client;

    state.attach(client);
    state.attach(client);
    REQUIRE(client.fire_notification(
        TIMELAPSE, json{{"action", "newframe"}, {"framefile", "f.jpg"}, {"framenum", 1}}));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(state.get_frame_count_subject()) == 1);

    state.detach(client);
    state.deinit_subjects();
}

TEST_CASE("detach leaves nothing subscribed", "[discovery_attach][threading]") {
    lv_init_safe();
    ScratchSentinelDir scratch;
    FakeMoonrakerClient client;
    const auto before = client.live_handlers();

    attach_connection_features(client);
    REQUIRE(client.live_handlers() != before);
    detach_connection_features(client);

    CHECK(client.live_handlers() == before);
}

TEST_CASE("a surviving self-restart sentinel is cleared once and Moonraker is told to refresh",
          "[discovery_attach][update_checker]") {
    ScratchSentinelDir scratch;
    FakeMoonrakerClient client;
    REQUIRE(UpdateChecker::write_self_restart_sentinel());
    REQUIRE(fs::exists(scratch.sentinel()));

    UpdateChecker::instance().on_connected(client);
    CHECK_FALSE(fs::exists(scratch.sentinel()));
    CHECK(client.rpc_count("machine.update.refresh") == 1);
    const auto* refresh = client.last_rpc();
    REQUIRE(refresh != nullptr);
    CHECK(refresh->params["name"] == "helixscreen");

    // A reconnect with no sentinel left asks for nothing.
    UpdateChecker::instance().on_connected(client);
    CHECK(client.rpc_count("machine.update.refresh") == 1);

    UpdateChecker::instance().detach(client);
}
