// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>

// Forward declarations
namespace helix {
class IMoonrakerClient;
class TemperatureController;
} // namespace helix
class IMoonrakerAPI;
class MoonrakerManager;
#ifdef HELIX_ENABLE_MOCKS
class MoonrakerClientMock;
#endif
namespace helix {
class PrinterState;
}
class JobQueueState;
class PrintHistoryManager;
class TemperatureHistoryManager;

/**
 * @brief Get global MoonrakerClient instance
 * @return Pointer to global MoonrakerClient (may be nullptr if not initialized)
 */
helix::IMoonrakerClient* get_moonraker_client();

/**
 * @brief Set global MoonrakerClient instance (called by main.cpp during init)
 * @param client Pointer to MoonrakerClient instance
 */
void set_moonraker_client(helix::IMoonrakerClient* client);

#ifdef HELIX_ENABLE_MOCKS
/**
 * @brief Get the global client under its concrete mock type
 *
 * Non-null only while the registered client is a MoonrakerClientMock. Consumers
 * that need the mock-only API (AmsBackend's simulated-gcode-tool subscription)
 * read this instead of downcasting get_moonraker_client() — the firmware builds
 * -fno-rtti and the desktop build must not diverge.
 *
 * @return Pointer to the mock client, or nullptr on a real-client run
 */
MoonrakerClientMock* get_moonraker_client_mock();

/**
 * @brief Publish the global client under its concrete mock type
 *
 * Called by MoonrakerManager::create_client(), which is the one place that
 * knows what it built. Pass nullptr when the client is real or being torn down.
 */
void set_moonraker_client_mock(MoonrakerClientMock* client);
#endif

/**
 * @brief Get global IMoonrakerAPI instance
 * @return Pointer to global IMoonrakerAPI (may be nullptr if not initialized)
 */
IMoonrakerAPI* get_moonraker_api();

/**
 * @brief Set global IMoonrakerAPI instance (called by main.cpp during init)
 * @param api Pointer to IMoonrakerAPI instance
 */
void set_moonraker_api(IMoonrakerAPI* api);

/**
 * @brief Get the global TemperatureController (shared resource registered by SubjectInitializer)
 * @return Pointer to the controller, or nullptr if not yet initialized
 */
helix::TemperatureController* get_temperature_controller();

/**
 * @brief Get global MoonrakerManager instance
 * @return Pointer to global MoonrakerManager (may be nullptr if not initialized)
 */
MoonrakerManager* get_moonraker_manager();

/**
 * @brief Set global MoonrakerManager instance (called by Application during init)
 * @param manager Pointer to MoonrakerManager instance
 */
void set_moonraker_manager(MoonrakerManager* manager);

/**
 * @brief Get global JobQueueState instance
 *
 * Provides centralized job queue state for queue panels and status indicators.
 *
 * @return Pointer to global JobQueueState (may be nullptr if not initialized)
 */
JobQueueState* get_job_queue_state();

/**
 * @brief Set global JobQueueState instance (called by Application during init)
 * @param state Pointer to JobQueueState instance
 */
void set_job_queue_state(JobQueueState* state);

/**
 * @brief Get global PrintHistoryManager instance
 *
 * Provides centralized print history cache for status indicators.
 * Used by PrintSelectPanel for file status and History panels for job lists.
 *
 * @return Pointer to global PrintHistoryManager (may be nullptr if not initialized)
 */
PrintHistoryManager* get_print_history_manager();

/**
 * @brief Set global PrintHistoryManager instance (called by Application during init)
 * @param manager Pointer to PrintHistoryManager instance
 */
void set_print_history_manager(PrintHistoryManager* manager);

/**
 * @brief Get global TemperatureHistoryManager instance
 *
 * Provides centralized temperature history tracking for chart panels.
 * Collects 20 minutes of temperature samples at 1Hz for all heaters.
 *
 * @return Pointer to global TemperatureHistoryManager (may be nullptr if not initialized)
 */
TemperatureHistoryManager* get_temperature_history_manager();

/**
 * @brief Set global TemperatureHistoryManager instance (called by Application during init)
 * @param manager Pointer to TemperatureHistoryManager instance
 */
void set_temperature_history_manager(TemperatureHistoryManager* manager);

