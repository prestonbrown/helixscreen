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
 * 2. Back up the original `printer.cfg` to a timestamped sibling
 * 3. Add `[include helix_macros.cfg]` to printer.cfg if not already present
 * 4. Restart Klipper to load the new macros (caller-owned — never during a print)
 * 5. Re-discover capabilities to confirm installation
 *
 * File staging and the Klipper restart are separate operations on purpose: the
 * restart must never fire while a print is active, so the caller re-checks
 * print activity between install_files()/update_files() and request_restart().
 *
 * ## Usage
 *
 * @code
 * HelixMacroManager manager(api, capabilities);
 *
 * // Check if installation is needed
 * if (manager.get_status() == MacroInstallStatus::NOT_INSTALLED) {
 *     // Prompt user to install
 *     manager.install_files(
 *         []() { spdlog::info("Macro files staged"); },
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
constexpr const char* kHelixMacrosFilename = "helix_macros.cfg";

/**
 * @brief Status of HelixScreen macro installation
 */
enum class MacroInstallStatus {
    NOT_INSTALLED, ///< No Helix macros detected
    INSTALLED,     ///< Current version installed
    OUTDATED,      ///< Older version installed, update available
    UNKNOWN        ///< Cannot determine (no objects list from the printer yet)
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
     * Checks for presence and version of Helix macros. UNKNOWN until the
     * printer has reported an objects list — before that, absence of the
     * macros means nothing.
     *
     * @return MacroInstallStatus indicating current state
     */
    [[nodiscard]] MacroInstallStatus get_status() const;

    /**
     * @brief Evaluate installation status from a discovery snapshot
     *
     * Same ladder as get_status(), usable without a MacroManager instance
     * (discovery folding in PrinterState calls this once per scan).
     *
     * @return UNKNOWN when the discovery consumed no objects list; otherwise
     *         NOT_INSTALLED / INSTALLED / OUTDATED from the version ladder.
     */
    [[nodiscard]] static MacroInstallStatus evaluate_status(const PrinterDiscovery& hardware);

    /**
     * @brief Compare two dotted version strings numerically per component
     *
     * Lexicographic string compare misorders versions the moment a component
     * reaches two digits ("2.10.0" vs "2.9.0"), so the update gate must not
     * use it. Non-numeric components compare as 0.
     *
     * @return true when a < b
     */
    [[nodiscard]] static bool version_less(const std::string& a, const std::string& b);

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
     * @brief Stage the Helix macros for installation
     *
     * Performs the following steps:
     * 1. Upload helix_macros.cfg, and helix_skips.cfg when stages_skips()
     * 2. Back up the original printer.cfg to a timestamped sibling
     * 3. Modify printer.cfg to include both
     *
     * Does NOT restart Klipper: the macros load at whatever restart happens
     * next, and restarting is the caller's decision (never during a print).
     * Idempotent — an existing include line is left alone and no backup is
     * written for an unmodified printer.cfg.
     *
     * @param on_success Called when both files are in place
     * @param on_error Called if any step fails (printer.cfg is never
     *                 overwritten when its backup upload failed)
     */
    void install_files(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Update Helix macros to latest version
     *
     * Overwrites helix_macros.cfg with the current version. When
     * stages_skips(), also writes helix_skips.cfg and adds its include if
     * missing; otherwise printer.cfg is not touched.
     * Does NOT restart Klipper — same rule as install_files().
     *
     * @param on_success Called when the new file is uploaded
     * @param on_error Called if upload fails
     */
    void update_files(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Request a Klipper restart to load staged macro files
     *
     * The caller owns the timing and must refuse to call this while a print
     * is active; this is a thin pass-through to the API's restart request.
     *
     * @param on_success Called when the restart request is accepted
     * @param on_error Called if the restart request fails
     */
    void request_restart(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Whether install_files()/update_files() also stage helix_skips.cfg
     *
     * True when discovery found a leveling step the skip wrappers can wrap
     * (PrinterDiscovery::skip_wrappable()).
     */
    [[nodiscard]] bool stages_skips() const;

    /**
     * @brief Remove helix_skips.cfg and its include, leaving the helper macros
     *
     * The way back when Klipper will not start with the skip wrappers loaded.
     * Does not restart Klipper.
     */
    void remove_skips(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Uninstall Helix macros from printer
     *
     * Removes helix_macros.cfg, helix_skips.cfg and both include lines from
     * printer.cfg.
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
     * @brief Remove the leading underscore from the signal and phase macro names
     *
     * Rewrites _HELIX_READY, _HELIX_ENDED, _HELIX_RESET and _HELIX_PHASE_*
     * wherever they appear. Other _HELIX_ names, such as _HELIX_STATE, stay as
     * they are.
     *
     * @param content helix_macros.cfg content
     * @return The content with the signal macros under their visible names
     */
    [[nodiscard]] static std::string with_visible_signal_names(std::string content);

    /**
     * @brief The helix_macros.cfg content to stage on this printer
     *
     * A printer that already defines HELIX_READY may call the visible signal
     * names from its own macros and sliced files, so it keeps them. Every other
     * printer gets the hidden names.
     *
     * @param hardware Discovery snapshot with the printer's macro names
     * @return Complete cfg file content, or empty if the file is not found
     */
    [[nodiscard]] static std::string macro_content_for(const PrinterDiscovery& hardware);

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

    /// Upload one file to the printer's config root.
    void upload_macro_file(const std::string& filename, const std::string& content,
                           SuccessCallback on_success, ErrorCallback on_error);

    /// Upload helix_skips.cfg when stages_skips(); succeed at once otherwise.
    void upload_skips_file(SuccessCallback on_success, ErrorCallback on_error);

    /**
     * @brief Add the helix_macros.cfg include (and, with_skips, the
     *        helix_skips.cfg one) to printer.cfg in one edit, backing up the
     *        original first. Writes nothing when both are already there.
     */
    void add_include_to_config(bool with_skips, SuccessCallback on_success, ErrorCallback on_error);

    /// Remove these include lines from printer.cfg in one edit.
    void remove_include_from_config(std::vector<std::string> filenames, SuccessCallback on_success,
                                    ErrorCallback on_error);

    /// Delete a file from the config root; a missing file counts as deleted.
    void delete_macro_file(const std::string& filename, SuccessCallback on_success,
                           ErrorCallback on_error);
};

} // namespace helix
