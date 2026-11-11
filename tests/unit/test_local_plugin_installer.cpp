// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_local_plugin_installer.cpp
 * @brief The shared local plugin installer's command sequence, run with each plugin's recipe,
 *        and the timelapse setup wizard's route
 *
 * Run with: ./build/bin/helix-tests "[timelapse][local_install]"
 */

#include "../test_helpers/scoped_env.h"
#include "../test_helpers/unique_temp_dir.h"
#include "helix_plugin_installer.h"
#include "timelapse_host_mock.h"
#include "timelapse_local_installer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
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
/// A clone leaves its destination behind the way git does when it stops part way,
/// and fills it with a checkout only when it succeeds.
struct FakeRunner {
    std::vector<std::string> calls;
    std::map<std::string, CommandResult> answers;

    ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, int) {
            calls.push_back(join(argv));
            CommandResult r;
            r.started = true;
            r.exit_code = 0;
            if (auto it = answers.find(calls.back()); it != answers.end()) {
                r = it->second;
            }
            if (std::find(argv.begin(), argv.end(), "clone") != argv.end()) {
                const std::string dest = argv.back();
                std::filesystem::create_directories(dest + "/.git");
                if (r.succeeded()) {
                    std::filesystem::create_directories(dest + "/component");
                    std::ofstream(dest + "/component/timelapse.py") << "# plugin\n";
                }
            }
            return r;
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

const std::string kInstallScript = "/opt/helix/moonraker-plugin/install.sh";
const std::string kFindFfmpeg = "/bin/sh -c command -v \"$1\" sh ffmpeg";

/// A home directory of its own, so the clone and its rename land on a real filesystem.
struct TempHome {
    std::string path = helix::test::unique_temp_dir("plugin_home");
    TempHome() {
        std::filesystem::create_directories(path);
    }
    ~TempHome() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] LocalPluginPlan plan() const {
        return make_local_plugin_plan(timelapse_plugin_recipe(), path, kInstallScript);
    }
    [[nodiscard]] std::string repo() const {
        return path + "/moonraker-timelapse";
    }
    /// Everything in home, so a test can see that nothing was left behind.
    [[nodiscard]] std::vector<std::string> entries() const {
        std::vector<std::string> names;
        for (const auto& e : std::filesystem::directory_iterator(path)) {
            names.push_back(e.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
};

std::string clone_cmd(const LocalPluginPlan& plan) {
    return "env GIT_TERMINAL_PROMPT=0 git -c http.lowSpeedLimit=1000 -c "
           "http.lowSpeedTime=30 clone "
           "https://github.com/mainsail-crew/moonraker-timelapse.git " +
           plan.fetch_dir;
}

std::string link_cmd(const LocalPluginPlan& plan) {
    return "sh " + kInstallScript + " --link-timelapse " + plan.repo_dir;
}

std::string git_in(const LocalPluginPlan& plan, const std::string& args) {
    return "git -C " + plan.repo_dir + " " + args;
}

/// A checkout already in home, so the install reuses it.
void make_existing_checkout(const TempHome& home) {
    std::filesystem::create_directories(home.repo() + "/component");
    std::ofstream(home.repo() + "/component/timelapse.py") << "# plugin\n";
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

TEST_CASE("the timelapse macros are included only when Klipper lacks them",
          "[timelapse][local_install]") {
    TimelapseMacroFacts f;
    f.objects_reported = true;
    f.helix_macros_loaded = true;
    f.macro_file_known = true;
    CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::IncludeFromHelixMacros);

    SECTION("Klipper's objects not in yet: nothing is said") {
        f.objects_reported = false;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::NotYetKnown);
        f.helix_macros_loaded = false;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::NotYetKnown);
    }
    SECTION("the frame macro is loaded") {
        f.has_frame_macro = true;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::Loaded);
        f.helix_macros_loaded = false;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::Loaded);
    }
    SECTION("no helper macros") {
        f.helix_macros_loaded = false;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::ShowIncludeLine);
    }
    SECTION("the checkout's path on the Klipper host is unknown") {
        f.macro_file_known = false;
        CHECK(decide_timelapse_macro_setup(f) == TimelapseMacroSetup::ShowIncludeLine);
    }
    CHECK(std::string(kTimelapseIncludeLine) == "[include timelapse.cfg]");
}

