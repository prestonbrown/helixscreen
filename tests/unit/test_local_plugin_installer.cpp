// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_local_plugin_installer.cpp
 * @brief The shared local plugin installer's command sequence, run with each plugin's recipe,
 *        and the timelapse setup wizard's route
 *
 * Run with: ./build/bin/helix-tests "[timelapse][local_install]"
 */

#include "helix_plugin_installer.h"
#include "timelapse_host_mock.h"
#include "timelapse_local_installer.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string join(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& a : argv) {
        out += (out.empty() ? "" : " ") + a;
    }
    return out;
}

/// Records every command; answers the ones named in `answers` (by joined argv), 0 otherwise.
struct FakeRunner {
    std::vector<std::string> calls;
    std::map<std::string, CommandResult> answers;

    ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, int) {
            calls.push_back(join(argv));
            auto it = answers.find(calls.back());
            if (it != answers.end()) {
                return it->second;
            }
            CommandResult ok;
            ok.started = true;
            ok.exit_code = 0;
            return ok;
        };
    }
};

CommandResult exited(int code, std::string output = "") {
    CommandResult r;
    r.started = true;
    r.exit_code = code;
    r.output = std::move(output);
    return r;
}

const std::string kClone = "git clone https://github.com/mainsail-crew/moonraker-timelapse.git "
                           "/home/pi/moonraker-timelapse";
const std::string kLink =
    "sh /opt/helix/moonraker-plugin/install.sh --link-timelapse /home/pi/moonraker-timelapse";
const std::string kFindFfmpeg = "sh -c command -v ffmpeg";
const std::string kSudoProbe = "sudo -n true";
const std::string kAptUpdate = "sudo -n apt-get update";
const std::string kAptInstall =
    "sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y ffmpeg";

LocalPluginPlan fresh_plan() {
    LocalPluginPlan plan;
    plan.install_script = "/opt/helix/moonraker-plugin/install.sh";
    plan.repo_dir = "/home/pi/moonraker-timelapse";
    plan.repo_present = false;
    return plan;
}

} // namespace

// ============================================================================
// Route
// ============================================================================

TEST_CASE("timelapse setup takes the local route only for a local Moonraker it can install on",
          "[timelapse][local_install]") {
    TimelapseSetupFacts f;
    f.moonraker_local = true;
    f.can_install_locally = true;
    CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::LocalInstall);

    SECTION("a plugin that answers needs nothing") {
        f.plugin_present = true;
        f.job_holds = true;
        CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::AlreadyInstalled);
    }
    SECTION("a remote or not-yet-known Moonraker gets the SSH commands") {
        f.moonraker_local = false;
        CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::RemoteInstructions);
    }
    SECTION("no install.sh beside the app gets the SSH commands") {
        f.can_install_locally = false;
        CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::RemoteInstructions);
    }
    SECTION("a job holding the machine holds a local install") {
        f.job_holds = true;
        CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::WaitForJob);
    }
    SECTION("a job does not hold the SSH commands back") {
        f.job_holds = true;
        f.moonraker_local = false;
        CHECK(decide_timelapse_setup_path(f) == TimelapseSetupPath::RemoteInstructions);
    }
}

// ============================================================================
// Command sequence
// ============================================================================

TEST_CASE("a fresh local install clones, links, and finds ffmpeg", "[timelapse][local_install]") {
    FakeRunner fake;
    std::vector<LocalPluginStage> stages;
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner(),
                               [&](LocalPluginStage s) { stages.push_back(s); });

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    CHECK(result.detail.empty());
    CHECK(fake.calls == std::vector<std::string>{kClone, kLink, kFindFfmpeg});
    CHECK(stages == std::vector<LocalPluginStage>{LocalPluginStage::Fetch,
                                                  LocalPluginStage::Install,
                                                  LocalPluginStage::CheckDependency});
}

TEST_CASE("a local install reuses a checkout already there", "[timelapse][local_install]") {
    FakeRunner fake;
    auto plan = fresh_plan();
    plan.repo_present = true;
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    CHECK(fake.calls == std::vector<std::string>{kLink, kFindFfmpeg});
}

TEST_CASE("a failed clone stops the install and names git's last line",
          "[timelapse][local_install]") {
    FakeRunner fake;
    fake.answers[kClone] =
        exited(128, "Cloning into 'x'...\nfatal: unable to access 'https://github.com/': "
                    "Could not resolve host: github.com\n");
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.failed_stage == LocalPluginStage::Fetch);
    CHECK(result.detail ==
          "fatal: unable to access 'https://github.com/': Could not resolve host: github.com");
    CHECK(fake.calls == std::vector<std::string>{kClone});
}

TEST_CASE("a clone that times out reports the timeout", "[timelapse][local_install]") {
    FakeRunner fake;
    CommandResult slow;
    slow.started = true;
    slow.timed_out = true;
    fake.answers[kClone] = slow;
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.detail == "timed out");
}

TEST_CASE("a failed link stops the install before ffmpeg", "[timelapse][local_install]") {
    FakeRunner fake;
    fake.answers[kLink] = exited(1, "\033[0;31m[ERROR]\033[0m Could not auto-detect Moonraker "
                                    "installation.\n");
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.failed_stage == LocalPluginStage::Install);
    CHECK(result.detail == "[ERROR] Could not auto-detect Moonraker installation.");
    CHECK(fake.calls == std::vector<std::string>{kClone, kLink});
}

