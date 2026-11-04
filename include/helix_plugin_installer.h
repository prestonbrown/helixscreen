// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file helix_plugin_installer.h
 * @brief Auto-installer for helix_print Moonraker plugin
 *
 * Handles detection and installation of the helix_print plugin:
 * - For local Moonraker (localhost): Auto-install via bundled install.sh
 * - For remote Moonraker: Show curl command for manual installation
 *
 * The plugin enables server-side G-code modification, which is faster and
 * safer than client-side modification on memory-constrained devices.
 */

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration

namespace helix {
class IMoonrakerAPI;
struct HelixPluginInstallerTestAccess; // test-only friend (tests/test_helpers/)

// ============================================================================
// Plugin Install State
// ============================================================================

/**
 * @brief State of the plugin installation process
 */
enum class PluginInstallState {
    IDLE,            ///< No installation in progress
    INSTALLING,      ///< Installation is running
    SUCCESS,         ///< Installation completed successfully
    NEEDS_ATTENTION, ///< Uninstall removed the plugin, but left a config file that needs a look
    FAILED           ///< Installation failed
};

/**
 * @brief Outcome of a local uninstall attempt
 *
 * Mirrors install.sh's own --uninstall / --uninstall-auto exit codes: 0
 * maps to SUCCESS, 2 to NEEDS_ATTENTION, anything else to FAILED.
 */
enum class UninstallOutcome {
    SUCCESS,         ///< Plugin removed; nothing left in the config to clean up
    NEEDS_ATTENTION, ///< Plugin removed, but a config file was skipped or failed
                     ///< during the phase-tracking strip and needs a look
    FAILED           ///< Uninstall failed; the plugin may still be present
};

// ============================================================================
// HelixPluginInstaller Class
// ============================================================================

/**
 * @brief Manages helix_print plugin detection and installation
 *
 * @note Thread Safety:
 *       - uninstall_local(): MUST be called from main thread only. It blocks
 *         during script execution and invokes its callback inline. For
 *         non-blocking UI, wrap in std::thread or std::async.
 *       - install_local_sync(): Blocks during script execution; invokes no
 *         callback, so it is safe to call from a worker thread.
 *       - get_state() / is_installing(): Thread-safe (atomic read).
 *       - All other methods: Main thread only.
 *
 * Usage:
 *   1. Create installer and set API
 *   2. Check if plugin is missing: !PrinterState::service_has_helix_plugin()
 *   3. Check if should prompt: installer.should_prompt_install()
 *   4. For local: installer.install_local_sync()
 *   5. For remote: show dialog with installer.get_remote_install_command()
 */
class HelixPluginInstaller {
  public:
    using UninstallCallback =
        std::function<void(UninstallOutcome outcome, const std::string& message)>;

    HelixPluginInstaller() = default;
    ~HelixPluginInstaller() = default;

    // Non-copyable, non-movable (atomic members are not movable)
    HelixPluginInstaller(const HelixPluginInstaller&) = delete;
    HelixPluginInstaller& operator=(const HelixPluginInstaller&) = delete;
    HelixPluginInstaller(HelixPluginInstaller&&) = delete;
    HelixPluginInstaller& operator=(HelixPluginInstaller&&) = delete;

    // === Configuration ===

    /**
     * @brief Set the IMoonrakerAPI instance for plugin status checks
     */
    void set_api(IMoonrakerAPI* api);

    /**
     * @brief Set the WebSocket URL (for localhost detection)
     *
     * Normally derived from IMoonrakerAPI, but can be set directly for testing.
     */
    void set_websocket_url(const std::string& url);

    // === Detection ===

    /**
     * @brief Check if Moonraker is running on localhost
     *
     * Uses the WebSocket URL to determine if we're connected locally.
     * Local connections can use auto-install via bundled install.sh.
     *
     * @return true if connected to localhost/127.0.0.1/::1
     */
    [[nodiscard]] bool is_local_moonraker() const;

    // === Installation ===