TEST_CASE("the macro file is known only on Moonraker's host with the checkout there",
          "[timelapse][local_install]") {
    TempHome home;
    helix::ScopedEnv home_env("HOME", home.path.c_str());
    CHECK(timelapse_macro_repo_on_host(true).empty());
    std::filesystem::create_directories(home.repo() + "/klipper_macro");
    std::ofstream(home.repo() + "/klipper_macro/timelapse.cfg") << "\n";
    CHECK(timelapse_macro_repo_on_host(true) == home.repo());
    CHECK(timelapse_macro_repo_on_host(false).empty());
}

TEST_CASE("a fresh local install clones, links, and finds ffmpeg", "[timelapse][local_install]") {
    TempHome home;
    FakeRunner fake;
    const auto plan = home.plan();
    std::vector<LocalPluginStage> stages;
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner(),
                                               [&](LocalPluginStage s) { stages.push_back(s); });

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    CHECK(result.detail.empty());
    CHECK(fake.calls == std::vector<std::string>{clone_cmd(plan), link_cmd(plan), kFindFfmpeg});
    CHECK(stages == std::vector<LocalPluginStage>{LocalPluginStage::Fetch,
                                                  LocalPluginStage::Install,
                                                  LocalPluginStage::CheckDependency});
    // The clone reached its place whole, and its staging directory is gone.
    CHECK(std::filesystem::exists(home.repo() + "/component/timelapse.py"));
    CHECK(home.entries() == std::vector<std::string>{"moonraker-timelapse"});
}

TEST_CASE("a local install clones beside home's checkout, never into it",
          "[timelapse][local_install]") {
    TempHome home;
    const auto plan = home.plan();
    CHECK(plan.fetch_dir.rfind(home.path + "/.moonraker-timelapse.partial-", 0) == 0);
    CHECK(plan.fetch_dir != plan.repo_dir);
}

TEST_CASE("a local install reuses a checkout already there", "[timelapse][local_install]") {
    TempHome home;
    std::filesystem::create_directories(home.repo() + "/component");
    std::ofstream(home.repo() + "/component/timelapse.py") << "# plugin\n";
    std::ofstream(home.repo() + "/local-edit") << "mine\n";
    FakeRunner fake;
    const auto plan = home.plan();
    REQUIRE(plan.repo_present);
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    // On a branch already: nothing in the checkout is touched.
    CHECK(fake.calls == std::vector<std::string>{git_in(plan, "symbolic-ref -q HEAD"),
                                                 link_cmd(plan), kFindFfmpeg});
    CHECK(std::filesystem::exists(home.repo() + "/local-edit"));
}

