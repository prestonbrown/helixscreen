// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <cstdint>
#include <optional>
#include <string>

namespace helix {
class Config;
}

/**
 * @file gcode_preview_gate.h
 * @brief The one answer to "may this file get a streamed preview now?"
 *
 * Two limits: the RAM a streamed preview needs (is_gcode_2d_streaming_safe())
 * and, in Auto render mode, the CPU it would cost on this board
 * (gcode_preview_cost.h). Every place that would start a preview asks here and
 * shows the slicer thumbnail on anything but Build.
 *
 * The CPU limit projects from the index-scan rate this board measured on earlier
 * idle scans, stored at /display/gcode_preview_rate and valid only for the board
 * and app version that measured it.
 */

namespace helix::ui {

/// One board's learned index-scan rate.
struct PreviewRateEntry {
    double mb_per_cpu_s = 0.0;
    std::string board;
    std::string version;

    bool operator==(const PreviewRateEntry& o) const {
        return mb_per_cpu_s == o.mb_per_cpu_s && board == o.board && version == o.version;
    }
};

/// "/display/gcode_preview_rate"
inline constexpr const char* kPreviewRatePath = "/display/gcode_preview_rate";

/// The entry at @p node, or nullopt unless it is an object with a positive
/// number "rate" and string "board" and "version".
std::optional<PreviewRateEntry> parse_preview_rate(const nlohmann::json* node);

nlohmann::json preview_rate_json(const PreviewRateEntry& entry);

/// The stored rate when it was measured on @p board by @p version, else 0.
double learned_preview_rate(const helix::Config& config, const std::string& board,
                            const std::string& version);

/// Writes the entry and saves the config file.
void save_preview_rate(helix::Config& config, const PreviewRateEntry& entry);

/// What the gate decides from, read once on the main thread.
struct PreviewGateInputs {
    bool memory_safe = true;   ///< is_gcode_2d_streaming_safe() for the file
    bool auto_mode = true;     ///< render mode is Auto: the CPU limit applies
    bool printing = false;     ///< a print holds the machine
    double learned_rate = 0.0; ///< learned_preview_rate() for this board, 0 = none
};

enum class PreviewGate {
    Build,        ///< Build the preview
    TooLarge,     ///< Not enough RAM to stream it
    TooExpensive, ///< Projected CPU over budget on this board right now
};

/// Pure: the gate for a file of @p bytes.
PreviewGate decide_preview_gate(uint64_t bytes, const PreviewGateInputs& in);

/// Main thread: the live inputs for a file of @p bytes.
PreviewGateInputs current_preview_gate_inputs(uint64_t bytes);

/// Main thread: decide_preview_gate() over the live inputs, logging any refusal
/// under @p log_tag.
PreviewGate preview_gate(uint64_t bytes, const char* log_tag);

/// This board's fingerprint and the running version, the key a learned rate is
/// stored under. Main thread; computed once.
const std::string& preview_rate_board();
const std::string& preview_rate_version();

/// What becomes of a freshly measured index rate.
enum class RateRecord {
    Ignore,      ///< Nothing to keep: no rate, or a build that keys no board
    SessionOnly, ///< Use it until restart: measured during a print
    Persist,     ///< Store it: measured idle, and different from what is stored
};

/**
 * @brief Pure: what to do with @p mb_per_cpu_s.
 *
 * A rate measured while a print runs is kept for the session only, so the next
 * file is judged before it downloads, but never stored: it measures the print as
 * much as the board. An idle rate within a tenth of @p stored_rate (0 = none
 * stored for this board and version) is the same board saying the same thing and
 * is not written again.
 */
RateRecord decide_rate_record(double mb_per_cpu_s, bool board_known, bool printing,
                              double stored_rate);

/// Main thread: keep a rate a scan just measured, per decide_rate_record(). The
/// gate prefers a stored rate and falls back to the last one kept this session.
void record_preview_rate(double mb_per_cpu_s);

} // namespace helix::ui
