// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_installer.h"

#include "config.h"
#include "helix_fs.h"
#include "helix_version.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "system/child_process.h"
#include "system/update_checker.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#if !defined(ESP_PLATFORM)
extern "C" {
extern char** environ; // the libc environment, which install.sh inherits
}
#endif

namespace hfs = helix::fs;

namespace helix {

// ============================================================================
// Process Execution Helpers
// ============================================================================

/**
 * @brief Wait result from wait_for_child_with_timeout()
 */
struct WaitResult {
    bool timed_out = false;
    bool error = false;
    bool spawn_failed = false;
    int exit_code = -1;
    std::string error_message;
    std::string output;
};

/**
 * @brief Log install.sh's output line by line.
 *
 * The script's own messages are the only record of why it failed, so a
 * failure logs them at warn; a success keeps them at debug, except its own
 * [WARN] lines, such as a config edit it could not make.
 */
static void log_script_output(const std::string& output, bool succeeded) {
    for (const auto& line : script_output_lines(output)) {
        if (succeeded && line.rfind("[WARN]", 0) != 0) {
            spdlog::debug("[PluginInstaller] install.sh: {}", line);
        } else {
            spdlog::warn("[PluginInstaller] install.sh: {}", line);
        }
    }
}

#if !defined(ESP_PLATFORM)
// install.sh prints a handful of lines; anything past this is noise.
constexpr size_t kMaxScriptOutputBytes = 16 * 1024;

/**
 * @brief Append whatever the child has written so far; fd is non-blocking.
 */
static void drain_output(int fd, std::string& out) {
    char buf[512];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < kMaxScriptOutputBytes) {
                out.append(buf,
                           std::min(static_cast<size_t>(n), kMaxScriptOutputBytes - out.size()));
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

/**
 * @brief Wait for the child, draining its output, and end it at the timeout.
 *
 * @param pid Child process ID to wait for
 * @param timeout_seconds Maximum time to wait
 * @param operation_name Name for logging ("Installation" or "Uninstallation")
 * @param out_fd Non-blocking read end of the child's stdout/stderr, drained
 *               while waiting so a full pipe can never stall the child
 * @return WaitResult with exit code, captured output, or error info
 */
static WaitResult wait_for_child_with_timeout(pid_t pid, int timeout_seconds,
                                              const char* operation_name, int out_fd) {
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    constexpr auto kTermGrace = std::chrono::milliseconds(500);
    WaitResult result;

    const ChildWaitResult wait = terminate_and_reap(
        pid, std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds), kTermGrace,
        kPollInterval, [&] {
            drain_output(out_fd, result.output);
            return true;
        });
    drain_output(out_fd, result.output);

    switch (wait.outcome) {
    case ChildWaitOutcome::Exited:
        result.exit_code = wait.exit_code();
        break;
    case ChildWaitOutcome::TimedOut:
    case ChildWaitOutcome::Cancelled:
        spdlog::error("[PluginInstaller] {} timed out after {} seconds", operation_name,
                      timeout_seconds);
        result.timed_out = true;
        break;
    case ChildWaitOutcome::WaitFailed:
        result.error = true;
        result.error_message = strerror(wait.wait_errno);
        spdlog::error("[PluginInstaller] waitpid error: {}", result.error_message);
        break;
    }
    return result;
}

#endif // !ESP_PLATFORM

/**
 * @brief Run install.sh in the given mode, capturing and logging its output.
 *
 * fork/exec instead of popen(): sh receives the script as a path argument,
 * not a command string, so nothing inside script_path is interpreted.
 * Running it through /bin/sh also works when the file has no exec bit:
 * Moonraker's type:web self-update extracts the release zip with Python's
 * zipfile, which does not restore unix modes. --no-restart, unless
 * @p no_restart is false: the caller restarts Moonraker over its API. An
 * inherited HELIX_JOB_STATE is left out of the script's environment: the
 * script asks Moonraker for the print state itself.
 */
static WaitResult run_install_script(const std::string& script_path, const char* mode_flag,
                                     int timeout_seconds, const char* operation_name,
                                     bool no_restart = true) {
    WaitResult result;
#if defined(ESP_PLATFORM)
    // No shell and no processes to spawn: the firmware cannot run install.sh.
    (void)script_path;
    (void)mode_flag;
    (void)timeout_seconds;
    (void)operation_name;
    (void)no_restart;
    result.error = result.spawn_failed = true;
    result.error_message = "not supported on this device";
    return result;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        result.error = result.spawn_failed = true;
        result.error_message = strerror(errno);
        spdlog::error("[PluginInstaller] pipe failed: {}", result.error_message);
        return result;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    // Built before the fork: the child of a threaded process may not allocate.
    constexpr std::string_view kJobStateVar = "HELIX_JOB_STATE=";
    std::vector<char*> envp;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        if (std::string_view(*e).substr(0, kJobStateVar.size()) != kJobStateVar) {
            envp.push_back(*e);
        }
    }
    envp.push_back(nullptr);
    std::array<const char*, 5> argv = {"sh", script_path.c_str(), mode_flag, nullptr, nullptr};
    if (no_restart) {
        argv = {"sh", script_path.c_str(), "--no-restart", mode_flag, nullptr};
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = result.spawn_failed = true;
        result.error_message = strerror(errno);
        spdlog::error("[PluginInstaller] Fork failed: {}", result.error_message);
        close(fds[0]);
        close(fds[1]);
        return result;
    }

    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execve("/bin/sh", const_cast<char* const*>(argv.data()), envp.data());
        _exit(127);
    }

    close(fds[1]);
    result = wait_for_child_with_timeout(pid, timeout_seconds, operation_name, fds[0]);
    close(fds[0]);
    return result;
