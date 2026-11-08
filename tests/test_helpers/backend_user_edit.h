// tests/test_helpers/backend_user_edit.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A person's edit applied to a backend alone, with nothing filed on the lane.
#pragma once

#include "ams_backend.h"
#include "ams_types.h"

namespace helix::test {

/// Apply @p info to @p slot_index as a person's edit, through the backend alone.
///
/// Nothing reaches the lane: no Pending edit is filed and the slot is not
/// repainted, so a test that reads the lane back wants edit_slot_as_user()
/// (seeded_override.h) instead.
inline AmsError apply_edit(AmsBackend& backend, int slot_index, const SlotInfo& info) {
    return backend.apply_user_edit(slot_index, info);
}

} // namespace helix::test
