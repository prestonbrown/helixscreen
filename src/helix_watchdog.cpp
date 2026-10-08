// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file helix_watchdog.cpp
 * @brief Ultra-stable watchdog supervisor for helix-screen crash recovery
 *
 * This is a lightweight supervisor process that monitors helix-screen for
 * crashes and displays a recovery dialog with user choices:
 * - Restart App: Fork a new helix-screen process
 * - Restart System: Reboot the system
 *
 * Design goals (same philosophy as helix-splash):
 * - Minimal dependencies (LVGL + display backend + spdlog)
 * - No networking (no libhv, no Moonraker)
 * - Direct LVGL API calls for crash dialog (no XML/theme system)
 * - Ultra-stable: must not crash when the main app crashes
 *
 * Only built and used on embedded Linux targets (DRM/fbdev).
 * Desktop developers use terminal output for crash debugging.
 */

#include "ui_fonts.h"

#include "backlight_backend.h"
#include "config.h"
#include "data_root_resolver.h"
#include "display_backend.h"
#ifdef HELIX_DISPLAY_DRM
#include "display_backend_drm.h"
#endif
#ifdef HELIX_DISPLAY_FBDEV
#include "display_backend_fbdev.h"
#endif
#include "app_constants.h"
#include "logging_init.h"
#include "watchdog_restart_policy.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <lvgl.h>
#include <regex>
#include <signal.h>
#include <string>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// =============================================================================
// Constants
// =============================================================================

static constexpr int DEFAULT_WIDTH = 800;
static constexpr int DEFAULT_HEIGHT = 480;
static constexpr int FRAME_DELAY_US = 16000; // ~60 FPS
static constexpr int DEFAULT_AUTO_RESTART_SEC = 30;

// Restart-loop detection: if the child exits non-zero (deliberate failure exit,
// not a crash signal) too often within a short window, the watchdog gives up
// and exits with RESTART_LOOP_EXIT_CODE so the system service manager (systemd,
// inittab, init script) sees the failure rather than the watchdog quietly
// busy-restarting forever. Typical trigger: "another instance is already
// running", config-validation failure, missing shared library — conditions that
// won't resolve via blind retry.
//
// Crash-handler exits (128..159) and signal terminations are EXCLUDED from the
// counter: those have their own crash-recovery dialog + crash-report pipeline
// and represent transient faults the user is already being notified about.
// Counting them here would suppress the recovery dialog after a few crashes,
// which is worse UX than letting the dialog keep appearing.
//
// TRANSIENT exec/fork failures are excluded too, and tracked separately with a
// far larger budget — see watchdog_restart_policy.h. Under memory pressure
// execv() of an intact binary can fail with ENOEXEC/ENOMEM; spending the small
// budget above on those turns a passing squeeze into a permanently black screen.
// The budget itself (RESTART_LOOP_MAX_FAILURES) and the pacing live in the
// policy header so they can be unit tested on hosts that never build this binary.
static constexpr int RESTART_LOOP_WINDOW_SEC = 60;
static constexpr int RESTART_LOOP_EXIT_CODE = 42;

// Crash-loop detection (separate window from non-zero-exit loops above).
// When the same crash signal repeats N times within M seconds, blind retry
// is provably futile — every "Restart App" produces the same crash from the
// same state (e.g. a JSON-null subscription field the printer keeps sending,
// f75b961d8 family). Don't give up; instead, expand the recovery dialog to
// offer a Safe Mode boot that defers Moonraker connection so the user can
// reach Settings and fix the underlying state without crashing again.
static constexpr int CRASH_LOOP_MAX_CRASHES = 3;
static constexpr int CRASH_LOOP_WINDOW_SEC = 90;

// UI Colors (dark theme, matches main app)
static constexpr uint32_t BG_COLOR_DARK = 0x121212;
static constexpr uint32_t CONTAINER_BG = 0x1E1E1E;
static constexpr uint32_t BORDER_ERROR = 0xF44336;
static constexpr uint32_t BUTTON_PRIMARY = 0x2196F3; // Blue - restart app
static constexpr uint32_t BUTTON_DANGER = 0xF44336;  // Red - restart system
static constexpr uint32_t TEXT_PRIMARY = 0xFFFFFF;
static constexpr uint32_t TEXT_SECONDARY = 0xAAAAAA;
static constexpr uint32_t TEXT_MUTED = 0x888888;

// =============================================================================
// Global State
// =============================================================================

// Signal handling
static volatile sig_atomic_t g_quit = 0;

// Dialog choice from button press
enum class DialogChoice { NONE, RESTART_APP, RESTART_SYSTEM, RESTART_SAFE_MODE };
static volatile DialogChoice g_dialog_choice = DialogChoice::NONE;

// Countdown state
static int g_countdown_seconds = 0;
static lv_obj_t* g_countdown_label = nullptr;

// Crash information
struct CrashInfo {
    int exit_code = 0;
    int signal_num = 0;
    bool was_signaled = false;
    std::string signal_name;
    time_t crash_time = 0;
};

// =============================================================================
// Signal Handling
// =============================================================================

static void signal_handler(int sig) {
    (void)sig;
    g_quit = 1;
}

static void setup_signal_handlers() {
    // Install SIGTERM/SIGINT WITHOUT SA_RESTART so they interrupt the blocking
    // waitpid() in run_child_process() with EINTR. glibc's signal() uses BSD
    // semantics (SA_RESTART set), which silently auto-restarts waitpid() — the
    // handler sets g_quit but it is never re-checked while the watchdog idles
    // supervising a healthy child, so a lone SIGTERM to the watchdog is ignored.
    // That defeats external supervisors that stop us by pidfile (e.g. the CC1
    // COSMOS gui-switcher's `start-stop-daemon -K`, run by GUI_STOP to free RAM
    // for resonance calibration). Interrupting waitpid() lets the EINTR path
    // (see run_child_process) reap helix-screen and exit cleanly.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: blocking syscalls return EINTR
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // Ignore SIGCHLD - we use waitpid explicitly
    signal(SIGCHLD, SIG_DFL);
}

// =============================================================================
// Configuration Reading
// =============================================================================

/**
 * @brief Read auto_restart_sec from settings.json
 * @return Timeout in seconds (0 = disabled), or default on failure
 */
