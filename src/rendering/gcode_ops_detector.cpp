// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_ops_detector.h"

#include "operation_patterns.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace helix {
namespace gcode {

// ============================================================================
// DetectedOperation implementation
// ============================================================================

std::string DetectedOperation::display_name() const {
    return helix::category_name(type);
}

// ============================================================================
// PrintStartCallInfo implementation
// ============================================================================

namespace {

/// Extract the first stated numeric value for a parameter key (case-insensitive)
/// from a whitespace-separated KEY=VALUE token list. Returns 0 when absent or
/// non-numeric.
int temp_from_call_line(const std::string& line, const char* key) {
    std::istringstream tokens(line);
    std::string token;
    const std::string key_upper = helix::to_upper(key);
    while (tokens >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos)
            continue;
        if (helix::to_upper(token.substr(0, eq)) != key_upper)
            continue;
        try {
            return static_cast<int>(std::stof(token.substr(eq + 1)));
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

void parse_call_temps(const std::string& line, PrintStartCallInfo& call) {
    // Explicit _TEMP spellings win over the short aliases so a line carrying
    // both keeps the precise one ("BED_TEMP=100 BED=60" -> 100).
    call.extruder_temp = temp_from_call_line(line, "EXTRUDER_TEMP");
    if (call.extruder_temp == 0)
        call.extruder_temp = temp_from_call_line(line, "EXTRUDER");
    call.bed_temp = temp_from_call_line(line, "BED_TEMP");
    if (call.bed_temp == 0)
        call.bed_temp = temp_from_call_line(line, "BED");
}

} // namespace

std::string PrintStartCallInfo::with_skip_params(
    const std::vector<std::pair<std::string, std::string>>& skip_params) const {
    if (!found || skip_params.empty()) {
        return raw_line;
    }

    // Start with the original line, trimmed of trailing whitespace/newlines
    std::string modified = raw_line;
    while (!modified.empty() && (modified.back() == '\n' || modified.back() == '\r' ||
                                 modified.back() == ' ' || modified.back() == '\t')) {
        modified.pop_back();
    }

    // Append skip parameters (validated for safe characters)
    for (const auto& [param_name, param_value] : skip_params) {
        // Validate param_name contains only safe characters (A-Z, 0-9, _)
        // This prevents injection of malformed parameters
        bool valid_name =
            !param_name.empty() && std::all_of(param_name.begin(), param_name.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            });
        if (!valid_name) {
            spdlog::warn("[PrintStartCallInfo] Skipping invalid param name: {}", param_name);
            continue;
        }

        modified += " ";
        modified += param_name;
        modified += "=";
        modified += param_value;
    }

    spdlog::debug("[PrintStartCallInfo] Modified line: {}... -> {}...",
                  raw_line.substr(0, std::min<size_t>(raw_line.size(), 50)),
                  modified.substr(0, std::min<size_t>(modified.size(), 80)));

    return modified;
}

// ============================================================================
// ScanResult implementation
// ============================================================================

bool ScanResult::has_operation(OperationType type) const {
    return std::any_of(operations.begin(), operations.end(),
                       [type](const DetectedOperation& op) { return op.type == type; });
}

std::optional<DetectedOperation> ScanResult::get_operation(OperationType type) const {
    auto it = std::find_if(operations.begin(), operations.end(),
                           [type](const DetectedOperation& op) { return op.type == type; });
    if (it != operations.end()) {
        return *it;
    }
    return std::nullopt;
}

std::vector<DetectedOperation> ScanResult::get_operations(OperationType type) const {
    std::vector<DetectedOperation> result;
    std::copy_if(operations.begin(), operations.end(), std::back_inserter(result),
                 [type](const DetectedOperation& op) { return op.type == type; });
    return result;
}

// ============================================================================
// GCodeOpsDetector implementation
// ============================================================================

GCodeOpsDetector::GCodeOpsDetector(const DetectionConfig& config) : config_(config) {
    init_default_patterns();
}

std::string GCodeOpsDetector::operation_type_name(OperationType type) {
    return category_key(type);
}

void GCodeOpsDetector::init_default_patterns() {
    // ========================================================================
    // Build patterns from shared operation_patterns.h registry
    // ========================================================================
    // All patterns are now defined in a single place (operation_patterns.h)
    // and used by both GCodeOpsDetector and PrintStartAnalyzer.

    for (size_t i = 0; i < OPERATION_KEYWORDS_COUNT; ++i) {
        const auto& kw = OPERATION_KEYWORDS[i];

        // Determine embedding type based on pattern characteristics
        // G-codes and direct Klipper commands = DIRECT_COMMAND
        // User macros = MACRO_CALL
        OperationEmbedding embedding = OperationEmbedding::MACRO_CALL;

        std::string pattern = kw.keyword;
        if (pattern.rfind("G", 0) == 0 ||    // G28, G29
            pattern.find("BED_MESH") == 0 || // BED_MESH, BED_MESH_CALIBRATE, etc.
            pattern == "QUAD_GANTRY_LEVEL" || pattern == "QGL" ||
            pattern.find("Z_TILT") == 0 || // Z_TILT, Z_TILT_ADJUST
            pattern.find("SET_HEATER_TEMPERATURE") == 0 ||
            pattern.find("SKEW") == 0) { // SKEW_PROFILE, SET_SKEW
            embedding = OperationEmbedding::DIRECT_COMMAND;
        }

        patterns_.push_back({kw.category, pattern, embedding, kw.exact_match});
    }

    spdlog::debug("[GCodeOpsDetector] Initialized {} patterns from shared registry",
                  patterns_.size());
}

void GCodeOpsDetector::add_pattern(OperationPattern pattern) {
    patterns_.push_back(std::move(pattern));
}

ScanResult GCodeOpsDetector::scan_file(const std::filesystem::path& filepath) const {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::warn("[GCodeOpsDetector] Failed to open file: {}", filepath.string());
        return {};
    }

