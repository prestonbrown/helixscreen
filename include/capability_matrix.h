// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file capability_matrix.h
 * @brief Unified capability source management for pre-print operations
 *
 * CapabilityMatrix unifies three sources of pre-print operation capabilities:
 * 1. DATABASE - PrinterDetector's PrePrintOptionSet (highest priority)
 * 2. MACRO_ANALYSIS - PrintStartAnalyzer's PrintStartAnalysis (medium priority)
 * 3. FILE_SCAN - GCodeOpsDetector's ScanResult (lowest priority)
 *
 * Priority ordering ensures that database-defined capabilities (which are
 * curated and tested) take precedence over dynamically detected ones.
 */

#include "flat_map.h"
#include "gcode_ops_detector.h"
#include "operation_patterns.h"
#include "print_start_analyzer.h"
#include "printer_detector.h"
#include "sort_small.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Origin of a capability source
 *
 * Priority ordering (lower = higher priority):
 * - DATABASE (0): Curated, tested capabilities from printer_database.json
 * - MACRO_ANALYSIS (1): Dynamically detected from PRINT_START macro
 * - FILE_SCAN (2): Detected in G-code file content
 */
enum class CapabilityOrigin {
    DATABASE,       ///< PrinterDetector - highest priority
    MACRO_ANALYSIS, ///< PrintStartAnalyzer - medium priority
    FILE_SCAN       ///< GCodeOpsDetector - lowest priority
};

/**
 * @brief A single source of capability information for an operation
 */
struct CapabilitySource {
    CapabilityOrigin origin;    ///< Where this capability came from
    std::string param_name;     ///< e.g., "FORCE_LEVELING", "SKIP_BED_MESH"
    std::string skip_value;     ///< Value to disable operation
    std::string enable_value;   ///< Value to enable operation
    ParameterSemantic semantic; ///< OPT_IN or OPT_OUT
    size_t line_number = 0;     ///< For file operations only
};

/**
 * @brief Unified capability management across multiple sources
 *
 * Collects capability information from database, macro analysis, and file scans,
 * then provides prioritized access to determine how to control operations.
 */
class CapabilityMatrix {
  public:
    CapabilityMatrix() = default;
    ~CapabilityMatrix() = default;

    // Copyable and movable
    CapabilityMatrix(const CapabilityMatrix&) = default;
    CapabilityMatrix& operator=(const CapabilityMatrix&) = default;
    CapabilityMatrix(CapabilityMatrix&&) = default;
    CapabilityMatrix& operator=(CapabilityMatrix&&) = default;

    // =========================================================================
    // Source Addition
    // =========================================================================

    /**
     * @brief Add capabilities from printer database
     *
     * Maps option ids (bed_mesh, qgl, etc.) to OperationCategory and stores
     * with DATABASE priority. Only options whose strategy is `MacroParam`
     * contribute to the matrix — other strategies don't expose a START_PRINT
     * parameter and aren't representable as a capability source.
     *
     * @param options PrePrintOptionSet from PrinterDetector
     */
    void add_from_database(const PrePrintOptionSet& options);

    /**
     * @brief Add capabilities from macro analysis
     *
     * Only adds operations that have has_skip_param == true.
     * Uses the param_semantic field for OPT_IN/OPT_OUT handling.
     *
     * @param analysis PrintStartAnalysis from PrintStartAnalyzer
     */
    void add_from_macro_analysis(const PrintStartAnalysis& analysis);

    /**
     * @brief Add capabilities from G-code file scan
     *
     * Only adds operations with embedding != NOT_FOUND.
     * FILE_SCAN sources without macro parameters cannot be skipped via params.
     *
     * @param scan ScanResult from GCodeOpsDetector
     */
    void add_from_file_scan(const gcode::ScanResult& scan);

    // =========================================================================
    // Query Methods
    // =========================================================================

    /**
     * @brief Check if an operation can be controlled
     *
     * @param op Operation category to check
     * @return true if at least one source exists for this operation
     */
    [[nodiscard]] bool is_controllable(OperationCategory op) const;

    /**
     * @brief Get the best (highest priority) source for an operation
     *
     * @param op Operation category
     * @return Best source if any exist, nullopt otherwise
     */
    [[nodiscard]] std::optional<CapabilitySource> get_best_source(OperationCategory op) const;

    /**
     * @brief Get all sources for an operation, sorted by priority
     *
     * @param op Operation category
     * @return Vector of sources, highest priority first
     */
    [[nodiscard]] std::vector<CapabilitySource> get_all_sources(OperationCategory op) const;