TEST_CASE("a reused checkout on a detached HEAD is put back on main for the update manager",
          "[timelapse][local_install]") {
    TempHome home;
    make_existing_checkout(home);
    FakeRunner fake;
    const auto plan = home.plan();
    REQUIRE(plan.repo_present);
    fake.answers[git_in(plan, "symbolic-ref -q HEAD")] = exited(1);

    SECTION("a clean tree with a local main switches to it") {
        const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());
        CHECK(result.outcome == LocalPluginOutcome::Installed);
        CHECK(fake.calls == std::vector<std::string>{
                                git_in(plan, "symbolic-ref -q HEAD"),
                                git_in(plan, "status --porcelain"),
                                git_in(plan, "rev-parse -q --verify refs/heads/main"),
                                git_in(plan, "merge-base --is-ancestor HEAD refs/heads/main"),
                                git_in(plan, "checkout -q main"), link_cmd(plan), kFindFfmpeg});
    }
    SECTION("local changes keep it detached") {
        fake.answers[git_in(plan, "status --porcelain")] = exited(0, " M component/timelapse.py\n");
        const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());
        CHECK(result.outcome == LocalPluginOutcome::Installed);
        CHECK(fake.calls == std::vector<std::string>{git_in(plan, "symbolic-ref -q HEAD"),
                                                     git_in(plan, "status --porcelain"),
                                                     link_cmd(plan), kFindFfmpeg});
    }
    SECTION("no local main keeps it detached") {
        fake.answers[git_in(plan, "rev-parse -q --verify refs/heads/main")] = exited(1);
        const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());
        CHECK(result.outcome == LocalPluginOutcome::Installed);
        CHECK(fake.calls ==
              std::vector<std::string>{git_in(plan, "symbolic-ref -q HEAD"),
                                       git_in(plan, "status --porcelain"),
                                       git_in(plan, "rev-parse -q --verify refs/heads/main"),
                                       link_cmd(plan), kFindFfmpeg});
    }
    SECTION("a main that does not contain HEAD keeps it detached: no downgrade") {
        fake.answers[git_in(plan, "merge-base --is-ancestor HEAD refs/heads/main")] = exited(1);
        const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());
        CHECK(result.outcome == LocalPluginOutcome::Installed);
        CHECK(std::find(fake.calls.begin(), fake.calls.end(), git_in(plan, "checkout -q main")) ==
              fake.calls.end());
        CHECK(fake.calls.back() == kFindFfmpeg);
    }
    SECTION("a failed switch still installs the plugin") {
        fake.answers[git_in(plan, "checkout -q main")] = exited(1, "error: cannot switch\n");
        const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());
        CHECK(result.outcome == LocalPluginOutcome::Installed);
        CHECK(fake.calls.back() == kFindFfmpeg);
    }
}

TEST_CASE("a reused directory that is not a git repository is left alone",
          "[timelapse][local_install]") {
    TempHome home;
    make_existing_checkout(home);
    FakeRunner fake;
    const auto plan = home.plan();
    fake.answers[git_in(plan, "symbolic-ref -q HEAD")] =
        exited(128, "fatal: not a git repository\n");
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Installed);
    CHECK(fake.calls == std::vector<std::string>{git_in(plan, "symbolic-ref -q HEAD"),
                                                 link_cmd(plan), kFindFfmpeg});
}

TEST_CASE("a failed clone stops the install and names git's last line",
          "[timelapse][local_install]") {
    TempHome home;
    FakeRunner fake;
    const auto plan = home.plan();
    fake.answers[clone_cmd(plan)] =
        exited(128, "Cloning into 'x'...\nfatal: unable to access 'https://github.com/': "
                    "Could not resolve host: github.com\n");
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.failed_stage == LocalPluginStage::Fetch);
    CHECK(result.detail ==
          "fatal: unable to access 'https://github.com/': Could not resolve host: github.com");
    CHECK(fake.calls == std::vector<std::string>{clone_cmd(plan)});
}

TEST_CASE("a clone stopped part way leaves nothing that blocks the retry",
          "[timelapse][local_install]") {
    TempHome home;
    FakeRunner fake;
    const auto plan = home.plan();
    CommandResult slow;
    slow.started = true;
    slow.timed_out = true;
    fake.answers[clone_cmd(plan)] = slow;
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.detail == "timed out");
    CHECK(home.entries().empty());

    FakeRunner retry;
    const auto again = home.plan();
    CHECK_FALSE(again.repo_present);
    CHECK(install_plugin_locally(timelapse_plugin_recipe(), again, retry.runner()).outcome ==
          LocalPluginOutcome::Installed);
    CHECK(std::filesystem::exists(home.repo() + "/component/timelapse.py"));
}

TEST_CASE("a directory of the user's in the clone's place is left alone",
          "[timelapse][local_install]") {
    TempHome home;
    std::filesystem::create_directories(home.repo());
    std::ofstream(home.repo() + "/notes.txt") << "mine\n";
    FakeRunner fake;
    const auto plan = home.plan();
    REQUIRE_FALSE(plan.repo_present);
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.failed_stage == LocalPluginStage::Fetch);
    CHECK(result.detail.find(home.repo()) != std::string::npos);
    CHECK(fake.calls == std::vector<std::string>{clone_cmd(plan)});
    CHECK(std::filesystem::exists(home.repo() + "/notes.txt"));
    CHECK_FALSE(std::filesystem::exists(home.repo() + "/component"));
    CHECK(home.entries() == std::vector<std::string>{"moonraker-timelapse"});
}

