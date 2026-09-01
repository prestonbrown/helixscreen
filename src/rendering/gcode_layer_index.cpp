// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_layer_index.h"

#include "gcode_color_metadata.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>

namespace helix {
namespace gcode {

namespace {

// Layer detection tolerance for Z changes
constexpr float Z_EPSILON = 0.001f;

// Minimum E delta that counts as extrusion rather than a travel/retract.
// Matches the threshold GCodeParser::parse_movement_command() uses so the index
// and the full-file parser agree on what "extruding" means.
constexpr float EXTRUSION_EPSILON = 0.00001f;

// Extract a single-letter float parameter (case-insensitive) from a G-code line.
// Returns true if found. Skips over coordinates embedded inside identifier
// tokens like "G1" by only matching at the start of a token (preceded by
// whitespace, comma, or start-of-line).
/// Length of `line` up to the first `;`, i.e. the part that is actual G-code.
///
/// Hoisted out of extract_axis_param() so the hot loop pays for it once per
/// line instead of once per axis looked up (four times on a typical move).
size_t gcode_code_len(const char* line, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (line[i] == ';') {
            return i;
        }
    }
    return len;
}

/// Powers of ten that are EXACTLY representable in a double, so scaling by one
/// of them introduces a single rounding rather than compounding error.
constexpr double POW10_EXACT[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
                                  1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
constexpr int POW10_MAX_DIGITS = 18;

/**
 * @brief Parse the plain decimal form G-code axis words actually use.
 *
 * THIS is the index scan's hot spot, and it is not obvious from reading the
 * loop. Measured on a K2 Plus with a micro-benchmark over realistic move
 * lines: the scanning around the number costs 1.82us/line, and adding
 * std::strtof takes it to 22.06us/line — strtof is 92% of the work. musl's
 * strtof runs ~5us per call on this ARM and the scan makes up to four calls per
 * line (Z, E, X, Y), which over a 5M-line print is the bulk of a 63-second
 * index build.
 *
 * Accepts `[+-]?digits[.digits]?` and nothing else. An exponent, an overlong
 * mantissa, or anything unexpected returns false and the caller falls back to
 * strtof — so the parsed VALUE is never worse than before, only cheaper in the
 * case that covers essentially every coordinate a slicer emits. Notably
 * `X10E5`, where 'E' would otherwise be read as an axis letter, falls back and
 * keeps strtof's scientific-notation reading.
 *
 * Digits accumulate into a uint64 and are scaled once by an exact power of ten,
 * so the result is within a ULP of strtof's — far inside the 0.001mm epsilon
 * layer detection uses and the millimetre scale of the bounds.
 *
 * @param p     First character of the number.
 * @param limit One past the last character available.
 * @param out   Parsed value on success.
 * @param endp  One past the last character consumed, on success.
 * @return true when the fast path handled it; false to fall back to strtof.
 */
bool parse_axis_value_fast(const char* p, const char* limit, float& out, const char** endp) {
    const char* s = p;
    bool negative = false;
    if (s < limit && (*s == '+' || *s == '-')) {
        negative = (*s == '-');
        ++s;
    }

    uint64_t mantissa = 0;
    int digits = 0;
    int frac_digits = 0;
    bool saw_digit = false;

    while (s < limit && *s >= '0' && *s <= '9') {
        if (digits >= POW10_MAX_DIGITS) {
            return false; // more precision than this path promises — strtof it
        }
        mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
        ++digits;
        saw_digit = true;
        ++s;
    }

    if (s < limit && *s == '.') {
        ++s;
        while (s < limit && *s >= '0' && *s <= '9') {
            if (digits >= POW10_MAX_DIGITS) {
                return false;
            }
            mantissa = mantissa * 10 + static_cast<uint64_t>(*s - '0');
            ++digits;
            ++frac_digits;
            saw_digit = true;
            ++s;
        }
    }

    if (!saw_digit) {
        return false;
    }
    // Scientific notation is strtof's business, not ours.
    if (s < limit && (*s == 'e' || *s == 'E')) {
        return false;
    }

    const double value = static_cast<double>(mantissa) / POW10_EXACT[frac_digits];
    out = static_cast<float>(negative ? -value : value);
    *endp = s;
    return true;
}

/// extract_axis_param() over a length the caller has ALREADY truncated at the
/// comment (see gcode_code_len).
bool extract_axis_param_in_code(const char* line, size_t len, char axis, float& out_value) {
    char upper = axis;
    char lower = static_cast<char>(axis | 0x20);
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != upper && line[i] != lower) {
            continue;
        }
        // Must be at token start: preceded by whitespace or beginning of line.
        if (i > 0) {
            char prev = line[i - 1];
            if (prev != ' ' && prev != '\t' && prev != ',') {
                continue;
            }
        }
        if (i + 1 >= len) {
            continue;
        }
        char next = line[i + 1];
        if (next != '-' && next != '+' && next != '.' && (next < '0' || next > '9')) {
            continue;
        }
        const char* num = line + i + 1;
        const char* fast_end = nullptr;
        float v = 0.0f;
        if (parse_axis_value_fast(num, line + len, v, &fast_end)) {
            out_value = v;
            return true;
        }
        char* end = nullptr;
        v = std::strtof(num, &end);
        if (end != num) {
            out_value = v;
            return true;
        }
    }
    return false;
}

bool extract_axis_param(const char* line, size_t len, char axis, float& out_value) {
    return extract_axis_param_in_code(line, gcode_code_len(line, len), axis, out_value);
}

bool extract_z_param(const char* line, size_t len, float& out_z) {
    return extract_axis_param(line, len, 'Z', out_z);
}

// Check if line is a movement command (G0 or G1)
bool is_movement_command(const char* line, size_t len) {
    // Skip leading whitespace
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }

