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

// Forward declaration
class IMoonrakerAPI;

namespace helix {

// ============================================================================
// URL Parsing Utilities (exposed for testing)
// ============================================================================

/**
 * @brief Check if a hostname represents localhost
 *
 * @param host Hostname to check (e.g., "localhost", "127.0.0.1", "::1")
 * @return true if the host is localhost
 */

/**
 * @brief Extract hostname from a WebSocket URL
 *
 * @param url WebSocket URL (e.g., "ws://192.168.1.100:7125/websocket")
 * @return Extracted hostname, or empty string if parsing fails
 */
[[nodiscard]] std::string extract_host_from_websocket_url(const std::string& url);

// ============================================================================
// Plugin Install State
// ============================================================================

/**
 * @brief State of the plugin installation process
 */
enum class PluginInstallState {
    IDLE,       ///< No installation in progress
    INSTALLING, ///< Installation is running
    SUCCESS,    ///< Installation completed successfully
    FAILED      ///< Installation failed
};

// ============================================================================
// HelixPluginInstaller Class
// ============================================================================

/**
 * @brief Manages helix_print plugin detection and installation
 *
 * @note Thread Safety:
 *       - install_local() / uninstall_local(): MUST be called from main thread only.
 *         These methods block during script execution. For non-blocking UI,
 *         wrap in std::thread or std::async.
 *       - get_state() / is_installing(): Thread-safe (atomic read).
 *       - All other methods: Main thread only.
 *
 * Usage:
 *   1. Create installer and set API
 *   2. Check if plugin is missing: !PrinterState::service_has_helix_plugin()
 *   3. Check if should prompt: installer.should_prompt_install()
 *   4. For local: installer.install_local(callback)
 *   5. For remote: show dialog with installer.get_remote_install_command()
 */
class HelixPluginInstaller {
  public:
    using InstallCallback = std::function<void(bool success, const std::string& message)>;

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
     * @param enable_phase_tracking If true, also installs phase tracking macros
     * @return Result with success flag and message
     */
    [[nodiscard]] SyncInstallResult install_local_sync(bool enable_phase_tracking = false);

    /**
     * @brief Attempt local auto-installation
     *
     * Runs the bundled install.sh script with --auto flag.
     * Only works when connected to local Moonraker.
     *
     * @param callback Called when installation completes or fails
     * @param enable_phase_tracking If true, also installs phase tracking macros
     */
    void install_local(InstallCallback callback, bool enable_phase_tracking = false);

    /**
     * @brief Attempt local auto-uninstallation
     *
     * Runs the bundled install.sh script with --uninstall flag.
     * Only works when connected to local Moonraker.
     *
     * @param callback Called when uninstallation completes or fails
     */
    void uninstall_local(InstallCallback callback);

    /**
     * @brief Get the curl command for remote installation
     *
     * Returns the one-liner curl command that users can copy and run
     * via SSH on their printer.
     *
     * @return Curl command string
     */
    [[nodiscard]] std::string get_remote_install_command() const;

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
     * Returns false if user previously checked "don't ask again".
     *
     * @return true if we should show the install prompt
     */
    [[nodiscard]] bool should_prompt_install() const;

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
    IMoonrakerAPI* api_ = nullptr;
    std::string websocket_url_;
    std::atomic<PluginInstallState> state_{PluginInstallState::IDLE};

    // Config key for "don't ask again" preference
    static constexpr const char* PREF_INSTALL_DECLINED = "/plugin_install_declined";

    // Remote install URL
    static constexpr const char* REMOTE_INSTALL_URL =
        "https://raw.githubusercontent.com/prestonbrown/helixscreen/main/moonraker-plugin/"
        "remote-install.sh";
};

} // namespace helix