/**
 * @brief Get global PrinterState singleton instance
 *
 * Returns a reference to the singleton PrinterState instance.
 * The instance is created on first call and persists for the lifetime of the program.
 * Thread-safe initialization guaranteed by C++11 static local variable semantics.
 *
 * @return Reference to singleton PrinterState (always valid)
 */
helix::PrinterState& get_printer_state();

/**
 * @brief Get the global notification subject
 *
 * Any module can emit notifications by calling:
 * ```cpp
 * NotificationData notif = {severity, title, message, show_modal};
 * lv_subject_set_pointer(&get_notification_subject(), &notif);
 * ```
 *
 * @return Reference to the global notification subject
 */
lv_subject_t& get_notification_subject();

/**
 * @brief Get the global home edit mode subject
 *
 * Controls navbar done button visibility during grid edit mode.
 * Value 0 = not editing, 1 = editing.
 *
 * @return Reference to the home edit mode subject
 */
lv_subject_t& get_home_edit_mode_subject();

/**
 * @brief Get the global wizard-active subject
 *
 * Observable mirror of is_wizard_active(), updated inside set_wizard_active().
 * Value 1 while the setup wizard owns the screen, 0 otherwise. Lets code that
 * must react to the wizard opening/closing (e.g. the PLR offer controller
 * re-evaluating once the wizard exits) observe an edge instead of polling a
 * plain bool. Seeded from the current flag at init so it is correct even if
 * set_wizard_active() ran before subject initialization.
 *
 * @return Reference to the wizard-active subject
 */
lv_subject_t& get_wizard_active_subject();

/**
 * @brief Initialize all global subjects
 *
 * Must be called during app initialization after LVGL is initialized.
 * Initializes reactive subjects used throughout the application.
 */
void app_globals_init_subjects();

/**
 * @brief Deinitialize global subjects
 *
 * Disconnects observers before shutdown. Called by StaticPanelRegistry.
 */
void app_globals_deinit_subjects();

/**
 * @brief Store original command-line arguments for restart capability
 *
 * Must be called early in main() before any argument processing.
 * Required for app_request_restart() to work.
 *
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 */
void app_store_argv(int argc, char** argv);

/**
 * @brief Request clean application shutdown
 *
 * Sets a flag that the main event loop checks. When set, the main loop
 * will exit cleanly, allowing proper cleanup (spdlog shutdown, etc.).
 * Use this instead of exit() or _Exit() for graceful termination.
 */
void app_request_quit();

/**
 * @brief Signal-safe version of app_request_quit()
 *
 * Only sets the quit flag without calling spdlog or any other
 * non-async-signal-safe function. Use this from signal handlers.
 */
void app_request_quit_signal_safe();

/**
 * @brief Request application restart, in place
 *
 * Sets the restart-after-quit flag and requests quit; main() execv()s the stored
 * argv once normal cleanup has run, so the process is replaced rather than
 * duplicated. This does NOT fork: the old fork+exec raced parent cleanup against
 * child startup, and the child exec'd while the parent still held
 * .helix-screen.lock, so it aborted as "Another instance is already running" and
 * lingered as a zombie.
 *
 * Requires app_store_argv() to have been called during startup; without it this
 * degrades to a plain quit.
 */
void app_request_restart();

/**
 * @brief How a restart request should be carried out for the current environment
 */
enum class AppRestartStrategy {
    Systemd,       ///< INVOCATION_ID set: quit and let systemd Restart= bring us back
    Watchdog,      ///< HELIX_SUPERVISED set: quit and let the watchdog bring us back
    ReExecInPlace, ///< nothing supervises us: re-exec the stored argv after cleanup
};

/**
 * @brief Decide the restart strategy from the environment
 *
 * Pure, and takes the values rather than reading them, so the decision can be
 * tested without mutating the process environment. Under any supervisor we must
 * only exit: re-exec'ing ourselves as well would leave two instances running.
 *
 * Systemd wins when both are set — a unit file is the more specific statement,
 * and HELIX_SUPERVISED may be inherited from an outer wrapper. Note a variable
 * set to the empty string still counts as set, matching getenv() semantics.
 *
 * @param invocation_id    getenv("INVOCATION_ID"), or nullptr
 * @param helix_supervised getenv("HELIX_SUPERVISED"), or nullptr
 */
