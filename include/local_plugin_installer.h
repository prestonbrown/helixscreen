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
 *      or the plugin ships with this app.
 *   2. Install: install.sh in the recipe's mode. install.sh owns where Moonraker
 *      lives, the link pre-flights, and any config section it writes.
 *   3. Each dependency: probe for it; when missing, install its apt package
 *      only through passwordless sudo (`sudo -n`, so nothing waits on a
 *      password), else report it to the user.
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
    std::string name;        ///< what it is called on screen and in the log: "ffmpeg"
    std::string program;     ///< what `command -v` looks for
    std::string apt_package; ///< what apt-get installs it from
};

/// Install-recipe argument the engine replaces with the plan's repo_dir.
inline constexpr const char* kLocalPluginRepoDirArg = "{repo_dir}";

/// Everything one plugin contributes to a local install.
struct LocalPluginRecipe {
    std::string name;          ///< for the log
    std::string repo_url;      ///< git repository to clone; empty when it ships with the app
    std::string repo_dir_name; ///< the clone's directory under home
    std::string repo_marker;   ///< a file only a checkout of this plugin holds
    std::vector<std::string> install_args; ///< install.sh's arguments after its path
    int install_timeout_seconds = 60;
    std::vector<LocalPluginDependency> dependencies;
};

/// Where one local install works.
struct LocalPluginPlan {
    std::string install_script; ///< install.sh beside this app
    std::string repo_dir;       ///< the clone; empty when the recipe has no repository
    bool repo_present = false;  ///< repo_dir already holds the recipe's repo_marker
};

/// The plan for @p recipe under @p home with @p install_script; looks at the filesystem.
LocalPluginPlan make_local_plugin_plan(const LocalPluginRecipe& recipe, const std::string& home,
                                       const std::string& install_script);

/// The step a local install is on, or the one it failed in.
enum class LocalPluginStage {
    Fetch,             ///< git clone of the repository
    Install,           ///< install.sh in the recipe's mode
    CheckDependency,   ///< looking for a dependency
    InstallDependency, ///< apt-get through passwordless sudo
};

enum class LocalPluginOutcome {
    Installed,                  ///< plugin installed, every dependency present
    InstalledMissingDependency, ///< plugin installed; missing_dependency is the user's to add
    Failed,                     ///< plugin not installed; failed_stage names the step
};

struct LocalPluginResult {
    LocalPluginOutcome outcome = LocalPluginOutcome::Failed;
    LocalPluginStage failed_stage = LocalPluginStage::Fetch;
    /// The failing step's command result (for a missing dependency, apt's when it ran).
    CommandResult failure;
    /// The failing command's last output line, or "timed out"; empty on success.
    std::string detail;
    /// The dependency left to the user, when outcome is InstalledMissingDependency.
    std::string missing_dependency;
};

/**
 * @brief Run a local install of @p recipe through @p run
 *
 * Blocks for as long as the commands take (an apt install can take minutes),
 * so it belongs on a worker thread. @p on_stage, when set, is called on that
 * same thread as each step starts.
 */
LocalPluginResult
install_plugin_locally(const LocalPluginRecipe& recipe, const LocalPluginPlan& plan,
                       const ProcessRunner& run,
                       const std::function<void(LocalPluginStage)>& on_stage = {});

} // namespace helix
