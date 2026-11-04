// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_file_modifier.h"

#include "app_globals.h"
#include "core/text_io.h"
#include "gcode_streaming_config.h"
#include "helix_fs.h"
#include "helix_regex.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string_view>
#include <unordered_map>

namespace helix {
namespace gcode {

namespace {

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    for (std::string_view line : helix::text_io::lines(text)) {
        out.emplace_back(line);
    }
    return out;
}

} // namespace

// ============================================================================
// GCodeFileModifier implementation
// ============================================================================

namespace {

/// @p a and @p b with surrounding whitespace (and a CR) ignored.
bool same_line(std::string_view a, std::string_view b) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return s;
    };
    return trim(a) == trim(b);
}

} // namespace

void GCodeFileModifier::add_modification(Modification mod) {
    modifications_.push_back(std::move(mod));
}

void GCodeFileModifier::clear_modifications() {
    modifications_.clear();
}

void GCodeFileModifier::sort_modifications() {
    // Sort by line number descending (process from end to start)
    // This preserves line numbers for earlier modifications
    std::sort(
        modifications_.begin(), modifications_.end(),
        [](const Modification& a, const Modification& b) { return a.line_number > b.line_number; });
}

std::string GCodeFileModifier::comment_out_line(const std::string& line,
                                                const std::string& reason) {
    std::string result = "; ";
    result += line;
    if (!reason.empty()) {
        result += "  ; [HelixScreen: ";
        result += reason;
        result += "]";
    }
    return result;
}

void GCodeFileModifier::apply_single_modification(std::vector<std::string>& lines,
                                                  const Modification& mod,
                                                  ModificationResult& result) {
    // Line numbers are 1-indexed, vector is 0-indexed
    size_t idx = mod.line_number - 1;

    if (idx >= lines.size()) {
        spdlog::warn("[GCodeFileModifier] Line {} out of range (file has {} lines)",
                     mod.line_number, lines.size());
        return;
    }

    if (!mod.expected_line.empty() && !same_line(lines[idx], mod.expected_line)) {
        result.error_message =
            "line " + std::to_string(mod.line_number) + " is not the line the scan found there";
        return;
    }

    size_t end_idx = (mod.end_line_number > 0) ? mod.end_line_number - 1 : idx;
    if (end_idx >= lines.size()) {
        end_idx = lines.size() - 1;
    }

    switch (mod.type) {
    case ModificationType::COMMENT_OUT: {
        // Comment out from idx to end_idx (inclusive)
        for (size_t i = idx; i <= end_idx; ++i) {
            // Skip if already a comment
            if (!lines[i].empty() && lines[i][0] == ';') {
                continue;
            }
            lines[i] = comment_out_line(lines[i], mod.comment);
            result.lines_modified++;
        }
        spdlog::debug("[GCodeFileModifier] Commented out lines {}-{}", mod.line_number,
                      end_idx + 1);
        break;
    }

    case ModificationType::DELETE: {
        // Delete from idx to end_idx (inclusive)
        size_t count = end_idx - idx + 1;
        lines.erase(lines.begin() + static_cast<long>(idx),
                    lines.begin() + static_cast<long>(end_idx + 1));
        result.lines_removed += count;
        spdlog::debug("[GCodeFileModifier] Deleted {} lines starting at {}", count,
                      mod.line_number);
        break;
    }

    case ModificationType::INJECT_BEFORE: {
        // Split the gcode to inject into lines
        std::vector<std::string> new_lines = split_lines(mod.gcode);

        // Insert before idx
        lines.insert(lines.begin() + static_cast<long>(idx), new_lines.begin(), new_lines.end());
        result.lines_added += new_lines.size();
        spdlog::debug("[GCodeFileModifier] Injected {} lines before line {}", new_lines.size(),
                      mod.line_number);
        break;
    }

    case ModificationType::INJECT_AFTER: {
        // Split the gcode to inject into lines
        std::vector<std::string> new_lines = split_lines(mod.gcode);

        // Insert after idx (at idx+1)
        lines.insert(lines.begin() + static_cast<long>(idx + 1), new_lines.begin(),
                     new_lines.end());
        result.lines_added += new_lines.size();
        spdlog::debug("[GCodeFileModifier] Injected {} lines after line {}", new_lines.size(),
                      mod.line_number);
        break;
    }

    case ModificationType::REPLACE: {
        // Replace lines from idx to end_idx with new gcode
        size_t count = end_idx - idx + 1;

        // Split the replacement gcode into lines
        std::vector<std::string> new_lines = split_lines(mod.gcode);

        // Erase old lines
        lines.erase(lines.begin() + static_cast<long>(idx),
                    lines.begin() + static_cast<long>(end_idx + 1));

        // Insert new lines
        lines.insert(lines.begin() + static_cast<long>(idx), new_lines.begin(), new_lines.end());

        result.lines_removed += count;
        result.lines_added += new_lines.size();
        result.lines_modified++;
        spdlog::debug("[GCodeFileModifier] Replaced {} lines at {} with {} lines", count,
                      mod.line_number, new_lines.size());
        break;
    }
    }
}

