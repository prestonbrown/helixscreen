// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file app_globals.cpp
 * @brief Global application state and accessors
 *
 * Provides centralized access to global singleton instances like MoonrakerClient,
 * PrinterState, and reactive subjects. This module exists to:
 * 1. Keep main.cpp cleaner and more focused
 * 2. Provide a single point of truth for global state
 * 3. Make it easier to add new global subjects/singletons
 */

#include "app_globals.h"

#ifdef __ANDROID__
#include <SDL.h>
#endif

#include "ui_modal.h"

#include "config.h"
#include "data_root_resolver.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#ifdef HELIX_ENABLE_MOCKS
// Complete type needed for the derived->base pointer comparison in
// set_moonraker_client().
#include "moonraker_client_mock.h"
#endif
#include "panel_widget_manager.h"
#include "platform_info.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "system/helix_paths.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Platform-specific includes for process restart and the sudo capability probe
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>    // open (redirect the probe child to /dev/null)
#include <sys/wait.h> // waitpid, WNOHANG
#include <unistd.h>   // fork, execv, usleep
using namespace helix;

#endif

// Global singleton instances (extern declarations in header, definitions here)
// These are set by main.cpp during initialization
static IMoonrakerClient* g_moonraker_client = nullptr;
#ifdef HELIX_ENABLE_MOCKS
// Alias for g_moonraker_client under the concrete mock type; see
// get_moonraker_client_mock(). Kept in lockstep with the pointer above.
static MoonrakerClientMock* g_moonraker_client_mock = nullptr;
#endif
static IMoonrakerAPI* g_moonraker_api = nullptr;
static MoonrakerManager* g_moonraker_manager = nullptr;
static JobQueueState* g_job_queue_state = nullptr;
static PrintHistoryManager* g_print_history_manager = nullptr;
static TemperatureHistoryManager* g_temp_history_manager = nullptr;

// Global reactive subjects with RAII cleanup
static SubjectManager g_subjects;
static lv_subject_t g_notification_subject;
static lv_subject_t g_show_beta_features_subject;
static lv_subject_t g_home_edit_mode_subject;
static lv_subject_t g_platform_extras_subject;
static lv_subject_t g_host_power_supported_subject;
static lv_subject_t g_wizard_active_subject;

// Application quit flag (volatile sig_atomic_t for async-signal-safety)
static volatile sig_atomic_t g_quit_requested = 0;

// When set, main() will execv() the stored argv after normal cleanup runs.
// Used to implement in-place restart without forking a second instance that
// would race the parent's cleanup and collide on the .helix-screen.lock file.
static bool g_restart_after_quit = false;

// Wizard active flag
static bool g_wizard_active = false;
static std::function<void()> g_wizard_completion_cb;
static std::function<void()> g_wizard_cancel_cb;

// Stored command-line arguments for restart capability
static std::vector<char*> g_stored_argv;
static std::string g_executable_path;

IMoonrakerClient* get_moonraker_client() {
    return g_moonraker_client;
}

void set_moonraker_client(IMoonrakerClient* client) {
    g_moonraker_client = client;
#ifdef HELIX_ENABLE_MOCKS
    // The mock alias only stays valid while it names the same object. Anyone
    // replacing or clearing the client invalidates it; the caller that installs
    // a mock re-publishes it right after this call.
    if (static_cast<IMoonrakerClient*>(g_moonraker_client_mock) != client) {
        g_moonraker_client_mock = nullptr;
    }
#endif
}

#ifdef HELIX_ENABLE_MOCKS
MoonrakerClientMock* get_moonraker_client_mock() {
    return g_moonraker_client_mock;
}

void set_moonraker_client_mock(MoonrakerClientMock* client) {
    g_moonraker_client_mock = client;
}
#endif

IMoonrakerAPI* get_moonraker_api() {
    return g_moonraker_api;
}

void set_moonraker_api(IMoonrakerAPI* api) {
    g_moonraker_api = api;
}

TemperatureController* get_temperature_controller() {
    return PanelWidgetManager::instance().shared_resource<TemperatureController>();
}

