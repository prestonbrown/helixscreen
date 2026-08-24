// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_enhancer.h"

#include "ui_emergency_stop.h"

#include "i_moonraker_api.h"
#include "moonraker_types.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace helix {

// ============================================================================
// Input Validation Helpers
// ============================================================================

/**
 * @brief Validate that a parameter name is safe for Jinja2 template injection
 *
 * Defense-in-depth: parameter names come from get_skip_param_for_category() which
 * returns hardcoded strings, but the public API accepts any string. Reject names
 * containing characters that could be used for template injection.
 *
 * @param name Parameter name to validate
 * @return true if safe to use in Jinja2 templates
 */
static bool is_valid_param_name(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (unsigned char c : name) {
        // Only allow alphanumeric and underscore - standard Klipper param naming
        if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Code Generation (Pure, No Side Effects)
// ============================================================================

std::string PrintStartEnhancer::generate_param_declaration(const std::string& param_name) {
    // Security: Validate parameter name to prevent Jinja2 template injection
    if (!is_valid_param_name(param_name)) {
        spdlog::error("[PrintStartEnhancer] Invalid parameter name rejected: {}",
                      param_name.substr(0, std::min<size_t>(32, param_name.size())));
        return "";
    }

    // Generates: {% set SKIP_BED_MESH = params.SKIP_BED_MESH|default(0)|int %}
    //
    // This is the standard Klipper pattern for extracting a parameter with a default value.
    // The |int filter ensures we get an integer for comparison.
    return "{% set " + param_name + " = params." + param_name + "|default(0)|int %}";
}

std::string PrintStartEnhancer::generate_conditional_block(const std::string& original_line,
                                                           const std::string& param_name,
                                                           bool include_declaration) {
    // Security: Validate parameter name to prevent Jinja2 template injection
    if (!is_valid_param_name(param_name)) {
        spdlog::error("[PrintStartEnhancer] Invalid parameter name rejected in conditional block");
        return "";
    }

    std::ostringstream ss;

    // Preserve original indentation
    size_t indent_end = original_line.find_first_not_of(" \t");
    std::string indent =
        (indent_end != std::string::npos) ? original_line.substr(0, indent_end) : "";

    // Trim the original line for the operation
    std::string trimmed =
        (indent_end != std::string::npos) ? original_line.substr(indent_end) : original_line;

    // Remove trailing whitespace
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                                trimmed.back() == ' ' || trimmed.back() == '\t')) {
        trimmed.pop_back();
    }

    // Generate the wrapper
    if (include_declaration) {
        ss << indent << generate_param_declaration(param_name) << "\n";
    }
    ss << indent << "{% if " << param_name << " == 0 %}\n";
    ss << indent << "  " << trimmed << "\n";
    ss << indent << "{% endif %}";

    return ss.str();
}

MacroEnhancement PrintStartEnhancer::generate_wrapper(const PrintStartOperation& operation,
                                                      const std::string& skip_param_name) {
    MacroEnhancement enhancement;
    enhancement.operation_name = operation.name;
    enhancement.category = operation.category;
    enhancement.skip_param_name = skip_param_name;
    enhancement.line_number = operation.line_number;
    enhancement.original_line = operation.name; // Will be updated with actual line from macro
    enhancement.user_approved = false;

    // Generate the enhanced code block
    // Use 2-space indentation (common in Klipper macros)
    enhancement.enhanced_code =
        generate_conditional_block("  " + operation.name, skip_param_name, true);

    return enhancement;
}