std::string GCodeFileModifier::apply_to_content(const std::string& content) {
    if (modifications_.empty()) {
        return content;
    }

    // Split content into lines
    std::vector<std::string> lines = split_lines(content);

    // Sort modifications by line number (descending)
    sort_modifications();

    // Apply each modification
    ModificationResult result;
    for (const auto& mod : modifications_) {
        apply_single_modification(lines, mod, result);
    }

    // Join lines back together
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) {
            out += '\n';
        }
    }

    return out;
}

ModificationResult GCodeFileModifier::apply(const std::string& filepath) {
    // Check file size to decide between buffered and streaming modes
    const auto size = helix::text_io::file_size(filepath);
    if (!size || !helix::fs::is_regular_file(filepath)) {
        ModificationResult result;
        result.success = false;
        result.error_message = "Failed to get file size: " + filepath;
        spdlog::error("[GCodeFileModifier] {}", result.error_message);
        return result;
    }

    const auto file_size = *size;
    if (helix::should_use_gcode_streaming(file_size)) {
        spdlog::info("[GCodeFileModifier] File {} ({} MB) - streaming mode",
                     std::string(helix::fs::filename(filepath)), file_size / (1024 * 1024));
        return apply_streaming(filepath);
    }

    return apply_buffered(filepath);
}

ModificationResult GCodeFileModifier::apply_buffered(const std::string& filepath) {
    ModificationResult result;

    // Read original file
    helix::text_io::LineReader infile(filepath);
    if (!infile) {
        result.success = false;
        result.error_message = "Failed to open file: " + filepath;
        spdlog::error("[GCodeFileModifier] {}", result.error_message);
        return result;
    }

    // Read all lines
    std::vector<std::string> lines;
    std::string line;
    bool ended_with_newline = false;
    while (infile.next(line)) {
        lines.push_back(line);
        result.original_size += line.size() + 1; // +1 for newline
        ended_with_newline = infile.last_had_delimiter();
    }

    spdlog::info("[GCodeFileModifier] Loaded {} lines ({} bytes) from {}", lines.size(),
                 result.original_size, std::string(helix::fs::filename(filepath)));

    if (modifications_.empty()) {
        // No modifications - just copy to temp
        result.success = true;
        result.modified_path = generate_temp_path(filepath);

        auto outfile = helix::text_io::open_file(result.modified_path, "wb");
        if (!outfile) {
            result.success = false;
            result.error_message = "Failed to create temp file: " + result.modified_path;
            return result;
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            helix::text_io::write_all(outfile.get(), lines[i]);
            if (i + 1 < lines.size() || ended_with_newline) {
                std::fputc('\n', outfile.get());
            }
        }
        result.modified_size = result.original_size;
        return result;
    }

    // Sort modifications by line number (descending)
    sort_modifications();

    spdlog::info("[GCodeFileModifier] Applying {} modifications (buffered mode)",
                 modifications_.size());

    // Apply each modification
    for (const auto& mod : modifications_) {
        apply_single_modification(lines, mod, result);
    }
    if (!result.error_message.empty()) {
        result.success = false;
        spdlog::error("[GCodeFileModifier] Refusing to rewrite {}: {}", filepath,
                      result.error_message);
        return result;
    }

    // Generate temp file path
    result.modified_path = generate_temp_path(filepath);

    // Write modified file
    auto outfile = helix::text_io::open_file(result.modified_path, "wb");
    if (!outfile) {
        result.success = false;
        result.error_message = "Failed to create temp file: " + result.modified_path;
        spdlog::error("[GCodeFileModifier] {}", result.error_message);
        return result;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        helix::text_io::write_all(outfile.get(), lines[i]);
        result.modified_size += lines[i].size();
        if (i + 1 < lines.size() || ended_with_newline) {
            std::fputc('\n', outfile.get());
            result.modified_size++;
        }
    }
    helix::text_io::close(outfile);

    result.success = true;
    spdlog::info("[GCodeFileModifier] Created modified file: {} ({} bytes, +{} -{} lines changed)",
                 result.modified_path, result.modified_size, result.lines_added,
                 result.lines_removed);

    return result;
}