MoonrakerManager* get_moonraker_manager() {
    return g_moonraker_manager;
}

void set_moonraker_manager(MoonrakerManager* manager) {
    g_moonraker_manager = manager;
}

JobQueueState* get_job_queue_state() {
    return g_job_queue_state;
}

void set_job_queue_state(JobQueueState* state) {
    g_job_queue_state = state;
}

PrintHistoryManager* get_print_history_manager() {
    return g_print_history_manager;
}

void set_print_history_manager(PrintHistoryManager* manager) {
    g_print_history_manager = manager;
}

TemperatureHistoryManager* get_temperature_history_manager() {
    return g_temp_history_manager;
}

void set_temperature_history_manager(TemperatureHistoryManager* manager) {
    g_temp_history_manager = manager;
}

PrinterState& get_printer_state() {
    // Singleton instance - created once, lives for lifetime of program
    static PrinterState instance;
    return instance;
}

lv_subject_t& get_notification_subject() {
    return g_notification_subject;
}

lv_subject_t& get_home_edit_mode_subject() {
    return g_home_edit_mode_subject;
}

lv_subject_t& get_wizard_active_subject() {
    return g_wizard_active_subject;
}

// Track if subjects are initialized
static bool g_subjects_initialized = false;

void app_globals_init_subjects() {
    if (g_subjects_initialized) {
        spdlog::debug("[App Globals] Subjects already initialized, skipping");
        return;
    }

    // Initialize notification subject (stores NotificationData pointer)
    // Note: Not using UI_MANAGED_SUBJECT_POINTER because this subject is accessed
    // programmatically via get_notification_subject(), not through XML bindings
    lv_subject_init_pointer(&g_notification_subject, nullptr);
    g_subjects.register_subject(&g_notification_subject);

    // Initialize beta features visibility subject (config-driven, used by multiple panels)
    Config* config = Config::get_instance();
    bool beta_enabled = config && config->is_beta_features_enabled();
    lv_subject_init_int(&g_show_beta_features_subject, beta_enabled ? 1 : 0);
    g_subjects.register_subject(&g_show_beta_features_subject);
    lv_xml_register_subject(nullptr, "show_beta_features", &g_show_beta_features_subject);

    // Initialize home edit mode subject (controls navbar done button visibility)
    lv_subject_init_int(&g_home_edit_mode_subject, 0);
    g_subjects.register_subject(&g_home_edit_mode_subject);
    lv_xml_register_subject(nullptr, "home_edit_mode", &g_home_edit_mode_subject);

    // Platform-availability gate for excluded v1 (ESP32 / K-Touch) hardware
    // features whose affordances are XML-declarative and cannot be hidden from a
    // compiled TU (their owning overlay is excluded from the app_srcs manifest).
    // 1 = affordances render exactly as today (every non-ESP32 build); 0 = hidden.
    // The default of 1 keeps all desktop/embedded builds behaviorally identical —
    // only the ESP32 v1 cut flips it to 0. XML rows bind via:
    //   <bind_flag_if_eq subject="platform_extras_available" flag="hidden" ref_value="0"/>
    // Cleanup is co-located: register_subject() below hands it to g_subjects, which
    // app_globals_deinit_subjects() (registered with StaticSubjectRegistry above)
    // deinits before lv_deinit().
#if defined(HELIX_PLATFORM_ESP32)
    lv_subject_init_int(&g_platform_extras_subject, 0);
#else
    lv_subject_init_int(&g_platform_extras_subject, 1);
#endif
    g_subjects.register_subject(&g_platform_extras_subject);
    lv_xml_register_subject(nullptr, "platform_extras_available", &g_platform_extras_subject);

    // Host power availability. Screen reboot/shutdown has no meaning on Android
    // (an app cannot call logind/systemctl/busybox), and the host-power RPCs
    // are gated off there too — see helix::platform_host_power_supported(),
    // the single home of the rule. Seeded at init (platform identity does not
    // change mid-run). XML gates via:
    //   <bind_flag_if_eq subject="platform_host_power_supported" flag="hidden" ref_value="0"/>
    // and the shutdown home widget's hardware gate reads it by name.
    lv_subject_init_int(&g_host_power_supported_subject,
                        helix::platform_host_power_supported() ? 1 : 0);
    g_subjects.register_subject(&g_host_power_supported_subject);
    lv_xml_register_subject(nullptr, "platform_host_power_supported",
                            &g_host_power_supported_subject);

    // Initialize wizard-active subject (observable mirror of is_wizard_active()).
    // Seed from the current flag so it is correct even when set_wizard_active()
    // ran before this init. Not XML-bound — observed programmatically only.
    lv_subject_init_int(&g_wizard_active_subject, g_wizard_active ? 1 : 0);
    g_subjects.register_subject(&g_wizard_active_subject);

    // Initialize modal dialog subjects (for modal_dialog.xml binding)
    helix::ui::modal_init_subjects();

    g_subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("AppGlobals", app_globals_deinit_subjects);

    spdlog::trace("[App Globals] Global subjects initialized");
}

