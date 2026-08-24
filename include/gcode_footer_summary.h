// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace helix::gcode {

/**
 * @brief What a slicer's footer block says about the filaments a file uses.
 *
 * Produced by parse_gcode_footer_summary() from the tail of a G-code file.
 * Both answers come from the settings block slicers append after the last
 * move, so a suffix range over the last few tens of kilobytes yields them
 * without downloading or geometry-parsing the whole print.
 */
struct GcodeFooterSummary {
    /// Per-tool hex colors, slot-aligned with tool index (`#RRGGBB[AA]`, or an
    /// empty string for a slot the slicer left blank). Empty when the footer
    /// carried no usable color line.
    std::vector<std::string> colours;

    /// Tool indices with nonzero filament usage — the tools the file actually
    /// prints with. Empty unless has_usage_line is true.
    std::set<int> tools_used;

    /// True when a per-tool `filament used [g]` vector was seen. Without it
    /// the used-tool question is unanswered (NOT "no tools used").
    bool has_usage_line = false;

    /// True when the summary's used-tool set can be trusted as the final
    /// answer for this file. Requires a usage line that named at least one
    /// nonzero tool: an all-zero vector is far more likely to be slicer
    /// placeholders than a print that extrudes nothing, and treating it as
    /// authoritative would cache "no tools" for the file.
    [[nodiscard]] bool usable() const {
        return has_usage_line && !tools_used.empty();
    }
};

/**
 * @brief Parse a G-code file's trailing bytes for its filament summary.
 *
 * Recognizes exactly two keys, both matched on the FULL key text (the part
 * before `=`, minus the leading `;` and surrounding space), case-insensitively:
 *
 *   - `filament used [g]` — comma-separated grams per tool, e.g.
 *     `; filament used [g] = 0.00, 0.00, 0.00, 0.00, 34.35` → tools_used {4}.
 *     `total filament used [g]` is deliberately NOT accepted: it is a scalar
 *     total, and reading it as a one-element vector would answer {0} for every
 *     multi-tool file.
 *   - `filament_colour` / `filament_color` / `extruder_colour` /
 *     `extruder_color` — the semicolon-separated palette, parsed by the shared
 *     parse_filament_color_palette(). `extruder_colour` wins when both are
 *     present and it parsed to something (same precedence the full G-code
 *     parser uses); slicers that emit an empty `extruder_colour = ;;;;` fall
 *     through to `filament_colour`.
 *
 * Exact key matching (rather than a substring test) keeps neighbours in the
 * same config block — `default_filament_colour`, `filament_colour_change` —
 * from hijacking the palette.
 *
 * "Used" means grams strictly greater than zero (epsilon 1e-9 to absorb
 * float noise; `0`, `0.00` and `0.0000` all parse to exact zero anyway). The
 * threshold is deliberately as close to zero as representable: including a
 * tool that barely extrudes costs one extra chip, while dropping a tool the
 * file really uses silently skips its filament check.
 *
 * Safe on a windowed read. The first line of a suffix range is usually a
 * fragment, but every key is anchored at the start of its line, so a fragment
 * cannot match one — it is simply ignored. Handles CRLF, missing keys, an
 * empty input, and unparsable value tokens (which count as zero and keep slot
 * alignment). When a key appears more than once the LAST occurrence wins.
 *
 * @param tail Trailing bytes of a G-code file (no ownership taken).
 * @return The summary; check usable() before trusting tools_used.
 */
[[nodiscard]] GcodeFooterSummary parse_gcode_footer_summary(std::string_view tail);

/// Smallest suffix range worth asking for — a request this small costs the
/// same round-trip as a larger one, so never undercut it.
inline constexpr size_t GCODE_TAIL_WINDOW_MIN = 16u * 1024u;

/// Window used when the footer offset is unknown. The footer of a real
/// Orca-family file measured 24.5 KB, so a 20 KB guess misses it; 64 KB
/// clears every sample we have while staying trivial to transfer.
inline constexpr size_t GCODE_TAIL_WINDOW_DEFAULT = 64u * 1024u;

/// Hard ceiling. A footer larger than this is not a footer — some slicer
/// wrote its whole config twice — and the full-file scan is the better answer
/// than an ever-growing range request.
inline constexpr size_t GCODE_TAIL_WINDOW_MAX = 512u * 1024u;

/**
 * @brief Choose how many trailing bytes to request for a footer read.
 *
 * Moonraker's metadata reports `gcode_end_byte`, the offset of the last move
 * (`; EXECUTABLE_BLOCK_END` sits exactly there on Orca-family output), so
 * `size - gcode_end_byte` is the footer's exact length. Clamped to
 * [GCODE_TAIL_WINDOW_MIN, GCODE_TAIL_WINDOW_MAX] and never larger than the
 * file itself.
 *
 * @param file_size Total file size in bytes; 0 when unknown.
 * @param gcode_end_byte Offset where the G-code body ends; 0 when unknown or
 *        not reported (then GCODE_TAIL_WINDOW_DEFAULT is used).
 * @return Byte count to request. Always nonzero — a zero-length suffix range
 *         is not expressible and download_file_tail() rejects it.
 */
[[nodiscard]] size_t gcode_tail_window_bytes(uint64_t file_size, uint64_t gcode_end_byte);

} // namespace helix::gcode
