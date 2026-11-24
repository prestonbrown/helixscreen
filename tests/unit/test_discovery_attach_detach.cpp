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
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_constants.h"
#include "system/update_checker.h"
#include "timelapse_state.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
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
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

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

TEST_CASE("a sentinel naming a running install.sh survives a reconnect",
          "[discovery_attach][update_checker][sentinel]") {
    ScratchSentinelDir scratch;
    FakeMoonrakerClient client;
    // A stand-in installer still running, as when install.sh restarts Moonraker.
    const fs::path script = scratch.root / "install.sh";
    { std::ofstream(script) << "#!/bin/sh\nsleep 30\n"; }
    const pid_t installer = fork();
    if (installer == 0) {
        execl("/bin/sh", "sh", script.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    REQUIRE(installer > 0);
    // Wait until the child is the shell running install.sh, as the check reads it.
    bool running = false;
    for (int i = 0; i < 500 && !running; ++i) {
        std::ifstream cmdline("/proc/" + std::to_string(installer) + "/cmdline");
        std::string args((std::istreambuf_iterator<char>(cmdline)),
                         std::istreambuf_iterator<char>());
        running = args.find("install.sh") != std::string::npos;
        if (!running) {
            usleep(10 * 1000);
        }
    }
    REQUIRE(running);
    REQUIRE(UpdateChecker::write_self_restart_sentinel(installer));
    // Old enough that only the running installer can keep it.
    fs::last_write_time(scratch.sentinel(),
                        fs::file_time_type::clock::now() - std::chrono::seconds(60));

    UpdateChecker::instance().on_connected(client);
    CHECK(fs::exists(scratch.sentinel()));
    CHECK(client.rpc_count("machine.update.refresh") == 0);

    kill(installer, SIGKILL);
    waitpid(installer, nullptr, 0);
    UpdateChecker::instance().on_connected(client);
    CHECK_FALSE(fs::exists(scratch.sentinel()));
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