    spdlog::debug("[GCodeOpsDetector] Scanning file: {}", filepath.string());
    return scan_stream(file);
}

ScanResult GCodeOpsDetector::scan_content(const std::string& content) const {
    std::istringstream stream(content);
    return scan_stream(stream);
}

ScanResult GCodeOpsDetector::scan_stream(std::istream& stream) const {
    ScanResult result;
    std::string line;
    size_t line_number = 0;
    size_t byte_offset = 0;

    while (std::getline(stream, line)) {
        line_number++;

        // Check limits
        if (byte_offset >= config_.max_scan_bytes) {
            spdlog::debug("[GCodeOpsDetector] Reached byte limit at {} bytes", byte_offset);
            result.reached_limit = true;
            break;
        }

        if (static_cast<int>(line_number) > config_.max_scan_lines) {
            spdlog::debug("[GCodeOpsDetector] Reached line limit at line {}", line_number);
            result.reached_limit = true;
            break;
        }

        // Check for first extrusion (stop scanning)
        if (config_.stop_at_first_extrusion && is_first_extrusion(line)) {
            spdlog::debug("[GCodeOpsDetector] First extrusion at line {}, stopping", line_number);
            break;
        }

        // Check for layer marker (stop scanning)
        if (config_.stop_at_layer_marker && is_layer_marker(line)) {
            spdlog::debug("[GCodeOpsDetector] Layer marker at line {}, stopping", line_number);
            break;
        }

        // Skip comment-only lines and empty lines (but still track byte offset)
        if (!line.empty() && line[0] != ';') {
            // Check for PRINT_START or START_PRINT macro call (case-insensitive)
            std::string upper_line = helix::to_upper(line);

            // Capture the PRINT_START call info (first occurrence only)
            if (!result.print_start.found) {
                // Look for PRINT_START (more common) or START_PRINT
                size_t ps_pos = upper_line.find("PRINT_START");
                size_t sp_pos = upper_line.find("START_PRINT");

                if (ps_pos != std::string::npos || sp_pos != std::string::npos) {
                    result.print_start.found = true;
                    result.print_start.macro_name =
                        (ps_pos != std::string::npos) ? "PRINT_START" : "START_PRINT";
                    result.print_start.raw_line = line;
                    result.print_start.line_number = line_number;
                    result.print_start.byte_offset = byte_offset;
                    parse_call_temps(line, result.print_start);

                    spdlog::debug("[GCodeOpsDetector] Found {} call at line {}: {}",
                                  result.print_start.macro_name, line_number,
                                  line.substr(0, std::min<size_t>(line.size(), 60)));
                }
            }

            // Also parse params from the START_PRINT line
            if (upper_line.find("START_PRINT") != std::string::npos) {
                parse_start_print_params(line, line_number, byte_offset, result);
            }

            // Check against all patterns
            check_line(line, line_number, byte_offset, result);
        }

        byte_offset += line.size() + 1; // +1 for newline
    }

    result.lines_scanned = line_number;
    result.bytes_scanned = byte_offset;

    spdlog::debug("[GCodeOpsDetector] Scan complete: {} lines, {} bytes, {} operations found",
                  result.lines_scanned, result.bytes_scanned, result.operations.size());

    return result;
}

void GCodeOpsDetector::check_line(const std::string& line, size_t line_number, size_t byte_offset,
                                  ScanResult& result) const {
    // Trim leading whitespace for matching
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return;
    }
    std::string trimmed = line.substr(start);