    /**
     * @brief Result of synchronous installation
     */
    struct SyncInstallResult {
        bool success = false;
        std::string message;
    };

    /**
     * @brief Attempt local auto-installation (synchronous, no callback)
     *
     * This method blocks during script execution (up to 60s timeout).
     * Returns a result struct instead of using callbacks, which avoids
     * std::function-related crashes on ARM/glibc static builds.
     *
     * @return Result with success flag and message
     */
    [[nodiscard]] SyncInstallResult install_local_sync();

    /**
     * @brief Attempt local auto-uninstallation
     *
     * Runs the bundled install.sh script with --uninstall-auto. Only works
     * when connected to local Moonraker.
     *
     * @param callback Called with the outcome once the script exits
     */
    void uninstall_local(UninstallCallback callback);

    /**
     * @brief The command a user runs on a remote Moonraker host to install the plugin
     *
     * plugin_remote_install_command() for this app's own release, so the
     * plugin installed matches the app talking to it.
     */
    [[nodiscard]] std::string get_remote_install_command() const;

    /**
     * @brief The PLUGIN_VERSION of the helix_print.py beside install.sh
     *
     * The plugin this app ships; empty when it cannot be found or read.
     */
    [[nodiscard]] std::string bundled_plugin_version() const;

    /**
     * @brief The helix_print.py beside install.sh: the plugin this app ships
     *
     * Empty when no install.sh is found.
     */
    [[nodiscard]] std::string bundled_plugin_file() const;

    /**
     * @brief Get path to bundled install.sh script
     *
     * Searches for install.sh relative to the executable.
     *
     * @return Path to script, or empty if not found
     */
    [[nodiscard]] std::string get_install_script_path() const;

    // === Preference Management ===

    /**
     * @brief Check if we should prompt for plugin installation
     *
     * Returns false before the first-run wizard is done, once the user checked
     * "don't ask again", and once the prompt is settled for this session.
     *
     * @return true if we should show the install prompt
     */
    [[nodiscard]] bool should_prompt_install() const;

    /**
     * @brief Ask no more for the rest of this session
     *
     * Called when the prompt is shown, whatever the user answers, and when the
     * plugin is uninstalled: the plugin then reads as absent, which is no
     * reason to offer it back. Process-wide, because each panel owns its own
     * installer. Main thread only.
     */
    static void settle_prompt_for_session();

    /**
     * @brief Save preference to not prompt again
     *
     * Called when user dismisses the install dialog with "don't ask again".
     */
    void set_install_declined();

    // === State ===

    /**
     * @brief Get current installation state
     */
    [[nodiscard]] PluginInstallState get_state() const;

    /**
     * @brief Check if installation is currently in progress
     */
    [[nodiscard]] bool is_installing() const;

  private:
    friend struct HelixPluginInstallerTestAccess;

    IMoonrakerAPI* api_ = nullptr;
    std::string websocket_url_;
    std::atomic<PluginInstallState> state_{PluginInstallState::IDLE};

    // Where install.sh is looked for. Empty means beside the executable, plus the
    // system share dir; a set root is searched alone, so nothing outside it runs.
    std::string script_search_root_;
    // How long install.sh may run before it is killed.
    int script_timeout_seconds_ = 60;

    // Set by settle_prompt_for_session(); cleared only by a new process.
    static bool prompt_settled_for_session_;