TEST_CASE("a failed link stops the install before ffmpeg", "[timelapse][local_install]") {
    TempHome home;
    FakeRunner fake;
    const auto plan = home.plan();
    fake.answers[link_cmd(plan)] = exited(1, "\033[0;31m[ERROR]\033[0m Could not auto-detect "
                                             "Moonraker installation.\n");
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner());

    CHECK(result.outcome == LocalPluginOutcome::Failed);
    CHECK(result.failed_stage == LocalPluginStage::Install);
    CHECK(result.detail == "[ERROR] Could not auto-detect Moonraker installation.");
    CHECK(fake.calls == std::vector<std::string>{clone_cmd(plan), link_cmd(plan)});
}

TEST_CASE("missing ffmpeg is left to the user: nothing is installed for it",
          "[timelapse][local_install]") {
    TempHome home;
    FakeRunner fake;
    const auto plan = home.plan();
    fake.answers[kFindFfmpeg] = exited(1);
    std::vector<LocalPluginStage> stages;
    const auto result = install_plugin_locally(timelapse_plugin_recipe(), plan, fake.runner(),
                                               [&](LocalPluginStage s) { stages.push_back(s); });

    CHECK(result.outcome == LocalPluginOutcome::InstalledMissingDependency);
    CHECK(result.missing_dependency == "ffmpeg");
    CHECK(fake.calls == std::vector<std::string>{clone_cmd(plan), link_cmd(plan), kFindFfmpeg});
    CHECK(stages.back() == LocalPluginStage::CheckDependency);
    for (const auto& call : fake.calls) {
        CHECK(call.find("sudo") == std::string::npos);
        CHECK(call.find("apt") == std::string::npos);
    }
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
    CHECK(mock::parse_timelapse_host("no-sudo") == TimelapseHost::Present);
    CHECK(mock::parse_timelapse_host("bogus") == TimelapseHost::Present);
    CHECK(mock::parse_timelapse_host("helix-macros, no-ffmpeg") == TimelapseHost::NoFfmpeg);
    CHECK(mock::parse_timelapse_host("helix-macros") == TimelapseHost::Present);
    CHECK(mock::parse_timelapse_helix_macros("absent,helix-macros"));
    CHECK(mock::parse_timelapse_helix_macros("helix-macros"));
    CHECK_FALSE(mock::parse_timelapse_helix_macros("absent"));
    CHECK_FALSE(mock::parse_timelapse_helix_macros(nullptr));
}

// ============================================================================
// The real runner
// ============================================================================

#if defined(__linux__)
TEST_CASE("run_process children read /dev/null and hold none of the app's descriptors",
          "[timelapse][local_install][process_runner]") {
    const auto stdin_target = run_process({"readlink", "/proc/self/fd/0"}, 10);
    REQUIRE(stdin_target.succeeded());
    CHECK(last_output_line(stdin_target.output) == "/dev/null");

    // A descriptor the app holds without close-on-exec must not reach the child.
    const int fd = open("/dev/null", O_RDONLY);
    REQUIRE(fd > 2);
    const std::string probe = "[ -e /proc/self/fd/" + std::to_string(fd) +
                              " ] && echo open || "
                              "echo closed";
    const auto leaked = run_process({"/bin/sh", "-c", probe}, 10);
    close(fd);
    REQUIRE(leaked.succeeded());
    CHECK(last_output_line(leaked.output) == "closed");
}
#endif

TEST_CASE("a cancelled runner ends its command at once",
          "[timelapse][local_install][process_runner]") {
    std::atomic<bool> cancel{true};
    const ProcessRunner run = cancellable_process_runner(cancel);
    const auto start = std::chrono::steady_clock::now();
    const auto r = run({"sleep", "30"}, 60);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK_FALSE(r.succeeded());
    CHECK(r.timed_out);
    CHECK(elapsed < std::chrono::seconds(5));

    cancel.store(false);
    CHECK(run({"true"}, 10).succeeded());
}