std::string PrintStartEnhancer::apply_to_source(const std::string& original_macro,
                                                const std::vector<MacroEnhancement>& enhancements) {
    if (enhancements.empty()) {
        return original_macro;
    }

    // Filter to only approved enhancements
    std::vector<const MacroEnhancement*> approved;
    for (const auto& e : enhancements) {
        if (e.user_approved) {
            approved.push_back(&e);
        }
    }

    if (approved.empty()) {
        spdlog::debug("[PrintStartEnhancer] No approved enhancements to apply");
        return original_macro;
    }

    // Sort by line number (descending) so we apply from bottom to top
    // This preserves line numbers for earlier enhancements
    std::sort(approved.begin(), approved.end(),
              [](const MacroEnhancement* a, const MacroEnhancement* b) {
                  return a->line_number > b->line_number;
              });

    // Split macro into lines
    std::vector<std::string> lines;
    std::istringstream stream(original_macro);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Apply each enhancement
    for (const auto* enhancement : approved) {
        size_t line_idx = enhancement->line_number - 1; // 0-indexed

        if (line_idx >= lines.size()) {
            spdlog::warn("[PrintStartEnhancer] Line {} out of range for {}",
                         enhancement->line_number, enhancement->operation_name);
            continue;
        }

        // Verify the line contains the operation
        const std::string& target_line = lines[line_idx];
        if (target_line.find(enhancement->operation_name) == std::string::npos) {
            spdlog::warn("[PrintStartEnhancer] Line {} doesn't contain {}: '{}'",
                         enhancement->line_number, enhancement->operation_name,
                         target_line.substr(0, std::min<size_t>(50, target_line.size())));
            continue;
        }

        // Get the original indentation
        size_t indent_end = target_line.find_first_not_of(" \t");
        std::string indent =
            (indent_end != std::string::npos) ? target_line.substr(0, indent_end) : "";

        // Generate replacement with correct indentation
        std::string replacement =
            generate_conditional_block(target_line, enhancement->skip_param_name, true);

        // Replace the line with the enhanced version
        lines[line_idx] = replacement;

        spdlog::debug("[PrintStartEnhancer] Enhanced {} at line {} with {}",
                      enhancement->operation_name, enhancement->line_number,
                      enhancement->skip_param_name);
    }

    // Reconstruct the macro
    std::ostringstream result;
    for (size_t i = 0; i < lines.size(); ++i) {
        result << lines[i];
        if (i < lines.size() - 1) {
            result << "\n";
        }
    }

    return result.str();
}

bool PrintStartEnhancer::validate_jinja2_syntax(const std::string& code) {
    // Basic Jinja2 syntax validation:
    // 1. Balanced {% ... %} blocks
    // 2. Balanced {{ ... }} expressions
    // 3. Matching if/endif, for/endfor
    //
    // This is NOT a full parser - just catches common errors.

    int brace_percent_depth = 0;
    int brace_brace_depth = 0;

    // Simple state machine for brace matching
    for (size_t i = 0; i < code.size(); ++i) {
        if (i + 1 < code.size()) {
            if (code[i] == '{' && code[i + 1] == '%') {
                brace_percent_depth++;
                i++; // Skip the %
                continue;
            }
            if (code[i] == '%' && code[i + 1] == '}') {
                brace_percent_depth--;
                if (brace_percent_depth < 0) {
                    spdlog::warn("[PrintStartEnhancer] Unbalanced %} at position {}", i);
                    return false;
                }
                i++; // Skip the }
                continue;
            }
            if (code[i] == '{' && code[i + 1] == '{') {
                brace_brace_depth++;
                i++;
                continue;
            }
            if (code[i] == '}' && code[i + 1] == '}') {
                brace_brace_depth--;
                if (brace_brace_depth < 0) {
                    spdlog::warn("[PrintStartEnhancer] Unbalanced }} at position {}", i);
                    return false;
                }
                i++;
                continue;
            }
        }
    }

    if (brace_percent_depth != 0) {
        spdlog::warn("[PrintStartEnhancer] Unclosed {% block");
        return false;
    }
    if (brace_brace_depth != 0) {
        spdlog::warn("[PrintStartEnhancer] Unclosed {{ expression");
        return false;
    }

    // Check for if/endif matching
    std::regex if_pattern(R"(\{%\s*if\s)", std::regex::icase);
    std::regex endif_pattern(R"(\{%\s*endif\s*%\})", std::regex::icase);
    std::regex for_pattern(R"(\{%\s*for\s)", std::regex::icase);
    std::regex endfor_pattern(R"(\{%\s*endfor\s*%\})", std::regex::icase);

    auto count_matches = [](const std::string& text, const std::regex& pattern) -> int {
        return static_cast<int>(std::distance(
            std::sregex_iterator(text.begin(), text.end(), pattern), std::sregex_iterator()));
    };

    int if_count = count_matches(code, if_pattern);
    int endif_count = count_matches(code, endif_pattern);
    if (if_count != endif_count) {
        spdlog::warn("[PrintStartEnhancer] Mismatched if/endif: {} if, {} endif", if_count,
                     endif_count);
        return false;
    }

    int for_count = count_matches(code, for_pattern);
    int endfor_count = count_matches(code, endfor_pattern);
    if (for_count != endfor_count) {
        spdlog::warn("[PrintStartEnhancer] Mismatched for/endfor: {} for, {} endfor", for_count,
                     endfor_count);
        return false;
    }

    return true;
}

