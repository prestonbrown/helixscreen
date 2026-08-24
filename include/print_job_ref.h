// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix {

/**
 * @brief Identity of the job being prepared
 *
 * Answers "which job is this?" for the whole panel during the window between
 * the user committing to a print and the printer reporting it. Moonraker's
 * print_stats still describes the PREVIOUS job for that entire window, so it
 * cannot be the source of truth there.
 *
 * `filename` is the name as the user chose it, before any temp-file rewrite -
 * it is what the preview, thumbnail, gcode viewer and filename label resolve
 * against, and the key the printer's eventual report is reconciled with.
 */
struct PrintJobRef {
    std::string filename; ///< User-facing name, pre-rewrite
    std::string path;     ///< Directory it came from ("" for the gcodes root)
    std::string source;   ///< Origin tag (e.g. "usb"), empty for Moonraker storage

    [[nodiscard]] bool empty() const {
        return filename.empty();
    }

    /// Full path used for metadata lookup.
    [[nodiscard]] std::string full_path() const {
        return path.empty() ? filename : path + "/" + filename;
    }
};

/**
 * @brief Why a preparing job stopped being the one we are preparing
 *
 * Confirmed  - the printer reported this job printing; hand off.
 * Superseded - the printer reported a DIFFERENT job; something else started a
 *              print while ours was preparing, so our claim is discarded.
 * Failed     - the start could not be completed (upload, rejected macro).
 * Cancelled  - the user abandoned the start.
 * TimedOut   - no confirmation arrived. Ungated: a commit-armed job can be
 *              raised on a printer that never reaches state=printing at all.
 */
enum class PreparingExit { Confirmed, Superseded, Failed, Cancelled, TimedOut };

/// Human-readable name, for logs.
const char* preparing_exit_name(PreparingExit reason);

} // namespace helix
