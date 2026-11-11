// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "local_plugin_installer.h"

#include "helix_fs.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unistd.h>

namespace helix {

namespace {
// A full clone, since Moonraker's update manager tracks the repository it
// names: well under a megabyte, so minutes only on a stalled link, which git
// itself gives up on below 1 KB/s for 30 s.
constexpr int kFetchTimeoutSeconds = 180;
constexpr const char* kFetchLowSpeedLimit = "http.lowSpeedLimit=1000";
constexpr const char* kFetchLowSpeedTime = "http.lowSpeedTime=30";
constexpr int kProbeTimeoutSeconds = 15;

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

/// Moves an existing checkout on a detached HEAD back onto recipe.repo_branch, so
/// Moonraker's update manager can validate it. Local git only, and only when the
/// tree is clean and the branch exists and contains HEAD; any other case is
/// logged and left alone.
void attach_checkout_to_branch(const LocalPluginRecipe& recipe, const std::string& repo_dir,
                               const ProcessRunner& run) {
    if (recipe.repo_branch.empty()) {
        return;
    }
    auto git = [&](std::vector<std::string> args) {
        std::vector<std::string> argv = {"git", "-C", repo_dir};
        argv.insert(argv.end(), args.begin(), args.end());
        return run(argv, kProbeTimeoutSeconds);
    };

    // symbolic-ref exits 0 on a branch, 1 on a detached HEAD, 128 outside a repository.
    const CommandResult head = git({"symbolic-ref", "-q", "HEAD"});
    if (head.succeeded()) {
        return;
    }
    if (!head.started || head.timed_out || head.exit_code != 1) {
        spdlog::info("[PluginInstaller] {} is not a git checkout git can read ({}); leaving it "
                     "as it is",
                     repo_dir, failure_detail(head));
        return;
    }

    const CommandResult status = git({"status", "--porcelain"});
    if (!status.succeeded()) {
        spdlog::info("[PluginInstaller] Could not read the state of {} ({}); leaving it on a "
                     "detached HEAD",
                     repo_dir, failure_detail(status));
        return;
    }
    if (!last_output_line(status.output).empty()) {
        spdlog::info("[PluginInstaller] {} is on a detached HEAD with local changes; leaving it "
                     "so they are kept. Moonraker will not update it until it is on {}",
                     repo_dir, recipe.repo_branch);
        return;
    }

    if (!git({"rev-parse", "-q", "--verify", "refs/heads/" + recipe.repo_branch}).succeeded()) {
        spdlog::info("[PluginInstaller] {} is on a detached HEAD and has no local {} branch; "
                     "leaving it as it is",
                     repo_dir, recipe.repo_branch);
        return;
    }

    // Only forward: a stale local branch would downgrade the plugin, and commits
    // on no branch would be left behind.
    const CommandResult ahead =
        git({"merge-base", "--is-ancestor", "HEAD", "refs/heads/" + recipe.repo_branch});
    if (!ahead.succeeded()) {
        spdlog::info("[PluginInstaller] {} is on a detached HEAD that its {} branch does not "
                     "contain ({}); leaving it as it is",
                     repo_dir, recipe.repo_branch,
                     ahead.exit_code == 1 ? std::string("switching would go back")
                                          : failure_detail(ahead));
        return;
    }

    const CommandResult switched = git({"checkout", "-q", recipe.repo_branch});
    log_command_output("git checkout", switched.output, switched.succeeded());
    if (!switched.succeeded()) {
        spdlog::warn("[PluginInstaller] Could not switch {} to {} ({}); leaving it detached",
                     repo_dir, recipe.repo_branch, failure_detail(switched));
        return;
    }
    spdlog::info("[PluginInstaller] Switched {} from a detached HEAD to {}", repo_dir,
                 recipe.repo_branch);
}

bool has_program(const ProcessRunner& run, const std::string& program) {
    return run({"/bin/sh", "-c", "command -v \"$1\"", "sh", program}, kProbeTimeoutSeconds)
        .succeeded();
}
} // namespace

LocalPluginPlan make_local_plugin_plan(const LocalPluginRecipe& recipe, const std::string& home,
                                       const std::string& install_script) {
    LocalPluginPlan plan;
    plan.install_script = install_script;
    if (!recipe.repo_url.empty()) {
        plan.repo_dir = home + "/" + recipe.repo_dir_name;
        plan.fetch_dir =
            home + "/." + recipe.repo_dir_name + ".partial-" + std::to_string(::getpid());
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
        spdlog::info("[PluginInstaller] Cloning {} into {} for {}", recipe.repo_url, plan.fetch_dir,
                     plan.repo_dir);
        // fetch_dir is this installer's own name, so whatever is there is ours to clear.
        fs::remove_all(plan.fetch_dir);
        // GIT_TERMINAL_PROMPT=0: a repository that asks for credentials fails at once
        // instead of waiting on a prompt nobody sees.
        const CommandResult r =
            run({"env", "GIT_TERMINAL_PROMPT=0", "git", "-c", kFetchLowSpeedLimit, "-c",
                 kFetchLowSpeedTime, "clone", recipe.repo_url, plan.fetch_dir},
                kFetchTimeoutSeconds);
        log_command_output("git clone", r.output, r.succeeded());
        if (!r.succeeded()) {
            fs::remove_all(plan.fetch_dir);
            return failed(LocalPluginStage::Fetch, r);
        }
        // rename() refuses a non-empty directory in repo_dir, which is the user's.
        if (!fs::rename(plan.fetch_dir, plan.repo_dir)) {
            const int err = errno;
            fs::remove_all(plan.fetch_dir);
            CommandResult in_the_way = r;
            in_the_way.exit_code = 1;
            in_the_way.output = fmt::format("{} is in the way ({}); move it aside and retry",
                                            plan.repo_dir, std::strerror(err));
            spdlog::warn("[PluginInstaller] {}", in_the_way.output);
            return failed(LocalPluginStage::Fetch, in_the_way);
        }
    } else if (!recipe.repo_url.empty()) {
        spdlog::info("[PluginInstaller] Using the existing {} checkout at {}", recipe.name,
                     plan.repo_dir);
        attach_checkout_to_branch(recipe, plan.repo_dir, run);
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
        spdlog::info("[PluginInstaller] {} is missing; the user installs it", dep.name);
        result.outcome = LocalPluginOutcome::InstalledMissingDependency;
        result.missing_dependency = dep.name;
        return result;
    }
    return result;
}

} // namespace helix