// ============================================================================
// Utility Methods
// ============================================================================

std::string PrintStartEnhancer::generate_backup_filename(const std::string& source_file) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now{};
    localtime_r(&time_t_now, &tm_now);

    std::ostringstream ss;
    ss << source_file << ".backup." << std::put_time(&tm_now, "%Y%m%d_%H%M%S");
    return ss.str();
}

std::string PrintStartEnhancer::get_skip_param_for_category(PrintStartOpCategory category) {
    switch (category) {
    case PrintStartOpCategory::BED_MESH:
        return "SKIP_BED_MESH";
    case PrintStartOpCategory::QGL:
        return "SKIP_QGL";
    case PrintStartOpCategory::Z_TILT:
        return "SKIP_Z_TILT";
    case PrintStartOpCategory::NOZZLE_CLEAN:
        return "SKIP_NOZZLE_CLEAN";
    case PrintStartOpCategory::HOMING:
        return "SKIP_HOMING";
    case PrintStartOpCategory::CHAMBER_SOAK:
        return "SKIP_SOAK";
    case PrintStartOpCategory::BED_LEVEL:
        return "SKIP_BED_LEVEL";
    case PrintStartOpCategory::UNKNOWN:
    default:
        return "";
    }
}

// ============================================================================
// Enhancement Workflow (Async, Side Effects)
// ============================================================================

