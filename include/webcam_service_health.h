// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Camera-service health, read out of Moonraker's machine.system_info.
//
// `server.webcams.list` answers "is a webcam CONFIGURED", never "is the thing
// that serves it running". A crowsnest install that has crashed still leaves
// its entry in the webcam database, and because a stock crowsnest registers a
// RELATIVE snapshot_url ("/webcam/?action=snapshot") the discovery reachability
// probe deliberately skips it - so the camera widget offers a stream nothing
// answers (prestonbrown/helixscreen#1351).
//
// machine.system_info carries `service_state`, systemd's view of every service
// Moonraker watches. This module is the ONLY place that knows which systemd
// units serve which Moonraker webcam `service` identifiers. Generic code (the
// discovery sequence) asks owning_service_is_down() and never names a camera
// vendor. Adding a streamer means adding one row to the table in
// webcam_service_health.cpp - no call site changes.
//
// It FAILS OPEN by design. An unknown service, an absent service_state, a unit
// we cannot match, or any non-terminal systemd state all read as "not down". A
// false negative hides a working camera, which is worse than the false positive
// this module exists to remove.

#include <string>

#include "hv/json.hpp"

namespace helix::webcam {

/**
 * @brief Pull `service_state` out of a machine.system_info response.
 *
 * Accepts the full JSON-RPC envelope (`result.system_info.service_state`), a
 * bare `system_info` wrapper, or the `system_info` object itself, so callers do
 * not have to know which layer they are holding.
 *
 * @return The service_state object, or an empty object when the response
 *         carries none (older Moonraker, non-systemd host, failed query).
 */
[[nodiscard]] nlohmann::json extract_service_state(const nlohmann::json& system_info_response);

/**
 * @brief The systemd unit in @p service_state that serves this webcam entry.
 *
 * Moonraker's webcam `service` field names the STREAM PROTOCOL
 * ("mjpegstreamer", "webrtc-go2rtc", ...), not the process serving it, so one
 * identifier maps to several plausible units. This returns whichever candidate
 * the printer actually runs, preferring a healthy one when more than one is
 * installed.
 *
 * @return The unit's key as it appears in service_state, or "" when no
 *         candidate is present (the fail-open case).
 */
[[nodiscard]] std::string owning_service_unit(const nlohmann::json& webcam_entry,
                                              const nlohmann::json& service_state);

/**
 * @brief Does systemd positively report this webcam's server as stopped/failed?
 *
 * True only when every candidate unit present in @p service_state is in a
 * terminal down state (failed / inactive / dead). Missing service_state,
 * unmatched service, unparseable entry, and transient states (activating,
 * reloading, "unknown") all return false - see the fail-open note above.
 *
 * @param out_reason Optional; when the result is true, filled with
 *                   "<unit> (<active_state>/<sub_state>)" for logging.
 */
[[nodiscard]] bool owning_service_is_down(const nlohmann::json& webcam_entry,
                                          const nlohmann::json& service_state,
                                          std::string* out_reason = nullptr);

} // namespace helix::webcam
