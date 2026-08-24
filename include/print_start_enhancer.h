// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file print_start_enhancer.h
 * @brief Enhances PRINT_START macros by adding conditional skip parameters
 *
 * When a PRINT_START macro contains operations like BED_MESH_CALIBRATE or
 * QUAD_GANTRY_LEVEL that are NOT wrapped in conditionals, this class can
 * generate the Jinja2 wrapper code to make them skippable.
 *
 * ## Safety Requirements (MUST be enforced by UI):
 * 1. Never auto-modify - require explicit user consent for each operation
 * 2. Always create timestamped backup before any changes
 * 3. Show diff preview of exact changes to user
 * 4. Validate Jinja2 syntax before applying
 * 5. Warn that Klipper will restart after changes
 *
 * ## Usage Flow:
 * 1. Analyze macro with PrintStartAnalyzer to find uncontrollable operations
 * 2. For each operation, call generate_wrapper() to get the Jinja2 code
 * 3. User reviews and confirms each enhancement
 * 4. Call apply_enhancements() to create backup, modify macro, restart Klipper
 */

#include "async_lifetime_guard.h"
#include "moonraker_error.h"
#include "print_start_analyzer.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;

namespace helix {

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief A single enhancement to apply to the macro
 */
struct MacroEnhancement {
    std::string operation_name; ///< e.g., "BED_MESH_CALIBRATE"
    PrintStartOpCategory category = PrintStartOpCategory::UNKNOWN;
    std::string skip_param_name; ///< e.g., "SKIP_BED_MESH"
    std::string original_line;   ///< The original line in the macro
    std::string enhanced_code;   ///< The Jinja2 conditional wrapper
    size_t line_number = 0;      ///< Line number in macro (1-indexed)
    bool user_approved = false;  ///< User has approved this enhancement
};

/**
 * @brief Result of applying enhancements
 */
struct EnhancementResult {
    bool success = false;
    std::string error_message;

    // Backup information
    std::string backup_filename;  ///< e.g., "printer.cfg.backup.20251222_170530"
    std::string backup_full_path; ///< Full path to backup file

    // Statistics
    size_t operations_enhanced = 0;
    size_t lines_added = 0;
    size_t lines_modified = 0;
};

/**
 * @brief Progress callback for multi-step enhancement process
 */
using EnhancementProgressCallback =
    std::function<void(const std::string& step_name, int current_step, int total_steps)>;

/**
 * @brief Completion callback for enhancement
 */
using EnhancementCompleteCallback = std::function<void(const EnhancementResult& result)>;

/**
 * @brief Error callback
 */
using EnhancementErrorCallback = std::function<void(const MoonrakerError& error)>;

// ============================================================================
// PrintStartEnhancer Class
// ============================================================================

/**
 * @brief Enhances PRINT_START macros with conditional skip parameters
 *
 * This class generates Jinja2 wrapper code for macro operations and
 * orchestrates the backup/modify/restart workflow.
 *
 * ## Example Enhancement:
 *
 * Before:
 * ```klipper
 * [gcode_macro PRINT_START]
 * gcode:
 *   G28
 *   BED_MESH_CALIBRATE
 *   QUAD_GANTRY_LEVEL
 * ```
 *
 * After (for BED_MESH_CALIBRATE):
 * ```klipper
 * [gcode_macro PRINT_START]
 * gcode:
 *   G28
 *   {% set SKIP_BED_MESH = params.SKIP_BED_MESH|default(0)|int %}
 *   {% if SKIP_BED_MESH == 0 %}
 *     BED_MESH_CALIBRATE
 *   {% endif %}
 *   QUAD_GANTRY_LEVEL
 * ```
 */
class PrintStartEnhancer {
  public:
    PrintStartEnhancer() = default;
    ~PrintStartEnhancer() = default;

    // Non-copyable
    PrintStartEnhancer(const PrintStartEnhancer&) = delete;
    PrintStartEnhancer& operator=(const PrintStartEnhancer&) = delete;

    // =========================================================================
    // Code Generation (Pure, No Side Effects)
    // =========================================================================

    /**
     * @brief Generate Jinja2 conditional wrapper for an operation
     *
     * @param operation The operation to wrap (from PrintStartAnalysis)
     * @param skip_param_name Parameter name to use (e.g., "SKIP_BED_MESH")
     * @return MacroEnhancement with the wrapper code
     */
    [[nodiscard]] static MacroEnhancement generate_wrapper(const PrintStartOperation& operation,
                                                           const std::string& skip_param_name);

    /**
     * @brief Generate the parameter declaration line
     *
     * Generates: {% set SKIP_BED_MESH = params.SKIP_BED_MESH|default(0)|int %}
     *
     * @param param_name The parameter name
     * @return The Jinja2 set statement
     */
    [[nodiscard]] static std::string generate_param_declaration(const std::string& param_name);

