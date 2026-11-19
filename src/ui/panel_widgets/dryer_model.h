// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_guard.h"
#include "dryer_summary.h"

#include <lvgl.h>

namespace helix::dryer_model {

/// Registers `home_dryer_available` and `home_dryer_version` and starts watching the
/// three drying sources: the box dryers (per-unit environment subjects and
/// `ams_units_dryer_version`), the chamber heater's cycle and drying on the heated bed.
/// Called from the dryer widget's subject hook, so the gate exists before the panel
/// widget manager resolves gates; calling it again re-attaches to sources that came up
/// since.
void init_subjects();

/// Re-reads every source and republishes when the summary changed.
void refresh();

/// The last summary refresh() computed.
const DryerSummary& summary();

/// Bumps whenever summary() changed; the widget observes it.
lv_subject_t* version_subject();

/// Death signal for version_subject() and the gate subject.
SubjectLifetime lifetime();

/// Publishes @p summary as though refresh() had computed it, so a widget test can set the
/// drying state without building every source. Refreshes leave it alone until
/// release_for_test().
void publish_for_test(DryerSummary summary);

/// Ends publish_for_test(): the next refresh reads the live sources again.
void release_for_test();

} // namespace helix::dryer_model