    // Check for G0 or G1
    if (i + 1 < len && (line[i] == 'G' || line[i] == 'g')) {
        char next = line[i + 1];
        if (next == '0' || next == '1') {
            // Make sure it's not G10, G100, etc.
            if (i + 2 >= len || line[i + 2] == ' ' || line[i + 2] == '\t' || line[i + 2] == ';' ||
                line[i + 2] == '\r' || line[i + 2] == '\n') {
                return true;
            }
            // G0 followed by coordinate is also valid (G0X10)
            char after = line[i + 2];
            if (after == 'X' || after == 'Y' || after == 'Z' || after == 'E' || after == 'F' ||
                after == 'x' || after == 'y' || after == 'z' || after == 'e' || after == 'f') {
                return true;
            }
        }
    }
    return false;
}

// NOTE: a has_positive_extrusion() text heuristic used to live here and drive
// the extrusion/travel stats. It scanned for an 'E' followed by a digit or '+',
// which missed OrcaSlicer's leading-dot form (E.05482 — so every Orca file was
// counted as ~100% travel) and could not distinguish a retraction from an
// extrusion. build_from_file() now tracks M82/M83 and the running E value and
// computes a real delta, which is both correct and what the XY bounds depend on.

// Extract filament/extruder color from metadata comment
// Thin wrapper over helix::gcode::parse_filament_color_palette() that also
// projects the legacy single-color field (palette[0]) for backward compat.
bool extract_filament_color(const char* line, size_t len, std::string& out_color,
                            std::vector<std::string>& out_palette) {
    if (len < 10 || line[0] != ';') {
        return false;
    }
    if (!helix::gcode::parse_filament_color_palette(std::string_view(line, len), out_palette)) {
        return false;
    }
    // First non-empty entry is the legacy "single color" surface.
    if (const std::string* first = helix::gcode::first_named_color(out_palette)) {
        out_color = *first;
        return true;
    }
    return false;
}