#endif
}

std::vector<std::string> script_output_lines(const std::string& output) {
    std::vector<std::string> lines;
    std::string line;
    auto flush = [&] {
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            lines.push_back(line);
        }
        line.clear();
    };
    for (size_t i = 0; i < output.size(); ++i) {
        const char c = output[i];
        if (c == '\033') {
            // A CSI sequence: ESC [ parameters, then a final byte in @..~
            if (i + 1 < output.size() && output[i + 1] == '[') {
                i += 2;
                while (i < output.size() && (output[i] < '@' || output[i] > '~')) {
                    ++i;
                }
            }
            continue;
        }
        if (c == '\n') {
            flush();
        } else {
            line.push_back(c);
        }
    }
    flush();
    return lines;
}

// ============================================================================
// HelixPluginInstaller Implementation
// ============================================================================

void HelixPluginInstaller::set_api(IMoonrakerAPI* api) {
    api_ = api;

    // Try to get WebSocket URL from API's client
    if (api_ && api_->get_connection_state() == ConnectionState::CONNECTED) {
        websocket_url_ = api_->get_websocket_url();
    }
}

void HelixPluginInstaller::set_websocket_url(const std::string& url) {
    websocket_url_ = url;
}

bool HelixPluginInstaller::is_local_moonraker() const {
    if (websocket_url_.empty()) {
        return false;
    }

    // Canonical predicate (own hostname + interface IPs, not just loopback
    // literals) — installing into a printer whose address resolves to THIS
    // machine is exactly the local case.
    //
    // A URL that fails to parse yields an empty host, and is_loopback_literal()
    // reads empty as loopback — which would call a remote printer local. A parse
    // failure is not evidence of locality, so it is rejected here rather than in
    // the shared predicate, where an unset host does legitimately mean local.
    const std::string host = extract_host_from_websocket_url(websocket_url_);
    if (host.empty()) {
        return false;
    }
    return is_moonraker_on_same_host(host);
}

HelixPluginInstaller::SyncInstallResult HelixPluginInstaller::install_local_sync() {
    // NOTE: This method is designed to be called from a background thread.
    // It does NOT use std::function to avoid ARM/glibc static linking issues.

    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-install on remote Moonraker");
        return {false, lv_tr("Auto-install only works on local Moonraker")};
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        return {false, lv_tr("Install script not found. Use the remote install command instead.")};
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info("[PluginInstaller] Starting local installation: {} --no-restart --auto",
                 script_path);

    WaitResult result =
        run_install_script(script_path, "--auto", script_timeout_seconds_, "Installation");
    log_script_output(result.output, !result.timed_out && !result.error && result.exit_code == 0);

    if (result.spawn_failed) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Failed to start installer: ") + result.error_message};
    }

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Installation timed out. The script may be stuck.")};
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        return {false, lv_tr("Installation failed: ") + result.error_message};
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Installation completed successfully");
        return {true, lv_tr("Plugin installed successfully.")};
    }

    state_.store(PluginInstallState::FAILED);
    spdlog::error("[PluginInstaller] Installation failed (exit code {})", result.exit_code);
    return {false, lv_tr("Installation failed. Check logs for details.")};
}

