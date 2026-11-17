// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_ams_lane_settings.h
 * @brief A lane's standing settings: its Assigned Tool and its Endless Spool backup
 *
 * Both are properties of the lane, not of the spool in it, and both are written
 * to the backend rather than to the slot's identity. The spool editor
 * (AmsEditOverlay) stages them in two dropdowns and commits them on Save; the
 * rules for what those dropdowns offer, refuse and write live here.
 */

#include "ams_types.h"
#include "display_numbering.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {
class AmsBackend;
} // namespace helix

namespace helix::ui::lane_settings {

/// Answers "may slot `candidate` stand in for slot `slot`?". Always
/// AmsBackend::endless_spool_backup_eligibility() in production; a parameter so
/// the pure functions below are testable without a backend.
using BackupEligibleFn = std::function<helix::printer::BackupEligibility(int slot, int candidate)>;

/// Whether the Assigned Tool row shows: only where a mapping-table write lands
/// (helix::printer::can_write_mapping_table()). A backend that honours remaps
/// only through its pre-print send refuses the table write, and an unusable
/// dropdown is worse than none.
[[nodiscard]] bool show_tool_row(const AmsBackend* backend);

/// Whether the backup row shows for @p slot.
///
/// Availability alone is not enough. A read-only backend still earns the row
/// (greyed out) so the user can SEE the backup the firmware is using, but only
/// on a slot that has one: a read-only row on a slot with no edge could only
/// ever read "None", which is indistinguishable from "no backup configured".
[[nodiscard]] bool show_backup_row(const helix::printer::EndlessSpoolCapabilities& caps,
                                   const helix::printer::EndlessSpoolConfig& cfg, int slot);

/// The backup dropdown's slots after "None", in order: every bay that exists
/// except @p slot. The option labels, the preselection and the refusal all index
/// this one list.
[[nodiscard]] std::vector<int> backup_candidates(const std::vector<int>& bays, int slot);

/// The backup dropdown's options, newline-separated, "None" first, each
/// candidate in the backend's own word and tagged "(incompatible)" or
/// "(different grade)" by @p eligible. An @p slot below 0 tags nothing.
[[nodiscard]] std::string backup_options(LaneNoun noun, const std::vector<int>& candidates,
                                         int slot, const BackupEligibleFn& eligible);

/// Whether picking @p backup for @p slot is refused. The same rule that tagged
/// the option, so a tagged option and a refused pick never disagree. "None"
/// (backup < 0) is always allowed, and a grade difference is tagged but allowed.
[[nodiscard]] bool backup_refused(int slot, int backup, const BackupEligibleFn& eligible);

/// @p backend's eligibility rule, or an always-eligible one with no backend.
[[nodiscard]] BackupEligibleFn eligibility_of(const AmsBackend* backend);

/// Why the Assigned Tool row reads disabled for @p slot, or empty when a tool
/// can be assigned there. Asks AmsBackend::can_set_tool_mapping() with the
/// lane's own tool (@p assigned, or the first tool when it has none); a refusal
/// about that one tool number is not a refusal of the row.
[[nodiscard]] std::string tool_row_refusal(const AmsBackend& backend, int slot, int assigned);

/// The Assigned Tool dropdown's options: "None", then one tool per slot.
[[nodiscard]] std::string tool_options(int tool_count);

/// The tool that maps to @p slot, or -1. On a lane several tools route to, the
/// one it is running.
[[nodiscard]] int assigned_tool(const AmsBackend& backend, int slot);

/// @p slot's Endless Spool backup, or -1.
[[nodiscard]] int backup_of(const AmsBackend& backend, int slot);

/// Route @p tool to @p slot, warning when another tool already maps there. A
/// negative @p tool writes nothing: a mapping is cleared by mapping that tool
/// to another slot. False when the backend refused the write (already toasted).
[[nodiscard]] bool commit_assigned_tool(AmsBackend& backend, int slot, int tool);

/// Set @p slot's backup to @p backup (-1 clears it). True when it was written.
[[nodiscard]] bool commit_backup(AmsBackend& backend, int slot, int backup);

/// Toast why @p backup cannot stand in for @p slot.
void notify_backup_refused(const AmsBackend& backend, int slot, int backup);

} // namespace helix::ui::lane_settings
