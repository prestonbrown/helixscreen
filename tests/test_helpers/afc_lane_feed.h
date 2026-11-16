// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "afc_status_parse.h"
#include "ams_backend_afc.h"
#include "test_helpers/afc_test_access.h"

#include <mutex>
#include <string>
#include <vector>

namespace helix::test {

/// An AFC backend with @p lanes already registered (none: the empty registry).
inline void init_afc_lanes(AmsBackendAfc& afc, const std::vector<std::string>& lanes) {
    if (!lanes.empty()) {
        AfcTestAccess::initialize_slots(afc, lanes);
    }
}

/// Apply a lane_data payload the way a database reply does.
inline void feed_lane_data(AmsBackendAfc& afc, const nlohmann::json& lane_data) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(afc));
    AfcTestAccess::apply_lane_data(afc, afc::parse_lane_data(lane_data));
}

/// Deliver one AFC_stepper object through the live status path.
inline void feed_lane_status(AmsBackendAfc& afc, const std::string& lane,
                             const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane] = data;
    AfcTestAccess::handle_status_update(
        afc, nlohmann::json{{"params", nlohmann::json::array({params, 0.0})}});
}

} // namespace helix::test