    /**
     * @brief Get the parameter name and skip value for an operation
     *
     * Returns the skip parameter from the best source. Returns nullopt if:
     * - No source exists for the operation
     * - Best source has no param_name (file scan with direct command)
     *
     * @param op Operation category
     * @return Pair of (param_name, skip_value) or nullopt
     */
    [[nodiscard]] std::optional<std::pair<std::string, std::string>>
    get_skip_param(OperationCategory op) const;

    /**
     * @brief Check if any operations are controllable
     *
     * @return true if at least one operation has a source
     */
    [[nodiscard]] bool has_any_controllable() const;

    /**
     * @brief Get all operations that have at least one source
     *
     * @return Vector of controllable operation categories
     */
    [[nodiscard]] std::vector<OperationCategory> get_controllable_operations() const;

    /**
     * @brief Clear all sources
     */
    void clear();

  private:
    /**
     * @brief Add a source for an operation
     */
    void add_source(OperationCategory op, CapabilitySource source);

    /**
     * @brief Get priority value for an origin (lower = higher priority)
     */
    static int origin_priority(CapabilityOrigin origin);

    /**
     * @brief Map a database option id to OperationCategory
     *
     * @param key Option id (e.g., "bed_mesh", "qgl")
     * @return Operation category, or UNKNOWN if not recognized
     */
    static OperationCategory category_from_key(const std::string& key);

    /**
     * @brief Infer semantic from parameter name
     *
     * FORCE_*, PERFORM_*, DO_*, ENABLE_* -> OPT_IN
     * Everything else (SKIP_*, NO_*, etc.) -> OPT_OUT
     */
    static ParameterSemantic infer_semantic(const std::string& param_name);

    /// Map of operation category to list of sources
    FlatMap<OperationCategory, std::vector<CapabilitySource>> capabilities_;
};

// =============================================================================
// Inline Implementations
// =============================================================================

inline void CapabilityMatrix::add_from_database(const PrePrintOptionSet& options) {
    for (const auto& opt : options.options) {
        // Only MacroParam options contribute to the capability matrix; other
        // strategies don't map to a START_PRINT parameter.
        const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
        if (!macro) {
            continue;
        }

        OperationCategory category = category_from_key(opt.id);
        if (category == OperationCategory::UNKNOWN) {
            continue;
        }

        // Skip options with empty param names (defensive — parser already rejects these)
        if (macro->param_name.empty()) {
            continue;
        }

        CapabilitySource source;
        source.origin = CapabilityOrigin::DATABASE;
        source.param_name = macro->param_name;
        source.skip_value = macro->skip_value;
        source.enable_value = macro->enable_value;
        source.semantic = infer_semantic(macro->param_name);
        source.line_number = 0;

        add_source(category, source);
    }
}

inline void CapabilityMatrix::add_from_macro_analysis(const PrintStartAnalysis& analysis) {
    for (const auto& op : analysis.operations) {
        if (!op.has_skip_param) {
            continue;
        }

        CapabilitySource source;
        source.origin = CapabilityOrigin::MACRO_ANALYSIS;
        source.param_name = op.skip_param_name;
        source.semantic = op.param_semantic;

        // Set skip/enable values based on semantic
        if (op.param_semantic == ParameterSemantic::OPT_OUT) {
            // SKIP_*: 1 means skip, 0 means do
            source.skip_value = "1";
            source.enable_value = "0";
        } else {
            // PERFORM_*/DO_*/FORCE_*: 0 means skip, 1 means do
            source.skip_value = "0";
            source.enable_value = "1";
        }
        source.line_number = 0;

        add_source(op.category, source);
    }
}

inline void CapabilityMatrix::add_from_file_scan(const gcode::ScanResult& scan) {
    for (const auto& op : scan.operations) {
        if (op.embedding == gcode::OperationEmbedding::NOT_FOUND) {
            continue;
        }

        CapabilitySource source;
        source.origin = CapabilityOrigin::FILE_SCAN;
        source.line_number = op.line_number;

        if (op.embedding == gcode::OperationEmbedding::MACRO_PARAMETER) {
            // Macro parameter: use the param name, infer semantic for values
            source.param_name = op.param_name;
            source.semantic = infer_semantic(op.param_name);

            // Set skip/enable values based on semantic
            if (source.semantic == ParameterSemantic::OPT_OUT) {
                // SKIP_*: 1 means skip, 0 means do
                source.skip_value = "1";
                source.enable_value = "0";
            } else {
                // PERFORM_*/FORCE_*: 0 means skip, 1 means do
                source.skip_value = "0";
                source.enable_value = "1";
            }
        } else {
            // Direct command or macro call: no parameter, requires file modification
            source.param_name = "";
            source.skip_value = "";
            source.enable_value = "";
            source.semantic = ParameterSemantic::OPT_OUT;
        }

        add_source(op.type, source);
    }
}