// Check if line is a layer change marker.
// Matches ";LAYER_CHANGE" / "; LAYER_CHANGE" (case-insensitive) where the marker
// is the entire comment content (or terminated by whitespace). Rejects OrcaSlicer's
// ";BEFORE_LAYER_CHANGE" / ";AFTER_LAYER_CHANGE" — those contain "LAYER_CHANGE"
// as a substring but are bracketing tags for the before-layer-change user macro,
// not real layer transitions.
bool is_layer_marker(const char* line, size_t len) {
    const char* marker = "LAYER_CHANGE";
    constexpr size_t marker_len = 12;
    for (size_t i = 0; i + marker_len <= len; ++i) {
        if (line[i] != ';') {
            continue;
        }
        size_t j = i + 1;
        while (j < len && line[j] == ' ') {
            ++j;
        }
        if (j + marker_len > len) {
            continue;
        }
        bool match = true;
        for (size_t k = 0; k < marker_len; ++k) {
            char c = line[j + k];
            char m = marker[k];
            if (c != m && c != (m + 32) && (c - 32) != m) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        // Marker must be terminated by end-of-line or whitespace (not a letter,
        // digit, or underscore — which would mean it's a longer identifier like
        // a continuation of BEFORE_/AFTER_ that we missed).
        size_t end = j + marker_len;
        if (end < len) {
            char c = line[end];
            if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) {
                continue;
            }
        }
        return true;
    }
    return false;
}

} // anonymous namespace

namespace {

/// Bytes pulled per read() while scanning, replacing std::getline on a
/// default-buffered ifstream (~8KB, so ~16k reads for a 133MB file).
///
/// Worth stating honestly, because it is easy to assume otherwise: this is a
/// SMALL win. Measured on a K2 Plus over the real 133MB / 5M-line print,
/// getline took 63.0-63.8s and this takes 60.0-61.8s — about 4%. The line
/// plumbing was never the bottleneck; number parsing was (see
/// parse_axis_value_fast below, which is where the 90% lives). Kept because 4%
/// on a minute-long scan is real, the syscall reduction matters more on slow
/// flash than on a desktop, and this is the natural place to sample progress.
/// 1MB is well under the smallest device's headroom (the AD5M runs in ~47MB).
constexpr size_t INDEX_READ_BLOCK_BYTES = 1024 * 1024;

/// Report progress at most this often. Coarse on purpose: the callback crosses
/// to an atomic the UI polls, and a 5M-line file would otherwise call it 5M
/// times to move a bar 100 steps.
constexpr size_t INDEX_PROGRESS_LINE_INTERVAL = 20000;

/**
 * @brief Feeds whole lines out of a block-buffered file read.
 *
 * Deliberately reproduces std::getline's framing byte for byte, because the
 * layer entries this scan emits are FILE OFFSETS and the renderer seeks to
 * them: split on '\n' only, keep any trailing '\r' in the line (the file is
 * opened binary, so getline did too), and let the caller advance its offset by
 * length + 1 exactly as before. Getting this subtly wrong would shift every
 * layer's byte range and corrupt the render rather than fail loudly.
 */
class BlockLineReader {
  public:
    explicit BlockLineReader(std::istream& in) : in_(in) {
        buf_.resize(INDEX_READ_BLOCK_BYTES);
    }

    /// Fill @p line with the next line (newline stripped). False at EOF.
    ///
    /// The empty-line cases are why the EOF test is `!line.empty()` and nothing
    /// more: a genuinely empty line in the middle of the file returns through
    /// the memchr branch (run == 0) and never reaches EOF, while a file ending
    /// in a newline leaves nothing accumulated and correctly reports done —
    /// matching std::getline on both.
    bool next(std::string& line) {
        line.clear();
        for (;;) {
            if (pos_ >= filled_) {
                if (!refill()) {
                    // EOF. A trailing fragment with no newline is still a line;
                    // an exhausted buffer with nothing accumulated is the end.
                    return !line.empty();
                }
            }
            const char* start = buf_.data() + pos_;
            const size_t avail = filled_ - pos_;
            const void* nl = std::memchr(start, '\n', avail);
            if (nl == nullptr) {
                // Line spans the block boundary — keep it and pull the next.
                line.append(start, avail);
                pos_ = filled_;
                continue;
            }
            const size_t run = static_cast<size_t>(static_cast<const char*>(nl) - start);
            line.append(start, run);
            pos_ += run + 1; // consume the '\n'
            return true;
        }
    }

