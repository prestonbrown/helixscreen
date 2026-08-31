// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file gcode_render_memory.h
 * @brief Itemized heap accounting for the G-code renderers.
 *
 * Exists because "this change saves memory" was an argument rather than a
 * measurement. Process RSS cannot settle it: cross-run RSS carries megabytes of
 * noise, and free() does not lower RSS at all without malloc_trim, so a freed
 * buffer can look like it changed nothing. Counting the allocation directly is
 * deterministic and answers the question the argument was about.
 *
 * Itemized rather than one total on purpose. A single number can tell you the
 * footprint moved but not which buffer moved, and every decision worth making
 * here is about a specific buffer.
 *
 * GCodeGLESRenderer already had a get_memory_usage() returning a bare size_t.
 * It had no callers, in any file, ever. Both renderers report through this now
 * so the two are comparable and so the 2D path (the only renderer the
 * constrained printers can run) stops being the one with no accounting at all.
 */

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace helix::gcode {

/**
 * @brief A renderer's heap, broken down by what holds it.
 *
 * Line items are appended in the order the renderer wants them read; nothing
 * sorts or merges them. A zero-byte item is still recorded, because "this
 * buffer exists and is empty" and "this buffer is not allocated" are different
 * states and the log should be able to say which.
 */
class RenderMemoryReport {
  public:
    struct Item {
        const char* name; ///< static storage; not owned
        size_t bytes;
    };

    /// Record one line item. `name` must outlive the report (use a literal).
    void add(const char* name, size_t bytes) {
        items_.push_back(Item{name, bytes});
        total_ += bytes;
    }

    /// Convenience for the common case: a w*h buffer at `bpp` bytes per pixel,
    /// counted as zero when the pointer is null rather than omitted.
    void add_buffer(const char* name, bool allocated, int w, int h, size_t bpp) {
        const size_t bytes = allocated ? static_cast<size_t>(w) * static_cast<size_t>(h) * bpp : 0;
        add(name, bytes);
    }

    size_t total() const {
        return total_;
    }

    const std::vector<Item>& items() const {
        return items_;
    }

    /// Bytes recorded under `name`, or 0 if there is no such item. For tests and
    /// for A/B comparisons that care about one buffer rather than the total.
    size_t bytes_for(const char* name) const;

    /// One log line: "1687 KB total (solid_cache 434, ghost_cache 434, ...)".
    /// Kilobytes because the interesting numbers here are hundreds of KB and
    /// byte counts make the line hard to scan on a printer's serial log.
    std::string format() const;

  private:
    std::vector<Item> items_;
    size_t total_ = 0;
};

inline size_t RenderMemoryReport::bytes_for(const char* name) const {
    for (const auto& i : items_) {
        // Pointer comparison would work for literals from one translation unit
        // and silently fail across them. Compare the text.
        if (std::string(i.name) == name) {
            return i.bytes;
        }
    }
    return 0;
}

inline std::string RenderMemoryReport::format() const {
    auto kb = [](size_t bytes) { return (bytes + 1023) / 1024; };

    char head[64];
    std::snprintf(head, sizeof(head), "%zu KB total", kb(total_));
    std::string out(head);

    if (items_.empty()) {
        return out;
    }

    out += " (";
    bool first = true;
    for (const auto& i : items_) {
        if (!first) {
            out += ", ";
        }
        first = false;
        char part[96];
        std::snprintf(part, sizeof(part), "%s %zu", i.name, kb(i.bytes));
        out += part;
    }
    out += ")";
    return out;
}

} // namespace helix::gcode
