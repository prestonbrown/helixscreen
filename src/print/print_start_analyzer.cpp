// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_analyzer.h"

#include "i_moonraker_api.h"
#include "klipper_config_includes.h"
#include "moonraker_types.h"
#include "operation_patterns.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

namespace helix {

// ============================================================================
// Category Helpers
// ============================================================================

const char* category_to_string(PrintStartOpCategory category) {
    // Delegate to the shared category_key() function from operation_patterns.h
    // Note: PrintStartOpCategory is now an alias for OperationCategory
    return category_key(category);
}

// ============================================================================
// PrintStartAnalysis Methods
// ============================================================================

bool PrintStartAnalysis::has_operation(PrintStartOpCategory category) const {
    return std::any_of(
        operations.begin(), operations.end(),
        [category](const PrintStartOperation& op) { return op.category == category; });
}

const PrintStartOperation* PrintStartAnalysis::get_operation(PrintStartOpCategory category) const {
    auto it =
        std::find_if(operations.begin(), operations.end(),
                     [category](const PrintStartOperation& op) { return op.category == category; });
    return (it != operations.end()) ? &(*it) : nullptr;
}

std::vector<const PrintStartOperation*> PrintStartAnalysis::get_uncontrollable_operations() const {
    std::vector<const PrintStartOperation*> result;
    for (const auto& op : operations) {
        if (!op.has_skip_param && op.category != PrintStartOpCategory::HOMING) {
            result.push_back(&op);
        }
    }
    return result;
}

std::string PrintStartAnalysis::summary() const {
    if (!found) {
        return "No print start macro found";
    }

    std::ostringstream ss;
    ss << macro_name << ": " << total_ops_count << " operations detected";
    if (controllable_count > 0) {
        ss << " (" << controllable_count << " controllable)";
    }

    if (!operations.empty()) {
        ss << " [";
        bool first = true;
        for (const auto& op : operations) {
            if (!first)
                ss << ", ";
            first = false;
            ss << op.name;
            if (op.has_skip_param) {
                ss << "(skip:" << op.skip_param_name << ")";
            }
        }
        ss << "]";
    }

    return ss.str();
}

// ============================================================================
// Operation Detection Patterns - Now using shared operation_patterns.h
// ============================================================================
// All patterns are defined in operation_patterns.h and accessed via
// OPERATION_KEYWORDS[] and get_skip_variations()

// ============================================================================
// PrintStartAnalyzer Implementation
// ============================================================================

namespace {

/**
 * @brief Extract gcode content from a macro section in config file text
 */
std::string extract_gcode_from_section(const std::string& content, const std::string& section_start,
                                       size_t section_pos) {
    // Find the gcode: line
    std::string content_lower = to_lower(content);

    size_t gcode_pos = content_lower.find("gcode:", section_pos);
    if (gcode_pos == std::string::npos) {
        return "";
    }

    // Find end of this section (next [section] or EOF)
    size_t section_end = content.find("\n[", section_pos + section_start.size());
    if (section_end == std::string::npos) {
        section_end = content.size();
    }

    // Make sure gcode: is within this section
    if (gcode_pos >= section_end) {
        return "";
    }

    // Find start of gcode content (after "gcode:" and newline)
    size_t gcode_content_start = content.find('\n', gcode_pos);
    if (gcode_content_start == std::string::npos || gcode_content_start >= section_end) {
        return "";
    }
    gcode_content_start++; // Skip the newline

    return content.substr(gcode_content_start, section_end - gcode_content_start);
}

} // anonymous namespace

void PrintStartAnalyzer::analyze(const std::set<std::string>& active_files,
                                 const std::map<std::string, std::string>& file_contents,
                                 AnalysisCallback on_complete) {
    spdlog::debug("[PrintStartAnalyzer] Searching {} cached config files for macro...",
                  active_files.size());

    for (const auto& filename : active_files) {
        auto content_it = file_contents.find(filename);
        if (content_it == file_contents.end()) {
            spdlog::debug("[PrintStartAnalyzer] No cached content for {}, skipping", filename);
            continue;
        }

        const std::string& content = content_it->second;

        // Search for each macro name variant
        for (size_t i = 0; i < MACRO_NAMES_COUNT; ++i) {
            std::string section = "[gcode_macro " + std::string(MACRO_NAMES[i]) + "]";

            if (contains_ci(content, section)) {
                std::string content_lower = to_lower(content);
                std::string section_lower = to_lower(section);

                size_t section_pos = content_lower.find(section_lower);
                std::string gcode = extract_gcode_from_section(content, section, section_pos);

                if (!gcode.empty()) {
                    spdlog::info("[PrintStartAnalyzer] Found macro '{}' in {} ({} chars)",
                                 MACRO_NAMES[i], filename, gcode.size());

                    PrintStartAnalysis result = parse_macro(MACRO_NAMES[i], gcode);
                    result.found = true;
                    result.macro_name = MACRO_NAMES[i];
                    result.source_file = filename;

                    if (on_complete) {
                        on_complete(result);
                    }
                    return;
                }
            }
        }
    }

    // Searched all files, macro not found
    spdlog::debug("[PrintStartAnalyzer] No PRINT_START macro found in any cached config file");
    PrintStartAnalysis result;
    result.found = false;
    if (on_complete) {
        on_complete(result);
    }
}

void PrintStartAnalyzer::analyze(IMoonrakerAPI* api, AnalysisCallback on_complete,
                                 ErrorCallback on_error) {
    if (!api) {
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "API not initialized";
            on_error(err);
        }
        return;
    }

