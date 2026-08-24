// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "operation_patterns.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Type of pre-print operation detected in G-code
 *
 * Alias to the shared OperationCategory enum in operation_patterns.h.
 * This ensures consistency between GCodeOpsDetector and PrintStartAnalyzer.
 */
using OperationType = helix::OperationCategory;

/**
 * @brief How the operation is embedded in the G-code file
 */
enum class OperationEmbedding {
    DIRECT_COMMAND,  ///< Raw command inline (e.g., BED_MESH_CALIBRATE, G29)
    MACRO_CALL,      ///< Calls a user macro (e.g., CLEAN_NOZZLE)
    MACRO_PARAMETER, ///< Parameter to START_PRINT (e.g., FORCE_LEVELING=true)
    NOT_FOUND,       ///< Operation not detected in file
};

/**
 * @brief A single detected operation in a G-code file
 */
struct DetectedOperation {
    OperationType type;
    OperationEmbedding embedding;
    std::string raw_line;    ///< Full line text from file
    std::string macro_name;  ///< "BED_MESH_CALIBRATE" or "START_PRINT"
    std::string param_name;  ///< "FORCE_LEVELING" if macro parameter
    std::string param_value; ///< "true" if macro parameter
    size_t line_number = 0;  ///< 1-indexed line number
    size_t byte_offset = 0;  ///< Byte offset in file (for efficient modification)

    /**
     * @brief Get human-readable display name for this operation
     */
    [[nodiscard]] std::string display_name() const;
};

/**
 * @brief Configuration for the G-code operation detector
 */
struct DetectionConfig {
    size_t max_scan_bytes = 50 * 1024;   ///< Stop scanning after this many bytes (50KB)
    int max_scan_lines = 500;            ///< Stop scanning after this many lines
    bool stop_at_first_extrusion = true; ///< Stop when G1 with positive E detected
    bool stop_at_layer_marker = true;    ///< Stop when ;LAYER_CHANGE detected
};

/**
 * @brief Information about the PRINT_START/START_PRINT macro call in the G-code
 *
 * This is used to modify the call to add skip parameters for macro-embedded operations.
 */
struct PrintStartCallInfo {
    bool found = false;     ///< True if PRINT_START/START_PRINT call was found
    std::string macro_name; ///< "PRINT_START" or "START_PRINT"
    std::string raw_line;   ///< Full line text (e.g., "PRINT_START EXTRUDER=210 BED=60")
    size_t line_number = 0; ///< 1-indexed line number
    size_t byte_offset = 0; ///< Byte offset in file
    int extruder_temp = 0;  ///< First-layer extruder temp from the call (C), 0 = not stated
    int bed_temp = 0;       ///< First-layer bed temp from the call (C), 0 = not stated

    /**
     * @brief Build a modified line with skip parameters appended
     *
     * @param skip_params Map of param name to value (e.g., {"SKIP_BED_MESH": "1"})
     * @return Modified line with params appended
     */
    [[nodiscard]] std::string
    with_skip_params(const std::vector<std::pair<std::string, std::string>>& skip_params) const;
};

/**
 * @brief Result of scanning a G-code file for operations
 */
struct ScanResult {
    std::vector<DetectedOperation> operations;
    size_t lines_scanned = 0;
    size_t bytes_scanned = 0;
    bool reached_limit = false;     ///< True if scan stopped due to limits
    PrintStartCallInfo print_start; ///< Info about PRINT_START call (if found)

    /**
     * @brief Check if a specific operation type was detected
     */
    [[nodiscard]] bool has_operation(OperationType type) const;

    /**
     * @brief Get the first detected operation of a specific type
     */
    [[nodiscard]] std::optional<DetectedOperation> get_operation(OperationType type) const;

    /**
     * @brief Get all detected operations of a specific type
     */
    [[nodiscard]] std::vector<DetectedOperation> get_operations(OperationType type) const;
};

/**
 * @brief Pattern definition for detecting an operation
 */
struct OperationPattern {
    OperationType type;
    std::string pattern;          ///< Substring or regex to match
    OperationEmbedding embedding; ///< How this pattern indicates embedding
    bool exact_match = false;     ///< True for G-codes (exact), false for macros (substring)
};

/**
 * @brief Detects pre-print operations in G-code files
 *
 * Scans the start of G-code files to detect operations like bed leveling,
 * nozzle cleaning, chamber soak, etc. Uses configurable heuristics to
 * identify common patterns across different slicers and printer types.
 *
 * Thread-safe for concurrent scans of different files.
 *
 * @code
 * GCodeOpsDetector detector;
 * auto result = detector.scan_file("/path/to/file.gcode");
 *
 * for (const auto& op : result.operations) {
 *     spdlog::info("Found {} at line {}", op.display_name(), op.line_number);
 * }
 * @endcode
 */
class GCodeOpsDetector {
  public:
    /**
     * @brief Construct detector with optional configuration
     */
    explicit GCodeOpsDetector(const DetectionConfig& config = {});

    // Non-copyable, movable (RAII pattern)
    GCodeOpsDetector(const GCodeOpsDetector&) = delete;
    GCodeOpsDetector& operator=(const GCodeOpsDetector&) = delete;
    GCodeOpsDetector(GCodeOpsDetector&&) = default;
    GCodeOpsDetector& operator=(GCodeOpsDetector&&) = default;
    ~GCodeOpsDetector() = default;

    /**
     * @brief Scan a G-code file for pre-print operations
     *
     * @param filepath Path to the G-code file
     * @return ScanResult containing all detected operations
     */
    [[nodiscard]] ScanResult scan_file(const std::filesystem::path& filepath) const;

    /**
     * @brief Scan G-code content from a string (for testing)
     *
     * @param content G-code content as string
     * @return ScanResult containing all detected operations
     */
    [[nodiscard]] ScanResult scan_content(const std::string& content) const;

    /**
     * @brief Add a custom detection pattern
     *
     * @param pattern The pattern to add
     */
    void add_pattern(OperationPattern pattern);

    /**
     * @brief Get the current configuration
     */
    [[nodiscard]] const DetectionConfig& config() const {
        return config_;
    }

    /**
     * @brief Get all registered patterns
     */
    [[nodiscard]] const std::vector<OperationPattern>& patterns() const {
        return patterns_;
    }

    /**
     * @brief Get human-readable name for an operation type
     */
    [[nodiscard]] static std::string operation_type_name(OperationType type);

  private:
    /**
     * @brief Initialize default detection patterns
     */
    void init_default_patterns();

    /**
     * @brief Scan a stream of G-code lines
     */
    [[nodiscard]] ScanResult scan_stream(std::istream& stream) const;

    /**
     * @brief Check a line against all patterns
     */
    void check_line(const std::string& line, size_t line_number, size_t byte_offset,
                    ScanResult& result) const;

    /**
     * @brief Check if line indicates first extrusion
     */
    [[nodiscard]] bool is_first_extrusion(const std::string& line) const;

    /**
     * @brief Check if line is a layer change marker
     */
    [[nodiscard]] bool is_layer_marker(const std::string& line) const;

    /**
     * @brief Parse START_PRINT parameters from a line
     */
    void parse_start_print_params(const std::string& line, size_t line_number, size_t byte_offset,
                                  ScanResult& result) const;

    DetectionConfig config_;
    std::vector<OperationPattern> patterns_;
};

} // namespace gcode
} // namespace helix