void app_globals_deinit_subjects() {
    if (!g_subjects_initialized) {
        return;
    }
    g_subjects.deinit_all();
    helix::ui::modal_deinit_subjects(); // Clean up modal subjects
    g_subjects_initialized = false;
    spdlog::debug("[App Globals] Global subjects deinitialized");
}

void app_store_argv(int argc, char** argv) {
    // Store a copy of argv for restart capability
    g_stored_argv.clear();

    if (argc > 0 && argv && argv[0]) {
        // Store executable path, resolved to absolute for safe restart via execv()
        g_executable_path = argv[0];

        // Resolve to absolute path to prevent symlink/CWD attacks on restart
#ifdef __linux__
        {
            char buf[PATH_MAX];
            ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                g_executable_path = buf;
            }
        }
#else
        {
            char* resolved = realpath(argv[0], nullptr);
            if (resolved) {
                g_executable_path = resolved;
                free(resolved);
            }
        }
#endif

        // Copy all arguments
        for (int i = 0; i < argc; ++i) {
            if (argv[i]) {
                g_stored_argv.push_back(strdup(argv[i]));
            }
        }
        // execv requires NULL-terminated array
        g_stored_argv.push_back(nullptr);

        spdlog::debug("[App Globals] Stored {} command-line arguments for restart capability",
                      argc);
    }
}

void app_request_quit() {
    spdlog::info("[App Globals] Application quit requested");
    g_quit_requested = 1;
}

void app_request_quit_signal_safe() {
    g_quit_requested = 1;
}

void app_request_restart() {
    spdlog::info("[App Globals] Application restart requested");

    if (g_stored_argv.empty() || g_executable_path.empty()) {
        spdlog::error(
            "[App Globals] Cannot restart: argv not stored. Call app_store_argv() at startup.");
        g_quit_requested = 1; // Fall back to quit
        return;
    }

    // In-place restart: set a flag, request quit, and let main() execv() the
    // stored argv after normal cleanup runs.  The old fork+exec pattern raced
    // parent cleanup against child startup — the child execv'd 100 ms later
    // while the parent still held the .helix-screen.lock file, so the child
    // aborted ("Another instance is already running") and lingered as a
    // zombie while the parent's own cleanup sometimes hung.  A single-process
    // restart avoids the lock collision entirely.
    g_restart_after_quit = true;
    g_quit_requested = 1;
}

void app_request_restart_service() {
    // Under any supervisor, just exit cleanly: the supervisor restarts us, and
    // re-exec'ing ourselves as well would leave two instances running. The choice
    // itself lives in app_restart_strategy_for_env() so it can be tested without
    // mutating the environment.
    switch (app_restart_strategy_for_env(getenv("INVOCATION_ID"), getenv("HELIX_SUPERVISED"))) {
    case AppRestartStrategy::Systemd:
        spdlog::info("[App Globals] Running under systemd - quitting for service restart");
        app_request_quit();
        break;
    case AppRestartStrategy::Watchdog:
        spdlog::info("[App Globals] Running under watchdog - quitting for supervised restart");
        app_request_quit();
        break;
    case AppRestartStrategy::ReExecInPlace:
        app_request_restart();
        break;
    }
}