namespace {
// install.sh's --uninstall / --uninstall-auto exit code for "the plugin was
// removed, but a config file was skipped or failed during the phase-tracking
// strip" (moonraker-plugin/install.sh, moonraker-plugin/strip_phase_tracking.py).
constexpr int kUninstallExitNeedsAttention = 2;
} // namespace

void HelixPluginInstaller::uninstall_local(UninstallCallback callback) {
    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-uninstall on remote Moonraker");
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Auto-uninstall only works on local Moonraker"));
        }
        return;
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        if (callback) {
            callback(UninstallOutcome::FAILED, lv_tr("Uninstall script not found."));
        }
        return;
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info(
        "[PluginInstaller] Starting local uninstallation: {} --no-restart --uninstall-auto",
        script_path);

    WaitResult result = run_install_script(script_path, "--uninstall-auto", script_timeout_seconds_,
                                           "Uninstallation");
    log_script_output(result.output, !result.timed_out && !result.error && result.exit_code == 0);

    if (result.spawn_failed) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Failed to start uninstaller: ") + result.error_message);
        }
        return;
    }

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation timed out. The script may be stuck."));
        }
        return;
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation failed: ") + result.error_message);
        }
        return;
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Uninstallation completed successfully");
        if (callback) {
            callback(UninstallOutcome::SUCCESS, lv_tr("Plugin uninstalled successfully."));
        }
    } else if (result.exit_code == kUninstallExitNeedsAttention) {
        state_.store(PluginInstallState::NEEDS_ATTENTION);
        spdlog::warn("[PluginInstaller] Uninstallation removed the plugin but left a config "
                     "file needing attention (exit code {})",
                     result.exit_code);
        if (callback) {
            callback(UninstallOutcome::NEEDS_ATTENTION,
                     lv_tr("Plugin removed. A config file needs attention - check the "
                           "HelixScreen log for details."));
        }
    } else {
        state_.store(PluginInstallState::FAILED);
        spdlog::error("[PluginInstaller] Uninstallation failed (exit code {})", result.exit_code);
        if (callback) {
            callback(UninstallOutcome::FAILED,
                     lv_tr("Uninstallation failed. Check logs for details."));
        }
    }
}

namespace {
// install.sh --relink exit codes (moonraker-plugin/install.sh, EXIT_RELINK_*).
constexpr int kRelinkExitCurrent = 0;
constexpr int kRelinkExitDone = 3;
constexpr int kRelinkExitNotOurs = 4;
} // namespace

PluginLinkCheck relink_plugin(const std::string& script_path, int timeout_seconds) {
    if (script_path.empty()) {
        return PluginLinkCheck::Failed;
    }
    const WaitResult result =
        run_install_script(script_path, "--relink", timeout_seconds, "Plugin relink");
    if (result.spawn_failed || result.timed_out || result.error) {
        log_script_output(result.output, false);
        return PluginLinkCheck::Failed;
    }
    switch (result.exit_code) {
    case kRelinkExitCurrent:
        log_script_output(result.output, true);
        return PluginLinkCheck::Current;
    case kRelinkExitDone:
        log_script_output(result.output, true);
        return PluginLinkCheck::Relinked;
    case kRelinkExitNotOurs:
        log_script_output(result.output, true);
        return PluginLinkCheck::NotOurs;
    default:
        log_script_output(result.output, false);
        return PluginLinkCheck::Failed;
    }
}

namespace {
// install.sh --restart-moonraker exit codes (moonraker_restart in
// moonraker-plugin/helix_print_conf.sh).
constexpr int kRestartExitDone = 0;
constexpr int kRestartExitManual = 3;
constexpr int kRestartExitHeldForPrint = 4;
constexpr int kRestartExitJobUnknown = 6;
constexpr int kRestartExitNotAnswering = 7;
} // namespace

PluginRestartOutcome restart_moonraker_for_plugin(const std::string& script_path,
                                                  int timeout_seconds) {
    if (script_path.empty()) {
        return PluginRestartOutcome::Failed;
    }
    const WaitResult result = run_install_script(script_path, "--restart-moonraker",
                                                 timeout_seconds, "Moonraker restart", false);
    if (result.timed_out) {
        log_script_output(result.output, false);
        return PluginRestartOutcome::TimedOut;
    }
    if (result.spawn_failed || result.error) {
        log_script_output(result.output, false);
        return PluginRestartOutcome::Failed;
    }
    switch (result.exit_code) {
    case kRestartExitDone:
        log_script_output(result.output, true);
        return PluginRestartOutcome::Restarted;
    case kRestartExitManual:
        log_script_output(result.output, false);
        return PluginRestartOutcome::NeedsManual;
    case kRestartExitHeldForPrint:
        log_script_output(result.output, true);
        return PluginRestartOutcome::HeldForPrint;
    case kRestartExitJobUnknown:
        log_script_output(result.output, false);
        return PluginRestartOutcome::JobUnknown;
    case kRestartExitNotAnswering:
        log_script_output(result.output, false);
        return PluginRestartOutcome::NotAnswering;
    default:
        log_script_output(result.output, false);
        return PluginRestartOutcome::Failed;
    }
}

