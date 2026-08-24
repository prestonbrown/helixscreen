// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file macro_modification_manager.h
 * @brief Manages PRINT_START macro modification capabilities
 *
 * This class handles:
 * - Automatic analysis after printer discovery
 * - Smart notification when uncontrollable ops are found
 * - Persistence of dismissed/configured state
 * - On-demand wizard launch from Advanced panel
 * - Integration with printer capability database for native params
 *
 * ## Flow:
 * 1. After discovery, check_and_notify() is called
 * 2. If macro has uncontrollable ops and not dismissed → show toast
 * 3. User can configure (launches wizard) or dismiss
 * 4. State is persisted via Config (hash, dismissed, configured)
 *
 * For printers in the capability database (like AD5M), native PRINT_START
 * params are used instead of macro modification, and the wizard toast
 * is suppressed.
 */

#include "async_lifetime_guard.h"
#include "print_start_analyzer.h"

#include <functional>
#include <memory>
#include <string>

// Forward declarations
namespace helix {
class Config;
}
class IMoonrakerAPI;
namespace helix::ui {
class MacroEnhanceWizard;
}

namespace helix {

/**
 * @brief Wizard state persisted in config
 */
struct PrintStartWizardConfig {
    bool dismissed = false;  ///< User clicked "Don't show again"
    bool configured = false; ///< Wizard completed successfully at least once
    std::string macro_hash;  ///< Hash of macro content (detect changes)
};

/**
 * @brief Manages PRINT_START macro modification and enhancement wizard
 */
class MacroModificationManager {
  public:
    /**
     * @brief Construct manager
     * @param config Config instance for persistence
     * @param api IMoonrakerAPI for macro operations
     */
    MacroModificationManager(Config* config, IMoonrakerAPI* api);
    ~MacroModificationManager();

    // Non-copyable
    MacroModificationManager(const MacroModificationManager&) = delete;
    MacroModificationManager& operator=(const MacroModificationManager&) = delete;

    // =========================================================================
    // Primary API
    // =========================================================================

    /**
     * @brief Check macro and show notification if needed
     *
     * Called after printer discovery completes. Analyzes PRINT_START macro
     * and shows a toast notification if:
     * - Macro has uncontrollable operations
     * - User hasn't dismissed the notification
     * - Macro has changed since last configuration
     */
    void check_and_notify();

    /**
     * @brief Launch wizard on-demand (from Advanced panel)
     *
     * Analyzes macro and launches wizard regardless of dismissed state.
     * If no uncontrollable ops found, shows informational toast.
     */
    void analyze_and_launch_wizard();

    /**
     * @brief Mark notification as dismissed
     *
     * Called when user clicks "Don't show again" on toast.
     * Persists dismissed=true to config.
     */
    void mark_dismissed();

    /**
     * @brief Reset dismissed state (for testing)
     */
    void reset_dismissed();

    // =========================================================================
    // State Access
    // =========================================================================

    /**
     * @brief Check if analysis is in progress
     */
    [[nodiscard]] bool is_analyzing() const {
        return analyzing_;
    }

    /**
     * @brief Get cached analysis result
     */
    [[nodiscard]] const PrintStartAnalysis& get_cached_analysis() const {
        return cached_analysis_;
    }

    /**
     * @brief Check if wizard is currently visible
     */
    [[nodiscard]] bool is_wizard_visible() const;

  private:
    // === Dependencies ===
    Config* config_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;

    // === State ===
    PrintStartAnalyzer analyzer_;
    PrintStartAnalysis cached_analysis_;
    std::unique_ptr<ui::MacroEnhanceWizard> wizard_;
    bool analyzing_ = false;

    // === Async callback guard ===
    helix::AsyncLifetimeGuard lifetime_;

    // === Internal Methods ===

    /**
     * @brief Load wizard config from Config
     */
    [[nodiscard]] PrintStartWizardConfig load_config() const;

    /**
     * @brief Save wizard config to Config
     */
    void save_config(const PrintStartWizardConfig& wizard_config);

    /**
     * @brief Compute hash of macro content
     */
    [[nodiscard]] static std::string compute_hash(const std::string& content);

    /**
     * @brief Check if notification should be shown
     */
    [[nodiscard]] bool should_show_notification(const PrintStartAnalysis& analysis,
                                                const PrintStartWizardConfig& wizard_config) const;

    /**
     * @brief Show toast notification with Configure/Dismiss options
     */
    void show_configure_toast();

    /**
     * @brief Launch the wizard modal
     */
    void launch_wizard();

    /**
     * @brief Handle wizard completion
     */
    void on_wizard_complete(bool applied, size_t operations_enhanced);
};

} // namespace helix