  private:
    bool refill() {
        if (eof_) {
            return false;
        }
        in_.read(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        const auto got = in_.gcount();
        filled_ = got > 0 ? static_cast<size_t>(got) : 0;
        pos_ = 0;
        if (filled_ == 0) {
            eof_ = true;
            return false;
        }
        return true;
    }

    std::istream& in_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    size_t filled_ = 0;
    bool eof_ = false;
};

} // namespace

bool GCodeLayerIndex::build_from_file(const std::string& filepath,
                                      const std::function<void(float)>& on_progress) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Clear any previous data
    entries_.clear();
    stats_ = LayerIndexStats{};
    source_path_ = filepath;
    uses_layer_markers_ = false;

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("[LayerIndex] Failed to open file: {}", filepath);
        return false;
    }

    // Get file size
    stats_.total_bytes = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    spdlog::debug("[LayerIndex] Building index for {} ({} bytes)", filepath, stats_.total_bytes);

    // Reserve estimated capacity (assume ~100 layers for now)
    entries_.reserve(100);

    // Read line by line
    std::string line;
    line.reserve(256);

    float current_z = -std::numeric_limits<float>::infinity();
    // Running head position. Snapshotted into each new layer entry so the
    // streaming parser can be seeded — without it, the first move of every
    // layer would be drawn from (0,0). We assume G90 absolute mode (the
    // ubiquitous OrcaSlicer/PrusaSlicer/Bambu convention); G91 relative
    // would silently desync but isn't used in slicer output.
    float current_x = 0.0f;
    float current_y = 0.0f;
    float current_seen_z =
        0.0f; // Z position seen so far (vs current_z which only updates on layer transition)
    // Running ;TYPE: section. Snapshotted into each new layer entry so the
    // streaming parser can be seeded — without it, segments in the prologue
    // (purge) get tagged Unknown and the bbox filter can't exclude them.
    FeatureType current_feature_type = FeatureType::Unknown;
    // Running extrusion state. M82/M83 appear once in the prologue, before every
    // layer byte range, so each entry snapshots the mode and E value in effect at
    // its offset for the per-layer parser to be seeded with (#1127). Tracking E
    // here also lets the scan compute a real extrusion delta instead of the
    // "is there an E followed by a digit" guess it used before — which is what
    // makes the XY bounds below trustworthy.
    bool absolute_extrusion = true;
    float current_e = 0.0f;
    // Running tool. Snapshotted into each new layer entry for the same reason as
    // the fields above: the `Tn` that selects a layer's tool sits in the PREVIOUS
    // layer's byte range, so a per-layer parse cannot see it. -1 until the file's
    // first tool change, which is what tells the streaming controller to fall
    // back to stats_.initial_tool_index.
    int16_t current_tool = -1;
    // False until a move has established a real head position, so the implicit
    // (0,0) origin never enters the XY bounds.
    bool any_position_seen = false;
    uint64_t current_layer_start = 0;
    uint64_t current_offset = 0;
    uint16_t current_layer_lines = 0;
    bool use_layer_markers = false;
    bool pending_layer_start = false;
    bool first_layer_started = false;

    BlockLineReader reader(file);
    const double total_for_progress =
        stats_.total_bytes > 0 ? static_cast<double>(stats_.total_bytes) : 1.0;

    while (reader.next(line)) {
        size_t line_len = line.length();
        // Computed once and reused by every axis lookup on this line — the
        // per-call version rescanned the whole line for the ';' four times on a
        // typical move.
        const size_t code_len = gcode_code_len(line.c_str(), line_len);
        stats_.total_lines++;

        if (on_progress && (stats_.total_lines % INDEX_PROGRESS_LINE_INTERVAL) == 0) {
            on_progress(
                static_cast<float>(static_cast<double>(current_offset) / total_for_progress));
        }

        // Check for layer marker
        if (is_layer_marker(line.c_str(), line_len)) {
            if (!use_layer_markers) {
                // First marker in the file. Marker mode cannot be known until
                // one appears, so the Z-change heuristic has been live over the
                // prologue — and a START_PRINT that lifts to a bed-clearance
                // height (OrcaSlicer emits `G1 Z3 F600` before the first
                // ;LAYER_CHANGE) looks exactly like a layer to it. Drop whatever
                // it guessed: none of it is a real layer, and layer 0 is about
                // to be created by this marker.
                entries_.clear();
                first_layer_started = false;
                current_layer_lines = 0;
            }
            use_layer_markers = true;
            pending_layer_start = true;
            // We'll start the new layer when we see the next Z move
        }

        // Track extrusion mode. M82/M83 are whole-line commands in the prologue.
        if (line_len >= 3 && line[0] == 'M' && line[1] == '8' &&
            (line[2] == '2' || line[2] == '3') &&
            (line_len == 3 || line[3] == ' ' || line[3] == ';' || line[3] == '\t' ||
             line[3] == '\r')) {
            absolute_extrusion = (line[2] == '2');
        }
        // G92 sets the current position of an axis without moving; G92 E0 is the
        // conventional absolute-mode extruder reset between layers/objects.
        if (line_len >= 3 && line[0] == 'G' && line[1] == '9' && line[2] == '2') {
            float e_reset;
            if (extract_axis_param_in_code(line.c_str(), code_len, 'E', e_reset)) {
                current_e = e_reset;
            }
        }

        // Track ;TYPE: section via the shared helper (kept in sync with the
        // full-file parser so streaming and full-file tag identically).
        if (auto t = GCodeParser::extract_type_marker(line.c_str(), line_len)) {
            current_feature_type = *t;
        }

        // Extract filament color from metadata (only if not already found)
        // Only check comment lines in the header (first ~1000 lines)
        if (stats_.filament_color.empty() && stats_.total_lines < 1000) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line_len, color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug("[LayerIndex] Found filament palette ({} entries, first={})",
                              stats_.filament_palette.size(), color);
            }
        }

        // Track the RUNNING tool, not just the file's first one. Streaming mode
        // parses each layer with a fresh GCodeParser, so every layer entry has
        // to carry the tool active at its own byte offset: seeding them all with
        // the file-global first tool tags a tool changer's whole model with T0
        // (and, before initial_tool_index existed, rendered a print sliced to a
        // non-T0 tool entirely in T0's colour — #776).
        //
        // Cheap pre-filter first: tool_index_for_line() scans for a ';' across
        // the whole line, and this loop runs over every line of a 10MB+ file.
        // A standalone tool change is 'T' after at most leading whitespace.
        {
            size_t lead = 0;
            while (lead < line_len && (line[lead] == ' ' || line[lead] == '\t')) {
                ++lead;
            }
            if (lead < line_len && line[lead] == 'T') {
                const int tool = tool_index_for_line(line);
                // Clamp to what StreamingLayerEntry::start_tool can hold. Real
                // tool indices are single digits; tool_index_for_line already
                // rejects anything above 100000.
                if (tool >= 0 && tool <= std::numeric_limits<int16_t>::max()) {
                    current_tool = static_cast<int16_t>(tool);
                    stats_.tools_used.insert(tool);
                    if (stats_.initial_tool_index < 0) {
                        stats_.initial_tool_index = tool;
                        spdlog::debug("[LayerIndex] Initial tool: T{}", tool);
                    }
                }
            }
        }

        // Check for movement commands
        if (is_movement_command(line.c_str(), line_len)) {
            float z;
            if (extract_axis_param_in_code(line.c_str(), code_len, 'Z', z)) {
                // Z change detected
                bool is_new_layer = false;

                if (use_layer_markers) {
                    // Use marker-based layer detection
                    if (pending_layer_start) {
                        is_new_layer = true;
                        pending_layer_start = false;
                    }
                } else {
                    // Use Z-change based layer detection
                    if (z > current_z + Z_EPSILON) {
                        is_new_layer = true;
                    }
                }

                if (is_new_layer) {
                    // Finalize previous layer if any
                    if (first_layer_started && current_layer_lines > 0) {
                        StreamingLayerEntry& last = entries_.back();
                        last.byte_length =
                            static_cast<uint32_t>(current_offset - current_layer_start);
                        last.line_count = current_layer_lines;
                    }

                    // Start new layer. Snapshot the head position BEFORE the
                    // line at file_offset has been applied — i.e., the
                    // position the streaming parser should be seeded with
                    // before it reads this layer's bytes.
                    StreamingLayerEntry entry{};
                    entry.file_offset = current_offset;
                    entry.z_height = z;
                    entry.byte_length = 0; // Will be filled when layer ends
                    entry.line_count = 0;  // Will be filled when layer ends
                    entry.flags = absolute_extrusion ? StreamingLayerEntry::FLAG_ABSOLUTE_EXTRUSION
                                                     : uint16_t{0};
                    entry.start_x = current_x;
                    entry.start_y = current_y;
                    entry.start_z = current_seen_z;
                    entry.start_feature_type = current_feature_type;
                    entry.start_tool = current_tool;
                    entry.start_e = current_e;
                    entries_.push_back(entry);

                    first_layer_started = true;

                    current_z = z;
                    current_layer_start = current_offset;
                    current_layer_lines = 0;
                }
                current_seen_z = z;
            }

            // Resolve the real extrusion delta for this move, honouring the
            // M82/M83 mode. The previous "E followed by a digit" heuristic
            // mis-flagged whole files: it missed OrcaSlicer's leading-dot form
            // (E.05482) and could not tell a retraction from an extrusion.
            float e_param;
            float e_delta = 0.0f;
            if (extract_axis_param_in_code(line.c_str(), code_len, 'E', e_param)) {
                const float new_e = absolute_extrusion ? e_param : current_e + e_param;
                e_delta = new_e - current_e;
                current_e = new_e;
            }
            const bool is_extruding = e_delta > EXTRUSION_EPSILON;

            // Update running X/Y from this move (for the next layer's snapshot),
            // and grow the model bounds over extruding moves only. Both the
            // start and end point of an extruding move are inside the model, but
            // the start is only valid once we have actually seen a prior move —
            // otherwise the implicit (0,0) origin drags the box to the corner.
            float v;
            const float prev_x = current_x;
            const float prev_y = current_y;
            bool moved = false;
            if (extract_axis_param_in_code(line.c_str(), code_len, 'X', v)) {
                current_x = v;
                moved = true;
            }
            if (extract_axis_param_in_code(line.c_str(), code_len, 'Y', v)) {
                current_y = v;
                moved = true;
            }

            // Parser parity: a segment exists only when XY actually changed
            // (gcode_parser.cpp's add path requires it). An E-only move - a
            // toolchange retract/prime pair parked mid-print - has no spatial
            // extent, and counting its parked position widens the stats by
            // wherever the head happens to sit. That is not hypothetical:
            // Orca primes at the tower BEFORE the tower's ;TYPE: marker
            // starts, under the previous part section's type, so the tower's
            // coordinates leaked into the fit bounds through a move that
            // drew nothing.
            const bool xy_changed = (current_x != prev_x || current_y != prev_y);
            if (is_extruding && xy_changed && !is_auxiliary_geometry(current_feature_type)) {
                if (any_position_seen) {
                    stats_.min_x = std::min(stats_.min_x, prev_x);
                    stats_.max_x = std::max(stats_.max_x, prev_x);
                    stats_.min_y = std::min(stats_.min_y, prev_y);
                    stats_.max_y = std::max(stats_.max_y, prev_y);
                }
                stats_.min_x = std::min(stats_.min_x, current_x);
                stats_.max_x = std::max(stats_.max_x, current_x);
                stats_.min_y = std::min(stats_.min_y, current_y);
                stats_.max_y = std::max(stats_.max_y, current_y);
                // Z rides the same gate as X/Y so the index reports the height
                // of extruded geometry, matching what the full-file parser puts
                // in global_bounding_box (the 3D path's input). Reading Z off
                // layer entries instead measured travel: it caught prologue
                // clearance moves and per-layer Z-hops, and because it recorded
                // "first entry" and "last entry" rather than min and max, a file
                // whose last hop sat below its prologue lift produced min > max.
                stats_.min_z = std::min(stats_.min_z, current_seen_z);
                stats_.max_z = std::max(stats_.max_z, current_seen_z);
            }
            if (moved) {
                any_position_seen = true;
            }

            // Track extrusion vs travel
            if (is_extruding) {
                stats_.extrusion_moves++;
            } else {
                stats_.travel_moves++;
            }
        }

        current_layer_lines++;
        // Account for line length + newline character
        current_offset += line_len + 1;
    }

    // Finalize last layer
    if (first_layer_started && !entries_.empty()) {
        StreamingLayerEntry& last = entries_.back();
        last.byte_length = static_cast<uint32_t>(stats_.total_bytes - current_layer_start);
        last.line_count = current_layer_lines;
    }

    stats_.total_layers = entries_.size();
    uses_layer_markers_ = use_layer_markers;

    // Release the slack left by the reserve(100) growth doubling. A 1200-layer
    // print reserves 1600 entries; handing 400 × 40 bytes back matters on a
    // 47MB-RAM AD5M.
    entries_.shrink_to_fit();

    // If no filament color found in header, scan the file footer (OrcaSlicer puts metadata at end)
    if (stats_.filament_color.empty() && stats_.total_bytes > 0) {
        // Read last 32KB of file to find metadata
        size_t footer_size = std::min(stats_.total_bytes, size_t(32768));
        file.clear();
        file.seekg(-static_cast<std::streamoff>(footer_size), std::ios::end);

        while (std::getline(file, line)) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line.length(), color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug(
                    "[LayerIndex] Found filament palette in footer ({} entries, first={})",
                    stats_.filament_palette.size(), color);
                break;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.build_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    spdlog::info("[LayerIndex] Built index: {} layers, {} lines, Z=[{:.2f}, {:.2f}], {:.1f}ms",
                 stats_.total_layers, stats_.total_lines, stats_.min_z, stats_.max_z,
                 stats_.build_time_ms);

    spdlog::debug("[LayerIndex] Memory usage: {} bytes ({} bytes/layer)", memory_usage_bytes(),
                  entries_.empty() ? 0 : memory_usage_bytes() / entries_.size());

    return !entries_.empty();
}

