// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_ops_detector.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Type of modification to apply to G-code
 */
enum class ModificationType {
    COMMENT_OUT,   ///< Comment out the line(s) by prefixing with "; "
    DELETE,        ///< Remove the line(s) entirely
    INJECT_BEFORE, ///< Inject G-code before a specific line
    INJECT_AFTER,  ///< Inject G-code after a specific line
    REPLACE,       ///< Replace the line with different G-code
};

/**
 * @brief A single modification to apply to a G-code file
 *
 * Modifications are applied in order from last line to first to preserve
 * line numbers during multi-edit operations.
 */
struct Modification {
    ModificationType type;

    /// For COMMENT_OUT, DELETE, REPLACE: the line number (1-indexed)
    size_t line_number = 0;

    /// For multi-line operations: end line (inclusive). If 0, single line.
    size_t end_line_number = 0;

    /// For INJECT_BEFORE, INJECT_AFTER, REPLACE: the G-code to inject
    std::string gcode;

    /// Optional comment explaining the modification (for debugging)
    std::string comment;

    /// For COMMENT_OUT and REPLACE: the line the scan saw there. When set, a
    /// file whose line differs is refused rather than rewritten: the scan
    /// was of other G-code, and its line numbers mean nothing here.
    std::string expected_line;

    /// This modification, checked against @p line before it is applied.
    [[nodiscard]] Modification expecting(std::string line) && {
        expected_line = std::move(line);
        return std::move(*this);
    }

    /// Create a COMMENT_OUT modification for a single line
    static Modification comment_out(size_t line, const std::string& reason = "") {
        return {ModificationType::COMMENT_OUT, line, 0, "", reason};
    }

    /// Create a COMMENT_OUT modification for a range of lines
    static Modification comment_out_range(size_t start, size_t end,
                                          const std::string& reason = "") {
        return {ModificationType::COMMENT_OUT, start, end, "", reason};
    }

    /// Create an INJECT_BEFORE modification
    static Modification inject_before(size_t line, const std::string& gcode,
                                      const std::string& reason = "") {
        return {ModificationType::INJECT_BEFORE, line, 0, gcode, reason};
    }

    /// Create an INJECT_AFTER modification
    static Modification inject_after(size_t line, const std::string& gcode,
                                     const std::string& reason = "") {
        return {ModificationType::INJECT_AFTER, line, 0, gcode, reason};
    }

    /// Create a REPLACE modification
    static Modification replace(size_t line, const std::string& gcode,
                                const std::string& reason = "") {
        return {ModificationType::REPLACE, line, 0, gcode, reason};
    }
};

/**
 * @brief Result of applying modifications
 */
struct ModificationResult {
    bool success = false;
    std::string error_message;

    /// Path to modified file (temp file if not in-place)
    std::string modified_path;

    /// Number of lines modified
    size_t lines_modified = 0;

    /// Number of lines added
    size_t lines_added = 0;

    /// Number of lines removed
    size_t lines_removed = 0;

    /// Original file size
    size_t original_size = 0;

    /// Modified file size
    size_t modified_size = 0;
};

/**
 * @brief Modifies G-code files by commenting out, injecting, or replacing lines
 *
 * This class provides safe G-code file modification for scenarios where
 * the user wants to disable operations that are embedded in the G-code file
 * (e.g., disable bed leveling when it's already in the slicer's start G-code).
 *
 * **Design philosophy:**
 * - Prefer G-code injection (execute_gcode) over file modification
 * - Only modify files when disabling operations already in the G-code
 * - Create temp files, never modify originals in place
 * - Use Moonraker's file upload to replace the file for printing
 *
 * @code
 * GCodeFileModifier modifier;
 *
 * // Disable detected operations
 * auto scan = detector.scan_file("3DBenchy.gcode");
 * if (auto op = scan.get_operation(OperationType::BED_MESH)) {
 *     if (!user_wants_bed_leveling) {
 *         modifier.add_modification(Modification::comment_out(
 *             op->line_number, "Disabled by HelixScreen"));
 *     }
 * }
 *
 * // Create modified version
 * auto result = modifier.apply("3DBenchy.gcode");
 * if (result.success) {
 *     // Upload result.modified_path to printer and start print
 * }
 * @endcode
 *
 * @note Thread-safe for concurrent modifications of different files.
 */
class GCodeFileModifier {
  public:
    GCodeFileModifier() = default;

    GCodeFileModifier(const GCodeFileModifier&) = delete;
    GCodeFileModifier& operator=(const GCodeFileModifier&) = delete;
    GCodeFileModifier(GCodeFileModifier&&) = default;
    GCodeFileModifier& operator=(GCodeFileModifier&&) = default;
    ~GCodeFileModifier() = default;

    /**
     * @brief Add a modification to the pending list
     *
     * Modifications are stored and applied when apply() is called.
     * Order of additions doesn't matter - they're sorted by line number
     * and applied from last to first to preserve line numbers.
     */
    void add_modification(Modification mod);

    /**
     * @brief Clear all pending modifications
     */
    void clear_modifications();

