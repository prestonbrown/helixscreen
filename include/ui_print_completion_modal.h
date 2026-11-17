// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_print_completion_modal.h
 * @brief The dialog that reports how a print ended (print_completion_modal.xml)
 *
 * The completion observer (print_completion.h) decides whether a job's end is
 * worth a dialog and asks for it with UiRequestKind::ShowPrintCompletion; a new
 * job takes it down with UiRequestKind::DismissPrintCompletion.
 */

#include <string>

namespace helix {

/// Publish the print_completion_* subjects the dialog binds. Idempotent; main thread.
void init_print_completion_subjects();

/**
 * @brief Rendered stat strings for the print-completion modal
 *
 * Optional fields are empty when the underlying value is unknown; the modal
 * binds their visibility off that.
 */
struct CompletionStats {
    std::string duration;
    std::string estimate;
    std::string layers;
    std::string filament;
};

/**
 * @brief Build the completion modal's stat strings
 *
 * Pure: no subjects, no LVGL objects. Every value routes through the shared
 * formatters in ui_format_utils so the modal cannot disagree with the rest of
 * the UI about how a layer count or a filament length reads.
 */
CompletionStats build_completion_stats(int duration_secs, int estimated_secs, int total_layers,
                                       int filament_mm);

} // namespace helix