char** app_get_stored_argv() {
    if (g_stored_argv.empty())
        return nullptr;
    return g_stored_argv.data();
}

const char* app_get_executable_path() {
    return g_executable_path.empty() ? nullptr : g_executable_path.c_str();
}

bool app_restart_after_quit_requested() {
    return g_restart_after_quit;
}

bool app_quit_requested() {
    return g_quit_requested != 0;
}

bool is_wizard_active() {
    return g_wizard_active;
}

void set_wizard_active(bool active) {
    bool was_active = g_wizard_active;
    g_wizard_active = active;
    spdlog::debug("[App Globals] Wizard active state set to: {}", active);

    // Mirror into the observable subject so observers (e.g. the PLR offer
    // controller) see the edge. Guard on init: set_wizard_active() can run
    // before app_globals_init_subjects(), and init seeds the subject from the
    // current flag, so skipping here loses nothing. Called on the main thread.
    if (g_subjects_initialized) {
        lv_subject_set_int(&g_wizard_active_subject, active ? 1 : 0);
    }

    // Fire completion callback when wizard transitions from active to inactive
    if (was_active && !active && g_wizard_completion_cb) {
        g_wizard_completion_cb();
    }
}

void set_wizard_completion_callback(std::function<void()> cb) {
    g_wizard_completion_cb = std::move(cb);
}

void set_wizard_cancel_callback(std::function<void()> cb) {
    g_wizard_cancel_cb = std::move(cb);
}

std::function<void()> get_wizard_cancel_callback() {
    return g_wizard_cancel_cb;
}

// ============================================================================
// CACHE DIRECTORY HELPER
// ============================================================================
//
// The resolution cascade lives in src/system/helix_cache_dir.cpp so the test
// binary can link it — app_globals.o is excluded from that link (mk/tests.mk)
// and the cascade was being stubbed out from under the tests. Declarations are
// still in app_globals.h.

std::string app_get_install_root() {
    static const std::string cached = []() {
        return helix::resolve_data_root_from_exe(g_executable_path);
    }();
    return cached;
}

std::string app_get_cache_dir() {
    static const std::string cached = []() {
        std::string p = get_helix_cache_dir(""); // base, no subdir
        while (p.size() > 1 && p.back() == '/')
            p.pop_back();
        return p;
    }();
    return cached;
}

std::string app_get_runtime_dir() {
    // Writable base dir for SHORT-LIVED runtime files (nmcli stderr capture,
    // screenshots, timelapse temp downloads). Prefers fast tmpfs (/tmp) so
    // existing file paths and scripts/screenshot.sh keep working on normal
    // devices; falls back to the persistent cache dir when /tmp is read-only
    // (ProtectSystem=strict sandbox on Sonic Pad / OrangePi Zero3). Returns a
    // base dir with NO trailing slash; callers append "/<name>".
    std::vector<std::string> candidates;
    if (const char* e = std::getenv("HELIX_TMP_DIR"); e && e[0] != '\0')
        candidates.emplace_back(e);
    // /tmp first (after the explicit override) so behavior is unchanged on normal
    // devices — scripts/screenshot.sh and other tooling read the usual /tmp paths,
    // and a desktop's writable XDG_RUNTIME_DIR must NOT pull writes off /tmp.
    candidates.emplace_back("/tmp");
    candidates.emplace_back("/var/tmp");
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && x[0] != '\0')
        candidates.emplace_back(x);
    std::string dir = helix::paths::first_writable_dir(candidates);
    if (!dir.empty())
        return helix::paths::strip_trailing_slash(dir);
    // /tmp-family read-only (sandbox) — use the persistent cache dir, which
    // resolves under ~/.cache / install root (writable under ReadWritePaths).
    const std::string cache = app_get_cache_dir();
    if (!cache.empty()) {
        std::string p = cache + "/runtime";
        // Unlike the cache cascade, this is the terminal rung and creating it
        // IS the intent — but check viability first anyway so a non-writable
        // cache root is rejected without a half-made directory left under it.
        if (helix::paths::can_create_dir(p) && helix::paths::ensure_dir(p) &&
            helix::paths::probe_writable(p))
            return p;
    }
    spdlog::warn("[App Globals] No writable runtime dir found; using /tmp (writes may fail)");
    return "/tmp";
}