    /**
     * @brief Get pending modifications
     */
    [[nodiscard]] const std::vector<Modification>& modifications() const {
        return modifications_;
    }

    /**
     * @brief Apply all pending modifications to a file
     *
     * Creates a modified copy in a temp location. The original file is never
     * modified. Use result.modified_path to access the modified file.
     *
     * @param filepath Path to the source G-code file
     * @return ModificationResult with success status and modified file path
     */
    [[nodiscard]] ModificationResult apply(const std::string& filepath);

    /**
     * @brief Apply modifications to G-code content string (for testing)
     *
     * @param content The G-code content as a string
     * @return Modified content as string, or empty on error
     */
    [[nodiscard]] std::string apply_to_content(const std::string& content);

    /**
     * @brief Apply modifications using streaming (for large files)
     *
     * This method processes the file line-by-line without loading it entirely
     * into memory. Critical for embedded devices with limited RAM where G-code
     * files can be 100MB+.
     *
     * **Limitations of streaming mode:**
     * - DELETE modifications skip lines (work correctly)
     * - COMMENT_OUT modifications work on single lines
     * - INJECT_BEFORE/AFTER work correctly
     * - REPLACE works for single lines
     * - Multi-line ranges (end_line_number > 0) are NOT supported and will
     *   be handled as single-line operations with a warning
     *
     * @param filepath Path to the source G-code file
     * @return ModificationResult with success status and modified file path
     *
     * @note apply() calls this for files should_use_gcode_streaming() says to stream.
     */
    [[nodiscard]] ModificationResult apply_streaming(const std::string& filepath);

    // =========================================================================
    // Convenience methods for common operations
    // =========================================================================

    /**
     * @brief Disable a detected operation by commenting it out
     *
     * Convenience method that adds the appropriate modification based on
     * the operation's embedding type.
     *
     * @param op The detected operation to disable
     * @return true if a modification was added, false if operation type
     *         doesn't support commenting out
     */
    bool disable_operation(const DetectedOperation& op);

    /**
     * @brief Modify START_PRINT parameter to disable an operation
     *
     * For operations embedded as macro parameters (e.g., FORCE_LEVELING=true),
     * this replaces the parameter value with 0/false.
     *
     * @param op The detected operation (must have MACRO_PARAMETER embedding)
     * @return true if modification added, false if not applicable
     */
    bool disable_macro_parameter(const DetectedOperation& op);

    /**
     * @brief Create modifications to disable multiple operations at once
     *
     * @param scan_result Result from GCodeOpsDetector::scan_file()
     * @param types_to_disable Set of operation types to disable
     */
    void disable_operations(const ScanResult& scan_result,
                            const std::vector<OperationType>& types_to_disable);

    /**
     * @brief Add skip parameters to the PRINT_START call
     *
     * Modifies the PRINT_START/START_PRINT line in the G-code to append
     * skip parameters like SKIP_BED_MESH=1, SKIP_QGL=1, etc.
     *
     * @param scan_result Result from GCodeOpsDetector (must have print_start.found == true)
     * @param skip_params Vector of (param_name, value) pairs to append
     * @return true if modification was added, false if PRINT_START not found
     */
    bool add_print_start_skip_params(
        const ScanResult& scan_result,
        const std::vector<std::pair<std::string, std::string>>& skip_params);

    // =========================================================================
    // Static utilities
    // =========================================================================

    /**
     * @brief Generate a temp file path for modified G-code
     *
     * @param original_path The original file path
     * @return Unique temp path like /tmp/helixscreen_mod_XXXXXX.gcode
     */
    [[nodiscard]] static std::string generate_temp_path(const std::string& original_path);

    /**
     * @brief Clean up temp files created by this modifier
     *
     * Call this periodically or on application exit to remove stale temp files.
     *
     * @param max_age_seconds Files older than this are deleted (default: 1 hour)
     * @return Number of files deleted
     */
    static size_t cleanup_temp_files(int max_age_seconds = 3600);

  private:
    /**
     * @brief Sort modifications by line number (descending)
     *
     * Processing from end to start preserves line numbers for earlier mods.
     */
    void sort_modifications();

    /**
     * @brief Apply a single modification to content lines
     */
    void apply_single_modification(std::vector<std::string>& lines, const Modification& mod,
                                   ModificationResult& result);

    /**
     * @brief Comment out a single line
     */
    static std::string comment_out_line(const std::string& line, const std::string& reason);

    /**
     * @brief Build line-number-indexed lookup map for streaming mode
     *
     * Creates a map where keys are line numbers (1-indexed) and values are
     * the modifications to apply. For COMMENT_OUT/DELETE with ranges, creates
     * entries for each line in the range.
     *
     * @return Map of line_number -> Modification for O(1) lookup during streaming
     */
    [[nodiscard]] std::unordered_map<size_t, Modification> build_streaming_lookup() const;

    /**
     * @brief Apply buffered mode (loads file into memory)
     *
     * The original implementation, now factored out for clarity.
     */
    [[nodiscard]] ModificationResult apply_buffered(const std::string& filepath);

    std::vector<Modification> modifications_;
};

} // namespace gcode
} // namespace helix