void PrintStartEnhancer::apply_enhancements(IMoonrakerAPI* api, const std::string& macro_name,
                                            const std::string& source_file,
                                            const std::vector<MacroEnhancement>& enhancements,
                                            EnhancementProgressCallback on_progress,
                                            EnhancementCompleteCallback on_complete,
                                            EnhancementErrorCallback on_error) {
    // Concurrency guard: prevent double-click or concurrent operations
    if (operation_in_progress_.exchange(true)) {
        spdlog::warn("[PrintStartEnhancer] Enhancement operation already in progress");
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Enhancement operation already in progress";
            on_error(err);
        }
        return;
    }

    // Helper to clear the concurrency flag on any exit path
    auto clear_operation_flag = [this]() { operation_in_progress_.store(false); };

    // Safety check
    if (!api) {
        clear_operation_flag();
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "API not initialized";
            on_error(err);
        }
        return;
    }

    // Default to printer.cfg if no source file specified
    std::string config_file = source_file.empty() ? "printer.cfg" : source_file;

    // Filter to only approved enhancements
    std::vector<MacroEnhancement> approved;
    for (const auto& e : enhancements) {
        if (e.user_approved) {
            approved.push_back(e);
        }
    }

    if (approved.empty()) {
        clear_operation_flag();
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "No approved enhancements to apply";
            on_error(err);
        }
        return;
    }

    spdlog::info("[PrintStartEnhancer] Applying {} enhancements to {} in {}", approved.size(),
                 macro_name, config_file);

    // Capture lifetime token and operation flag pointer for async callbacks
    auto token = lifetime_.token();
    auto* op_flag = &operation_in_progress_;

    // Step 1: Create backup
    std::string backup_filename = generate_backup_filename(config_file);
    if (on_progress) {
        on_progress("Creating backup", 1, 4);
    }

    // Wrap error callback to clear operation flag and check lifetime.
    // L081 Mechanism C: defer to main — the bg-side store on op_flag (= &operation_in_progress_)
    // would be a UAF if the wizard owner is destroyed mid-op. token.defer gates on expired()
    // atomically on main, so the op_flag deref + on_error invoke are both guarded.
    auto safe_error = [token, op_flag, on_error](const MoonrakerError& err) {
        token.defer("PrintStartEnhancer::safe_error", [op_flag, on_error, err]() {
            if (op_flag) {
                op_flag->store(false);
            }
            if (on_error) {
                on_error(err);
            }
        });
    };

    create_backup(
        api, config_file, backup_filename,
        [this, api, macro_name, config_file, approved, backup_filename, on_progress, on_complete,
         safe_error, token, op_flag]() {
            // L081 Mechanism C: defer chained this-> work to main thread
            // (create_backup cb fires on HTTP bg thread). If token is expired,
            // the deferred lambda will be skipped — `this` is dead so op_flag
            // is moot.
            token.defer("PrintStartEnhancer::apply_modify_step", [this, api, macro_name,
                                                                  config_file, approved,
                                                                  backup_filename, on_progress,
                                                                  on_complete, safe_error, token,
                                                                  op_flag]() {
                spdlog::debug("[PrintStartEnhancer] Backup created: {}", backup_filename);

                // Step 2: Download, modify, upload config
                if (on_progress) {
                    on_progress("Modifying configuration", 2, 4);
                }

                modify_and_upload_config(
                    api, macro_name, config_file, approved,
                    [this, api, backup_filename, on_progress, on_complete, safe_error, token,
                     op_flag](size_t ops, size_t lines) {
                        // L081 Mechanism C: defer chained this-> work to main thread.
                        token.defer(
                            "PrintStartEnhancer::apply_restart_step",
                            [this, api, backup_filename, ops, lines, on_progress, on_complete,
                             safe_error, token, op_flag]() {
                                spdlog::debug(
                                    "[PrintStartEnhancer] Config modified: {} ops, {} lines", ops,
                                    lines);

                                // Step 3: Restart Klipper
                                if (on_progress) {
                                    on_progress("Restarting Klipper", 3, 4);
                                }

                                restart_klipper(
                                    api,
                                    [backup_filename, ops, lines, on_progress, on_complete, token,
                                     op_flag]() {
                                        // L081 Mechanism C: defer to main thread so
                                        // op_flag (member pointer) and on_complete
                                        // run safely.
                                        token.defer("PrintStartEnhancer::apply_complete_step",
                                                    [backup_filename, ops, lines, on_progress,
                                                     on_complete, op_flag]() {
                                                        // Clear operation flag on success
                                                        if (op_flag)
                                                            op_flag->store(false);

                                                        spdlog::info("[PrintStartEnhancer] Klipper "
                                                                     "restart initiated");

                                                        // Step 4: Complete
                                                        if (on_progress) {
                                                            on_progress("Complete", 4, 4);
                                                        }

                                                        EnhancementResult result;
                                                        result.success = true;
                                                        result.backup_filename = backup_filename;
                                                        result.operations_enhanced = ops;
                                                        result.lines_added = lines;

                                                        if (on_complete) {
                                                            on_complete(result);
                                                        }
                                                    });
                                    },
                                    safe_error);
                            });
                    },
                    safe_error);
            });
        },
        safe_error);
}

