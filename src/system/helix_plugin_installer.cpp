// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_plugin_installer.h"

#include "config.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace helix {

// ============================================================================
// URL Parsing Utilities
// ============================================================================

std::string extract_host_from_websocket_url(const std::string& url) {
    // Expected format: ws://host:port/websocket or wss://host:port/websocket
    // or: ws://[ipv6]:port/websocket

    if (url.empty()) {
        return "";
    }

    std::string remainder;

    // Check for ws:// or wss:// prefix
    const std::string ws_prefix = "ws://";
    const std::string wss_prefix = "wss://";

    if (url.find(ws_prefix) == 0) {
        remainder = url.substr(ws_prefix.length());
    } else if (url.find(wss_prefix) == 0) {
        remainder = url.substr(wss_prefix.length());
    } else {
        return ""; // Unknown scheme
    }

    // Handle IPv6 addresses in brackets [::1]
    if (!remainder.empty() && remainder[0] == '[') {
        auto close_bracket = remainder.find(']');
        if (close_bracket != std::string::npos) {
            // Return content between brackets (the IPv6 address)
            return remainder.substr(1, close_bracket - 1);
        }
        return ""; // Malformed IPv6
    }

    // Find the port separator
    auto colon_pos = remainder.find(':');
    if (colon_pos == std::string::npos) {
        // No port - find the path separator
        auto slash_pos = remainder.find('/');
        if (slash_pos != std::string::npos) {
            return remainder.substr(0, slash_pos);
        }
        return remainder; // Just hostname
    }

    // Return everything before the colon (the host)
    return remainder.substr(0, colon_pos);
}

// ============================================================================
// Process Execution Helpers
// ============================================================================

/**
 * @brief Wait result from wait_for_child_with_timeout()
 */
struct WaitResult {
    bool timed_out = false;
    bool error = false;
    int exit_code = -1;
    std::string error_message;
};

/**
 * @brief Wait for child process with timeout, handling EINTR
 *
 * Uses non-blocking waitpid with polling to implement timeout.
 * Properly handles EINTR interruptions from signals.
 *
 * @param pid Child process ID to wait for
 * @param timeout_seconds Maximum time to wait
 * @param operation_name Name for logging ("Installation" or "Uninstallation")
 * @return WaitResult with exit code or error info
 */
static WaitResult wait_for_child_with_timeout(pid_t pid, int timeout_seconds,
                                              const char* operation_name) {
    constexpr int POLL_INTERVAL_MS = 100;
    WaitResult result;

    int status = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        pid_t wait_result = waitpid(pid, &status, WNOHANG);

        if (wait_result == pid) {
            // Child exited
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return result;
        }

        if (wait_result < 0) {
            // Handle EINTR - signal interrupted waitpid, just retry
            if (errno == EINTR) {
                continue;
            }
            // Actual error
            result.error = true;
            result.error_message = strerror(errno);
            spdlog::error("[PluginInstaller] waitpid error: {}", result.error_message);
            return result;
        }

        // Check timeout
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > std::chrono::seconds(timeout_seconds)) {
            spdlog::error("[PluginInstaller] {} timed out after {} seconds", operation_name,
                          timeout_seconds);
            // Kill the child process
            kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            kill(pid, SIGKILL); // Force kill if still running

            // Reap the zombie (blocking, but child should be dead)
            waitpid(pid, &status, 0);

            result.timed_out = true;
            return result;
        }

        // Sleep before next poll
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }
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

    std::string host = extract_host_from_websocket_url(websocket_url_);
    if (host.empty()) {
        return false;
    }
    return is_moonraker_on_same_host(host);
}

