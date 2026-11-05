// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "maintenance_meters.h"

#include <lvgl.h>
#include <string>

/// The live maintenance meters: the active printer's state, the print time that accrues
/// into it, its persistence through SettingsManager, and the subjects the home tile and
/// the detail overlay bind.
///
/// Print time comes from print_stats.print_duration, which counts printing and leaves out
/// pauses. The meters accrue whether or not the tile is on a page, so the hours are there
/// when it is added.
///
/// Subjects, all main thread:
/// - `maintenance_version` (int): bumps whenever anything below changes
/// - `maintenance_any_on` (int 0/1): some meter has an interval
/// - `maintenance_worst_pct`, `maintenance_worst_status`, `maintenance_worst_left`,
///   `maintenance_worst_name`: the meter closest to due (used percent, ValueStatus, hours
///   left, short name); "--" and "Off" when every meter is off
/// - per meter i (MeterId order): `maintenance_m<i>_on`, `_pct`, `_status`, `_interval`
///   (index into kIntervalChoices, the dropdown's selection), `_name`, `_short`, `_left`,
///   `_line` (used of interval), `_note` (share left, and the reset stamp when there is one)
/// - `maintenance_total_text`: every print hour counted for this printer
namespace helix::maintenance_model {

/// Registers the subjects, loads the active printer's meters (taking over the example
/// plugin's saved state the first time there is no record) and starts accruing. Called
/// from the maintenance widget's subject hook, once per session; a printer switch tears
/// it down through StaticSubjectRegistry, which saves first.
void init_subjects();

/// The state the subjects show.
const maintenance::State& state();

/// Bumps whenever state() changed.
lv_subject_t* version_subject();

/// Death signal for every subject above.
SubjectLifetime lifetime();

/// Asks before discarding a meter's hours, then resets it.
void confirm_reset(int meter);

/// Resets a meter now and saves.
void reset_meter(int meter);

/// Starts a new interval on this screen only, without asking the printer; reset_meter()
/// calls it once a firmware-kept counter has been cleared.
void reset_meter_locally(int meter);

/// Sets a meter's interval from a kIntervalChoices index and saves.
void set_interval_choice(int meter, int choice_index);

/// Whole hours as every meter text shows them ("37 h").
std::string hours_text(int hours);

/// What `maintenance_total_text` shows: every print hour counted for this printer.
std::string total_line();

/// The meter's full and short names, translated.
std::string meter_name(int meter);
std::string meter_short_name(int meter);

/// The interval dropdown's options, one line per kIntervalChoices entry in its order
/// ("Off", then "25 h" and up), translated.
std::string interval_options();

} // namespace helix::maintenance_model
