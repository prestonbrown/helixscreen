// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_filename_utils.h"

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
        return gcode::join_gcode_path(path, filename);
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
 * EndedByPrinter - the printer took this job and ended it before reporting it
 *              printing (a cancel queued behind its start sequence). Its own
 *              terminal state reports the outcome, so this exit says nothing.
 */
enum class PreparingExit { Confirmed, Superseded, Failed, Cancelled, TimedOut, EndedByPrinter };

/// How far the preparing job's start has got towards the printer. Int values
/// are the wire format of the preparing_start subject.
enum class PreparingStart : int {
    None = 0,     ///< nothing sent, or the send was refused
    InFlight = 1, ///< sent, not answered: the printer may or may not have it
    Accepted = 2, ///< answered OK: the printer has received the start
    /// The answer was lost: a timeout, or a dropped connection while a cancel
    /// was requested. A timeout means Klipper is busy with the start, so a
    /// cancel sent now queues behind it.
    Unanswered = 3,
};

/// Human-readable name, for logs.
const char* preparing_exit_name(PreparingExit reason);

} // namespace helix