    // Config key for "don't ask again" preference
    static constexpr const char* kPrefInstallDeclined = "/plugin_install_declined";
};

/**
 * @brief The git ref holding the plugin of app version @p version
 *
 * A release version names its tag (1.2.0-beta.3 is v1.2.0-beta.3; build
 * metadata after '+' is dropped). Anything else, such as a "dev" build, is main.
 */
std::string plugin_release_ref(std::string_view version);

/**
 * @brief The shell one-liner that installs the plugin of git ref @p ref
 *
 * A release fetches remote-install.sh from the release CDN, which installs
 * that release's plugin tarball (or, for a release the CDN has pruned, runs
 * the release's own copy from GitHub). main fetches main's remote-install.sh
 * from GitHub. Either way the wrapper is pinned to @p ref.
 */
std::string plugin_remote_install_command(std::string_view ref);

/**
 * @brief When a plugin action that must not compete with a print may go
 *
 * The boot-time install prompt and a Moonraker restart for the plugin both
 * follow this step.
 */
enum class PluginTurnStep {
    Stop,          ///< nothing is wanted
    WaitForJob,    ///< a job holds the machine; ask again when it lets go
    WaitForScreen, ///< a modal or the print status overlay is up; ask again shortly
    Go,            ///< act now
};

/// What is on screen that a plugin action waits behind.
struct PluginScreenState {
    bool modal_up = false;          ///< a modal is on screen
    bool print_status_open = false; ///< the print status overlay is in the navigation stack
};

/**
 * @brief Decide a plugin action's next step
 *
 * The action never competes with a print: not while a job holds the machine,
 * and not over what follows one. A job that has just let go hands the screen to
 * its completion flow, which may not have put its modal up yet, so the first
 * check after a wait for the job always waits for the screen once more.
 *
 * @param wanted there is something to do (for the prompt: the plugin is absent)
 * @param job_holds job_holds_machine() of the current print lifecycle
 * @param waited_for_job the previous step was WaitForJob
 * @param screen what is on screen now
 */
constexpr PluginTurnStep decide_plugin_turn(bool wanted, bool job_holds, bool waited_for_job,
                                            PluginScreenState screen) {
    if (!wanted) {
        return PluginTurnStep::Stop;
    }
    if (job_holds) {
        return PluginTurnStep::WaitForJob;
    }
    if (waited_for_job || screen.modal_up || screen.print_status_open) {
        return PluginTurnStep::WaitForScreen;
    }
    return PluginTurnStep::Go;
}

/**
 * @brief What `install.sh --relink` found and did
 */
enum class PluginLinkCheck {
    Current,  ///< Moonraker's link already names the plugin beside this app
    Relinked, ///< the link was re-pointed at the plugin beside this app
    NotOurs,  ///< no installed plugin to repair: no link, a regular file, no [helix_print]
    Failed,   ///< the script failed, timed out or could not run
};

/**
 * @brief Run `install.sh --relink` at @p script_path and read its outcome
 *
 * Blocks while the script runs, up to @p timeout_seconds. Touches no
 * installer state, so any thread may call it. An empty path is Failed.
 */
PluginLinkCheck relink_plugin(const std::string& script_path, int timeout_seconds);

/**
 * @brief What `install.sh --restart-moonraker` did
 */
enum class PluginRestartOutcome {
    Restarted,    ///< Moonraker restarted and runs the plugin this app ships
    NeedsManual,  ///< Moonraker refused the restart, or still runs the old plugin
    HeldForPrint, ///< a print is printing or paused
    JobUnknown,   ///< the print state could not be read, so nothing restarted
    NotAnswering, ///< Moonraker does not answer, before the restart or after it
    TimedOut,     ///< the script outlived its timeout and was stopped
    Failed,       ///< the script failed or could not run
};

/**
 * @brief Whether a restart with @p outcome counts as done for the plugin it was for
 *
 * A restart that finished counts, and so does one Moonraker refused or that
 * left the old plugin running when @p plugin_was_loaded: asking again for the
 * same versions would only repeat the warning. With no plugin loaded, one
 * that did not load it is tried again, as the next start may find what kept
 * it out repaired. The next app start repeats any other (a print hold, an
 * unreadable print state, a Moonraker that did not answer), which can clear
 * by itself.
 */
bool plugin_restart_finished(PluginRestartOutcome outcome, bool plugin_was_loaded);

/**
 * @brief Run `install.sh --restart-moonraker` at @p script_path and read its outcome
 *
 * The script asks Moonraker over HTTP to restart its own service, so a
 * helix_print.py it already loaded is read again, and checks the version the
 * plugin then reports. It asks Moonraker for the print state right before
 * the restart and holds it off for a print (HeldForPrint). Blocks while
 * Moonraker restarts, up to @p timeout_seconds. Touches no installer state,
 * so any thread may call it. An empty path is Failed.
 */
PluginRestartOutcome restart_moonraker_for_plugin(const std::string& script_path,
                                                  int timeout_seconds);

/**
 * @brief The version in a helix_print.py's `PLUGIN_VERSION = "x"` line, or empty
 */
std::string parse_plugin_version(std::string_view helix_print_py);

/**
 * @brief What the startup check of the installed plugin does next
 */
enum class PluginSelfHealStep {
    Nothing,    ///< nothing to repair, or nothing this app may repair
    ReportOnly, ///< another plugin version runs where this app cannot repair it: log it, change
                ///< nothing
    RestartNow, ///< Moonraker must restart to load the plugin this app ships
    WaitForJob, ///< as RestartNow, once no job holds the machine
    GiveUp,     ///< a restart for this same version pair or link already failed: warn only
};

/**
 * @brief What the startup check keeps in the config from one app start to the next
 */
struct PluginSelfHealMemory {
    /// The (bundled, reported) version pair a restart was requested for; empty when none.
    std::string restarted_for_bundled;
    std::string restarted_for_reported;
    /// The plugin file a re-pointed link was restarted for; empty when none.
    std::string relinked_for;

