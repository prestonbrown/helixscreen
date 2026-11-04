// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_analyzer.h"

#include "core/text_io.h"
#include "helix_regex.h"
#include "i_moonraker_api.h"
#include "klipper_config_includes.h"
#include "macro_params.h"
#include "moonraker_types.h"
#include "operation_patterns.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <set>

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

namespace {

std::string chain_text(const PrintStartAnalysis& analysis) {
    if (analysis.macro_chain.empty()) {
        return analysis.macro_name;
    }
    std::string text = analysis.macro_chain.front();
    for (size_t i = 1; i < analysis.macro_chain.size(); ++i) {
        text += " -> " + analysis.macro_chain[i];
    }
    return text;
}

} // namespace

std::string PrintStartAnalysis::summary() const {
    if (!found) {
        return "No print start macro found";
    }

    std::string ss = fmt::format("{}: {} operations detected", chain_text(*this), total_ops_count);
    if (controllable_count > 0) {
        ss += fmt::format(" ({} controllable)", controllable_count);
    }

    if (!operations.empty()) {
        ss += " [";
        bool first = true;
        for (const auto& op : operations) {
            if (!first)
                ss += ", ";
            first = false;
            ss += op.name;
            if (op.has_skip_param) {
                ss += "(skip:" + op.skip_param_name + ")";
            }
        }
        ss += "]";
    }

    return ss;
}

// ============================================================================
// Operation Detection Patterns - Now using shared operation_patterns.h
// ============================================================================
// All patterns are defined in operation_patterns.h and accessed via
// kOperationKeywords[] and get_skip_variations()

// ============================================================================
// PrintStartAnalyzer Implementation
// ============================================================================