inline AppRestartStrategy app_restart_strategy_for_env(const char* invocation_id,
                                                       const char* helix_supervised) {
    if (invocation_id != nullptr) {
        return AppRestartStrategy::Systemd;
    }
    if (helix_supervised != nullptr) {
        return AppRestartStrategy::Watchdog;
    }
    return AppRestartStrategy::ReExecInPlace;
}

/**
 * @brief Request application restart, deferring to a supervisor when there is one
 *
 * Routes on app_restart_strategy_for_env(): under systemd or the watchdog it
 * quits and lets the supervisor restart us; standalone it re-execs in place via
 * app_request_restart().
 *
 * Use this instead of app_request_restart() for all user-facing restart actions.
 */
void app_request_restart_service();

/**
 * @brief Get stored argv for use with execv()
 *
 * Returns the NULL-terminated argv array stored by app_store_argv().
 * Returns nullptr if app_store_argv() was not called.
 */
char** app_get_stored_argv();

/**
 * @brief Get stored absolute executable path for execv()
 *
 * Returns the resolved absolute path captured by app_store_argv().
 * Returns nullptr if app_store_argv() was not called or the path was empty.
 */
const char* app_get_executable_path();

/**
 * @brief Check whether app_request_restart() was called
 *
 * When true, main() should execv(app_get_executable_path(), app_get_stored_argv())
 * after normal cleanup returns, instead of exiting to the shell.  This replaces
 * the old fork+exec pattern (which raced the cleanup and lock file).
 */
bool app_restart_after_quit_requested();

/**
 * @brief Check if quit has been requested
 * @return true if app_request_quit() or app_request_restart() was called
 */
bool app_quit_requested();

/**
 * @brief Check if setup wizard is currently active
 * @return true if wizard is running, false otherwise
 */
bool is_wizard_active();

/**
 * @brief Set wizard active state
 * @param active true when wizard starts, false when it completes
 */
void set_wizard_active(bool active);

/**
 * @brief Register a callback invoked when wizard completes (transitions false)
 *
 * Used by Application to clear add-printer recovery state on successful wizard completion.
 * Only one callback is supported; setting a new one replaces the previous.
 *
 * @param cb Callback to invoke, or nullptr to clear
 */
void set_wizard_completion_callback(std::function<void()> cb);

/**
 * @brief Register a callback invoked when wizard is cancelled (back from first step)
 *
 * Used by Application to clean up empty printer entries on add-printer cancellation.
 * Only one callback is supported; setting a new one replaces the previous.
 *
 * @param cb Callback to invoke, or nullptr to clear
 */
void set_wizard_cancel_callback(std::function<void()> cb);

/**
 * @brief Get the registered wizard cancel callback (may be nullptr)
 */
std::function<void()> get_wizard_cancel_callback();

/**
 * @brief Get appropriate cache directory for temp files
 *
 * Determines best location for cache/temp files with priority:
 * 1. HELIX_CACHE_DIR env var + /<subdir>
 * 2. Config /cache/base_directory + /<subdir>
 * 3. Platform-specific (compile-time):
 *    - AD5M:  /data/helixscreen/cache/<subdir>
 *    - K1:    /usr/data/helixscreen/cache/<subdir>
 *    - K2:    /mnt/UDISK/helixscreen/cache/<subdir>, then /usr/data
 *             (/usr/data is the small root overlay on the K2, not user storage)
 * 4. XDG_CACHE_HOME/helix/<subdir>
 * 5. $HOME/.cache/helix/<subdir>
 * 6. /var/tmp/helix_<subdir>
 * 7. /tmp/helix_<subdir> (last resort, with warning)
 *
 * Creates the winning directory, and only that one: each candidate is first
 * tested for viability without touching the filesystem (see
 * peek_helix_cache_dir), so a candidate that cannot be used is skipped rather
 * than created-then-discarded. On embedded systems, prefers persistent storage
 * over RAM-backed tmpfs.
 *
 * @param subdir Subdirectory name (e.g., "gcode_temp", "thumbs")
 * @return Full path to cache directory, or empty string on failure
 */
std::string get_helix_cache_dir(const std::string& subdir);