    spdlog::debug("[PrintStartAnalyzer] Resolving active config files to find macro location...");

    // Resolve active config files WITH content to avoid re-downloading each file
    helix::system::resolve_active_config_files_with_content(
        *api,
        [on_complete](const std::set<std::string>& active_files,
                      const std::map<std::string, std::string>& file_contents) {
            // Use the synchronous cached analyze path
            PrintStartAnalyzer analyzer;
            analyzer.analyze(active_files, file_contents, on_complete);
        },
        [on_error](const std::string& error_msg) {
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::CONNECTION_LOST;
                err.message = error_msg;
                on_error(err);
            }
        });
}

PrintStartAnalysis PrintStartAnalyzer::parse_macro(const std::string& macro_name,
                                                   const std::string& gcode) {
    PrintStartAnalysis result;
    result.found = true;
    result.macro_name = macro_name;
    result.raw_gcode = gcode;

    // Detect operations
    result.operations = detect_operations(gcode);
    result.total_ops_count = result.operations.size();

    // Check each operation for skip/perform conditionals
    for (auto& op : result.operations) {
        std::string param_name;
        ParameterSemantic semantic = ParameterSemantic::OPT_OUT;
        if (detect_skip_conditional(gcode, op.name, param_name, semantic)) {
            op.has_skip_param = true;
            op.skip_param_name = param_name;
            op.param_semantic = semantic;
            result.controllable_count++;
        }
    }

    result.is_controllable = (result.controllable_count > 0);

    // Extract known parameters
    result.known_params = extract_parameters(gcode);

    spdlog::debug("[PrintStartAnalyzer] Parsed {}: {} ops, {} controllable, {} params", macro_name,
                  result.total_ops_count, result.controllable_count, result.known_params.size());

    return result;
}

std::string PrintStartAnalyzer::get_suggested_skip_param(const std::string& op_name) {
    // Use shared pattern registry
    const auto* kw = find_keyword(op_name);
    if (kw) {
        return kw->skip_param;
    }
    // Default: SKIP_ + operation name
    return "SKIP_" + op_name;
}

PrintStartOpCategory PrintStartAnalyzer::categorize_operation(const std::string& command) {
    // Extract just the command name (before any parameters)
    std::string cmd = command;
    auto space_pos = cmd.find(' ');
    if (space_pos != std::string::npos) {
        cmd = cmd.substr(0, space_pos);
    }

    // Use shared pattern registry
    // Note: PrintStartOpCategory is now an alias for OperationCategory
    const auto* kw = find_keyword(cmd);
    if (kw) {
        return kw->category;
    }

    return PrintStartOpCategory::UNKNOWN;
}

// ============================================================================
// Parsing Helpers
// ============================================================================

