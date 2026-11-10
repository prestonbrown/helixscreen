// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file local_plugin_installer.h
 * @brief Installs a Moonraker plugin on the host HelixScreen shares with Moonraker
 *
 * One engine for every plugin HelixScreen installs locally (HelixPrint,
 * moonraker-timelapse). A plugin contributes only data, a LocalPluginRecipe:
 * where its files come from, the install.sh mode that links them into
 * Moonraker, and the programs it needs. The engine runs the steps in order
 * through a ProcessRunner, so tests drive it with a fake:
 *
 *   1. Fetch: git clone the recipe's repository, unless the checkout is there
 *      or the plugin ships with this app. The clone lands in repo_dir only once
 *      it is whole; a failed one leaves nothing behind, and a directory already
 *      in repo_dir is never replaced or removed. A checkout already there that
 *      sits on a detached HEAD is switched to the recipe's repo_branch when its
 *      tree is clean and that branch exists locally, since Moonraker's update
 *      manager refuses to track a detached checkout. Nothing is fetched for it.
 *   2. Install: install.sh in the recipe's mode. install.sh owns where Moonraker
 *      lives, the link pre-flights, and any config section it writes.
 *   3. Each dependency: probe for it, and report a missing one to the user.
 *      The engine never installs packages: a package manager run from the UI
 *      can leave the host's package database half-configured if it is stopped.
 *
 * Restarting Moonraker afterwards is the caller's: each plugin's UI already
 * owns when a restart may happen.
 */

#include "system/process_runner.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {

/// A program a plugin needs that is not part of it.
struct LocalPluginDependency {
    std::string name;    ///< what it is called on screen and in the log: "ffmpeg"
    std::string program; ///< what `command -v` looks for
};

/// Install-recipe argument the engine replaces with the plan's repo_dir.
inline constexpr const char* kLocalPluginRepoDirArg = "{repo_dir}";

/// Everything one plugin contributes to a local install.
struct LocalPluginRecipe {
    std::string name;          ///< for the log
    std::string repo_url;      ///< git repository to clone; empty when it ships with the app
    std::string repo_dir_name; ///< the clone's directory under home
    std::string repo_marker;   ///< a file only a checkout of this plugin holds
    /// The branch Moonraker's update manager tracks; a detached checkout is put back on it.
    std::string repo_branch;
    std::vector<std::string> install_args; ///< install.sh's arguments after its path
    int install_timeout_seconds = 60;
    std::vector<LocalPluginDependency> dependencies;
};

/// Where one local install works.
struct LocalPluginPlan {
    std::string install_script; ///< install.sh beside this app
    std::string repo_dir;       ///< the clone; empty when the recipe has no repository
    /// Where the clone runs before it is renamed to repo_dir: a hidden sibling
    /// only this process names, so a clone stopped part way never sits in repo_dir.
    std::string fetch_dir;
    bool repo_present = false; ///< repo_dir already holds the recipe's repo_marker
};

/// The plan for @p recipe under @p home with @p install_script; looks at the filesystem.
LocalPluginPlan make_local_plugin_plan(const LocalPluginRecipe& recipe, const std::string& home,
                                       const std::string& install_script);

/// The step a local install is on, or the one it failed in.
enum class LocalPluginStage {
    Fetch,           ///< git clone of the repository
    Install,         ///< install.sh in the recipe's mode
    CheckDependency, ///< looking for a dependency
};

enum class LocalPluginOutcome {
    Installed,                  ///< plugin installed, every dependency present
    InstalledMissingDependency, ///< plugin installed; missing_dependency is the user's to add
    Failed,                     ///< plugin not installed; failed_stage names the step
};

struct LocalPluginResult {
    LocalPluginOutcome outcome = LocalPluginOutcome::Failed;
    LocalPluginStage failed_stage = LocalPluginStage::Fetch;
    /// The failing step's command result.
    CommandResult failure;
    /// The failing command's last output line, or "timed out"; empty on success.
    std::string detail;
    /// The dependency left to the user, when outcome is InstalledMissingDependency.
    std::string missing_dependency;
};

/**
 * @brief Run a local install of @p recipe through @p run
 *
 * Blocks for as long as the commands take (a clone on a slow link can take
 * minutes), so it belongs on a worker thread. @p on_stage, when set, is called on that
 * same thread as each step starts.
 */
LocalPluginResult
install_plugin_locally(const LocalPluginRecipe& recipe, const LocalPluginPlan& plan,
                       const ProcessRunner& run,
                       const std::function<void(LocalPluginStage)>& on_stage = {});

} // namespace helix