inline bool CapabilityMatrix::is_controllable(OperationCategory op) const {
    auto it = capabilities_.find(op);
    return it != capabilities_.end() && !it->second.empty();
}

inline std::optional<CapabilitySource>
CapabilityMatrix::get_best_source(OperationCategory op) const {
    auto sources = get_all_sources(op);
    if (sources.empty()) {
        return std::nullopt;
    }
    return sources[0];
}

inline std::vector<CapabilitySource> CapabilityMatrix::get_all_sources(OperationCategory op) const {
    auto it = capabilities_.find(op);
    if (it == capabilities_.end()) {
        return {};
    }

    // Return copy sorted by priority
    std::vector<CapabilitySource> result = it->second;
    sort_small(result.begin(), result.end(),
               [](const CapabilitySource& a, const CapabilitySource& b) {
                   return origin_priority(a.origin) < origin_priority(b.origin);
               });
    return result;
}

inline std::optional<std::pair<std::string, std::string>>
CapabilityMatrix::get_skip_param(OperationCategory op) const {
    auto best = get_best_source(op);
    if (!best.has_value() || best->param_name.empty()) {
        return std::nullopt;
    }
    return std::make_pair(best->param_name, best->skip_value);
}

inline bool CapabilityMatrix::has_any_controllable() const {
    return !capabilities_.empty();
}

inline std::vector<OperationCategory> CapabilityMatrix::get_controllable_operations() const {
    std::vector<OperationCategory> result;
    result.reserve(capabilities_.size());
    for (const auto& [category, sources] : capabilities_) {
        if (!sources.empty()) {
            result.push_back(category);
        }
    }
    return result;
}

inline void CapabilityMatrix::clear() {
    capabilities_.clear();
}

inline void CapabilityMatrix::add_source(OperationCategory op, CapabilitySource source) {
    capabilities_[op].push_back(std::move(source));
}

inline int CapabilityMatrix::origin_priority(CapabilityOrigin origin) {
    switch (origin) {
    case CapabilityOrigin::DATABASE:
        return 0; // Highest priority
    case CapabilityOrigin::MACRO_ANALYSIS:
        return 1;
    case CapabilityOrigin::FILE_SCAN:
        return 2; // Lowest priority
    default:
        return 99;
    }
}

inline OperationCategory CapabilityMatrix::category_from_key(const std::string& key) {
    if (key == "bed_mesh") {
        return OperationCategory::BED_MESH;
    }
    if (key == "qgl") {
        return OperationCategory::QGL;
    }
    if (key == "z_tilt") {
        return OperationCategory::Z_TILT;
    }
    if (key == "bed_level") {
        return OperationCategory::BED_LEVEL;
    }
    if (key == "nozzle_clean") {
        return OperationCategory::NOZZLE_CLEAN;
    }
    if (key == "purge_line") {
        return OperationCategory::PURGE_LINE;
    }
    if (key == "homing") {
        return OperationCategory::HOMING;
    }
    if (key == "chamber_soak") {
        return OperationCategory::CHAMBER_SOAK;
    }
    if (key == "skew_correct") {
        return OperationCategory::SKEW_CORRECT;
    }
    if (key == "motor_sync") {
        return OperationCategory::MOTOR_SYNC;
    }
    return OperationCategory::UNKNOWN;
}

inline ParameterSemantic CapabilityMatrix::infer_semantic(const std::string& param_name) {
    // OPT_IN patterns: FORCE_*, PERFORM_*, DO_*, ENABLE_*
    if (param_name.find("FORCE_") == 0 || param_name.find("PERFORM_") == 0 ||
        param_name.find("DO_") == 0 || param_name.find("ENABLE_") == 0) {
        return ParameterSemantic::OPT_IN;
    }
    // Everything else is OPT_OUT (SKIP_*, NO_*, etc.)
    return ParameterSemantic::OPT_OUT;
}

} // namespace helix