void PrintStartEnhancer::restore_from_backup(IMoonrakerAPI* api, const std::string& backup_filename,
                                             std::function<void()> on_complete,
                                             EnhancementErrorCallback on_error) {
    if (!api) {
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "API not initialized";
            on_error(err);
        }
        return;
    }

    spdlog::info("[PrintStartEnhancer] Restoring from backup: {}", backup_filename);
    auto token = lifetime_.token();

    // Wrap error callback with lifetime check
    auto safe_error = [on_error, token](const MoonrakerError& err) {
        if (!token.expired() && on_error) {
            on_error(err);
        }
    };

    // Copy backup over printer.cfg
    // Note: copy_file uses full paths like "config/printer.cfg"
    api->files().copy_file(
        "config/" + backup_filename, "config/printer.cfg",
        [this, api, on_complete, safe_error, token]() {
            // L081 Mechanism C: defer chained this-> work to main thread
            // (copy_file cb fires on HTTP bg thread).
            token.defer(
                "PrintStartEnhancer::restore_restart", [this, api, on_complete, safe_error]() {
                    spdlog::debug("[PrintStartEnhancer] Backup restored, restarting Klipper");
                    restart_klipper(api, on_complete, safe_error);
                });
        },
        safe_error);
}

void PrintStartEnhancer::list_backups(
    IMoonrakerAPI* api, std::function<void(const std::vector<std::string>&)> on_complete,
    EnhancementErrorCallback on_error) {
    if (!api) {
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "API not initialized";
            on_error(err);
        }
        return;
    }

    spdlog::debug("[PrintStartEnhancer] Listing backups");
    auto token = lifetime_.token();

    // Defer error callback to main so the lifetime check is atomic and
    // on_error's captured caller state is only touched from the main thread.
    auto safe_error = [on_error, token](const MoonrakerError& err) {
        token.defer("PrintStartEnhancer::list_backups_error", [on_error, err]() {
            if (on_error)
                on_error(err);
        });
    };

    // List files in config root matching printer.cfg.backup.*
    // list_files returns FileInfo objects via FileListCallback
    api->files().list_files(
        "config", "", false,
        [on_complete, token](const std::vector<FileInfo>& file_infos) {
            // Defer to main: lifetime check is atomic there, and on_complete's
            // own `this` capture (owned by the caller) is only safe on main.
            token.defer("PrintStartEnhancer::list_backups", [on_complete, file_infos]() {
                std::vector<std::string> backups;
                for (const auto& info : file_infos) {
                    if (info.filename.find("printer.cfg.backup.") == 0) {
                        backups.push_back(info.filename);
                    }
                }
                std::sort(backups.rbegin(), backups.rend());
                spdlog::debug("[PrintStartEnhancer] Found {} backups", backups.size());
                if (on_complete) {
                    on_complete(backups);
                }
            });
        },
        safe_error);
}

// ============================================================================
// Private Workflow Helpers
// ============================================================================

void PrintStartEnhancer::create_backup(IMoonrakerAPI* api, const std::string& source_file,
                                       const std::string& backup_filename,
                                       std::function<void()> on_success,
                                       EnhancementErrorCallback on_error) {
    // Copy source config file to backup
    // Note: copy_file uses full paths like "config/macros.cfg"
    api->files().copy_file("config/" + source_file, "config/" + backup_filename, on_success,
                           on_error);
}