static int read_auto_restart_timeout() {
    // writable_path honors HELIX_CONFIG_DIR (Yocto) or "config/" (tarball);
    // legacy paths are kept for AD5M-style installs that predate either env.
    std::vector<std::string> paths{
        helix::writable_path("settings.json"),
        helix::writable_path("helixconfig.json"),
        "helixconfig.json",
        "/opt/helixscreen/helixconfig.json",
    };

    for (const std::string& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Look for "watchdog" section with "auto_restart_sec"
        // Simple regex parsing to avoid JSON library dependency
        std::regex timeout_regex(R"("auto_restart_sec"\s*:\s*(\d+))");
        std::smatch match;
        if (std::regex_search(content, match, timeout_regex) && match.size() > 1) {
            int timeout = std::stoi(match[1].str());
            spdlog::debug("[Watchdog] Read auto_restart_sec={} from {}", timeout, path);
            return timeout;
        }
    }

    return DEFAULT_AUTO_RESTART_SEC;
}

/**
 * @brief Read brightness from settings.json (same as splash)
 */
static int read_config_brightness(int default_value = 100) {
    std::vector<std::string> paths{
        helix::writable_path("settings.json"),
        helix::writable_path("helixconfig.json"),
        "helixconfig.json",
        "/opt/helixscreen/helixconfig.json",
    };

    for (const std::string& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        std::regex brightness_regex(R"("brightness"\s*:\s*(\d+))");
        std::smatch match;
        if (std::regex_search(content, match, brightness_regex) && match.size() > 1) {
            int brightness = std::stoi(match[1].str());
            if (brightness < 10)
                brightness = 10;
            if (brightness > 100)
                brightness = 100;
            return brightness;
        }
    }

    return default_value;
}

// =============================================================================
// Command Line Parsing
// =============================================================================

struct WatchdogArgs {
    int width = 0; // 0 = auto-detect from display hardware
    int height = 0;
    int rotation = 0;          // Display rotation for the current launch (0, 90, 180, 270)
    int cli_rotation = 0;      // -r value; 0 = read the saved setting before each launch
    std::string splash_binary; // Optional: --splash-bin=<path>
    pid_t splash_pid = 0;      // Optional: --splash-pid=N (externally started splash)
    std::string child_binary;
    std::vector<std::string> child_args;
};

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage: %s [-w width] [-h height] [--splash-bin=<path>] [--splash-pid=N] -- "
            "<helix-screen> [args...]\n",
            program);
    fprintf(stderr, "  -w <width>          Screen width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -h <height>         Screen height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr,
            "  -r <degrees>        Display rotation: 0, 90, 180, 270 (default: from config)\n");
    fprintf(stderr, "  --splash-bin=<path> Path to splash screen binary (optional)\n");
    fprintf(stderr, "  --splash-pid=<pid>  PID of externally started splash process (optional)\n");
    fprintf(stderr, "  --                  Separator before child binary and args\n");
}