namespace {

/**
 * @brief Extract gcode content from a macro section in config file text
 */
std::string extract_gcode_from_section(const std::string& content, const std::string& content_lower,
                                       const std::string& section_start, size_t section_pos) {
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

struct MacroDefinition {
    std::string gcode;
    std::string file;
};

/// Uppercased macro name -> the definition Klipper runs
using MacroDefinitions = std::map<std::string, MacroDefinition>;

/// Every [gcode_macro NAME] in the active config. A header counts only at column 0,
/// as Klipper reads it, and a later definition replaces an earlier one in read order.
MacroDefinitions collect_macro_definitions(const std::map<std::string, std::string>& file_contents,
                                           const std::string& root_file) {
    static constexpr std::string_view HEADER = "[gcode_macro ";
    std::vector<helix::system::ConfigSegment> read_order;
    (void)helix::system::resolve_active_files(file_contents, root_file, 5, &read_order);

    MacroDefinitions definitions;
    std::map<std::string, std::string> lowered; // file -> lowercased content
    for (const auto& segment : read_order) {
        // resolve_active_files() emits segments only for files present in the map.
        const std::string& content = file_contents.find(segment.file)->second;
        auto lower_it = lowered.find(segment.file);
        if (lower_it == lowered.end()) {
            lower_it = lowered.emplace(segment.file, helix::text_io::to_lower(content)).first;
        }
        const std::string& content_lower = lower_it->second;

        for (size_t pos = content_lower.find(HEADER, segment.begin);
             pos != std::string::npos && pos < segment.end;
             pos = content_lower.find(HEADER, pos + 1)) {
            if (pos > 0 && content[pos - 1] != '\n') {
                continue;
            }
            size_t close = content.find(']', pos);
            if (close == std::string::npos || close > content.find('\n', pos)) {
                continue;
            }
            std::string gcode = extract_gcode_from_section(
                content, content_lower, content.substr(pos, close - pos + 1), pos);
            // A repeated section without its own gcode: keeps the earlier one, as Klipper merges.
            if (gcode.empty()) {
                continue;
            }
            std::string name =
                helix::text_io::to_upper(helix::text_io::trim(std::string_view(content).substr(
                    pos + HEADER.size(), close - pos - HEADER.size())));
            definitions[name] = {std::move(gcode), segment.file};
        }
    }
    return definitions;
}

/// The command a gcode line starts with, uppercased. Jinja {% %} tags are removed
/// first, so a call inside an inline conditional still counts. Empty for comments
/// and lines that start with an expression.
std::string called_command(std::string_view line) {
    std::string stripped;
    size_t pos = 0;
    while (pos < line.size()) {
        size_t open = line.find("{%", pos);
        stripped.append(line.substr(pos, open == std::string_view::npos ? open : open - pos));
        if (open == std::string_view::npos) {
            break;
        }
        size_t close = line.find("%}", open);
        if (close == std::string_view::npos) {
            break;
        }
        pos = close + 2;
    }
    size_t first = stripped.find_first_not_of(" \t");
    if (first == std::string::npos || stripped[first] == '#' || stripped[first] == ';' ||
        stripped[first] == '{') {
        return "";
    }
    size_t end = stripped.find_first_of(" \t{", first);
    return helix::text_io::to_upper(
        stripped.substr(first, end == std::string::npos ? end : end - first));
}

/// Klipper's own G/M codes are commands, never macros worth following.
bool is_numbered_gcode(const std::string& cmd) {
    return cmd.size() > 1 && (cmd[0] == 'G' || cmd[0] == 'M') &&
           std::isdigit(static_cast<unsigned char>(cmd[1]));
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// `token` appears in `line` (case-insensitive) with no identifier character
/// directly before it, nor after it when the token ends in one.
bool contains_token(std::string_view line, std::string_view token) {
    const std::string lower = helix::text_io::to_lower(line);
    const std::string needle = helix::text_io::to_lower(token);
    for (size_t pos = lower.find(needle); pos != std::string::npos;
         pos = lower.find(needle, pos + 1)) {
        const size_t after = pos + needle.size();
        if ((pos == 0 || !is_word_char(lower[pos - 1])) &&
            (after == lower.size() || !is_word_char(needle.back()) ||
             !is_word_char(lower[after]))) {
            return true;
        }
    }
    return false;
}

/// A `{% for ... in params %}` loop on the line, which re-sends every param the
/// macro received. An `{% if 'X' in params %}` guard is not one.
bool loops_over_params(std::string_view line) {
    const std::string lower = helix::text_io::to_lower(line);
    for (size_t pos = lower.find("in params"); pos != std::string::npos;
         pos = lower.find("in params", pos + 1)) {
        if (pos > 0 && is_word_char(lower[pos - 1])) {
            continue;
        }
        // Back to the start of the statement: `for` and the loop variables
        // (`p`, or `k, v`) must be all that precedes `in params`.
        size_t i = pos;
        while (i > 0 && (is_word_char(lower[i - 1]) || lower[i - 1] == ',' || lower[i - 1] == ' ' ||
                         lower[i - 1] == '\t')) {
            --i;
        }
        const std::string_view head =
            helix::text_io::trim(std::string_view(lower).substr(i, pos - i));
        if (head.size() > 4 && head.compare(0, 4, "for ") == 0) {
            return true;
        }
    }
    return false;
}

/// Whether a skip param sent to the calling macro reaches the macro `call_line` calls.
/// It does through {rawparams}, a `for ... in params` loop, or NAME={params.NAME};
/// a renamed or hardcoded argument does not.
bool call_forwards_param(std::string_view call_line, const std::string& param) {
    return contains_token(call_line, "rawparams") || loops_over_params(call_line) ||
           (contains_token(call_line, param + "=") && contains_token(call_line, "params." + param));
}

/// Parse the print start macro, then every gcode macro it calls, depth first,
/// merging their operations. A callee is skipped when it is itself a detected
/// operation (an override such as a BED_MESH_CALIBRATE wrapper), a G/M code,
/// undefined, or already analyzed. Iterative: the ESP32 runs this on a small stack.
/// `contributing` receives the macros that added at least one operation.
PrintStartAnalysis analyze_chain(const std::string& name, const std::string& gcode,
                                 const MacroDefinitions& definitions,
                                 std::vector<std::string>& contributing) {
    struct Visit {
        std::string name;
        const std::string* gcode;
        int depth;
        int caller;            ///< index into `visits`, -1 for the print start macro
        std::string call_line; ///< the caller's line that calls this macro
    };
    std::vector<Visit> visits;
    std::vector<int> pending; // indices into `visits`, top is next
    std::set<std::string> visited;

    PrintStartAnalysis result;
    visits.push_back({name, &gcode, 0, -1, ""});
    pending.push_back(0);

    while (!pending.empty()) {
        const int index = pending.back();
        pending.pop_back();
        if (!visited.insert(helix::text_io::to_upper(visits[index].name)).second) {
            continue;
        }
        result.macro_chain.push_back(visits[index].name);

        PrintStartAnalysis parsed =
            PrintStartAnalyzer::parse_macro(visits[index].name, *visits[index].gcode);
        if (index == 0) {
            result.known_params = std::move(parsed.known_params);
        }
        const size_t ops_before = result.operations.size();
        for (auto& op : parsed.operations) {
            for (int hop = index; op.has_skip_param && visits[hop].caller >= 0;
                 hop = visits[hop].caller) {
                if (!call_forwards_param(visits[hop].call_line, op.skip_param_name)) {
                    op.has_skip_param = false;
                    op.skip_param_name.clear();
                }
            }
            bool duplicate = std::any_of(
                result.operations.begin(), result.operations.end(),
                [&op](const PrintStartOperation& existing) { return existing.name == op.name; });
            if (!duplicate) {
                result.operations.push_back(std::move(op));
            }
        }
        if (result.operations.size() > ops_before) {
            contributing.push_back(visits[index].name);
        }

        if (visits[index].depth >= PrintStartAnalyzer::kMaxFollowDepth) {
            continue;
        }
        const size_t first_callee = visits.size();
        for (std::string_view line : helix::text_io::lines(*visits[index].gcode)) {
            std::string callee = called_command(line);
            if (callee.empty() || find_keyword(callee) || is_numbered_gcode(callee)) {
                continue;
            }
            auto def_it = definitions.find(callee);
            if (def_it == definitions.end()) {
                continue;
            }
            visits.push_back(
                {callee, &def_it->second.gcode, visits[index].depth + 1, index, std::string(line)});
        }
        // Pushed in reverse so the first call in the body is visited next.
        for (size_t i = visits.size(); i > first_callee; --i) {
            pending.push_back(static_cast<int>(i - 1));
        }
    }

    result.total_ops_count = result.operations.size();
    result.controllable_count = static_cast<size_t>(
        std::count_if(result.operations.begin(), result.operations.end(),
                      [](const PrintStartOperation& op) { return op.has_skip_param; }));
    result.is_controllable = result.controllable_count > 0;
    return result;
}

} // anonymous namespace

void PrintStartAnalyzer::analyze(const std::map<std::string, std::string>& file_contents,
                                 AnalysisCallback on_complete) {
    spdlog::debug("[PrintStartAnalyzer] Searching {} cached config files for macro...",
                  file_contents.size());

    const MacroDefinitions definitions = collect_macro_definitions(file_contents, kRootConfigFile);

    for (size_t i = 0; i < kMacroNamesCount; ++i) {
        auto def_it = definitions.find(kMacroNames[i]);
        if (def_it == definitions.end()) {
            continue;
        }
        const MacroDefinition& definition = def_it->second;
        std::vector<std::string> contributing;
        PrintStartAnalysis result =
            analyze_chain(kMacroNames[i], definition.gcode, definitions, contributing);
        result.found = true;
        result.macro_name = kMacroNames[i];
        result.source_file = definition.file;
        std::string sources;
        for (const auto& macro : contributing) {
            sources += (sources.empty() ? "" : ", ") + macro;
        }
        spdlog::info("[PrintStartAnalyzer] Found macro '{}' in {} ({} chars), operations from: {}",
                     kMacroNames[i], definition.file, definition.gcode.size(),
                     sources.empty() ? "none" : sources);
        spdlog::debug("[PrintStartAnalyzer] Analyzed {}", chain_text(result));
        if (on_complete) {
            on_complete(result);
        }
        return;
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
        [on_complete](const std::set<std::string>& /* active_files */,
                      const std::map<std::string, std::string>& file_contents) {
            // Use the synchronous cached analyze path
            PrintStartAnalyzer analyzer;
            analyzer.analyze(file_contents, on_complete);
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
    for (auto& param : parse_macro_params(gcode)) {
        result.known_params.push_back(std::move(param.name));
    }

    spdlog::debug("[PrintStartAnalyzer] Parsed {}: {} ops, {} controllable, {} params", macro_name,
                  result.total_ops_count, result.controllable_count, result.known_params.size());

    return result;
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
    size_t line_num = 0;

    for (std::string_view line_view : helix::text_io::lines(gcode)) {
        std::string line(line_view);
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
    std::string context_lower = helix::text_io::to_lower(context);

    // Helper lambda to check if a param is in an if statement or set statement
    auto check_param_in_context = [&](const std::string& param) -> bool {
        std::string param_lower = helix::text_io::to_lower(param);

        if (context_lower.find(param_lower) == std::string::npos) {
            return false;
        }

        // Verify it's in an if statement context
        // Look for patterns like: {% if ... param ...
        helix::Regex if_pattern(R"(\{%\s*if\s+.*)" + param_lower + R"(.*%\})", helix::Regex::ICase);
        if (helix::regex_search(context, if_pattern)) {
            out_param_name = param;
            spdlog::trace("[PrintStartAnalyzer] {} is controlled by {}", op_name, param);
            return true;
        }

        // Also check for variable assignment: {% set X = params.PARAM_...
        helix::Regex set_pattern(R"(\{%\s*set\s+\w+\s*=\s*params\.)" + param_lower,
                                 helix::Regex::ICase);
        if (helix::regex_search(context, set_pattern)) {
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

} // namespace helix