void PrintStartEnhancer::modify_and_upload_config(
    IMoonrakerAPI* api, const std::string& macro_name, const std::string& source_file,
    const std::vector<MacroEnhancement>& enhancements,
    std::function<void(size_t ops, size_t lines)> on_success, EnhancementErrorCallback on_error) {
    auto token = lifetime_.token();

    // Download current config file
    // Note: download_file takes (root, path, on_success, on_error)
    api->transfers().download_file(
        "config", source_file,
        [api, macro_name, source_file, enhancements, on_success, on_error,
         token](const std::string& content) {
            // Defer parse + caller-callback to main. Body is bounded (a few
            // string ops on a config file); detector hates a bare bg-thread
            // expired check, and on_success/on_error capture caller state
            // that's only safe to touch from main.
            token.defer("PrintStartEnhancer::modify_and_upload_config", [api, macro_name,
                                                                         source_file, enhancements,
                                                                         on_success, on_error,
                                                                         content]() {
                // Find the macro section
                std::string section_start = "[gcode_macro " + macro_name + "]";

                // Case-insensitive search
                std::string content_lower = content;
                std::string section_lower = section_start;
                std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(),
                               ::tolower);
                std::transform(section_lower.begin(), section_lower.end(), section_lower.begin(),
                               ::tolower);

                size_t section_pos = content_lower.find(section_lower);
                if (section_pos == std::string::npos) {
                    if (on_error) {
                        MoonrakerError err;
                        err.type = MoonrakerErrorType::VALIDATION_ERROR;
                        err.message = "Macro " + macro_name + " not found in " + source_file;
                        on_error(err);
                    }
                    return;
                }

                // Find the end of this macro section FIRST (next [section] or EOF)
                // This ensures we don't accidentally find gcode: in a DIFFERENT section
                size_t section_end = content.find("\n[", section_pos + 1);
                if (section_end == std::string::npos) {
                    section_end = content.size();
                }

                // Find the gcode: line WITHIN this section's bounds
                size_t gcode_pos = content_lower.find("gcode:", section_pos);
                if (gcode_pos == std::string::npos || gcode_pos >= section_end) {
                    if (on_error) {
                        MoonrakerError err;
                        err.type = MoonrakerErrorType::VALIDATION_ERROR;
                        err.message = "No gcode: found in macro " + macro_name;
                        on_error(err);
                    }
                    return;
                }

                // Extract the macro gcode (everything from gcode: to section_end)
                size_t gcode_content_start = content.find('\n', gcode_pos);
                if (gcode_content_start == std::string::npos) {
                    gcode_content_start = gcode_pos + 6; // After "gcode:"
                } else {
                    gcode_content_start++; // Skip the newline
                }

                std::string macro_gcode =
                    content.substr(gcode_content_start, section_end - gcode_content_start);

                // Apply enhancements
                std::string modified_gcode = apply_to_source(macro_gcode, enhancements);

                // Validate the result
                if (!validate_jinja2_syntax(modified_gcode)) {
                    if (on_error) {
                        MoonrakerError err;
                        err.type = MoonrakerErrorType::VALIDATION_ERROR;
                        err.message = "Generated code has syntax errors";
                        on_error(err);
                    }
                    return;
                }

                // Reconstruct the config file
                std::string modified_content;
                modified_content.reserve(content.size() + modified_gcode.size());
                modified_content += content.substr(0, gcode_content_start);
                modified_content += modified_gcode;
                modified_content += content.substr(section_end);

                // Count lines added
                size_t original_lines = std::count(macro_gcode.begin(), macro_gcode.end(), '\n');
                size_t new_lines = std::count(modified_gcode.begin(), modified_gcode.end(), '\n');
                size_t lines_added = (new_lines > original_lines) ? new_lines - original_lines : 0;

                spdlog::debug("[PrintStartEnhancer] Modified config: {} → {} lines (+{})",
                              original_lines, new_lines, lines_added);

                // Upload modified config
                // Note: upload_file takes (root, path, content, on_success, on_error)
                api->transfers().upload_file(
                    "config", source_file, modified_content,
                    [enhancements, lines_added, on_success]() {
                        size_t ops_count = 0;
                        for (const auto& e : enhancements) {
                            if (e.user_approved) {
                                ops_count++;
                            }
                        }
                        if (on_success) {
                            on_success(ops_count, lines_added);
                        }
                    },
                    on_error);
            });
        },
        on_error);
}

void PrintStartEnhancer::restart_klipper(IMoonrakerAPI* api, std::function<void()> on_success,
                                         EnhancementErrorCallback on_error) {
    // Suppress recovery modal during intentional restart.
    // Without this, users see error modals even though we just told Klipper to restart.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::NORMAL);

    api->restart_klipper(on_success, on_error);
}

} // namespace helix