std::unordered_map<size_t, Modification> GCodeFileModifier::build_streaming_lookup() const {
    std::unordered_map<size_t, Modification> lookup;

    for (const auto& mod : modifications_) {
        // For range modifications, we create entries for each line but note
        // that streaming mode has limitations with multi-line operations
        size_t end_line = (mod.end_line_number > 0) ? mod.end_line_number : mod.line_number;

        if (mod.end_line_number > 0 && mod.type != ModificationType::COMMENT_OUT &&
            mod.type != ModificationType::DELETE) {
            spdlog::warn("[GCodeFileModifier] Streaming mode: multi-line {} not fully supported, "
                         "processing line {} only",
                         static_cast<int>(mod.type), mod.line_number);
            end_line = mod.line_number;
        }

        for (size_t line = mod.line_number; line <= end_line; ++line) {
            // Create a modified copy for each line in the range
            Modification line_mod = mod;
            line_mod.line_number = line;
            line_mod.end_line_number = 0; // Reset to single-line for streaming
            lookup[line] = line_mod;
        }
    }

    return lookup;
}

ModificationResult GCodeFileModifier::apply_streaming(const std::string& filepath) {
    ModificationResult result;

    // Open input file
    helix::text_io::LineReader infile(filepath);
    if (!infile) {
        result.success = false;
        result.error_message = "Failed to open file: " + filepath;
        spdlog::error("[GCodeFileModifier] {}", result.error_message);
        return result;
    }

    // Generate output path
    result.modified_path = generate_temp_path(filepath);

    // Open output file
    auto outfile = helix::text_io::open_file(result.modified_path, "wb");
    if (!outfile) {
        result.success = false;
        result.error_message = "Failed to create temp file: " + result.modified_path;
        spdlog::error("[GCodeFileModifier] {}", result.error_message);
        return result;
    }

    // Build lookup map for O(1) per-line checks
    auto lookup = build_streaming_lookup();

    spdlog::info("[GCodeFileModifier] Processing file in streaming mode ({} modifications)",
                 modifications_.size());

    // Process line by line
    std::string line;
    size_t line_number = 0;
    bool first_line = true;
    bool ended_with_newline = false;

    while (infile.next(line)) {
        line_number++;
        result.original_size += line.size() + 1;
        ended_with_newline = infile.last_had_delimiter();

        // Check if this line has a modification
        auto it = lookup.find(line_number);
        if (it != lookup.end()) {
            const auto& mod = it->second;

            if (!mod.expected_line.empty() && !same_line(line, mod.expected_line)) {
                helix::text_io::close(outfile);
                std::remove(result.modified_path.c_str());
                result.success = false;
                result.error_message =
                    "line " + std::to_string(line_number) + " is not the line the scan found there";
                spdlog::error("[GCodeFileModifier] Refusing to rewrite {}: {}", filepath,
                              result.error_message);
                return result;
            }

            switch (mod.type) {
            case ModificationType::COMMENT_OUT: {
                // Skip if already a comment
                if (!line.empty() && line[0] == ';') {
                    if (!first_line)
                        std::fputc('\n', outfile.get());
                    helix::text_io::write_all(outfile.get(), line);
                } else {
                    std::string commented = comment_out_line(line, mod.comment);
                    if (!first_line)
                        std::fputc('\n', outfile.get());
                    helix::text_io::write_all(outfile.get(), commented);
                    result.lines_modified++;
                }
                result.modified_size += line.size() + (first_line ? 0 : 1);
                first_line = false;
                break;
            }

            case ModificationType::DELETE: {
                // Simply skip the line (don't write it)
                result.lines_removed++;
                // Note: we don't add to modified_size for deleted lines
                break;
            }

            case ModificationType::INJECT_BEFORE: {
                // Write injected content first
                for (std::string_view inject_line : helix::text_io::lines(mod.gcode)) {
                    if (!first_line)
                        std::fputc('\n', outfile.get());
                    helix::text_io::write_all(outfile.get(), inject_line);
                    result.modified_size += inject_line.size() + (first_line ? 0 : 1);
                    result.lines_added++;
                    first_line = false;
                }
                // Then write the original line
                if (!first_line)
                    std::fputc('\n', outfile.get());
                helix::text_io::write_all(outfile.get(), line);
                result.modified_size += line.size() + 1;
                first_line = false;
                break;
            }

            case ModificationType::INJECT_AFTER: {
                // Write original line first
                if (!first_line)
                    std::fputc('\n', outfile.get());
                helix::text_io::write_all(outfile.get(), line);
                result.modified_size += line.size() + (first_line ? 0 : 1);
                first_line = false;
                // Then write injected content
                for (std::string_view inject_line : helix::text_io::lines(mod.gcode)) {
                    std::fputc('\n', outfile.get());
                    helix::text_io::write_all(outfile.get(), inject_line);
                    result.modified_size += inject_line.size() + 1;
                    result.lines_added++;
                }
                break;
            }

            case ModificationType::REPLACE: {
                // Write replacement content instead of original
                bool first_replace = true;
                for (std::string_view replace_line : helix::text_io::lines(mod.gcode)) {
                    if (!first_line && !first_replace)
                        std::fputc('\n', outfile.get());
                    if (!first_line && first_replace)
                        std::fputc('\n', outfile.get());
                    helix::text_io::write_all(outfile.get(), replace_line);
                    result.modified_size += replace_line.size() + (first_line ? 0 : 1);
                    first_replace = false;
                    first_line = false;
                }
                result.lines_modified++;
                break;
            }
            }
        } else {
            // No modification - write line as-is
            if (!first_line)
                std::fputc('\n', outfile.get());
            helix::text_io::write_all(outfile.get(), line);
            result.modified_size += line.size() + (first_line ? 0 : 1);
            first_line = false;
        }
    }

    // Klipper's virtual_sdcard never runs a final line the file does not
    // terminate, so the copy ends the way its source did.
    if (!first_line && ended_with_newline) {
        std::fputc('\n', outfile.get());
        result.modified_size++;
    }
    helix::text_io::close(outfile);

    result.success = true;
    spdlog::info("[GCodeFileModifier] Streaming complete: {} ({} bytes, +{} -{} lines)",
                 result.modified_path, result.modified_size, result.lines_added,
                 result.lines_removed);

    return result;
}