HelixPluginInstaller::SyncInstallResult
HelixPluginInstaller::install_local_sync(bool enable_phase_tracking) {
    // NOTE: This method is designed to be called from a background thread.
    // It does NOT use std::function to avoid ARM/glibc static linking issues.

    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-install on remote Moonraker");
        return {false, lv_tr("Auto-install only works on local Moonraker")};
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        spdlog::warn("[PluginInstaller] Install script not found");
        return {false, lv_tr("Install script not found. Use the remote install command instead.")};
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info("[PluginInstaller] Starting local installation: {} --auto (phase_tracking={})",
                 script_path, enable_phase_tracking);

    pid_t pid = fork();

    if (pid < 0) {
        state_.store(PluginInstallState::FAILED);
        std::string err_msg = strerror(errno);
        spdlog::error("[PluginInstaller] Fork failed: {}", err_msg);
        return {false, lv_tr("Failed to start installer: ") + err_msg};
    }

    if (pid == 0) {
        if (enable_phase_tracking) {
            execl(script_path.c_str(), script_path.c_str(), "--auto", "--with-phase-tracking",
                  nullptr);
        } else {
            execl(script_path.c_str(), script_path.c_str(), "--auto", nullptr);
        }
        _exit(127);
    }

    constexpr int INSTALL_TIMEOUT_SECONDS = 60;
    WaitResult result = wait_for_child_with_timeout(pid, INSTALL_TIMEOUT_SECONDS, "Installation");

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
        return {true, lv_tr("Plugin installed successfully. Moonraker is restarting...")};
    }

    state_.store(PluginInstallState::FAILED);
    spdlog::error("[PluginInstaller] Installation failed (exit code {})", result.exit_code);
    return {false, lv_tr("Installation failed. Check logs for details.")};
}

void HelixPluginInstaller::install_local(InstallCallback callback, bool enable_phase_tracking) {
    // NOTE: Thread safety - this method must be called from the main thread only.
    // The state_ member is not protected by a mutex for performance reasons.

    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-install on remote Moonraker");
        if (callback) {
            callback(false, lv_tr("Auto-install only works on local Moonraker"));
        }
        return;
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        spdlog::warn("[PluginInstaller] Install script not found");
        if (callback) {
            callback(false,
                     lv_tr("Install script not found. Use the remote install command instead."));
        }
        return;
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info("[PluginInstaller] Starting local installation: {} --auto (phase_tracking={})",
                 script_path, enable_phase_tracking);

    // Use fork/exec instead of popen() to avoid shell command injection.
    // The script path is passed directly to execl() without shell interpretation.
    // NOTE: This is a blocking call. For production UI, consider std::async.

    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        state_.store(PluginInstallState::FAILED);
        std::string err_msg = strerror(errno);
        spdlog::error("[PluginInstaller] Fork failed: {}", err_msg);
        if (callback) {
            callback(false, lv_tr("Failed to start installer: ") + err_msg);
        }
        return;
    }

    if (pid == 0) {
        // Child process - execute the script
        // Note: execl() does NOT go through shell, preventing command injection
        if (enable_phase_tracking) {
            execl(script_path.c_str(), script_path.c_str(), "--auto", "--with-phase-tracking",
                  nullptr);
        } else {
            execl(script_path.c_str(), script_path.c_str(), "--auto", nullptr);
        }

        // If execl returns, it failed
        _exit(127);
    }

    // Parent process - wait for child with timeout
    constexpr int INSTALL_TIMEOUT_SECONDS = 60;
    WaitResult result = wait_for_child_with_timeout(pid, INSTALL_TIMEOUT_SECONDS, "Installation");

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(false, lv_tr("Installation timed out. The script may be stuck."));
        }
        return;
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(false, lv_tr("Installation failed: ") + result.error_message);
        }
        return;
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Installation completed successfully");
        if (callback) {
            callback(true, lv_tr("Plugin installed successfully. Moonraker is restarting..."));
        }
    } else {
        state_.store(PluginInstallState::FAILED);
        spdlog::error("[PluginInstaller] Installation failed (exit code {})", result.exit_code);
        if (callback) {
            callback(false, lv_tr("Installation failed. Check logs for details."));
        }
    }
}