std::string app_get_config_dir() {
    static const std::string cached = []() {
        std::string cfg = helix::get_user_config_dir(); // "config" or $HELIX_CONFIG_DIR
        while (cfg.size() > 1 && cfg.back() == '/')
            cfg.pop_back();
        if (!cfg.empty() && cfg.front() == '/') {
            return cfg; // absolute already
        }
        const std::string root = app_get_install_root();
        if (!root.empty()) {
            return root + "/" + cfg;
        }
        return cfg; // best-effort relative fallback
    }();
    return cached;
}

bool helix_parse_truthy_env(const char* value) {
    if (!value || value[0] == '\0') {
        return false;
    }
    std::string v(value);
    // Trim surrounding whitespace (helixscreen.env values can carry a stray space).
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
    v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool compute_updates_externally_managed(const char* disable_auto_updates, bool platform_default) {
    // An explicit flag decides it, in either direction. helix_parse_truthy_env()
    // only answers "is this truthy", which cannot distinguish "0" from unset, so
    // presence is tested separately and a falsy value force-enables self-update
    // where the platform would otherwise default it off.
    //
    // Blank counts as ABSENT, not as falsy. helixscreen.env values routinely carry
    // a stray space (which is why parsing trims), and an all-whitespace value read
    // as an explicit "no" would silently switch self-update back on for a
    // firmware-managed install — the exact failure this predicate exists to stop.
    if (disable_auto_updates) {
        const char* p = disable_auto_updates;
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (*p != '\0') {
            return helix_parse_truthy_env(disable_auto_updates);
        }
    }
    return platform_default;
}

bool updates_externally_managed() {
    static const bool cached = compute_updates_externally_managed(
        std::getenv("HELIX_DISABLE_AUTO_UPDATES"), helix::platform_defaults_to_external_updates());
    return cached;
}

// Run `sudo -n true` and report whether it exited 0. `-n` is non-interactive:
// with NOPASSWD sudoers it succeeds immediately, otherwise it fails immediately
// rather than prompting, which matches how install.sh runs (forked with no tty).
//
// Bounded: sudoers can be backed by LDAP/SSSD, where a lookup against an
// unreachable directory blocks for the resolver's own timeout. This is called
// from the main thread during startup, so poll for the child instead of a
// blocking waitpid() and treat "still running at the deadline" as no.
static bool probe_passwordless_sudo() {
    constexpr int TIMEOUT_MS = 2000;
    constexpr int POLL_INTERVAL_MS = 20;

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::warn("[Updates] fork() for sudo probe failed: {}", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child — discard sudo's output; only the exit status matters.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // systemd services can start with PATH unset, which would break the
        // execvp lookup and misreport a sudo-capable box as not.
        const char* cur_path = std::getenv("PATH");
        if (!cur_path || cur_path[0] == '\0') {
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        }
        char* const argv[] = {const_cast<char*>("sudo"), const_cast<char*>("-n"),
                              const_cast<char*>("true"), nullptr};
        execvp("sudo", argv);
        _exit(127); // no sudo binary
    }

    for (int waited_ms = 0; waited_ms < TIMEOUT_MS; waited_ms += POLL_INTERVAL_MS) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0) {
            return false; // child vanished (SIGCHLD reaped elsewhere) — assume no
        }
        usleep(POLL_INTERVAL_MS * 1000);
    }

    spdlog::warn("[Updates] sudo probe did not finish in {}ms — assuming no escalation",
                 TIMEOUT_MS);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

bool root_escalation_available() {
    static const bool cached = []() {
        if (geteuid() == 0) {
            return true; // already root — every embedded platform runs this way
        }
        const bool ok = probe_passwordless_sudo();
        spdlog::info("[Updates] Passwordless sudo {}", ok ? "available" : "unavailable");
        return ok;
    }();
    return cached;
}