    bool operator==(const PluginSelfHealMemory& o) const {
        return restarted_for_bundled == o.restarted_for_bundled &&
               restarted_for_reported == o.restarted_for_reported && relinked_for == o.relinked_for;
    }
};

/**
 * @brief The facts the startup check of the installed plugin decides on
 */
struct PluginSelfHealFacts {
    bool local = false;                              ///< Moonraker runs on this host
    PluginLinkCheck link = PluginLinkCheck::NotOurs; ///< install.sh --relink's outcome (local only)
    std::string reported_version; ///< what the running plugin reports; empty when it did not answer
    std::string bundled_version;  ///< what this app ships; empty when unknown
    std::string plugin_file;      ///< the helix_print.py this app ships, which a relink names
    bool job_holds = false;       ///< job_holds_machine() of the current print lifecycle
    PluginSelfHealMemory memory;  ///< what earlier starts did
};

/**
 * @brief Whether the running plugin reports another version than this app
 *        ships; false when either is unknown
 */
bool plugin_versions_differ(const PluginSelfHealFacts& facts);

/**
 * @brief Decide what the startup check does about the installed plugin
 *
 * Only a plugin the user installed is repaired, never one installed here, and
 * only on this host: a remote Moonraker's plugin, or a local one with no link
 * of ours, is the user's to update with the command the install modal shows,
 * so a version mismatch there is only reported. A re-pointed link, or a running plugin
 * whose version is not the one this app ships, takes a Moonraker restart to
 * load the right code, which never interrupts a job. A version mismatch an
 * earlier start already restarted for is not restarted for again: the
 * restart did not load it, and another would not either. A new pair, after
 * either side changes, gets one attempt. So does a re-pointed link: a link
 * re-pointed at the same file again, with no start finding it current in
 * between, is one install.sh cannot see as current, and restarting for it
 * would restart Moonraker at every app start.
 */
PluginSelfHealStep decide_plugin_self_heal(const PluginSelfHealFacts& facts);

/**
 * @brief What the startup check keeps for the next start
 *
 * A restart that finished (@p restart_finished) records the version pair it
 * was for when the versions differ, and the plugin file when it was for a
 * re-pointed link. One held off, failed or cut short by the app stopping
 * records nothing, so the next start gets its attempt. Versions that agree
 * clear the pair, and a link found current clears the file: a later mismatch
 * or relink is a new problem.
 */
PluginSelfHealMemory remember_plugin_self_heal(const PluginSelfHealFacts& facts,
                                               bool restart_finished);

/**
 * @brief Split install.sh output into log lines: color escapes removed,
 *        blank lines dropped.
 */
std::vector<std::string> script_output_lines(const std::string& output);

} // namespace helix
