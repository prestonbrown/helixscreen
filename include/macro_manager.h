// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @file macro_manager.h
 * @brief HelixScreen helper macro detection and installation
 *
 * The HelixMacroManager handles detection and installation of HelixScreen-specific
 * Klipper macros that provide enhanced functionality for pre-print operations.
 *
 * ## Helix Macros
 *
 * HelixScreen provides optional helper macros that can be installed on the printer:
 *
 * | Macro | Purpose |
 * |-------|---------|
 * | HELIX_BED_MESH_IF_NEEDED | Conditional bed mesh based on mesh age |
 * | HELIX_CLEAN_NOZZLE | Standardized nozzle cleaning sequence |
 * | HELIX_START_PRINT | Unified start print with all pre-print options |
 *
 * ## Installation Process
 *
 * 1. Upload `helix_macros.cfg` to printer's config directory via Moonraker HTTP API
 * 2. Add `[include helix_macros.cfg]` to printer.cfg if not already present
 * 3. Trigger Klipper restart to load new macros
 * 4. Re-discover capabilities to confirm installation
 *
 * ## Usage
 *
 * @code
 * HelixMacroManager manager(api, capabilities);
 *
 * // Check if installation is needed
 * if (!manager.is_installed()) {
 *     // Prompt user to install
 *     manager.install(
 *         []() { spdlog::info("Macros installed successfully"); },
 *         [](const MoonrakerError& e) { spdlog::error("Install failed: {}", e.message); }
 *     );
 * }
 * @endcode
 *
 * @see PrinterDiscovery for macro detection
 * @see IMoonrakerAPI for file upload operations
 */

namespace helix {

/**
 * @brief Filename for the HelixScreen macros config file
 */
constexpr const char* HELIX_MACROS_FILENAME = "helix_macros.cfg";

/**
 * @brief Status of HelixScreen macro installation
 */
enum class MacroInstallStatus {
    NOT_INSTALLED, ///< No Helix macros detected
    INSTALLED,     ///< Current version installed
    OUTDATED,      ///< Older version installed, update available
    UNKNOWN        ///< Cannot determine (no connection)
};

/**
 * @brief Result of installation attempt
 */
struct InstallResult {
    bool success = false;
    std::string message;
    bool restart_required = false; ///< True if Klipper restart is needed
};

/**
 * @brief Manages HelixScreen helper macro installation
 *
 * Provides functionality to:
 * - Detect if Helix macros are installed
 * - Install macros via Moonraker file upload
 * - Update outdated macro versions
 * - Trigger Klipper restart after installation
 */
class MacroManager {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using StatusCallback = std::function<void(MacroInstallStatus)>;

    /**
     * @brief Construct MacroManager with API and hardware discovery references
     *
     * @param api IMoonrakerAPI for file operations
     * @param hardware PrinterDiscovery for macro detection
     */
    MacroManager(IMoonrakerAPI& api, const PrinterDiscovery& hardware);

    // Non-copyable, non-movable (holds references)
    MacroManager(const MacroManager&) = delete;
    MacroManager& operator=(const MacroManager&) = delete;
    MacroManager(MacroManager&&) = delete;
    MacroManager& operator=(MacroManager&&) = delete;
    ~MacroManager();

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Check if Helix macros are installed
     *
     * @return true if HELIX_START_PRINT or similar macro is detected
     */
    [[nodiscard]] bool is_installed() const;

    /**
     * @brief Get detailed installation status
     *
     * Checks for presence and version of Helix macros.
     *
     * @return MacroInstallStatus indicating current state
     */
    [[nodiscard]] MacroInstallStatus get_status() const;

    /**
     * @brief Get installed version string
     *
     * @return Version string if installed, empty string otherwise
     */
    [[nodiscard]] std::string get_installed_version() const;

    /**
     * @brief Check if an update is available
     *
     * Compares installed version against the local file version.
     *
     * @return true if installed version is older than local file
     */
    [[nodiscard]] bool update_available() const;

    // ========================================================================
    // Installation Operations
    // ========================================================================

    /**
     * @brief Install Helix macros to printer
     *
     * Performs the following steps:
     * 1. Upload helix_macros.cfg to config directory
     * 2. Modify printer.cfg to include helix_macros.cfg
     * 3. Request Klipper restart
     *
     * @param on_success Called when installation completes
     * @param on_error Called if installation fails
     */
    void install(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Update Helix macros to latest version
     *
     * Overwrites existing helix_macros.cfg with current version.
     * Does not modify printer.cfg include (assumed already present).
     *
     * @param on_success Called when update completes
     * @param on_error Called if update fails
     */
    void update(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Uninstall Helix macros from printer
     *
     * Removes helix_macros.cfg and the include line from printer.cfg.
     * Requires Klipper restart to take effect.
     *
     * @param on_success Called when uninstall completes
     * @param on_error Called if uninstall fails
     */
    void uninstall(SuccessCallback on_success, ErrorCallback on_error);

    // ========================================================================
    // Macro Content
    // ========================================================================

    /**
     * @brief Get the macro configuration file content
     *
     * Reads and returns the complete helix_macros.cfg content from disk.
     *
     * @return String containing complete cfg file content, or empty if not found
     */
    [[nodiscard]] static std::string get_macro_content();

    /**
     * @brief Get the version from the local macro file
     *
     * Parses the version from the file header (e.g., "# helix_macros v2.0.0").
     *
     * @return Version string (e.g., "2.0.0"), or empty if not found
     */
    [[nodiscard]] static std::string get_version();

    /**
     * @brief Get list of macro names that will be installed
     *
     * @return Vector of macro names (e.g., "HELIX_START_PRINT")
     */
    [[nodiscard]] static std::vector<std::string> get_macro_names();

  private:
    IMoonrakerAPI& api_;
    const PrinterDiscovery& hardware_;

    /// Async callback safety guard (prevents use-after-free)
    helix::AsyncLifetimeGuard lifetime_;

    /**
     * @brief Upload macro file to printer config directory
     */
    void upload_macro_file(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Add include line to printer.cfg
     */
    void add_include_to_config(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Remove include line from printer.cfg
     */
    void remove_include_from_config(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Delete macro file from printer config directory
     */
    void delete_macro_file(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Request Klipper restart
     */
    void restart_klipper(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Parse version from installed macros
     */
    [[nodiscard]] std::optional<std::string> parse_installed_version() const;
};

} // namespace helix
