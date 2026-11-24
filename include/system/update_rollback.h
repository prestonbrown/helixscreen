// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

/// Automatic-update rollback (#1304). An automatic install keeps the previous
/// install beside the new one and arms helix-launcher.sh's guard by writing
/// kRollbackArmedFile into the directory the launcher exports as
/// HELIX_ROLLBACK_DIR. The new version reports itself healthy (or asks for a
/// rollback when the crash-loop detector fired); the launcher restores the
/// previous tree when it does not, and leaves kRollbackResultFile for the
/// restored version to tell the user. The file names are shared with the
/// launcher and the watchdog, so they live here as constants.
namespace helix::update {

inline constexpr const char* kRollbackDirEnv = "HELIX_ROLLBACK_DIR";
inline constexpr const char* kRollbackArmedFile = "update_rollback.armed";
inline constexpr const char* kRollbackHealthyFile = "update_rollback.healthy";
inline constexpr const char* kRollbackRequestFile = "update_rollback.request";
inline constexpr const char* kRollbackResultFile = "update_rollback.result";
/// Written by install.sh when it kept the previous tree: `previous_dir=<path>`.
inline constexpr const char* kRollbackPreviousFile = "update_rollback.previous";
/// Present while the guarded version is running a job, so the launcher holds
/// the restart it would otherwise make after a healthy report.
inline constexpr const char* kRollbackBusyFile = "update_rollback.busy";
/// Written by helix-watchdog when it was told to stop (SIGTERM, SIGINT): the
/// guarded launch ended because of a stop or shutdown, not a failure.
inline constexpr const char* kRollbackStoppingFile = "update_rollback.stopping";

/// How long the new version runs before it calls itself healthy. Longer than
/// the crash-loop detector's 120 s window, so a loop is seen first.
inline constexpr int kRollbackHealthySeconds = 150;

/// How often the guarded version refreshes kRollbackBusyFile.
inline constexpr uint32_t kRollbackBusyPollMs = 30 * 1000;

/// Why this install can or cannot roll an automatic update back.
enum class RollbackSupport {
    Supported,
    NoGuard,           ///< The launcher exported no HELIX_ROLLBACK_DIR (no watchdog)
    NotSupervised,     ///< No watchdog above this process to hand the restart back
    InstallSuppressed, ///< The app cannot install updates here at all
    ModPayload,        ///< A mod owns the install tree and updates it in place
    Platform,          ///< This platform's install layout cannot keep both versions
};

/// What decides rollback support, gathered so the rule itself stays pure.
struct RollbackFacts {
    std::string platform_key; ///< helix::platform::current_key()
    bool guard_dir = false;   ///< HELIX_ROLLBACK_DIR is set
    bool supervised = false;  ///< HELIX_SUPERVISED is set (a watchdog parent)
    bool install_suppressed = false;
    bool mod_payload = false; ///< Forge-X or ZMOD owns the install tree
};

/// The rule, per platform. Every platform difference for rollback lives here.
RollbackSupport rollback_support(const RollbackFacts& facts);
const char* rollback_support_name(RollbackSupport support);
RollbackFacts current_rollback_facts();

/// Can this install put the previous version back on its own when an automatic
/// install does not come up healthy? Install overnight is offered only where it can.
bool auto_rollback_supported();

/// Has the rollback path (install.sh keeping the previous tree, the launcher's
/// guard, the watchdog hand-back) passed the device test plan in
/// docs/devel/plans/2026-11-23-auto-update-rollback-device-tests.md? Until it
/// has, auto_rollback_supported() is false everywhere and Install overnight
/// stays hidden. It becomes true per platform, inside rollback_support()'s
/// platform rule, as each platform class passes the plan.
inline constexpr bool kRollbackDeviceProven = false;

/// Replaces auto_rollback_supported()'s answer for tests; std::nullopt restores it.
void set_auto_rollback_supported_for_test(std::optional<bool> value);

/// Replaces kRollbackDeviceProven for tests, so the rule under it stays tested;
/// std::nullopt restores the constant.
void set_rollback_device_proven_for_test(std::optional<bool> value);

/// The launcher's rollback directory, or empty when no guard is listening.
std::string rollback_dir();

/// Is a rollback armed right now (the previous version installed is still on
/// trial)? The scheduler installs nothing automatically until it is settled.
bool rollback_guard_armed();

/// Drops an armed guard and every file of its trial. A manual install does this
/// first: the trial it would otherwise end in is for the version it replaces,
/// and the guard must not restore that over what the user installed.
void disarm_rollback_guard();

/// Does this install keep the previous version for the guard? Only an
/// automatic install does, and only where rollback is supported and a guard
/// is listening.
bool keep_previous_for_install(bool automatic, bool rollback_supported, bool guard_listening);

/// Is @p previous the tree install.sh keeps beside @p install_root
/// (`<install_root>.old` or `<install_root>.old.<digits>`)? Pure string rule;
/// callers canonicalize both first.
bool is_kept_previous_of(const std::string& install_root, const std::string& previous);

/// Removes a kept previous tree nothing will restore (a marker that could not
/// be armed), after checking it is the sibling install.sh made.
void discard_kept_previous(const std::string& install_root, const std::string& previous);

/// The kept previous install an armed rollback restores.
struct RollbackMarker {
    std::string install_dir;
    std::string previous_dir;
    std::string from_version;
    std::string to_version;
};

/// `key=value` lines, the form the launcher reads with sed.
std::string format_rollback_marker(const RollbackMarker& marker);

/// The value of the first `key=` line in @p text, or empty.
std::string rollback_value(const std::string& text, const std::string& key);

/// Writes @p name in @p dir through a temporary file and a rename.
bool write_rollback_file(const std::string& dir, const char* name, const std::string& content);
bool read_rollback_file(const std::string& dir, const char* name, std::string& out);
void remove_rollback_file(const std::string& dir, const char* name);

/// What the just-started version tells the guard.
enum class StartupHealth {
    NotArmed,          ///< No rollback pending: nothing to report
    RequestRollback,   ///< The crash-loop detector put this run in safe mode
    ReportWhenSettled, ///< Report healthy after kRollbackHealthySeconds
};
StartupHealth decide_startup_health(bool armed, bool safe_mode);

/// What the launcher left after putting the previous version back.
struct RollbackResult {
    std::string from_version; ///< The version running now, restored
    std::string to_version;   ///< The version that did not come up
    std::string reason;       ///< timeout, crash_loop, too_many_starts, exit_<code>
};
std::optional<RollbackResult> parse_rollback_result(const std::string& text);

/// The notice for a rollback, stored in config until it has been on screen a
/// while, so a restart right after the rollback cannot lose it.
std::string format_rollback_notice(const RollbackResult& result);
std::optional<RollbackResult> parse_rollback_notice(const std::string& stored);

/// Writes kRollbackBusyFile while a rollback is armed and a job (or a pending
/// power-loss resume) holds the machine, and removes it otherwise. on_ui_ready()
/// repeats it every kRollbackBusyPollMs. Main thread.
void refresh_rollback_busy_file();

/// Called once the UI is up: tells the user about a rollback the launcher
/// made, and reports this version's health to an armed guard. Needs LVGL.
void on_ui_ready(bool safe_mode);

} // namespace helix::update