    /**
     * @brief Generate the conditional wrapper for a single line
     *
     * @param original_line The original G-code line
     * @param param_name The skip parameter name
     * @param include_declaration If true, include the {% set ... %} line
     * @return The wrapped code block
     */
    [[nodiscard]] static std::string generate_conditional_block(const std::string& original_line,
                                                                const std::string& param_name,
                                                                bool include_declaration = true);

    /**
     * @brief Apply enhancements to macro source code (in-memory)
     *
     * This method modifies the macro source in memory without touching
     * any files. Use for preview/validation before actual apply.
     *
     * @param original_macro Original macro gcode content
     * @param enhancements Approved enhancements to apply
     * @return Modified macro gcode, or empty string on error
     */
    [[nodiscard]] static std::string
    apply_to_source(const std::string& original_macro,
                    const std::vector<MacroEnhancement>& enhancements);

    /**
     * @brief Validate that generated Jinja2 code is syntactically correct
     *
     * Performs basic validation (balanced braces, valid keywords).
     * Does NOT execute the code or validate Klipper-specific macros.
     *
     * @param code The Jinja2 code to validate
     * @return true if syntax appears valid
     */
    [[nodiscard]] static bool validate_jinja2_syntax(const std::string& code);

    // =========================================================================
    // Enhancement Workflow (Async, Side Effects)
    // =========================================================================

    /**
     * @brief Apply approved enhancements to the printer
     *
     * This is the main workflow method that:
     * 1. Creates a timestamped backup of the config file
     * 2. Downloads the current config
     * 3. Modifies the macro with approved enhancements
     * 4. Uploads the modified config
     * 5. Restarts Klipper to apply changes
     *
     * @param api IMoonrakerAPI instance (must be connected)
     * @param macro_name Name of macro to enhance (e.g., "PRINT_START")
     * @param source_file Config file containing the macro (e.g., "macros.cfg")
     * @param enhancements List of approved enhancements
     * @param on_progress Progress callback (optional)
     * @param on_complete Completion callback
     * @param on_error Error callback
     */
    void apply_enhancements(IMoonrakerAPI* api, const std::string& macro_name,
                            const std::string& source_file,
                            const std::vector<MacroEnhancement>& enhancements,
                            EnhancementProgressCallback on_progress,
                            EnhancementCompleteCallback on_complete,
                            EnhancementErrorCallback on_error);

    /**
     * @brief Restore printer.cfg from a backup
     *
     * @param api IMoonrakerAPI instance
     * @param backup_filename Backup filename (in config root)
     * @param on_complete Called on success
     * @param on_error Called on error
     */
    void restore_from_backup(IMoonrakerAPI* api, const std::string& backup_filename,
                             std::function<void()> on_complete, EnhancementErrorCallback on_error);

    /**
     * @brief List available backups
     *
     * @param api IMoonrakerAPI instance
     * @param on_complete Called with list of backup filenames
     * @param on_error Called on error
     */
    void list_backups(IMoonrakerAPI* api,
                      std::function<void(const std::vector<std::string>&)> on_complete,
                      EnhancementErrorCallback on_error);

    // =========================================================================
    // Utility Methods
    // =========================================================================

    /**
     * @brief Generate a timestamped backup filename
     *
     * @param source_file The config file being backed up (e.g., "macros.cfg")
     * @return Filename like "macros.cfg.backup.20251222_170530"
     */
    [[nodiscard]] static std::string generate_backup_filename(const std::string& source_file);

    /**
     * @brief Get the standard skip parameter name for an operation category
     *
     * @param category The operation category
     * @return Standard parameter name (e.g., "SKIP_BED_MESH")
     */
    [[nodiscard]] static std::string get_skip_param_for_category(PrintStartOpCategory category);

  private:
    // === Workflow Step Helpers ===

    /**
     * @brief Create a backup of a config file
     */
    void create_backup(IMoonrakerAPI* api, const std::string& source_file,
                       const std::string& backup_filename, std::function<void()> on_success,
                       EnhancementErrorCallback on_error);

    /**
     * @brief Download, modify, and upload a config file
     */
    void modify_and_upload_config(IMoonrakerAPI* api, const std::string& macro_name,
                                  const std::string& source_file,
                                  const std::vector<MacroEnhancement>& enhancements,
                                  std::function<void(size_t ops, size_t lines)> on_success,
                                  EnhancementErrorCallback on_error);

    /**
     * @brief Restart Klipper to apply config changes
     */
    void restart_klipper(IMoonrakerAPI* api, std::function<void()> on_success,
                         EnhancementErrorCallback on_error);

    // === Lifetime Guard for Async Callbacks ===
    helix::AsyncLifetimeGuard lifetime_;

    // === Concurrency Guard ===
    // Prevents concurrent apply_enhancements() calls (e.g., from double-click)
    std::atomic<bool> operation_in_progress_{false};
};

} // namespace helix