static bool parse_args(int argc, char** argv, WatchdogArgs& args) {
    bool after_separator = false;

    for (int i = 1; i < argc; i++) {
        if (after_separator) {
            if (args.child_binary.empty()) {
                args.child_binary = argv[i];
            } else {
                args.child_args.push_back(argv[i]);
            }
        } else if (strcmp(argv[i], "--") == 0) {
            after_separator = true;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            args.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            args.height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            args.cli_rotation = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--splash-bin=", 13) == 0) {
            args.splash_binary = argv[i] + 13;
        } else if (strncmp(argv[i], "--splash-pid=", 13) == 0) {
            args.splash_pid = static_cast<pid_t>(atoi(argv[i] + 13));
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
    }

    if (args.child_binary.empty()) {
        fprintf(stderr, "Error: No child binary specified after '--'\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

// =============================================================================
// Splash Process Management
// =============================================================================

// Global splash PID for cleanup
static volatile pid_t g_splash_pid = 0;

/**
 * @brief Check if the display backend will use DRM
 *
 * The external splash process uses fbdev, which conflicts with DRM — both
 * backends fight over the same display hardware, causing mmap failures on
 * DRM dumb buffer allocation. When DRM will be used (forced or auto-detected),
 * skip the external splash and let the main app show its internal LVGL splash.
 */
static bool would_use_drm() {
    const char* env = std::getenv("HELIX_DISPLAY_BACKEND");
    if (env != nullptr) {
        return strcmp(env, "drm") == 0;
    }
    // Auto mode: DRM is tried first in create_auto(), check if it's available
#ifdef HELIX_DISPLAY_DRM
    DisplayBackendDRM drm;
    return drm.is_available();
#else
    return false;
#endif
}

/**
 * @brief Start splash screen process
 * @return PID of splash process, or 0 if not started
 */
static pid_t start_splash_process(const WatchdogArgs& args) {
    if (args.splash_binary.empty()) {
        return 0;
    }

    // Check if binary exists
    if (access(args.splash_binary.c_str(), X_OK) != 0) {
        spdlog::warn("[Watchdog] Splash binary not found or not executable: {}",
                     args.splash_binary);
        return 0;
    }

    pid_t pid = fork();

    if (pid < 0) {
        spdlog::error("[Watchdog] Failed to fork splash process: {}", strerror(errno));
        return 0;
    }

    if (pid == 0) {
        // Child process: exec splash with rotation if configured
        if (args.rotation != 0) {
            std::string rot_str = std::to_string(args.rotation);
            execl(args.splash_binary.c_str(), "helix-splash", "-r", rot_str.c_str(), nullptr);
        } else {
            execl(args.splash_binary.c_str(), "helix-splash", nullptr);
        }

        // If exec fails
        fprintf(stderr, "[Watchdog] Failed to exec splash: %s\n", strerror(errno));
        _exit(127);
    }

    // Parent: splash started successfully
    spdlog::info("[Watchdog] Started splash process (PID {})", pid);
    g_splash_pid = pid;
    return pid;
}

/**
 * @brief Verify that pid belongs to our splash binary
 *
 * Reads /proc/<pid>/comm and confirms it matches "helix-splash". Guards
 * against PID recycling: if the original splash was reaped by a different
 * parent and the kernel handed the PID to an unrelated process, sending
 * signals to it would be a serious bug. Returns false on any uncertainty
 * — better to leak a splash than kill the wrong process.
 */
static bool pid_is_our_splash(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
    FILE* f = fopen(comm_path, "r");
    if (!f) {
        return false;
    }
    char comm[64] = {0};
    bool got_line = (fgets(comm, sizeof(comm), f) != nullptr);
    fclose(f);
    if (!got_line) {
        return false;
    }
    // Strip trailing newline
    size_t n = strlen(comm);
    if (n > 0 && comm[n - 1] == '\n') {
        comm[n - 1] = '\0';
    }
    // Linux truncates /proc/<pid>/comm to 15 chars. "helix-splash" is 12, fits.
    return strcmp(comm, "helix-splash") == 0;
}

/**
 * @brief Clean up splash process if still running
 *
 * Verifies the PID actually belongs to our splash binary (PID-recycling
 * guard) and escalates SIGTERM → SIGKILL so a misbehaving splash cannot
 * leak past helix-screen exit. Total worst-case wait ~800ms (30 × 20ms
 * after SIGTERM, 10 × 20ms after SIGKILL).
 *
 * Safe to call on processes we did not fork: when waitpid() returns ECHILD,
 * we fall through to kill(pid, 0) for liveness checks.
 */
static void cleanup_splash(pid_t splash_pid) {
    if (splash_pid <= 0) {
        return;
    }

    auto clear_global = [splash_pid]() {
        if (g_splash_pid == splash_pid) {
            g_splash_pid = 0;
        }
    };

    if (!pid_is_our_splash(splash_pid)) {
        // Either already gone, never existed, or PID got recycled. In all
        // three cases, do not signal — stale-PID kills cause incident reports
        // worse than the leak we're trying to prevent.
        spdlog::debug("[Watchdog] Skipping splash cleanup: PID {} is not helix-splash", splash_pid);
        clear_global();
        return;
    }

    auto reap_or_check = [splash_pid]() {
        int status;
        pid_t result = waitpid(splash_pid, &status, WNOHANG);
        if (result == splash_pid) {
            return false; // reaped
        }
        // Keep the comm-check as a continuous guard — if the PID gets recycled
        // mid-loop, stop signaling immediately.
        return pid_is_our_splash(splash_pid);
    };

    spdlog::debug("[Watchdog] Cleaning up splash process (PID {})", splash_pid);
    kill(splash_pid, SIGTERM);

    // Wait up to 600ms for SIGTERM, then escalate to SIGKILL.
    bool alive = true;
    for (int i = 0; i < 30 && alive; ++i) {
        usleep(20000); // 20ms
        alive = reap_or_check();
    }

    if (alive) {
        spdlog::warn("[Watchdog] Splash (PID {}) didn't exit after SIGTERM, sending SIGKILL",
                     splash_pid);
        kill(splash_pid, SIGKILL);
        for (int i = 0; i < 10 && alive; ++i) {
            usleep(20000); // 20ms
            alive = reap_or_check();
        }
    }

    clear_global();
}

// =============================================================================
// Process Management
// =============================================================================

/**
 * @brief Fork and exec helix-screen, wait for it to exit
 * @param args Watchdog arguments
 * @param splash_pid PID of splash process to pass to helix-screen (0 if none)
 * @return CrashInfo with exit status
 */
static CrashInfo run_child_process(const WatchdogArgs& args, pid_t splash_pid) {
    CrashInfo crash = {};

    // Build argv for execv - need to own the strings for splash_pid arg
    std::vector<std::string> arg_strings;
    arg_strings.push_back(args.child_binary);

    // Add splash PID argument if splash is running
    if (splash_pid > 0) {
        arg_strings.push_back("--splash-pid=" + std::to_string(splash_pid));
    }

    // Pass rotation to child if configured
    if (args.rotation != 0) {
        arg_strings.push_back("--rotate=" + std::to_string(args.rotation));
    }

    // Add remaining child args, but skip any --splash-pid from the original
    // launcher invocation — the watchdog manages splash PIDs itself and the
    // original PID is stale on restart.
    for (const auto& arg : args.child_args) {
        if (arg.rfind("--splash-pid=", 0) == 0) {
            continue;
        }
        arg_strings.push_back(arg);
    }

    // Build char* argv from owned strings
    std::vector<char*> child_argv;
    for (auto& arg : arg_strings) {
        child_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    child_argv.push_back(nullptr);

    spdlog::info("[Watchdog] Launching: {}", args.child_binary);
    if (splash_pid > 0) {
        spdlog::debug("[Watchdog] Passing splash PID {} to child", splash_pid);
    }

    pid_t child_pid = fork();

    if (child_pid < 0) {
        // fork() failure is classified with the same table as exec: EAGAIN
        // (process/thread limit) and ENOMEM are resource pressure that lifts,
        // everything else is a standing condition. Encoding the class in
        // crash.exit_code routes it through the same restart policy below.
        const int fork_errno = errno;
        const auto cls = helix::watchdog::classify_exec_errno(fork_errno);
        spdlog::error("[Watchdog] fork() failed: binary={} errno={} ({}) class={}",
                      args.child_binary, fork_errno, strerror(fork_errno),
                      helix::watchdog::exec_failure_class_name(cls));
        crash.was_signaled = false;
        crash.exit_code = helix::watchdog::exec_failure_exit_code(fork_errno);
        crash.crash_time = time(nullptr);
        return crash;
    }

    if (child_pid == 0) {
        // Child process: set supervisor env var so app knows not to fork on restart
        setenv("HELIX_SUPERVISED", "1", 1);

        // exec helix-screen
        execv(args.child_binary.c_str(), child_argv.data());

        // Exec failed. Two things matter here.
        //
        // First, the exit code carries the errno CLASS back to the parent so it
        // can tell "install is broken" from "system was briefly out of memory"
        // and pace its retries accordingly.
        //
        // Second, this line has to stand on its own. On platforms where
        // /var/log is tmpfs, spdlog goes to syslog and every surrounding
        // "[Watchdog] Child binary:" / "[Watchdog] Launching:" line is gone
        // after a reboot — this stderr write, captured in launcher.log, is all
        // that survives. So it names the binary, the numeric errno, its text,
        // and the classification.
        //
        // This is a forked child, so it formats into a stack buffer and issues
        // one write() rather than calling spdlog (whose sinks and mutexes were
        // inherited mid-flight from the parent).
        const int exec_errno = errno;
        const auto cls = helix::watchdog::classify_exec_errno(exec_errno);
        const int exit_code = helix::watchdog::exec_failure_exit_code(exec_errno);

        char msg[768];
        int len = snprintf(msg, sizeof(msg),
                           "[Watchdog] execv failed: binary=%s errno=%d (%s) class=%s exit=%d\n",
                           args.child_binary.c_str(), exec_errno, strerror(exec_errno),
                           helix::watchdog::exec_failure_class_name(cls), exit_code);
        if (len > 0) {
            if (len > static_cast<int>(sizeof(msg)) - 1) {
                len = static_cast<int>(sizeof(msg)) - 1;
            }
            ssize_t written = write(STDERR_FILENO, msg, static_cast<size_t>(len));
            (void)written; // nothing useful to do if even stderr is gone
        }
        _exit(exit_code);
    }

    // Parent: wait for child with proper EINTR handling
    // Use waitpid(-1) to reap any child, including splash process
    int status;
    while (true) {
        pid_t result = waitpid(-1, &status, 0);

        if (result == child_pid) {
            // helix-screen exited
            break;
        }

        if (result == splash_pid) {
            // Splash exited (reaped) - this is expected, continue waiting for helix-screen
            spdlog::debug("[Watchdog] Splash process reaped (PID {})", splash_pid);
            continue;
        }

        if (result < 0) {
            if (errno == EINTR) {
                // Signal interrupted waitpid, check if we should quit
                if (g_quit) {
                    // Watchdog is shutting down, kill child
                    spdlog::info("[Watchdog] Shutting down, terminating child");
                    kill(child_pid, SIGTERM);
                    // Reap across further signals. Handlers are installed
                    // without SA_RESTART, so a second SIGTERM/SIGINT during
                    // shutdown (e.g. start-stop-daemon -K's TERM-then-KILL
                    // retry) would otherwise return EINTR and leave helix-screen
                    // unreaped, holding RAM through calibration.
                    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
                    }
                    crash.exit_code = 0;
                    crash.was_signaled = false;
                    return crash;
                }
                continue;
            }
            if (errno == ECHILD) {
                // No more children - shouldn't happen but handle gracefully
                spdlog::warn("[Watchdog] No children to wait for");
                crash.exit_code = 0;
                crash.was_signaled = false;
                return crash;
            }
            // Actual error
            spdlog::error("[Watchdog] waitpid error: {}", strerror(errno));
            crash.exit_code = 127;
            crash.was_signaled = false;
            crash.crash_time = time(nullptr);
            return crash;
        }
    }

    crash.crash_time = time(nullptr);

    if (WIFEXITED(status)) {
        crash.exit_code = WEXITSTATUS(status);
        crash.was_signaled = false;
        spdlog::info("[Watchdog] Child exited with code {}", crash.exit_code);
    } else if (WIFSIGNALED(status)) {
        crash.signal_num = WTERMSIG(status);
        crash.was_signaled = true;
        crash.signal_name = strsignal(crash.signal_num);
        spdlog::warn("[Watchdog] Child killed by signal {} ({})", crash.signal_num,
                     crash.signal_name);
    }

    return crash;
}

// =============================================================================
// System Restart
// =============================================================================

/**
 * @brief Try a reboot method via fork+exec, return true if exec succeeded
 *
 * Uses fork so that exec failure doesn't replace the watchdog process,
 * allowing fallback to the next method.
 */
static bool try_reboot_exec(const char* desc, const char* path,
                            const std::vector<const char*>& argv) {
    spdlog::info("[Watchdog] Trying {}", desc);

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::warn("[Watchdog] fork() failed for {}: {}", desc, strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child - exec the reboot command
        execv(path, const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    }

    // Parent - wait briefly for the child
    int status = 0;
    int attempts = 0;
    while (attempts < 10) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                spdlog::info("[Watchdog] {} succeeded, system rebooting...", desc);
                // Give the system a moment to process the reboot request
                sleep(10);
                return true; // Shouldn't reach here if reboot worked
            }
            spdlog::warn("[Watchdog] {} failed (exit code {})", desc,
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return false;
        }
        if (ret < 0 && errno != EINTR) {
            break;
        }
        usleep(200000); // 200ms
        attempts++;
    }

    // Still running after 2s - assume it's working (reboot in progress)
    spdlog::info("[Watchdog] {} still running, assuming reboot in progress...", desc);
    sleep(10);
    // If we're still here, it didn't work
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

/**
 * @brief Perform system restart using appropriate method
 *
 * Tries multiple reboot strategies in order:
 * 1. systemctl reboot (needs privileges or polkit)
 * 2. gdbus call to logind Reboot (works without privilege escalation)
 * 3. /sbin/reboot
 * 4. reboot() syscall (needs CAP_SYS_BOOT)
 */
[[noreturn]] static void perform_system_restart() {
    spdlog::info("[Watchdog] Initiating system restart");

    // Flush filesystems
    sync();

    std::error_code ec;
    if (std::filesystem::exists("/run/systemd/system", ec)) {
        // Try systemctl reboot (works if polkit allows it or running as root)
        try_reboot_exec("systemctl reboot", "/usr/bin/systemctl",
                        {"/usr/bin/systemctl", "reboot", nullptr});

        // Try D-Bus call to logind - works for unprivileged users on systems
        // with active sessions (no polkit prompt needed for interactive=true)
        try_reboot_exec("logind D-Bus Reboot", "/usr/bin/gdbus",
                        {"/usr/bin/gdbus", "call", "--system", "--dest", "org.freedesktop.login1",
                         "--object-path", "/org/freedesktop/login1", "--method",
                         "org.freedesktop.login1.Manager.Reboot", "true", nullptr});
    }

    // Fallback to /sbin/reboot
    try_reboot_exec("/sbin/reboot", "/sbin/reboot", {"/sbin/reboot", nullptr});

    // Last resort: direct syscall (requires CAP_SYS_BOOT)
    spdlog::warn("[Watchdog] All exec methods failed, trying reboot syscall");
    reboot(RB_AUTOBOOT);

    // Should never reach here
    spdlog::error("[Watchdog] All reboot methods failed");
    _exit(1);
}

// =============================================================================
// Crash Dialog UI
// =============================================================================

// Portable timing (same as ui_fatal_error.cpp)
static uint32_t get_ticks_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Button callbacks
static void on_restart_app_clicked(lv_event_t* /*e*/) {
    g_dialog_choice = DialogChoice::RESTART_APP;
}

static void on_restart_system_clicked(lv_event_t* /*e*/) {
    g_dialog_choice = DialogChoice::RESTART_SYSTEM;
}

static void on_safe_mode_clicked(lv_event_t* /*e*/) {
    g_dialog_choice = DialogChoice::RESTART_SAFE_MODE;
}

/**
 * @brief Path to the safe-mode marker file.
 *
 * The watchdog writes this when the user picks "Boot Safe Mode" from the
 * crash-loop dialog. Application reads it at startup, defers the Moonraker
 * connection, and shows a banner explaining the state. The marker is one-shot
 * — the application clears it once the main loop is running and the user can
 * see the banner, so a clean reboot exits safe mode automatically.
 */
static std::string safe_mode_marker_path() {
    return helix::writable_path("safe_mode.flag");
}

/**
 * @brief Write the safe-mode marker.
 *
 * @return true on success. On failure, the caller MUST NOT just `continue` —
 *         restarting the app without a marker means the next boot reconnects
 *         Moonraker, hits the same JSON-null bug, and crashes again. The user
 *         is then stuck in a worse loop than before because they thought they
 *         escaped via Safe Mode.
 *
 * Tries the canonical writable path first; if that fails (read-only fs,
 * missing parent dir, sudo lockout) falls back to /tmp/helix-screen-safe-mode.flag
 * which is world-writable and exists on every supported platform.
 * Application::consume_safe_mode_marker checks both locations.
 */
static bool write_safe_mode_marker() {
    auto try_write = [](const std::string& path) -> bool {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) {
            return false;
        }
        fprintf(f, "watchdog_crash_loop\n%lld\n", static_cast<long long>(time(nullptr)));
        fclose(f);
        return true;
    };

    std::string primary = safe_mode_marker_path();
    if (try_write(primary)) {
        spdlog::info("[Watchdog] Wrote safe-mode marker: {}", primary);
        return true;
    }
    spdlog::error("[Watchdog] Failed to write safe-mode marker at {}: {}", primary,
                  strerror(errno));

    const char* fallback = "/tmp/helix-screen-safe-mode.flag";
    if (try_write(fallback)) {
        spdlog::warn("[Watchdog] Wrote safe-mode marker to fallback path: {}", fallback);
        return true;
    }
    spdlog::critical("[Watchdog] Failed to write safe-mode marker at fallback {}: {}", fallback,
                     strerror(errno));
    return false;
}

// Cancel countdown on any touch
static void on_screen_pressed(lv_event_t* /*e*/) {
    if (g_countdown_seconds > 0 && g_countdown_label) {
        g_countdown_seconds = 0;
        lv_obj_add_flag(g_countdown_label, LV_OBJ_FLAG_HIDDEN);
        spdlog::debug("[Watchdog] Countdown cancelled by touch");
    }
}

/**
 * @brief Create a styled button
 */
static lv_obj_t* create_button(lv_obj_t* parent, const char* text, uint32_t color,
                               lv_event_cb_t callback) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 180, 56);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &noto_sans_bold_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_center(label);

    return btn;
}

/**
 * @brief Create the crash dialog UI
 *
 * @param crash_loop_detected When true, the same crash has fired ≥3× in the
 *        last 90s — blind retry won't help. Disable the auto-restart
 *        countdown, swap in a more informative title, and add a "Boot Safe
 *        Mode" button that writes the safe-mode marker so the next launch
 *        skips Moonraker auto-connect.
 */
static void create_crash_dialog(lv_obj_t* screen, int /*width*/, int /*height*/,
                                const CrashInfo& crash, int auto_restart_sec,
                                bool crash_loop_detected) {
    // Dark background
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR_DARK), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Touch anywhere cancels countdown
    lv_obj_add_event_cb(screen, on_screen_pressed, LV_EVENT_PRESSED, nullptr);

    // Main container
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_center(container);
    lv_obj_set_style_bg_color(container, lv_color_hex(CONTAINER_BG), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(BORDER_ERROR), 0);
    lv_obj_set_style_radius(container, 12, 0);
    lv_obj_set_style_pad_all(container, 24, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon
    lv_obj_t* icon = lv_label_create(container);
    lv_label_set_text(icon, ICON_TRIANGLE_EXCLAMATION);
    lv_obj_set_style_text_font(icon, &mdi_icons_64, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(BORDER_ERROR), 0);

    // Title
    lv_obj_t* title = lv_label_create(container);
    lv_label_set_text(title,
                      crash_loop_detected ? "HelixScreen Keeps Crashing" : "HelixScreen Crashed");
    lv_obj_set_style_text_font(title, &noto_sans_bold_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_top(title, 16, 0);

    // Crash details
    lv_obj_t* details = lv_label_create(container);
    if (crash.was_signaled) {
        lv_label_set_text_fmt(details, "Signal: %d (%s)", crash.signal_num,
                              crash.signal_name.c_str());
    } else {
        lv_label_set_text_fmt(details, "Exit code: %d", crash.exit_code);
    }
    lv_obj_set_style_text_font(details, &noto_sans_14, 0);
    lv_obj_set_style_text_color(details, lv_color_hex(TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_top(details, 8, 0);

    // Loop hint: when the same crash repeats, restarting won't help on its
    // own — point the user at Safe Mode.
    if (crash_loop_detected) {
        lv_obj_t* hint = lv_label_create(container);
        lv_label_set_text(hint, "Same crash 3+ times in a row. Try Safe Mode\n"
                                "(boots without connecting to the printer) so\n"
                                "you can adjust settings.");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_font(hint, &noto_sans_14, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(hint, 12, 0);
    }

    // Countdown timer (hidden if auto_restart_sec == 0 OR loop detected —
    // auto-restart in a deterministic loop just hides the dialog without
    // letting the user choose Safe Mode).
    g_countdown_label = lv_label_create(container);
    if (auto_restart_sec > 0 && !crash_loop_detected) {
        g_countdown_seconds = auto_restart_sec;
        lv_label_set_text_fmt(g_countdown_label, "Auto-restart in %d seconds...",
                              g_countdown_seconds);
        lv_obj_set_style_text_font(g_countdown_label, &noto_sans_14, 0);
        lv_obj_set_style_text_color(g_countdown_label, lv_color_hex(TEXT_MUTED), 0);
        lv_obj_set_style_pad_top(g_countdown_label, 12, 0);
    } else {
        g_countdown_seconds = 0;
        lv_obj_add_flag(g_countdown_label, LV_OBJ_FLAG_HIDDEN);
    }

    // Button container
    lv_obj_t* btn_container = lv_obj_create(container);
    lv_obj_set_size(btn_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(btn_container, 24, 0);
    lv_obj_set_style_pad_column(btn_container, 16, 0);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);

    if (crash_loop_detected) {
        // Loop dialog: Safe Mode is the primary action since plain restart
        // is provably futile here.
        create_button(btn_container, "Safe Mode", BUTTON_PRIMARY, on_safe_mode_clicked);
        create_button(btn_container, "Restart App", BUTTON_DANGER, on_restart_app_clicked);
    } else {
        // Normal dialog: standard two-button layout.
        create_button(btn_container, "Restart App", BUTTON_PRIMARY, on_restart_app_clicked);
        create_button(btn_container, "Restart System", BUTTON_DANGER, on_restart_system_clicked);
    }
}

/**
 * @brief Show crash dialog and wait for user choice
 * @return User's choice
 */
static DialogChoice show_crash_dialog(int width, int height, int rotation, const CrashInfo& crash,
                                      bool crash_loop_detected) {
    int auto_restart_sec = read_auto_restart_timeout();

    spdlog::info("[Watchdog] Showing crash dialog (auto_restart={}s)", auto_restart_sec);

    // Initialize config so touch calibration data is available
    helix::Config::get_instance()->init(helix::writable_path("settings.json"));

    // Initialize LVGL
    lv_init();

    // Create display backend (with fbdev fallback for crash resilience)
    auto backend = DisplayBackend::create();
    if (!backend) {
        spdlog::error("[Watchdog] Failed to create display backend");
        return DialogChoice::RESTART_APP;
    }

    lv_display_t* display = backend->create_display(width, height);
#ifdef HELIX_DISPLAY_FBDEV
    if (!display) {
        // DRM may fail (e.g., mmap contention). Try fbdev as fallback so the
        // crash recovery screen is visible instead of looping silently.
        spdlog::warn("[Watchdog] Primary display failed, trying fbdev fallback");
        backend = std::make_unique<DisplayBackendFbdev>();
        if (backend->is_available()) {
            display = backend->create_display(width, height);
            if (display) {
                spdlog::info("[Watchdog] Crash dialog using fbdev fallback");
            }
        }
    }
#endif
    if (!display) {
        spdlog::error("[Watchdog] Failed to create display");
        return DialogChoice::RESTART_APP;
    }

    // Apply rotation to crash dialog display
    if (rotation != 0) {
        lv_display_set_rotation(display, degrees_to_lv_rotation(rotation));
        spdlog::info("[Watchdog] Crash dialog rotated {}°", rotation);
    }

    // Turn on backlight
    auto backlight = BacklightBackend::create();
    if (backlight && backlight->is_available()) {
        int brightness = read_config_brightness(100);
        backlight->set_brightness(brightness);
    }

    // Create touch input
    backend->create_input_pointer();

    // Create dialog UI
    lv_obj_t* screen = lv_screen_active();
    create_crash_dialog(screen, width, height, crash, auto_restart_sec, crash_loop_detected);

    // Event loop with countdown
    g_dialog_choice = DialogChoice::NONE;
    uint32_t last_second = get_ticks_ms() / 1000;

    while (g_dialog_choice == DialogChoice::NONE && !g_quit) {
        lv_timer_handler();
        usleep(FRAME_DELAY_US);

        // Update countdown every second
        uint32_t current_second = get_ticks_ms() / 1000;
        if (g_countdown_seconds > 0 && current_second != last_second) {
            last_second = current_second;
            g_countdown_seconds--;

            if (g_countdown_seconds > 0) {
                lv_label_set_text_fmt(g_countdown_label, "Auto-restart in %d seconds...",
                                      g_countdown_seconds);
            } else {
                // Countdown reached zero - auto restart
                spdlog::info("[Watchdog] Countdown expired, auto-restarting app");
                g_dialog_choice = DialogChoice::RESTART_APP;
            }
        }
    }

    DialogChoice result = g_dialog_choice;

    // Cleanup LVGL
    lv_deinit();

    spdlog::info("[Watchdog] User choice: {}",
                 result == DialogChoice::RESTART_SYSTEM ? "restart system" : "restart app");

    return result == DialogChoice::NONE ? DialogChoice::RESTART_APP : result;
}

// =============================================================================
// Main Watchdog Loop
// =============================================================================

/**
 * @brief Sleep for @p seconds, yielding promptly if a shutdown was requested.
 *
 * Backoff delays reach a full minute, so a plain sleep() would leave
 * `start-stop-daemon -K` (or systemctl stop) waiting that long before the
 * supervisor noticed. Chunking into one-second naps bounds that to ~1s.
 */
static void backoff_sleep(int seconds) {
    for (int i = 0; i < seconds && !g_quit; ++i) {
        sleep(1);
    }
}

static int run_watchdog(WatchdogArgs args) {
    spdlog::info("[Watchdog] Starting watchdog supervisor");
    spdlog::info("[Watchdog] Child binary: {}", args.child_binary);
    if (!args.splash_binary.empty()) {
        spdlog::info("[Watchdog] Splash binary: {}", args.splash_binary);
    }
    if (args.splash_pid > 0) {
        spdlog::info("[Watchdog] External splash PID: {}", args.splash_pid);
    }

    bool first_launch = true;

    // Rolling window of recent non-zero deliberate-exit timestamps. See the
    // RESTART_LOOP_* constants at the top of this file for rationale.
    std::deque<std::chrono::steady_clock::time_point> recent_failures;

    // TRANSIENT exec/fork failures get their own counters, deliberately NOT
    // windowed: with exponential backoff a fixed window would drain faster than
    // failures arrive and the budget would never bite. Consecutive-count
    // semantics instead — any launch that actually reached helix-screen clears
    // them, so a single successful start wipes the history.
    int consecutive_transient_failures = 0;
    int transient_cooldown_rounds = 0;

    // Rolling window of recent crash signatures (signal_num for SIGNAL crashes,
    // or exit_code for crash-handler 128..159 exits). Detecting the same
    // signature ≥ CRASH_LOOP_MAX_CRASHES times within CRASH_LOOP_WINDOW_SEC
    // means the same crash is reproducing every restart — blind retry won't
    // fix it. See the CRASH_LOOP_* constants for rationale.
    struct CrashEvent {
        std::chrono::steady_clock::time_point at;
        int signature; // signal_num (1..31) or exit code (128..159)
    };
    std::deque<CrashEvent> recent_crashes;

    while (!g_quit) {
        const int launch_rotation = helix::watchdog::rotation_for_launch(
            args.cli_rotation, [] { return read_config_rotation(0); });
        if (launch_rotation != args.rotation) {
            spdlog::info("[Watchdog] Display rotation: {}° (was {}°)", launch_rotation,
                         args.rotation);
            args.rotation = launch_rotation;
        }

        // Start or adopt splash screen before launching helix-screen.
        // On first launch, prefer an externally-started splash (args.splash_pid)
        // over starting a new one. On restarts, always start fresh (the original
        // PID is stale).
        //
        // Skip the external splash entirely when DRM will be used — the fbdev-based
        // splash conflicts with DRM (both fight over display hardware, causing mmap
        // failures). The main app will show its internal LVGL splash instead.
        pid_t splash_pid = 0;
        bool use_drm = would_use_drm();
        if (use_drm) {
            spdlog::info("[Watchdog] DRM backend detected, skipping external splash");
            // Init scripts (helixscreen.init) start splash unconditionally before the
            // backend is selected. On DRM platforms (Snapmaker U1, etc.) that splash
            // is invisible — DRM owns the panel — but the fbdev-compat /dev/fb0 layer
            // lets it run, where it spins burning CPU forever. Reap it now.
            if (first_launch && args.splash_pid > 0) {
                spdlog::info("[Watchdog] Reaping orphaned external splash (PID {})",
                             args.splash_pid);
                cleanup_splash(args.splash_pid);
            }
        } else if (first_launch && args.splash_pid > 0) {
            // Adopt externally-started splash (e.g. from init script)
            splash_pid = args.splash_pid;
            g_splash_pid = splash_pid;
            spdlog::info("[Watchdog] Adopting external splash (PID {})", splash_pid);
        } else if (first_launch || !args.splash_binary.empty()) {
            splash_pid = start_splash_process(args);
        }
        first_launch = false;

        // Launch and monitor child process
        CrashInfo crash = run_child_process(args, splash_pid);

        // Clean up splash if still running (safety net)
        cleanup_splash(splash_pid);

        // Check for update restart marker — always consume it to prevent staleness.
        // Written by UpdateChecker before _exit(0) after a successful update install.
        bool was_update_restart = false;
        {
            std::string marker = AppConstants::Update::update_restart_marker_path();
            namespace fs = std::filesystem;
            std::error_code ec;
            if (fs::exists(marker, ec) && !ec) {
                fs::remove(marker, ec);
                was_update_restart = true;
                spdlog::info("[Watchdog] Consumed update restart marker");
            }
        }

        // Check if we're shutting down
        if (g_quit) {
            spdlog::info("[Watchdog] Shutting down");
            break;
        }

        // Did this launch fail in exec/fork, and if so, is that failure the kind
        // that retrying fixes? The forked child encodes the answer in reserved
        // exit codes helix-screen itself never returns (see
        // watchdog_restart_policy.h); anything else means the app really ran.
        const auto exec_class = crash.was_signaled
                                    ? helix::watchdog::ExecFailureClass::NONE
                                    : helix::watchdog::classify_child_exit_code(crash.exit_code);

        // Any launch that got past exec ends the transient streak, so one
        // successful start wipes the accumulated pressure history.
        if (exec_class != helix::watchdog::ExecFailureClass::TRANSIENT) {
            consecutive_transient_failures = 0;
            transient_cooldown_rounds = 0;
        }

        // Normal exit (code 0) - just restart silently
        if (!crash.was_signaled && crash.exit_code == 0) {
            if (helix::watchdog::hand_clean_exit_to_service_manager(
                    was_update_restart, getenv("HELIX_SERVICE_RESTARTS"))) {
                spdlog::info(
                    "[Watchdog] Update restart under a restarting service - exiting so the "
                    "service restarts and refreshes its units");
                break;
            }
            spdlog::info("[Watchdog] Child exited normally, restarting");
            continue;
        }

        // The app's signal handler catches SIGSEGV/SIGABRT/SIGBUS etc., writes
        // the crash report, then calls exit(128+signum). From waitpid()'s view
        // that's WIFEXITED with a non-zero code, not WIFSIGNALED — but it IS a
        // crash. Exit codes in [128, 159] follow the POSIX shell convention of
        // encoding signal N as exit 128+N. Translate back so the recovery
        // dialog reflects the real signal.
        if (!crash.was_signaled && crash.exit_code > 128 && crash.exit_code < 160) {
            int signum = crash.exit_code - 128;
            spdlog::warn("[Watchdog] Child exited with code {} (signal {} via crash handler)",
                         crash.exit_code, signum);
            crash.was_signaled = true;
            crash.signal_num = signum;
            crash.signal_name = strsignal(signum);
            crash.exit_code = 0;
            // fall through to crash-handling path below
        } else if (!crash.was_signaled) {
            using helix::watchdog::ExecFailureClass;
            using helix::watchdog::RestartAction;

            int failure_count = 0;

            if (exec_class == ExecFailureClass::TRANSIENT) {
                // exec/fork lost to momentary resource pressure. The binary is
                // intact and runs fine once the squeeze passes (an input-shaper
                // FFT, a large upload — anything that pushes a small board into
                // swap). Counted on its own so it cannot spend the small
                // non-transient budget and strand the user at a black screen.
                failure_count = ++consecutive_transient_failures;
                spdlog::warn("[Watchdog] Child launch failed transiently (exit {}); "
                             "consecutive transient failures: {}/{}, cooldown rounds used {}/{}",
                             crash.exit_code, failure_count,
                             helix::watchdog::TRANSIENT_MAX_FAILURES, transient_cooldown_rounds,
                             helix::watchdog::TRANSIENT_MAX_COOLDOWN_ROUNDS);
            } else {
                // Genuine non-zero exit (no crash): deliberate exit() with a
                // failure code — "another instance running", config validation,
                // CLI arg error — or a PERMANENT exec failure (missing binary,
                // no execute permission, path is a directory).
                //
                // Tracked in a rolling window: if they keep happening, the
                // underlying problem won't fix itself by retrying, so bail out
                // and let the service manager (or a human) see the failure
                // rather than spamming logs forever.
                auto now = std::chrono::steady_clock::now();
                const auto window = std::chrono::seconds(RESTART_LOOP_WINDOW_SEC);
                while (!recent_failures.empty() && (now - recent_failures.front()) > window) {
                    recent_failures.pop_front();
                }
                recent_failures.push_back(now);
                failure_count = static_cast<int>(recent_failures.size());
            }

            const auto decision = helix::watchdog::decide_restart_action(exec_class, failure_count,
                                                                         transient_cooldown_rounds);

            if (decision.action == RestartAction::GIVE_UP) {
                if (exec_class == ExecFailureClass::TRANSIENT) {
                    spdlog::critical(
                        "[Watchdog] Transient launch failures never cleared: {} cooldown "
                        "rounds exhausted (last exit code: {}). Exiting watchdog with code "
                        "{} so the service manager sees it.",
                        transient_cooldown_rounds, crash.exit_code, RESTART_LOOP_EXIT_CODE);
                } else {
                    spdlog::critical(
                        "[Watchdog] Restart loop detected: child exited non-zero {} times "
                        "in the last {}s (last exit code: {}). Giving up to avoid busy-loop; "
                        "exiting watchdog with code {}. Investigate the underlying failure "
                        "(another instance running, bad config, missing library, etc.).",
                        failure_count, RESTART_LOOP_WINDOW_SEC, crash.exit_code,
                        RESTART_LOOP_EXIT_CODE);
                }
                cleanup_splash(g_splash_pid);
                return RESTART_LOOP_EXIT_CODE;
            }

            if (decision.action == RestartAction::COOLDOWN_RETRY) {
                ++transient_cooldown_rounds;
                consecutive_transient_failures = 0;
                spdlog::warn("[Watchdog] Transient launch failures outlasted the backoff "
                             "budget; cooling down {}s before retrying (round {}/{})",
                             decision.delay_seconds, transient_cooldown_rounds,
                             helix::watchdog::TRANSIENT_MAX_COOLDOWN_ROUNDS);
            } else {
                spdlog::warn("[Watchdog] Child exited with code {} ({}, not a crash), "
                             "restarting in {}s (failure {})",
                             crash.exit_code, helix::watchdog::exec_failure_class_name(exec_class),
                             decision.delay_seconds, failure_count);
            }

            backoff_sleep(decision.delay_seconds);
            continue;
        }

        // Graceful shutdown signals (SIGTERM, SIGINT) - exit watchdog, don't treat as crash
        // These are intentional termination requests (systemctl stop, kill, Ctrl+C)
        if (crash.was_signaled && (crash.signal_num == SIGTERM || crash.signal_num == SIGINT)) {
            spdlog::info("[Watchdog] Child received {} ({}), shutting down gracefully",
                         crash.signal_num, crash.signal_name);
            break;
        }

        // Post-update restart: process crashed between marker write and _exit(0).
        // This is expected — skip the crash dialog and restart silently.
        if (was_update_restart) {
            spdlog::info("[Watchdog] Post-update restart (exit={}), skipping crash dialog",
                         crash.was_signaled ? crash.signal_num : crash.exit_code);
            continue;
        }

        // Track this crash for loop detection. The signature folds both
        // signal-killed and crash-handler-translated cases into a single
        // value so any deterministic crash (same signal repeating) trips
        // the loop branch.
        {
            int signature = crash.was_signaled ? crash.signal_num : crash.exit_code;
            auto now = std::chrono::steady_clock::now();
            const auto window = std::chrono::seconds(CRASH_LOOP_WINDOW_SEC);
            while (!recent_crashes.empty() && (now - recent_crashes.front().at) > window) {
                recent_crashes.pop_front();
            }
            recent_crashes.push_back({now, signature});
        }

        // Loop detected when we have ≥ N entries in the window AND at least
        // CRASH_LOOP_MAX_CRASHES of them share a signature. Same-signature
        // matters: alternating crashes from different bugs aren't a single
        // deterministic state, so blind retry might still help.
        bool crash_loop_detected = false;
        if (static_cast<int>(recent_crashes.size()) >= CRASH_LOOP_MAX_CRASHES) {
            int last_sig = recent_crashes.back().signature;
            int matches = 0;
            for (const auto& ev : recent_crashes) {
                if (ev.signature == last_sig)
                    ++matches;
            }
            if (matches >= CRASH_LOOP_MAX_CRASHES) {
                crash_loop_detected = true;
                spdlog::critical("[Watchdog] Crash loop detected: signature {} repeated {} times "
                                 "in the last {}s. Offering Safe Mode.",
                                 last_sig, matches, CRASH_LOOP_WINDOW_SEC);
            }
        }

        // Crash detected - show recovery dialog (no splash during dialog)
        spdlog::warn("[Watchdog] Crash detected, showing recovery dialog{}",
                     crash_loop_detected ? " (loop detected)" : "");

        DialogChoice choice =
            show_crash_dialog(args.width, args.height, args.rotation, crash, crash_loop_detected);

        if (choice == DialogChoice::RESTART_SYSTEM) {
            perform_system_restart();
            // Never returns
        }

        if (choice == DialogChoice::RESTART_SAFE_MODE) {
            if (!write_safe_mode_marker()) {
                // Couldn't write the marker anywhere. Restarting the app now
                // would just reconnect Moonraker and re-crash on the same bug,
                // so escalate to a system restart instead — the kernel umount
                // / re-mount cycle often fixes the writability issue (stuck
                // namespace, transient ENOSPC, etc., per the bundle 7ZGHW5KX
                // pattern in MEMORY.md).
                spdlog::critical("[Watchdog] Safe Mode marker write failed — falling "
                                 "back to system restart so the user isn't stranded");
                perform_system_restart();
                // Never returns
            }
            // Clear the recent_crashes deque so we don't immediately re-show
            // the loop dialog if Safe Mode itself crashes once for unrelated
            // reasons (less likely without Moonraker, but possible).
            recent_crashes.clear();
            spdlog::info("[Watchdog] Restarting helix-screen in Safe Mode");
            continue;
        }

        // RESTART_APP: loop continues, will fork new child with splash
        spdlog::info("[Watchdog] Restarting helix-screen");
    }

    // Final cleanup
    cleanup_splash(g_splash_pid);

    return 0;
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    // Set up signal handlers
    setup_signal_handlers();

    // Parse command line arguments
    WatchdogArgs args;
    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    // Initialize logging (auto-detect journal/syslog/console)
    helix::logging::LogConfig log_config;
    log_config.level = spdlog::level::info;
    log_config.target = helix::logging::LogTarget::Auto;
    log_config.enable_console = true;
    helix::logging::init(log_config);

    // Auto-detect resolution from display hardware if not overridden via CLI
    if (args.width == 0 || args.height == 0) {
        auto backend = DisplayBackend::create();
        if (backend) {
            auto res = backend->detect_resolution();
            if (res.valid) {
                args.width = res.width;
                args.height = res.height;
                spdlog::info("[Watchdog] Auto-detected resolution: {}x{}", args.width, args.height);
            }
        }
        // Fall back to defaults if detection failed
        if (args.width == 0 || args.height == 0) {
            args.width = DEFAULT_WIDTH;
            args.height = DEFAULT_HEIGHT;
            spdlog::info("[Watchdog] Using default resolution: {}x{}", args.width, args.height);
        }
    }

    args.rotation = helix::watchdog::rotation_for_launch(args.cli_rotation,
                                                         [] { return read_config_rotation(0); });
    if (args.rotation != 0) {
        spdlog::info("[Watchdog] Display rotation: {}°", args.rotation);
    }

    // Advertise our PID to an external UI supervisor that stops us by pidfile.
    // On CC1/COSMOS the platform hook sets HELIX_GUI_PIDFILE=/var/run/gui.pid so
    // the stock `gui-switcher stop` (start-stop-daemon -K) signals the watchdog —
    // the one process that supervises the whole launcher->watchdog->helix-screen
    // tree and reaps it on SIGTERM. Signalling the launcher can't (its cleanup
    // trap is deferred behind the foreground child), and signalling helix-screen
    // just makes us respawn it. Opt-in: no-op when the env var is unset.
    const char* gui_pidfile = getenv("HELIX_GUI_PIDFILE");
    if (gui_pidfile && gui_pidfile[0] != '\0') {
        std::ofstream pf(gui_pidfile, std::ios::trunc);
        if (pf) {
            pf << getpid() << "\n";
            pf.close();
            spdlog::info("[Watchdog] Wrote GUI pidfile {} = {}", gui_pidfile, getpid());
        } else {
            spdlog::warn("[Watchdog] Could not write GUI pidfile {}", gui_pidfile);
        }
    }

    // Run the watchdog
    int rc = run_watchdog(args);

    // Remove our GUI pidfile on clean exit so a stale PID can't mislead the
    // supervisor. A SIGKILL leaves it behind, but the next watchdog start
    // overwrites it and start-stop-daemon tolerates a stale pidfile.
    if (gui_pidfile && gui_pidfile[0] != '\0') {
        std::error_code ec;
        std::filesystem::remove(gui_pidfile, ec);
    }
    return rc;
}