void HelixPluginInstaller::uninstall_local(InstallCallback callback) {
    if (!is_local_moonraker()) {
        spdlog::warn("[PluginInstaller] Cannot auto-uninstall on remote Moonraker");
        if (callback) {
            callback(false, lv_tr("Auto-uninstall only works on local Moonraker"));
        }
        return;
    }

    std::string script_path = get_install_script_path();
    if (script_path.empty()) {
        spdlog::warn("[PluginInstaller] Install script not found for uninstall");
        if (callback) {
            callback(false, lv_tr("Uninstall script not found."));
        }
        return;
    }

    state_.store(PluginInstallState::INSTALLING);
    spdlog::info("[PluginInstaller] Starting local uninstallation: {} --uninstall-auto",
                 script_path);

    pid_t pid = fork();

    if (pid < 0) {
        state_.store(PluginInstallState::FAILED);
        std::string err_msg = strerror(errno);
        spdlog::error("[PluginInstaller] Fork failed: {}", err_msg);
        if (callback) {
            callback(false, lv_tr("Failed to start uninstaller: ") + err_msg);
        }
        return;
    }

    if (pid == 0) {
        execl(script_path.c_str(), script_path.c_str(), "--uninstall-auto", nullptr);
        _exit(127);
    }

    // Parent process - wait for child with timeout
    constexpr int UNINSTALL_TIMEOUT_SECONDS = 60;
    WaitResult result =
        wait_for_child_with_timeout(pid, UNINSTALL_TIMEOUT_SECONDS, "Uninstallation");

    if (result.timed_out) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(false, lv_tr("Uninstallation timed out. The script may be stuck."));
        }
        return;
    }

    if (result.error) {
        state_.store(PluginInstallState::FAILED);
        if (callback) {
            callback(false, lv_tr("Uninstallation failed: ") + result.error_message);
        }
        return;
    }

    if (result.exit_code == 0) {
        state_.store(PluginInstallState::SUCCESS);
        spdlog::info("[PluginInstaller] Uninstallation completed successfully");
        if (callback) {
            callback(true, lv_tr("Plugin uninstalled successfully."));
        }
    } else {
        state_.store(PluginInstallState::FAILED);
        spdlog::error("[PluginInstaller] Uninstallation failed (exit code {})", result.exit_code);
        if (callback) {
            callback(false, lv_tr("Uninstallation failed. Check logs for details."));
        }
    }
}

std::string HelixPluginInstaller::get_remote_install_command() const {
    return std::string("curl -sSL ") + REMOTE_INSTALL_URL + " | bash";
}

std::string HelixPluginInstaller::get_install_script_path() const {
    // Try to find install.sh relative to the executable

    // Get executable path
    std::string exe_dir;

#ifdef __APPLE__
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::filesystem::path exe_path(path);
        exe_dir = exe_path.parent_path().string();
    }
#else
    // Linux: read /proc/self/exe
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        std::filesystem::path exe_path(path);
        exe_dir = exe_path.parent_path().string();
    }
#endif

    if (exe_dir.empty()) {
        return "";
    }

    // Try common locations relative to executable
    std::vector<std::string> search_paths = {
        exe_dir + "/moonraker-plugin/install.sh",    // Deployed alongside binary
        exe_dir + "/../moonraker-plugin/install.sh", // Development build
        exe_dir + "/../../moonraker-plugin/install.sh",
        exe_dir + "/../share/helix/moonraker-plugin/install.sh", // Installed
        "/usr/share/helix/moonraker-plugin/install.sh"};

    for (const auto& candidate : search_paths) {
        try {
            std::filesystem::path p(candidate);
            if (std::filesystem::exists(p)) {
                // canonical() resolves symlinks and normalizes the path
                // It can throw filesystem_error if the path doesn't exist or is inaccessible
                auto canonical_path = std::filesystem::canonical(p);

                // Security check: ensure the resolved path still points to install.sh
                // This prevents symlink attacks where a malicious symlink points elsewhere
                if (canonical_path.filename() != "install.sh") {
                    spdlog::warn("[PluginInstaller] Skipping {} - resolved to unexpected file: {}",
                                 candidate, canonical_path.string());
                    continue;
                }

                // Check if script is executable
                auto status = std::filesystem::status(canonical_path);
                auto perms = status.permissions();
                if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
                    spdlog::warn("[PluginInstaller] Script not executable: {}",
                                 canonical_path.string());
                    continue;
                }

                return canonical_path.string();
            }
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::warn("[PluginInstaller] Failed to resolve {}: {}", candidate, e.what());
            continue;
        }
    }

    return "";
}

bool HelixPluginInstaller::should_prompt_install() const {
    Config* config = Config::get_instance();
    if (!config) {
        return false; // Don't prompt if config not available
    }

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

    // If user previously declined, don't prompt
    bool declined = config->get<bool>(PREF_INSTALL_DECLINED, false);
    return !declined;
}

void HelixPluginInstaller::set_install_declined() {
    Config* config = Config::get_instance();
    if (config) {
        config->set<bool>(PREF_INSTALL_DECLINED, true);
        config->save();
        spdlog::debug("[PluginInstaller] User declined plugin install prompt");
    }
}

PluginInstallState HelixPluginInstaller::get_state() const {
    return state_.load();
}

bool HelixPluginInstaller::is_installing() const {
    return state_.load() == PluginInstallState::INSTALLING;
}

} // namespace helix