    // Check each pattern
    for (const auto& pattern : patterns_) {
        bool found = false;

        // Always case-insensitive, but exact_match controls exact vs substring
        std::string upper_trimmed = helix::to_upper(trimmed);
        std::string upper_pattern = helix::to_upper(pattern.pattern);

        if (pattern.exact_match) {
            // G-codes: exact match at start of line (avoid G28 inside FOO_G28_BAR)
            found = (upper_trimmed.find(upper_pattern) == 0);
        } else {
            // Macros: substring match (catches _PRIME_NOZZLE, AUTO_BED_LEVEL, etc.)
            found = (upper_trimmed.find(upper_pattern) != std::string::npos);
        }

        if (found) {
            // Check if we already have this operation type (avoid duplicates)
            bool already_detected = std::any_of(
                result.operations.begin(), result.operations.end(),
                [&pattern](const DetectedOperation& op) { return op.type == pattern.type; });

            if (!already_detected) {
                DetectedOperation op;
                op.type = pattern.type;
                op.embedding = pattern.embedding;
                op.raw_line = line;
                // Extract actual command from line (first word before space/params)
                size_t space_pos = trimmed.find(' ');
                op.macro_name =
                    (space_pos != std::string::npos) ? trimmed.substr(0, space_pos) : trimmed;
                op.line_number = line_number;
                op.byte_offset = byte_offset;

                result.operations.push_back(std::move(op));

                spdlog::trace("[GCodeOpsDetector] Detected {} at line {}: {}",
                              operation_type_name(pattern.type), line_number, trimmed);
            }
        }
    }
}

bool GCodeOpsDetector::is_first_extrusion(const std::string& line) const {
    // Look for G1 with positive E value (actual extrusion, not retract)
    // Must start with G1 (or have whitespace before it)
    size_t g1_pos = line.find("G1");
    if (g1_pos == std::string::npos) {
        // Also check for G1 at start after whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            return false;
        }
        if (line.substr(start, 2) != "G1") {
            return false;
        }
    }

    // Find E parameter
    size_t e_pos = line.find(" E");
    if (e_pos == std::string::npos) {
        e_pos = line.find("\tE");
    }
    if (e_pos == std::string::npos) {
        return false;
    }

    // Extract value after E
    try {
        std::string e_str;
        for (size_t i = e_pos + 2; i < line.size(); i++) {
            char c = line[i];
            if (c == '-' || c == '.' || std::isdigit(c)) {
                e_str += c;
            } else {
                break;
            }
        }
        if (e_str.empty()) {
            return false;
        }
        float e_val = std::stof(e_str);
        return e_val > 0.001f; // Positive extrusion
    } catch (...) {
        return false;
    }
}