/**
 * @brief Where get_helix_cache_dir() WOULD put @p subdir, creating nothing.
 *
 * Same cascade, same order, same result — but it only asks whether each
 * candidate could be used, never makes one usable. Use this for any query that
 * is not about to write to the cache: deciding whether a directory found on
 * disk is the live cache or a stale leftover, reporting the resolved path in a
 * diagnostic, or comparing against a path from a previous release.
 *
 * Calling get_helix_cache_dir() for those answers materializes the directory as
 * a side effect of the question, which on a device whose real cache sits at a
 * lower tier silently splits the cache across two locations.
 *
 * @param subdir Subdirectory name (e.g., "gcode_temp", "thumbs")
 * @return Full path the cascade resolves to, or empty string if no candidate
 *         is usable. A non-empty result may not exist yet.
 */
std::string peek_helix_cache_dir(const std::string& subdir);

/**
 * @brief Remove cache directories an older layout left on the wrong filesystem.
 *
 * Only runs on a build with a compile-time platform rung — the embedded
 * devices, where the fall-through rungs are dead. That rung need not win: every
 * platform hook exports HELIX_CACHE_DIR, so rung 1 wins on all of them. On the
 * K1 the cache lives under /usr/data, so a leftover /root/.cache/helix sits on
 * the ~97MB root overlay, competing with the firmware for the smallest
 * partition on the box.
 *
 * Reclaims only the fall-through rungs (XDG, HOME, /var/tmp, /tmp) below the
 * winner; an explicitly chosen path is never deleted.
 *
 * A no-op on desktop, where the XDG/HOME rung IS the live cache. Call once at
 * startup, and before anything reaches get_thumbnail_cache(): that singleton
 * latches its directory on first use.
 *
 * @return Number of stale directories reclaimed.
 */
int sweep_stale_helix_cache_dirs();

/**
 * @brief Returns the installation root directory (containing bin/, ui_xml/, assets/).
 *
 * Derived from the cached executable path populated by app_store_argv().
 * Returns empty string if the path cannot be resolved (e.g., executable is
 * not under a recognized /bin or /build/bin directory).
 * Result is cached after first call.
 */
std::string app_get_install_root();

// Returns the cache directory (thumbnails, update tarballs, transient
// assets). Resolved via the same platform-specific hierarchy as
// get_helix_cache_dir(""), with any trailing slash stripped for display.
// Never returns empty — falls back to /tmp/helix_ or similar.
// Result is cached after first call.
std::string app_get_cache_dir();

// Returns a writable base dir for SHORT-LIVED runtime files (nmcli stderr
// capture, screenshots, timelapse temp downloads). Prefers a fast tmpfs
// (HELIX_TMP_DIR, XDG_RUNTIME_DIR, /tmp, /var/tmp) so normal devices keep
// working with the usual /tmp paths; falls back to the persistent cache dir
// (~/.cache/helix/runtime) when the /tmp family is read-only, which happens
// under ProtectSystem=strict (Sonic Pad / OrangePi Zero3). Returns a base dir
// with NO trailing slash; callers append "/<name>". Never returns empty.
std::string app_get_runtime_dir();

// Returns the config directory (settings.json, printer_database.json,
// helixscreen.env). If HELIX_CONFIG_DIR is set, returns it verbatim.
// Otherwise joins the install root with "config" to form an absolute path.
// Falls back to the relative "config" when install root is unknown.
// Result is cached after first call.
std::string app_get_config_dir();

// Parses an environment-variable value as a boolean. Truthy values are
// "1", "true", "yes", "on" (case-insensitive); everything else (including
// nullptr, "", "0", "false") is false. Pure/side-effect-free so it can be
// unit-tested without depending on the process environment.
bool helix_parse_truthy_env(const char* value);

// Pure predicate behind updates_externally_managed(), split out for testing so
// both inputs can be exercised without mutating the process env or the platform.
//
// HELIX_DISABLE_AUTO_UPDATES wins in EITHER direction when set to a value that
// parses: truthy suppresses, falsy force-enables. Unset falls back to
// `platform_default`, which is helix::platform_defaults_to_external_updates().
//
// The falsy arm is not symmetry for its own sake — it is how a dev box on a
// platform that defaults to managed turns self-update back on, from the CLI or a
// deploy script, without a rebuild.
//
// This used to be the flag alone, defaulting to self-managed everywhere. On the
// Snapmaker U1 that meant firmware-managed installs self-updated over a package
// the firmware owns, because the firmware hook never set the flag (it exports
// HELIX_DATA_DIR/HELIX_SUPERVISED and nothing else).
bool compute_updates_externally_managed(const char* disable_auto_updates, bool platform_default);