bool compute_self_update_supported(const std::string& install_root, bool can_escalate) {
    // install.sh applies an update one of two ways, and this predicate must
    // recognise BOTH or it hides an updater that would have worked:
    //
    //   atomic swap    "mv <root> <root>.old; mv <new> <root>" — renames mutate
    //                  the PARENT's directory entries, so it needs write
    //                  permission on the parent, not on the root.
    //   in-place       delete the root's contents (bar config/) and move the new
    //                  ones in. Everything happens INSIDE the root, so it needs
    //                  write permission on the root alone. install.sh picks this
    //                  automatically when the parent is not writable
    //                  (scripts/install.sh, "replacing install contents in-place").
    //
    // Testing only the parent was a false negative on the standalone-display
    // layout: no local Klipper, so detect_pi_install_dir() falls through to
    // /opt/helixscreen, whose parent is root-owned. The service user owns the root
    // itself (the unit's ExecStartPre chowns it), so the in-place path applies
    // fine — but the gate hid the updater, permanently, since the fix for it can
    // only arrive through an update.
    if (install_root.empty()) {
        // Unresolvable layout (bind-mounted binary). Conservative: assume
        // supported — the installer's own fallbacks and the explicit
        // HELIX_DISABLE_AUTO_UPDATES flag remain the deciding factors.
        return true;
    }
    // probe_writable(), not is_writable_dir(): the latter is access(W_OK), which
    // answers from the permission bits alone and so cannot see a read-only mount,
    // a restrictive ACL, an immutable bit, or an LSM denial. This predicate exists
    // to PREDICT what install.sh will manage, and install.sh settles the same
    // question by writing a probe file ("touch ${INSTALL_DIR}/.update_test"), so
    // answering it a different way is how the two drift apart. probe_writable
    // creates a uniquely-named file, writes a byte, and removes it; the result is
    // cached process-wide by self_update_supported(), so this costs one create per
    // directory per boot.
    const std::string parent = std::filesystem::path(install_root).parent_path().string();
    if (!parent.empty() && helix::paths::probe_writable(parent)) {
        return true; // atomic swap
    }
    if (parent.empty()) {
        return true; // no parent to test (e.g. a bare relative name) — don't block.
    }
    if (helix::paths::probe_writable(install_root)) {
        return true; // in-place replacement
    }
    // Neither path is open to this user. That is still NOT the same as impossible:
    // install.sh escalates with sudo when it can. Only a box with no write access
    // anywhere in the install tree and no route to root is genuinely stuck.
    return can_escalate;
}

bool self_update_supported() {
    static const bool cached = []() {
        const std::string root = app_get_install_root();
        // Ask without escalation first so a writable install tree never pays for
        // the sudo probe. That matters beyond speed: the shipped systemd unit sets
        // NoNewPrivileges=true, which makes sudo fail regardless of sudoers, so
        // escalation is a dead end for exactly the services that need it most.
        if (compute_self_update_supported(root, /*can_escalate=*/false)) {
            return true;
        }
        const bool escalate = root_escalation_available();
        if (!escalate) {
            spdlog::info("[Updates] Self-update unsupported: neither {} nor its parent is "
                         "writable and root is not obtainable",
                         root);
        }
        return compute_self_update_supported(root, escalate);
    }();
    return cached;
}

bool compute_update_install_suppressed(bool externally_managed, bool self_update_ok) {
    return externally_managed || !self_update_ok;
}

bool update_install_suppressed() {
    return compute_update_install_suppressed(updates_externally_managed(), self_update_supported());
}

bool update_checks_suppressed() {
    // Deliberately NOT the install gate. Checking is a manifest fetch over the
    // network — it needs nothing from the filesystem, so a tree we cannot write
    // is no reason to refuse to LOOK. The two questions shared one predicate
    // until now, which made every false negative in self_update_supported() a
    // permanent lockout: the rows vanished, so the user could not see that an
    // update existed, and the fix could only ship inside the update they were
    // being kept from. Only a firmware opt-out silences the check.
    return updates_externally_managed();
}