TEST_CASE("missing ffmpeg and a sudo that wants a password leave ffmpeg to the user",
          "[timelapse][local_install]") {
    FakeRunner fake;
    fake.answers[kFindFfmpeg] = exited(1);
    fake.answers[kSudoProbe] = exited(1, "sudo: a password is required\n");
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::InstalledMissingDependency);
    CHECK(result.missing_dependency == "ffmpeg");
    // Nothing is ever run through sudo without -n, so nothing can wait on a password.
    CHECK(fake.calls == std::vector<std::string>{kClone, kLink, kFindFfmpeg, kSudoProbe});
}

TEST_CASE("missing ffmpeg is installed through passwordless sudo", "[timelapse][local_install]") {
    FakeRunner fake;
    int finds = 0;
    auto base = fake.runner();
    ProcessRunner runner = [&](const std::vector<std::string>& argv, int t) {
        if (join(argv) == kFindFfmpeg) {
            fake.calls.push_back(kFindFfmpeg);
            // Missing before the install, there after it.
            return exited(++finds == 1 ? 1 : 0);
        }
        return base(argv, t);
    };
    std::vector<LocalPluginStage> stages;
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), runner,
                                               [&](LocalPluginStage s) { stages.push_back(s); });

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    CHECK(fake.calls == std::vector<std::string>{kClone, kLink, kFindFfmpeg, kSudoProbe, kAptUpdate,
                                                 kAptInstall, kFindFfmpeg});
    CHECK(stages.back() == LocalPluginStage::InstallDependency);
}

TEST_CASE("a failed apt install leaves ffmpeg to the user with apt's last line",
          "[timelapse][local_install]") {
    FakeRunner fake;
    fake.answers[kFindFfmpeg] = exited(1);
    fake.answers[kAptUpdate] = exited(100, "E: Could not get lock /var/lib/apt/lists/lock\n");
    fake.answers[kAptInstall] = exited(100, "E: Unable to locate package ffmpeg\n");
    const auto result =
        install_plugin_locally(timelapse_plugin_recipe(), fresh_plan(), fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::InstalledMissingDependency);
    CHECK(result.detail == "E: Unable to locate package ffmpeg");
    // A failed list refresh still tries the install.
    CHECK(fake.calls == std::vector<std::string>{kClone, kLink, kFindFfmpeg, kSudoProbe, kAptUpdate,
                                                 kAptInstall});
}

TEST_CASE("make_timelapse_install_plan names the clone under home", "[timelapse][local_install]") {
    const auto plan =
        make_local_plugin_plan(timelapse_plugin_recipe(), "/nonexistent-home", "/x/install.sh");
    CHECK(plan.repo_dir == "/nonexistent-home/moonraker-timelapse");
    CHECK(plan.install_script == "/x/install.sh");
    CHECK_FALSE(plan.repo_present);
}

// ============================================================================
// HelixPrint through the same engine
// ============================================================================

TEST_CASE("HelixPrint's recipe runs only install.sh --auto: nothing to fetch, no dependencies",
          "[timelapse][local_install][plugin_installer]") {
    FakeRunner fake;
    LocalPluginPlan plan;
    plan.install_script = "/opt/helix/moonraker-plugin/install.sh";
    const auto recipe = helix_print_plugin_recipe(42);
    CHECK(recipe.install_timeout_seconds == 42);

    const auto ok = install_plugin_locally(recipe, plan, fake.runner());
    CHECK(ok.outcome == LocalPluginOutcome::Installed);
    CHECK(fake.calls == std::vector<std::string>{
                            "sh /opt/helix/moonraker-plugin/install.sh --no-restart --auto"});

    fake.calls.clear();
    fake.answers["sh /opt/helix/moonraker-plugin/install.sh --no-restart --auto"] =
        exited(1, "[ERROR] Cannot write to components directory: /x\n");
    const auto bad = install_plugin_locally(recipe, plan, fake.runner());
    CHECK(bad.outcome == LocalPluginOutcome::Failed);
    CHECK(bad.failed_stage == LocalPluginStage::Install);
    CHECK(bad.failure.exit_code == 1);
    CHECK(fake.calls.size() == 1);
}

// ============================================================================
// Mock knob
// ============================================================================

TEST_CASE("HELIX_MOCK_TIMELAPSE values", "[timelapse][local_install]") {
    using mock::TimelapseHost;
    CHECK(mock::parse_timelapse_host(nullptr) == TimelapseHost::Present);
    CHECK(mock::parse_timelapse_host("") == TimelapseHost::Present);
    CHECK(mock::parse_timelapse_host("absent") == TimelapseHost::Absent);
    CHECK(mock::parse_timelapse_host("no-ffmpeg") == TimelapseHost::NoFfmpeg);
    CHECK(mock::parse_timelapse_host("no-sudo") == TimelapseHost::NoSudo);
    CHECK(mock::parse_timelapse_host("bogus") == TimelapseHost::Present);
}