bool plugin_restart_finished(PluginRestartOutcome outcome, bool plugin_was_loaded) {
    return outcome == PluginRestartOutcome::Restarted ||
           (outcome == PluginRestartOutcome::NeedsManual && plugin_was_loaded);
}

std::string parse_plugin_version(std::string_view text) {
    constexpr std::string_view kPrefix = "PLUGIN_VERSION = \"";
    size_t at = 0;
    while ((at = text.find(kPrefix, at)) != std::string_view::npos) {
        if (at == 0 || text[at - 1] == '\n') {
            const size_t start = at + kPrefix.size();
            const size_t end = text.find('"', start);
            const size_t eol = text.find('\n', start);
            if (end != std::string_view::npos && end < eol) {
                return std::string(text.substr(start, end - start));
            }
        }
        at += kPrefix.size();
    }
    return "";
}

bool plugin_versions_differ(const PluginSelfHealFacts& facts) {
    return !facts.reported_version.empty() && !facts.bundled_version.empty() &&
           facts.reported_version != facts.bundled_version;
}

PluginSelfHealStep decide_plugin_self_heal(const PluginSelfHealFacts& facts) {
    const bool version_differs = plugin_versions_differ(facts);
    if (!facts.local) {
        return version_differs ? PluginSelfHealStep::ReportOnly : PluginSelfHealStep::Nothing;
    }
    if (facts.link == PluginLinkCheck::Current && version_differs &&
        facts.memory.restarted_for_bundled == facts.bundled_version &&
        facts.memory.restarted_for_reported == facts.reported_version) {
        return PluginSelfHealStep::GiveUp;
    }
    if (facts.link == PluginLinkCheck::Relinked && !facts.plugin_file.empty() &&
        facts.memory.relinked_for == facts.plugin_file) {
        return PluginSelfHealStep::GiveUp;
    }
    const bool needs_restart = facts.link == PluginLinkCheck::Relinked ||
                               (facts.link == PluginLinkCheck::Current && version_differs);
    if (!needs_restart) {
        return facts.link == PluginLinkCheck::NotOurs && version_differs
                   ? PluginSelfHealStep::ReportOnly
                   : PluginSelfHealStep::Nothing;
    }
    return facts.job_holds ? PluginSelfHealStep::WaitForJob : PluginSelfHealStep::RestartNow;
}

PluginSelfHealMemory remember_plugin_self_heal(const PluginSelfHealFacts& facts,
                                               bool restart_finished) {
    PluginSelfHealMemory memory = facts.memory;
    const bool version_differs = plugin_versions_differ(facts);
    if (restart_finished) {
        if (version_differs) {
            memory.restarted_for_bundled = facts.bundled_version;
            memory.restarted_for_reported = facts.reported_version;
        }
        if (facts.link == PluginLinkCheck::Relinked) {
            memory.relinked_for = facts.plugin_file;
        }
    } else if (!version_differs) {
        memory.restarted_for_bundled.clear();
        memory.restarted_for_reported.clear();
    }
    if (facts.link == PluginLinkCheck::Current) {
        memory.relinked_for.clear();
    }
    return memory;
}

std::string HelixPluginInstaller::bundled_plugin_file() const {
    const std::string script = get_install_script_path();
    if (script.empty()) {
        return "";
    }
    return std::string(hfs::parent_path(script)) + "/helix_print.py";
}

std::string HelixPluginInstaller::bundled_plugin_version() const {
    const std::string plugin = bundled_plugin_file();
    if (plugin.empty()) {
        return "";
    }
    // PLUGIN_VERSION sits near the top; the rest of the file is not needed.
    const auto text = text_io::read_file(plugin, 16 * 1024);
    if (!text) {
        spdlog::warn("[PluginInstaller] Cannot read {}", plugin);
        return "";
    }
    return parse_plugin_version(*text);
}

