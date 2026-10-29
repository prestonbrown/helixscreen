// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Where a pressure-advance result goes once a screen has one: the Spoolman
// spool it belongs to, and the Moonraker record other clients read
// (docs/devel/PRESSURE_ADVANCE_TUNING.md). Shared by every screen that ends
// with a K, tuned or measured, so they find the same spool and write the same
// record. Callbacks run on the API's thread; a screen wraps them in its own
// lifetime guard before touching UI.

#include "moonraker_error.h"
#include "pa_tuning.h"
#include "spoolman_types.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::pa {

/// What looking for a result's spool found.
enum class SpoolLookup {
    /// A spool whose filament can carry the value.
    Found,
    /// Spoolman answered, with no spool or one without a filament.
    NoSpool,
    /// Spoolman is not configured or did not answer.
    Unavailable,
};

/// The lookup outcome for a spool reply. A spool without a filament cannot
/// hold the value, which the user experiences the same as having no spool.
SpoolLookup classify_spool(const std::optional<SpoolInfo>& spool);

/// The spool a result belongs to: `tool_spool_id` when the printer assigns
/// spools per tool (> 0), Spoolman's active spool otherwise. `done` gets the
/// spool whenever Spoolman returned one, even one classify_spool() rejects,
/// so the record can still name it.
void find_result_spool(IMoonrakerAPI& api, int tool_spool_id,
                       std::function<void(SpoolLookup, const std::optional<SpoolInfo>&)> done);

/// `saved` with the spool's ids and name filled in.
SavedK with_spool(SavedK saved, const std::optional<SpoolInfo>& spool);

/// Record `saved` in Moonraker's database, keeping every other filament's and
/// extruder's entry. Failures are logged; nothing waits on this.
void publish_result(IMoonrakerAPI& api, const SavedK& saved);

/// Store `k` on Spoolman filament `filament_id`, where spool activation reads
/// it back. `field_label` names the extra field when Spoolman has to create it.
void save_to_filament(IMoonrakerAPI& api, int filament_id, float k, const std::string& field_label,
                      std::function<void()> on_saved,
                      std::function<void(const MoonrakerError&)> on_error);

/// The current time as ISO 8601 UTC, the record's `updated` format.
std::string utc_now_iso();

} // namespace helix::pa
