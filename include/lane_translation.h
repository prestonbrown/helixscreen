// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

namespace helix {
struct SlotInfo;
}

namespace helix::ams {

/// The user's statement in one edit: the fields that differ between what the
/// editor opened on and what it committed, minus the differences no person
/// caused.
///
/// A field the user did not move is not theirs to claim, and a plain diff of
/// two snapshots claims three kinds of field they did not move:
///   - a binding change carries the spool's own values, so a commit that
///     changes spoolman_id states the binding and nothing else, in both
///     directions;
///   - a cleared field lands on a sentinel (AMS_DEFAULT_SLOT_COLOR, a
///     negative weight) that means "no reading", never a chosen value;
///   - catalog_id and product_name arrive from the editor's auto-highlighted
///     product, which is why AmsEditOverlay::is_dirty() excludes them too.
[[nodiscard]] Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited);

} // namespace helix::ams
