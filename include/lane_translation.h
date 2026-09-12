// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

namespace helix {
struct SlotInfo;
}

namespace helix::ams {

/// The user's statement in one edit: the fields that differ between what the
/// editor opened on and what it committed, and nothing else.
///
/// A field the user did not move is not theirs to claim. A binding change is
/// the one case where the diff lies about that: linking a spool hands the
/// editor that spool's colour, brand and material in the same commit, and
/// unlinking clears them. So a commit that changes spoolman_id states the
/// binding and nothing else; the fields that rode in with it belong to
/// whichever source supplied them.
[[nodiscard]] Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited);

} // namespace helix::ams