std::vector<PrintStartOperation> PrintStartAnalyzer::detect_operations(const std::string& gcode) {
    std::vector<PrintStartOperation> operations;

    // Split into lines and process each
    std::istringstream stream(gcode);
    std::string line;
    size_t line_num = 0;

    while (std::getline(stream, line)) {
        ++line_num;

        // Skip empty lines and comments
        auto first_non_space = line.find_first_not_of(" \t");
        if (first_non_space == std::string::npos)
            continue;
        if (line[first_non_space] == '#' || line[first_non_space] == ';')
            continue;

        // Skip Jinja2 control statements ({% ... %})
        if (line.find("{%") != std::string::npos)
            continue;

        // Extract the command (first word on the line, excluding Jinja2 expressions)
        std::string trimmed = line.substr(first_non_space);

        // Skip lines that are just Jinja2 expressions
        if (trimmed[0] == '{')
            continue;

        // Get the command name
        auto end_of_cmd = trimmed.find_first_of(" \t{");
        std::string cmd =
            (end_of_cmd != std::string::npos) ? trimmed.substr(0, end_of_cmd) : trimmed;

        // Check against shared pattern registry
        // Note: PrintStartOpCategory is now an alias for OperationCategory
        const auto* kw = find_keyword(cmd);
        if (kw) {
            PrintStartOperation op;
            op.name = cmd; // Store actual command, not just the pattern keyword
            op.category = kw->category;
            op.line_number = line_num;

            // Avoid duplicates (same operation appearing multiple times)
            bool duplicate = std::any_of(
                operations.begin(), operations.end(),
                [&op](const PrintStartOperation& existing) { return existing.name == op.name; });

            if (!duplicate) {
                operations.push_back(op);
                spdlog::trace("[PrintStartAnalyzer] Detected {} at line {}", op.name, line_num);
            }
        }
    }

    return operations;
}

bool PrintStartAnalyzer::detect_skip_conditional(const std::string& gcode,
                                                 const std::string& op_name,
                                                 std::string& out_param_name,
                                                 ParameterSemantic& out_semantic) {
    // Get the category to know which skip/perform param variations to look for
    PrintStartOpCategory category = categorize_operation(op_name);
    if (category == PrintStartOpCategory::UNKNOWN) {
        return false;
    }

    // First, find the operation in the gcode
    auto op_pos = gcode.find(op_name);
    if (op_pos == std::string::npos) {
        return false;
    }

    // Look backwards from the operation for an {% if ... %} block
    // Search up to 500 characters before the operation
    size_t search_start = (op_pos > 500) ? op_pos - 500 : 0;
    std::string context = gcode.substr(search_start, op_pos - search_start);
    std::string context_lower = helix::to_lower(context);

    // Helper lambda to check if a param is in an if statement or set statement
    auto check_param_in_context = [&](const std::string& param) -> bool {
        std::string param_lower = helix::to_lower(param);

        if (context_lower.find(param_lower) == std::string::npos) {
            return false;
        }

        // Verify it's in an if statement context
        // Look for patterns like: {% if ... param ...
        std::regex if_pattern(R"(\{%\s*if\s+.*)" + param_lower + R"(.*%\})", std::regex::icase);
        if (std::regex_search(context, if_pattern)) {
            out_param_name = param;
            spdlog::trace("[PrintStartAnalyzer] {} is controlled by {}", op_name, param);
            return true;
        }

        // Also check for variable assignment: {% set X = params.PARAM_...
        std::regex set_pattern(R"(\{%\s*set\s+\w+\s*=\s*params\.)" + param_lower,
                               std::regex::icase);
        if (std::regex_search(context, set_pattern)) {
            out_param_name = param;
            spdlog::trace("[PrintStartAnalyzer] {} is controlled by params.{}", op_name, param);
            return true;
        }

        return false;
    };

    // PrintStartOpCategory is now an alias for OperationCategory,
    // so we can pass it directly to the variation functions
    // First check SKIP_* patterns (opt-out semantics)
    auto skip_variations = get_all_skip_variations(category);
    for (const auto& param : skip_variations) {
        if (check_param_in_context(param)) {
            out_semantic = ParameterSemantic::OPT_OUT;
            return true;
        }
    }

    // Then check PERFORM_* patterns (opt-in semantics)
    auto perform_variations = get_all_perform_variations(category);
    for (const auto& param : perform_variations) {
        if (check_param_in_context(param)) {
            out_semantic = ParameterSemantic::OPT_IN;
            return true;
        }
    }

    return false;
}

std::vector<std::string> PrintStartAnalyzer::extract_parameters(const std::string& gcode) {
    std::vector<std::string> params;

    // Look for patterns like:
    //   params.BED
    //   params.EXTRUDER|default(...)
    //   {% set BED = params.BED|default(60) %}

    std::regex params_pattern(R"(params\.([A-Z_][A-Z0-9_]*))", std::regex::icase);

    std::smatch match;
    std::string::const_iterator search_start = gcode.cbegin();

    while (std::regex_search(search_start, gcode.cend(), match, params_pattern)) {
        std::string param = to_upper(match[1].str());

        // Avoid duplicates
        if (std::find(params.begin(), params.end(), param) == params.end()) {
            params.push_back(param);
        }

        search_start = match.suffix().first;
    }

    return params;
}

} // namespace helix
