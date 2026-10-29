// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console_line.h"

#include "text_io.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace helix::ui {

namespace {

// HTML span tag delimiters used by AFC/Happy Hare plugins (Mainsail-style)
constexpr const char kSpanOpen[] = "<span class=";
constexpr size_t kSpanOpenLen = sizeof(kSpanOpen) - 1;
constexpr const char kSpanClose[] = "</span>";
constexpr size_t kSpanCloseLen = sizeof(kSpanClose) - 1;

/// Maps "success--text" -> "success", etc. Empty for unrecognized classes.
std::string extract_color_class(const std::string& class_attr) {
    static constexpr std::pair<const char*, const char*> mappings[] = {
        {"success--text", "success"},
        {"info--text", "info"},
        {"warning--text", "warning"},
        {"error--text", "error"},
    };
    for (const auto& [pattern, name] : mappings) {
        if (class_attr.find(pattern) != std::string::npos) {
            return name;
        }
    }
    return {};
}

} // namespace

bool is_console_error_message(const std::string& message) {
    if (message.size() >= 2 && message[0] == '!' && message[1] == '!') {
        return true;
    }

    // Case-insensitive check for "error" at start (covers "Error:", "ERROR:", etc.)
    return helix::text_io::equals_ignore_case(std::string_view(message).substr(0, 5), "error");
}

bool is_console_temp_message(const std::string& message) {
    if (message.empty()) {
        return false;
    }

    // Temperature status messages look like:
    // "ok T:210.5 /210.0 B:60.2 /60.0"
    // "T:210.5 /210.0 B:60.2 /60.0"
    // "ok B:60.0 /60.0 T0:210.0 /210.0"

    // Check for "T:" or "B:" followed immediately by a digit, with "/" somewhere after
    size_t t_pos = message.find("T:");
    size_t b_pos = message.find("B:");

    auto check_temp_pattern = [&](size_t pos) -> bool {
        if (pos == std::string::npos)
            return false;
        // Require digit immediately after the colon (e.g. "T:210" not "T: see docs")
        size_t val_start = pos + 2; // skip "T:" or "B:"
        if (val_start < message.size() &&
            std::isdigit(static_cast<unsigned char>(message[val_start]))) {
            // Also require "/" somewhere after the pattern (target temp separator)
            size_t slash_pos = message.find('/', val_start);
            return slash_pos != std::string::npos;
        }
        return false;
    };

    return check_temp_pattern(t_pos) || check_temp_pattern(b_pos);
}

bool contains_console_html_spans(const std::string& message) {
    return message.find(kSpanOpen) != std::string::npos &&
           (message.find("success--text") != std::string::npos ||
            message.find("info--text") != std::string::npos ||
            message.find("warning--text") != std::string::npos ||
            message.find("error--text") != std::string::npos);
}

std::vector<ConsoleTextSegment> parse_console_html_spans(const std::string& message) {
    std::vector<ConsoleTextSegment> segments;

    size_t pos = 0;
    const size_t len = message.size();

    while (pos < len) {
        size_t span_start = message.find(kSpanOpen, pos);

        if (span_start == std::string::npos) {
            std::string remaining = message.substr(pos);
            if (!remaining.empty()) {
                segments.push_back({std::move(remaining), {}});
            }
            break;
        }

        // Add any text before the span as a plain segment
        if (span_start > pos) {
            segments.push_back({message.substr(pos, span_start - pos), {}});
        }

        // Find the class value (ends at >)
        size_t class_start = span_start + kSpanOpenLen;
        size_t class_end = message.find('>', class_start);

        if (class_end == std::string::npos) {
            // Malformed - add rest as plain text
            segments.push_back({message.substr(span_start), {}});
            break;
        }

        std::string color_class =
            extract_color_class(message.substr(class_start, class_end - class_start));

        // Find the closing </span>
        size_t content_start = class_end + 1;
        size_t span_close = message.find(kSpanClose, content_start);

        if (span_close == std::string::npos) {
            // No closing tag - add rest as colored text
            segments.push_back({message.substr(content_start), color_class});
            break;
        }

        std::string content = message.substr(content_start, span_close - content_start);
        if (!content.empty()) {
            segments.push_back({std::move(content), std::move(color_class)});
        }

        pos = span_close + kSpanCloseLen;
    }

    return segments;
}

const char* console_line_color_token(bool is_command, bool is_error) {
    if (is_error) {
        return "danger";
    }
    return is_command ? "text" : "success";
}

std::vector<ConsoleSpan> console_line_spans(const std::string& message, bool is_command,
                                            bool is_error) {
    const char* line_token = console_line_color_token(is_command, is_error);
    std::vector<ConsoleSpan> spans;
    if (is_command) {
        spans.push_back({"> ", "text"});
    }

    if (!contains_console_html_spans(message)) {
        spans.push_back({message, line_token});
        return spans;
    }

    for (auto& seg : parse_console_html_spans(message)) {
        // The "error" class draws in the "danger" token; the others share their token's name.
        const char* token = line_token;
        if (seg.color_class == "error") {
            token = "danger";
        } else if (seg.color_class == "success") {
            token = "success";
        } else if (seg.color_class == "info") {
            token = "info";
        } else if (seg.color_class == "warning") {
            token = "warning";
        }
        spans.push_back({std::move(seg.text), token});
    }
    return spans;
}

} // namespace helix::ui