bool GCodeFileModifier::disable_operation(const DetectedOperation& op) {
    switch (op.embedding) {
    case OperationEmbedding::DIRECT_COMMAND:
    case OperationEmbedding::MACRO_CALL:
        // Comment out the line containing the operation
        add_modification(Modification::comment_out(op.line_number, "Disabled " + op.display_name())
                             .expecting(op.raw_line));
        spdlog::debug("[GCodeFileModifier] Will disable {} at line {}", op.display_name(),
                      op.line_number);
        return true;

    case OperationEmbedding::MACRO_PARAMETER:
        // Need to modify the parameter, not comment out the whole line
        return disable_macro_parameter(op);

    case OperationEmbedding::NOT_FOUND:
        // Nothing to disable
        return false;
    }

    return false;
}

bool GCodeFileModifier::disable_macro_parameter(const DetectedOperation& op) {
    if (op.embedding != OperationEmbedding::MACRO_PARAMETER) {
        return false;
    }

    if (op.param_name.empty() || op.raw_line.empty()) {
        spdlog::warn("[GCodeFileModifier] Cannot disable macro parameter: missing param name or "
                     "raw line");
        return false;
    }

    // Build regex pattern to find PARAM_NAME=value (case-insensitive)
    // Replace the value with 0 or FALSE
    std::string pattern = op.param_name + R"(=\S+)";
    helix::Regex re(pattern, helix::Regex::ICase);

    // Determine replacement value
    std::string replacement = op.param_name + "=0";

    // Check if original value was boolean-like
    std::string upper_value = op.param_value;
    upper_value = helix::text_io::to_upper(upper_value);
    if (upper_value == "TRUE" || upper_value == "YES") {
        replacement = op.param_name + "=FALSE";
    }

    // Build the modified line
    std::string modified_line = helix::regex_replace(op.raw_line, re, replacement);

    // Add a replacement modification
    add_modification(
        Modification::replace(op.line_number, modified_line, "Disabled " + op.param_name)
            .expecting(op.raw_line));

    spdlog::debug("[GCodeFileModifier] Will replace {} param at line {} with value 0/FALSE",
                  op.param_name, op.line_number);

    return true;
}