StreamingLayerEntry GCodeLayerIndex::get_entry(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index];
    }
    // Out of range. byte_length == 0 is what makes this distinguishable from a
    // real entry: every indexed layer is finalized with a non-zero length (the
    // line that opens a layer is itself counted, so consecutive layer starts
    // still bracket at least one line), and is_valid() tests exactly that field.
    // The zeroed start_x/y/z here are NOT a usable seed — a caller that skipped
    // is_valid() would draw the layer's first move from the origin, which is the
    // stray-lines bug those fields were added to prevent.
    return StreamingLayerEntry{0, 0, 0.0f, 0, 0};
}

int GCodeLayerIndex::find_layer_at_z(float z) const {
    if (entries_.empty()) {
        return -1;
    }

    // Binary search for closest layer
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), z,
        [](const StreamingLayerEntry& entry, float target_z) { return entry.z_height < target_z; });

    if (it == entries_.end()) {
        return static_cast<int>(entries_.size() - 1);
    }

    size_t idx = std::distance(entries_.begin(), it);

    // Check if previous layer is closer
    if (idx > 0) {
        float dist_curr = std::abs(it->z_height - z);
        float dist_prev = std::abs((it - 1)->z_height - z);
        if (dist_prev < dist_curr) {
            return static_cast<int>(idx - 1);
        }
    }

    return static_cast<int>(idx);
}

float GCodeLayerIndex::get_layer_z(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index].z_height;
    }
    return 0.0f;
}

} // namespace gcode
} // namespace helix

#endif // HELIX_HAS_GCODE_VIEWER