// Returns true when software updates are owned by the device firmware, in which
// case HelixScreen must NOT run its in-app self-update: the periodic auto-check
// is suppressed and manual check/download entry points short-circuit.
//
// Explicit HELIX_DISABLE_AUTO_UPDATES wins either way; otherwise the platform
// default applies (see helix::platform_defaults_to_external_updates(), which owns
// the list). Read once and cached (like app_get_install_root()).
bool updates_externally_managed();

// Can this process obtain root for the install swap? True when already euid 0,
// or when `sudo -n true` succeeds (passwordless sudo). This mirrors what
// install.sh actually does: check_permissions() sets SUDO="sudo" on Pi-class
// platforms and runs the privileged steps through it, and the app forks
// install.sh with no tty, so non-interactive sudo is the exact capability.
// Probed once and cached; the probe is bounded so a sudoers lookup that hangs
// (network-backed sudoers) cannot stall the caller.
bool root_escalation_available();

// Pure predicate: can an in-app self-update PHYSICALLY be applied to this install
// tree? It must recognise BOTH of the routes install.sh implements, since hiding
// the updater is permanent — the fix for a false negative can only ship inside an
// update the user is being prevented from installing:
//
//   parent writable  → atomic swap ("mv <root> <root>.old; mv <new> <root>").
//                      rename mutates the PARENT's entries, so that is what it
//                      needs write permission on.
//   root writable    → in-place replacement: delete the root's contents (bar
//                      config/) and move the new ones in, entirely inside the
//                      root. install.sh selects this by itself whenever the
//                      parent is not writable.
//   can_escalate     → neither is open, but the steps can run as root anyway.
//
// The root-writable term is what covers the standalone-display Pi: no local
// Klipper, so the installer falls through to /opt/helixscreen, whose parent is
// root-owned while the root itself is chowned to the service user. Escalation is
// NOT a substitute there — the shipped unit sets NoNewPrivileges=true, so sudo
// cannot succeed from the app or from the install.sh it forks.
//
// An empty install_root (unresolvable/bind-mounted layout) or an empty parent
// returns TRUE conservatively, deferring to the installer fallbacks and the
// explicit flag rather than a false negative. Split out for testing so both the
// path and the escalation input can be exercised without the cached process
// install root or a real sudo probe.
bool compute_self_update_supported(const std::string& install_root, bool can_escalate);

// Cached wrapper over compute_self_update_supported(app_get_install_root(), ...).
// False only when NEITHER the install root nor its parent is writable AND root
// can't be obtained — a genuinely read-only rootfs — so the in-app updater won't
// offer an update it physically cannot apply. root_escalation_available() is
// consulted lazily: a writable install tree answers the question with no sudo
// probe at all. Read once and cached (like app_get_install_root()).
bool self_update_supported();

// Pure predicate behind update_install_suppressed(), split out for testing so
// both branches can be exercised without mutating the process-wide caches that
// updates_externally_managed() and self_update_supported() sit behind.
bool compute_update_install_suppressed(bool externally_managed, bool self_update_ok);

// Gate on APPLYING an update: true when a download/install must NOT run, for
// EITHER reason — updates are firmware-managed via the explicit
// HELIX_DISABLE_AUTO_UPDATES flag (updates_externally_managed()), OR a self-update
// is physically impossible because the install tree isn't writable
// (!self_update_supported()). start_download() gates on THIS, and the About screen
// hides the "Install Update" row on it.
bool update_install_suppressed();

// Gate on LOOKING for an update: true only when updates are firmware-managed.
//
// Deliberately a different, weaker predicate than update_install_suppressed().
// Checking is a manifest fetch over the network and needs nothing from the
// filesystem, so an install tree we cannot write is no reason to refuse to look:
// knowing a newer version exists is useful even when the button to apply it is
// not available, and it is the only thing that makes a suppressed install
// recoverable — the user can still be told to re-run the installer.
//
// The two questions shared one predicate until now, and that is what made a false
// negative in self_update_supported() a PERMANENT lockout: the rows vanished
// wholesale, so nothing could tell the user an update existed, and the fix could
// only ship inside the update they were being kept from (v0.99.96 through
// v0.99.113 on /opt installs). check_for_updates() and start_auto_check() gate on
// THIS; the "Managed by your firmware" notice keeps keying off
// updates_externally_managed() alone.
bool update_checks_suppressed();