void GCodeFileModifier::disable_operations(const ScanResult& scan_result,
                                           const std::vector<OperationType>& types_to_disable) {
    for (OperationType type : types_to_disable) {
        auto ops = scan_result.get_operations(type);
        for (const auto& op : ops) {
            disable_operation(op);
        }
    }
}

bool GCodeFileModifier::add_print_start_skip_params(
    const ScanResult& scan_result,
    const std::vector<std::pair<std::string, std::string>>& skip_params) {
    if (!scan_result.print_start.found || skip_params.empty()) {
        spdlog::debug("[GCodeFileModifier] Cannot add skip params: PRINT_START {} found, {} params",
                      scan_result.print_start.found ? "" : "NOT", skip_params.size());
        return false;
    }

    // Build the modified line with skip params appended
    std::string modified_line = scan_result.print_start.with_skip_params(skip_params);

    // Create a REPLACE modification for the PRINT_START line
    add_modification(Modification::replace(scan_result.print_start.line_number, modified_line,
                                           "HelixScreen: Added skip parameters")
                         .expecting(scan_result.print_start.raw_line));

    spdlog::info("[GCodeFileModifier] Adding skip params to {} at line {}: {}",
                 scan_result.print_start.macro_name, scan_result.print_start.line_number,
                 modified_line.substr(0, 80));

    return true;
}

std::string GCodeFileModifier::generate_temp_path(const std::string& original_path) {
    // Generate unique temp file path in persistent cache directory
    // Format: <cache_dir>/mod_XXXXXX_filename.gcode

    std::string cache_dir = get_helix_cache_dir("gcode_mod");
    if (cache_dir.empty()) {
        spdlog::error("[GCodeFileModifier] No writable cache directory");
        return "";
    }

    std::string filename = std::string(helix::fs::filename(original_path));

    // Generate random suffix
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    int suffix = dis(gen);

    return fmt::format("{}/mod_{}_{}", cache_dir, suffix, filename);
}

size_t GCodeFileModifier::cleanup_temp_files(int max_age_seconds) {
    size_t deleted = 0;

    std::string cache_dir = get_helix_cache_dir("gcode_mod");
    if (cache_dir.empty()) {
        return 0;
    }

    const auto entries = helix::fs::list_dir(cache_dir);
    if (!entries) {
        spdlog::warn("[GCodeFileModifier] Error cleaning up temp files: {}", std::strerror(errno));
        return 0;
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    for (const auto& entry : *entries) {
        if (!entry.is_regular) {
            continue;
        }

        const std::string& name = entry.name;
        if (name.rfind("mod_", 0) != 0) {
            continue; // Not our file
        }

        // Check file age
        const auto mtime = helix::fs::mtime_ns(entry.path);
        if (!mtime) {
            continue; // Removed since the listing
        }
        const auto age = (now_ns - *mtime) / 1'000'000'000;

        if (age > max_age_seconds) {
            if (!helix::fs::remove(entry.path)) {
                spdlog::warn("[GCodeFileModifier] Error cleaning up temp file {}: {}", name,
                             std::strerror(errno));
                continue;
            }
            deleted++;
            spdlog::debug("[GCodeFileModifier] Cleaned up old temp file: {}", name);
        }
    }

    if (deleted > 0) {
        spdlog::info("[GCodeFileModifier] Cleaned up {} temp files", deleted);
    }

    return deleted;
}

} // namespace gcode
} // namespace helix
