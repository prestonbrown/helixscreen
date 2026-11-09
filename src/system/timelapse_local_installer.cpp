// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_local_installer.h"

#include "helix_fs.h"
#include "helix_plugin_installer.h"
#include "runtime_config.h"
#include "timelapse_host_mock.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {
// A full clone: Moonraker's update manager tracks the repository it names.
constexpr int kCloneTimeoutSeconds = 300;
// install.sh --link-timelapse walks /proc and the config; it never waits on Moonraker.
constexpr int kLinkTimeoutSeconds = 60;
constexpr int kProbeTimeoutSeconds = 15;
// apt on a Pi 3: the package lists, then ffmpeg and its libraries.
constexpr int kAptUpdateTimeoutSeconds = 600;
constexpr int kAptInstallTimeoutSeconds = 1200;

TimelapseInstallResult failed(TimelapseInstallStage stage, const CommandResult& r) {
    TimelapseInstallResult result;
    result.outcome = TimelapseInstallOutcome::Failed;
    result.failed_stage = stage;
    result.detail = r.timed_out ? std::string("timed out") : last_output_line(r.output);
    return result;
}

void log_failure(const char* what, const std::vector<std::string>& argv, const CommandResult& r) {
    spdlog::warn("[TimelapseInstaller] {} failed (exit {}{}): {}", what, r.exit_code,
                 r.timed_out ? ", timed out" : "", argv.empty() ? "" : argv.front());
    for (const auto& line : script_output_lines(r.output)) {
        spdlog::warn("[TimelapseInstaller]   {}", line);
    }
}
} // namespace

TimelapseSetupPath decide_timelapse_setup_path(const TimelapseSetupFacts& facts) {
    if (facts.plugin_present) {
        return TimelapseSetupPath::AlreadyInstalled;
    }
    if (!facts.moonraker_local || !facts.can_install_locally) {
        return TimelapseSetupPath::RemoteInstructions;
    }
    return facts.job_holds ? TimelapseSetupPath::WaitForJob : TimelapseSetupPath::LocalInstall;
}

TimelapseInstallPlan make_timelapse_install_plan(const std::string& home,
                                                 const std::string& install_script) {
    TimelapseInstallPlan plan;
    plan.install_script = install_script;
    plan.repo_dir = home + "/" + kTimelapseRepoDirName;
    plan.repo_present = fs::exists(plan.repo_dir + "/component/timelapse.py");
    return plan;
}

TimelapseInstallResult
install_timelapse_locally(const TimelapseInstallPlan& plan, const ProcessRunner& run,
                          const std::function<void(TimelapseInstallStage)>& on_stage) {
    auto stage = [&](TimelapseInstallStage s) {
        if (on_stage) {
            on_stage(s);
        }
    };

    if (!plan.repo_present) {
        stage(TimelapseInstallStage::Clone);
        const std::vector<std::string> argv = {"git", "clone", kTimelapseRepoUrl, plan.repo_dir};
        spdlog::info("[TimelapseInstaller] Cloning {} into {}", kTimelapseRepoUrl, plan.repo_dir);
        const CommandResult r = run(argv, kCloneTimeoutSeconds);
        if (!r.succeeded()) {
            log_failure("git clone", argv, r);
            return failed(TimelapseInstallStage::Clone, r);
        }
    } else {
        spdlog::info("[TimelapseInstaller] Using the existing checkout at {}", plan.repo_dir);
    }

    stage(TimelapseInstallStage::Link);
    {
        const std::vector<std::string> argv = {"sh", plan.install_script, "--link-timelapse",
                                               plan.repo_dir};
        const CommandResult r = run(argv, kLinkTimeoutSeconds);
        if (!r.succeeded()) {
            log_failure("install.sh --link-timelapse", argv, r);
            return failed(TimelapseInstallStage::Link, r);
        }
        for (const auto& line : script_output_lines(r.output)) {
            spdlog::info("[TimelapseInstaller] install.sh: {}", line);
        }
    }

    TimelapseInstallResult result;
    result.outcome = TimelapseInstallOutcome::Installed;

    stage(TimelapseInstallStage::CheckFfmpeg);
    const std::vector<std::string> find_ffmpeg = {"sh", "-c", "command -v ffmpeg"};
    if (run(find_ffmpeg, kProbeTimeoutSeconds).succeeded()) {
        spdlog::info("[TimelapseInstaller] ffmpeg is installed");
        return result;
    }
    if (!run({"sudo", "-n", "true"}, kProbeTimeoutSeconds).succeeded()) {
        spdlog::info("[TimelapseInstaller] ffmpeg is missing and sudo needs a password; the "
                     "user installs it");
        result.outcome = TimelapseInstallOutcome::InstalledNoFfmpeg;
        return result;
    }

    stage(TimelapseInstallStage::InstallFfmpeg);
    spdlog::info("[TimelapseInstaller] Installing ffmpeg through passwordless sudo");
    // A failed list refresh still leaves the install a chance with the lists already there.
    const std::vector<std::string> update = {"sudo", "-n", "apt-get", "update"};
    const CommandResult updated = run(update, kAptUpdateTimeoutSeconds);
    if (!updated.succeeded()) {
        log_failure("apt-get update", update, updated);
    }
    const std::vector<std::string> install = {
        "sudo",    "-n",      "env", "DEBIAN_FRONTEND=noninteractive",
        "apt-get", "install", "-y",  "ffmpeg"};
    const CommandResult installed = run(install, kAptInstallTimeoutSeconds);
    if (!installed.succeeded() || !run(find_ffmpeg, kProbeTimeoutSeconds).succeeded()) {
        log_failure("apt-get install ffmpeg", install, installed);
        result.outcome = TimelapseInstallOutcome::InstalledNoFfmpeg;
        result.detail = installed.timed_out ? "timed out" : last_output_line(installed.output);
        return result;
    }
    spdlog::info("[TimelapseInstaller] ffmpeg installed");
    return result;
}

ProcessRunner timelapse_install_runner() {
    // A mock Moonraker is not on this host: its install is simulated, never run here.
    if (get_runtime_config()->should_mock_moonraker()) {
        return mock::timelapse_host_runner();
    }
    return run_process;
}

} // namespace helix
