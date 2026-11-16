// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "i_moonraker_client.h"

#include "hv/json.hpp" // JSON_HPP_OK: make_status_notification builds a json inline

namespace helix {

/**
 * @brief Wrap a raw status object as the notify_status_update a client dispatches
 *
 * The inverse of parse_status_notification() (printer_state.h). A synthetic
 * dispatch is not a Klipper frame, so its eventtime is 0.0 ("untimestamped") and
 * provenance travels in the markers instead of being inferred from the clock.
 */
inline json make_status_notification(const json& status, bool from_cached_snapshot,
                                     bool whole_objects) {
    json notification = {{"method", "notify_status_update"},
                         {"params", json::array({status, 0.0})}}; // [status, eventtime]
    if (from_cached_snapshot) {
        notification[kCachedSnapshotMarker] = true;
    }
    if (whole_objects) {
        notification[kWholeObjectsMarker] = true;
    }
    return notification;
}

} // namespace helix