bool GCodeOpsDetector::is_layer_marker(const std::string& line) const {
    // Check for common layer change markers
    if (line.find(";LAYER_CHANGE") != std::string::npos) {
        return true;
    }
    if (line.find(";LAYER:") != std::string::npos) {
        return true;
    }
    // PrusaSlicer/OrcaSlicer format: ;Z:0.3
    if (line.find(";Z:") != std::string::npos && line.find(";Z:") < 5) {
        return true;
    }
    return false;
}

namespace {

/**
 * @brief Check if a value string indicates "truthy" (enabled)
 *
 * Returns true for: "TRUE", "1", "YES", or positive numbers
 * Returns false for: "FALSE", "0", "NO", or non-numeric strings
 */
bool is_truthy_value(const std::string& value) {
    std::string upper_value = helix::to_upper(value);

    if (upper_value == "TRUE" || upper_value == "1" || upper_value == "YES") {
        return true;
    }
    if (upper_value == "FALSE" || upper_value == "0" || upper_value == "NO") {
        return false;
    }

    // Try parsing as number
    try {
        float num = std::stof(value);
        return num > 0;
    } catch (...) {
        // Not a number - be conservative
        return false;
    }
}

} // namespace

void GCodeOpsDetector::parse_start_print_params(const std::string& line, size_t line_number,
                                                size_t byte_offset, ScanResult& result) const {
    // Parse parameters like: START_PRINT EXTRUDER_TEMP=220 BED_TEMP=60 FORCE_LEVELING=true
    // Uses shared parameter matching from operation_patterns.h

    std::string upper_line = helix::to_upper(line);

    // Parse all KEY=VALUE pairs from the line
    // We scan for patterns like "KEY=" and extract the value
    size_t pos = 0;
    while (pos < upper_line.size()) {
        // Find next '=' sign
        size_t eq_pos = upper_line.find('=', pos);
        if (eq_pos == std::string::npos || eq_pos == 0) {
            break;
        }

        // Extract key (scan backwards from '=' to find start of key)
        size_t key_start = eq_pos;
        while (key_start > 0 && upper_line[key_start - 1] != ' ' &&
               upper_line[key_start - 1] != '\t') {
            key_start--;
        }
        std::string key = upper_line.substr(key_start, eq_pos - key_start);

        // Extract value (from line, not upper_line, to preserve original case)
        size_t value_start = eq_pos + 1;
        std::string value;
        for (size_t i = value_start; i < line.size(); i++) {
            char c = line[i];
            if (c == ' ' || c == '\t') {
                break;
            }
            value += c;
        }

        // Try to match this parameter to an operation category
        // Include slicer-style short params (MESH, QGL, NOZZLE_CLEAN, etc.)
        auto match = helix::match_parameter_to_category(key, true);
        if (match) {
            // Determine if enabled based on semantic
            bool enabled = false;
            if (match->semantic == helix::ParameterSemantic::OPT_IN) {
                // PERFORM_*, DO_*, FORCE_*, or short names: enabled if value is truthy
                enabled = is_truthy_value(value);
            } else {
                // SKIP_*: enabled if value is falsy (meaning "don't skip")
                enabled = !is_truthy_value(value);
            }

            if (enabled) {
                // Check if we already have this operation type
                OperationType op_type = match->category;
                bool already_detected = std::any_of(
                    result.operations.begin(), result.operations.end(),
                    [op_type](const DetectedOperation& op) { return op.type == op_type; });

                if (!already_detected) {
                    DetectedOperation op;
                    op.type = op_type;
                    op.embedding = OperationEmbedding::MACRO_PARAMETER;
                    op.raw_line = line;
                    op.macro_name = "START_PRINT";
                    op.param_name = key;
                    op.param_value = value;
                    op.line_number = line_number;
                    op.byte_offset = byte_offset;

                    result.operations.push_back(std::move(op));

                    spdlog::trace(
                        "[GCodeOpsDetector] Detected {} via START_PRINT param {}={} at line {}",
                        operation_type_name(op_type), key, value, line_number);
                }
            }
        }

        // Move past this key=value pair
        pos = eq_pos + 1 + value.size();
    }
}

} // namespace gcode
} // namespace helix