std::string plugin_release_ref(std::string_view version) {
    if (version.empty() || version.front() < '0' || version.front() > '9') {
        return "main";
    }
    return "v" + std::string(version.substr(0, version.find('+')));
}

std::string plugin_remote_install_command(std::string_view ref) {
    // -f: an HTTP error pipes nothing into sh rather than an error page.
    const std::string r(ref);
    if (r == "main") {
        return "curl -fsSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/"
               "moonraker-plugin/remote-install.sh | sh -s -- main";
    }
    return std::string("curl -fsSL ") + UpdateChecker::kDefaultR2BaseUrl +
           "/plugin/remote-install.sh | sh -s -- " + r;
}

std::string HelixPluginInstaller::get_remote_install_command() const {
    return plugin_remote_install_command(plugin_release_ref(helix_version()));
}

std::string HelixPluginInstaller::get_install_script_path() const {
    std::vector<std::string> search_paths;
    std::string exe_dir;

    if (!script_search_root_.empty()) {
        exe_dir = script_search_root_;
        search_paths = {script_search_root_ + "/moonraker-plugin/install.sh"};
    } else {
#ifdef __APPLE__
        char path[1024];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0) {
            exe_dir = std::string(hfs::parent_path(path));
        }
#else
        // Linux: read /proc/self/exe
        char path[1024];
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (len != -1) {
            path[len] = '\0';
            exe_dir = std::string(hfs::parent_path(path));
        }
#endif

        if (exe_dir.empty()) {
            return "";
        }

        // Try common locations relative to executable
        search_paths = {exe_dir + "/moonraker-plugin/install.sh",    // Deployed alongside binary
                        exe_dir + "/../moonraker-plugin/install.sh", // Development build
                        exe_dir + "/../../moonraker-plugin/install.sh",
                        exe_dir + "/../share/helix/moonraker-plugin/install.sh", // Installed
                        "/usr/share/helix/moonraker-plugin/install.sh"};
    }

    for (const auto& candidate : search_paths) {
        if (!hfs::exists(candidate)) {
            continue;
        }
        // canonical() resolves symlinks and normalizes the path; nullopt when
        // the path is inaccessible
        auto canonical_path = hfs::canonical(candidate);
        if (!canonical_path) {
            spdlog::warn("[PluginInstaller] Failed to resolve {}: {}", candidate,
                         std::strerror(errno));
            continue;
        }

        // Security check: ensure the resolved path still points to install.sh
        // This prevents symlink attacks where a malicious symlink points elsewhere
        if (hfs::filename(*canonical_path) != "install.sh") {
            spdlog::warn("[PluginInstaller] Skipping {} - resolved to unexpected file: {}",
                         candidate, *canonical_path);
            continue;
        }

        // No exec-bit check: the script runs via /bin/sh, and a release
        // unpacked by Moonraker's zip extractor carries no unix modes.
        return *canonical_path;
    }

    spdlog::warn("[PluginInstaller] install.sh not found below exe dir {}", exe_dir);
    return "";
}

bool HelixPluginInstaller::should_prompt_install() const {
    Config* config = Config::get_instance();

    // Gate plugin install prompt behind beta_features flag
    if (!config->is_beta_features_enabled()) {
        spdlog::debug("[PluginInstaller] Beta features disabled, skipping plugin prompt");
        return false;
    }

    // Don't show plugin prompt until the first-run wizard is complete.
    // The prompt will be triggered when wizard completes and connects to Moonraker,
    // which fires PrintSelectPanel's connection observer.
    if (config->is_wizard_required()) {
        spdlog::debug("[PluginInstaller] Wizard not complete, deferring plugin prompt");
        return false;
    }

    if (prompt_settled_for_session_) {
        spdlog::debug("[PluginInstaller] Plugin prompt already settled this session");
        return false;
    }

    // If user previously declined, don't prompt
    bool declined = config->get<bool>(kPrefInstallDeclined, false);
    return !declined;
}

bool HelixPluginInstaller::prompt_settled_for_session_ = false;

void HelixPluginInstaller::settle_prompt_for_session() {
    prompt_settled_for_session_ = true;
}

void HelixPluginInstaller::set_install_declined() {
    Config* config = Config::get_instance();
    config->set<bool>(kPrefInstallDeclined, true);
    config->save();
    spdlog::debug("[PluginInstaller] User declined plugin install prompt");
}

PluginInstallState HelixPluginInstaller::get_state() const {
    return state_.load();
}

bool HelixPluginInstaller::is_installing() const {
    return state_.load() == PluginInstallState::INSTALLING;
}

} // namespace helix
