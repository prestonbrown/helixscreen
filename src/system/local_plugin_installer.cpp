// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "local_plugin_installer.h"

#include "helix_fs.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {
// A full clone: Moonraker's update manager tracks the repository it names.
constexpr int kFetchTimeoutSeconds = 300;
constexpr int kProbeTimeoutSeconds = 15;
// apt on a Pi 3: the package lists, then the package and its libraries.
constexpr int kAptUpdateTimeoutSeconds = 600;
constexpr int kAptInstallTimeoutSeconds = 1200;

std::string failure_detail(const CommandResult& r) {
    return r.timed_out ? std::string("timed out") : last_output_line(r.output);
}

LocalPluginResult failed(LocalPluginStage stage, const CommandResult& r) {
    LocalPluginResult result;
    result.outcome = LocalPluginOutcome::Failed;
    result.failed_stage = stage;
    result.failure = r;
    result.detail = failure_detail(r);
    return result;
}

bool has_program(const ProcessRunner& run, const std::string& program) {
    return run({"sh", "-c", "command -v " + program}, kProbeTimeoutSeconds).succeeded();
}
} // namespace

LocalPluginPlan make_local_plugin_plan(const LocalPluginRecipe& recipe, const std::string& home,
                                       const std::string& install_script) {
    LocalPluginPlan plan;
    plan.install_script = install_script;
    if (!recipe.repo_url.empty()) {
        plan.repo_dir = home + "/" + recipe.repo_dir_name;
        plan.repo_present = fs::exists(plan.repo_dir + "/" + recipe.repo_marker);
    }
    return plan;
}

LocalPluginResult install_plugin_locally(const LocalPluginRecipe& recipe,
                                         const LocalPluginPlan& plan, const ProcessRunner& run,
                                         const std::function<void(LocalPluginStage)>& on_stage) {
    auto stage = [&](LocalPluginStage s) {
        if (on_stage) {
            on_stage(s);
        }
    };

    if (!recipe.repo_url.empty() && !plan.repo_present) {
        stage(LocalPluginStage::Fetch);
        spdlog::info("[PluginInstaller] Cloning {} into {}", recipe.repo_url, plan.repo_dir);
        const CommandResult r =
            run({"git", "clone", recipe.repo_url, plan.repo_dir}, kFetchTimeoutSeconds);
        log_command_output("git clone", r.output, r.succeeded());
        if (!r.succeeded()) {
            return failed(LocalPluginStage::Fetch, r);
        }
    } else if (!recipe.repo_url.empty()) {
        spdlog::info("[PluginInstaller] Using the existing {} checkout at {}", recipe.name,
                     plan.repo_dir);
    }

    stage(LocalPluginStage::Install);
    std::vector<std::string> argv = {"sh", plan.install_script};
    for (const auto& arg : recipe.install_args) {
        argv.push_back(arg == kLocalPluginRepoDirArg ? plan.repo_dir : arg);
    }
    const CommandResult installed = run(argv, recipe.install_timeout_seconds);
    log_command_output("install.sh", installed.output, installed.succeeded());
    if (!installed.succeeded()) {
        return failed(LocalPluginStage::Install, installed);
    }

    LocalPluginResult result;
    result.outcome = LocalPluginOutcome::Installed;
    for (const auto& dep : recipe.dependencies) {
        stage(LocalPluginStage::CheckDependency);
        if (has_program(run, dep.program)) {
            spdlog::info("[PluginInstaller] {} is installed", dep.name);
            continue;
        }
        if (!run({"sudo", "-n", "true"}, kProbeTimeoutSeconds).succeeded()) {
            spdlog::info("[PluginInstaller] {} is missing and sudo needs a password; the user "
                         "installs it",
                         dep.name);
            result.outcome = LocalPluginOutcome::InstalledMissingDependency;
            result.missing_dependency = dep.name;
            return result;
        }
        stage(LocalPluginStage::InstallDependency);
        spdlog::info("[PluginInstaller] Installing {} through passwordless sudo", dep.name);
        // A failed list refresh still leaves the install a chance with the lists already there.
        const CommandResult updated =
            run({"sudo", "-n", "apt-get", "update"}, kAptUpdateTimeoutSeconds);
        log_command_output("apt-get update", updated.output, updated.succeeded());
        const CommandResult apt = run({"sudo", "-n", "env", "DEBIAN_FRONTEND=noninteractive",
                                       "apt-get", "install", "-y", dep.apt_package},
                                      kAptInstallTimeoutSeconds);
        log_command_output("apt-get install", apt.output, apt.succeeded());
        if (!apt.succeeded() || !has_program(run, dep.program)) {
            result.outcome = LocalPluginOutcome::InstalledMissingDependency;
            result.missing_dependency = dep.name;
            result.failure = apt;
            result.detail = failure_detail(apt);
            return result;
        }
        spdlog::info("[PluginInstaller] {} installed", dep.name);
    }
    return result;
}

} // namespace helix
