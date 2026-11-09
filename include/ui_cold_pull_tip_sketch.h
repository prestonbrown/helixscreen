// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::ui {

/// Register `<cold_pull_tip_sketch>`: a drawn cold pull tip, a round strand stepping
/// up into the thicker melt zone and tapering to a nib with a tiny wisp past it.
/// `dirty="true"` adds carbon specks and the old colour at the tip. Size it from XML;
/// the drawing scales to fit.
void register_cold_pull_tip_sketch_widget();

} // namespace helix::ui
