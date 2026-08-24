// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure gcode tool remapper for Snapmaker U1 / ACE.
//
// On the U1 the logical->physical tool mapping is IDENTITY-baked across three
// command families in a sliced file. A remap of logical tool a -> physical head
// b must rewrite ALL THREE consistently:
//   1. Prestart:  SM_PRINT_AUTO_FEED / SM_PRINT_EXTRUDER_PREHEAT /
//                 SM_PRINT_FLOW_CALIBRATE  with  EXTRUDER=<n>
//   2. Body:      a bare "T<n>" toolchange line
//   3. Temps:     M104 / M109 lines carrying a "T<n>" tool token
//
// Matching is deliberately conservative so comment lines and unrelated commands
// are never touched. Each line is transformed from its ORIGINAL text only, so a
// swap (1<->2) does not chain. No regex callbacks are used (GCC 7.5 on some
// cross targets lacks a reliable functional regex_replace overload) -- the
// single-token splice is hand-rolled.

#include "gcode_tool_remapper.h"

#include <cctype>

namespace helix {

namespace {

int mapped(int n, const std::map<int, int>& remap) {
    auto it = remap.find(n);
    return it != remap.end() ? it->second : n;
}

// Parse an unsigned integer starting at `pos` (which must point at a digit).
// On return `pos` is advanced past the last digit. Caller guarantees a digit.
int parse_uint(const std::string& s, size_t& pos) {
    int value = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        value = value * 10 + (s[pos] - '0');
        ++pos;
    }
    return value;
}

bool starts_with(const std::string& s, const char* prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= s.size() || s[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

// --- Family 2: bare toolchange line "T<digits>" (optional trailing whitespace) ---
// Returns true and fills `out` if `line` is a bare toolchange whose index is
// remapped. The trailing whitespace (if any) is preserved.
bool try_bare_toolchange(const std::string& line, const std::map<int, int>& remap,
                         std::string& out) {
    if (line.size() < 2 || line[0] != 'T') {
        return false;
    }
    size_t pos = 1;
    if (!std::isdigit(static_cast<unsigned char>(line[pos]))) {
        return false;
    }
    int idx = parse_uint(line, pos);
    // Whatever remains must be whitespace only (e.g. trailing \r or spaces).
    std::string tail = line.substr(pos);
    for (char c : tail) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false; // e.g. "T1X" or "TOOL" -- not a bare toolchange
        }
    }
    int m = mapped(idx, remap);
    if (m == idx) {
        return false; // unmapped: leave untouched (preserves exact bytes)
    }
    out = "T" + std::to_string(m) + tail;
    return true;
}

// --- Family 1: prestart "SM_PRINT_<CMD> EXTRUDER=<digits><rest>" ---
bool try_prestart(const std::string& line, const std::map<int, int>& remap, std::string& out) {
    static const char* PREFIXES[] = {
        "SM_PRINT_AUTO_FEED EXTRUDER=",
        "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=",
        "SM_PRINT_FLOW_CALIBRATE EXTRUDER=",
    };
    for (const char* prefix : PREFIXES) {
        if (!starts_with(line, prefix)) {
            continue;
        }
        size_t prefix_len = std::string(prefix).size();
        if (prefix_len >= line.size() ||
            !std::isdigit(static_cast<unsigned char>(line[prefix_len]))) {
            return false; // "EXTRUDER=" not followed by a number
        }
        size_t pos = prefix_len;
        int idx = parse_uint(line, pos);
        int m = mapped(idx, remap);
        if (m == idx) {
            return false;
        }
        out = line.substr(0, prefix_len) + std::to_string(m) + line.substr(pos);
        return true;
    }
    return false;
}

// --- Family 3: M104 / M109 line carrying the FIRST "T<digits>" token ---
// A tool token is a standalone parameter: preceded by whitespace (or line start
// of the param after the command) and the "T" immediately followed by digits.
// We rewrite only the first such token; gcode never carries two on one line.
bool try_temp(const std::string& line, const std::map<int, int>& remap, std::string& out) {
    if (!starts_with(line, "M104") && !starts_with(line, "M109")) {
        return false;
    }
    // Never look inside the trailing comment.
    size_t comment = line.find(';');
    size_t scan_end = (comment == std::string::npos) ? line.size() : comment;

    for (size_t i = 0; i + 1 < scan_end; ++i) {
        if (line[i] != 'T') {
            continue;
        }
        // Must start a token: previous char is whitespace (the command "M104"
        // itself guarantees the first 'T' we care about is never at index 0).
        if (i == 0 || !std::isspace(static_cast<unsigned char>(line[i - 1]))) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(line[i + 1]))) {
            continue;
        }
        size_t pos = i + 1;
        int idx = parse_uint(line, pos);
        int m = mapped(idx, remap);
        if (m == idx) {
            return false; // first tool token is unmapped -> nothing to do
        }
        out = line.substr(0, i + 1) + std::to_string(m) + line.substr(pos);
        return true;
    }
    return false;
}

// Shared per-line transform. Returns the rewritten line, or `line` unchanged.
// `line` must NOT contain a trailing '\n' (callers split on newlines first).
std::string transform_line(const std::string& line, const std::map<int, int>& remap) {
    std::string out;
    if (try_bare_toolchange(line, remap, out)) {
        return out;
    }
    if (try_prestart(line, remap, out)) {
        return out;
    }
    if (try_temp(line, remap, out)) {
        return out;
    }
    return line;
}

} // namespace

std::string GcodeToolRemapper::apply_to_string(const std::string& gcode,
                                               const std::map<int, int>& remap) {
    std::string result;
    result.reserve(gcode.size());

    size_t start = 0;
    const size_t n = gcode.size();
    while (start < n) {
        size_t nl = gcode.find('\n', start);
        if (nl == std::string::npos) {
            // Final line with no trailing newline.
            result += transform_line(gcode.substr(start), remap);
            break;
        }
        result += transform_line(gcode.substr(start, nl - start), remap);
        result += '\n';
        start = nl + 1;
    }
    return result;
}

std::vector<GcodeLineReplacement>
GcodeToolRemapper::build_line_replacements(const std::string& gcode,
                                           const std::map<int, int>& remap) {
    std::vector<GcodeLineReplacement> out;

    size_t start = 0;
    const size_t n = gcode.size();
    int line_number = 0;
    while (start < n) {
        ++line_number;
        size_t nl = gcode.find('\n', start);
        std::string line =
            (nl == std::string::npos) ? gcode.substr(start) : gcode.substr(start, nl - start);
        std::string rewritten = transform_line(line, remap);
        if (rewritten != line) {
            out.push_back(GcodeLineReplacement{line_number, std::move(rewritten)});
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

} // namespace helix
