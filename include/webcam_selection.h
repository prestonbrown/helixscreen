// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which of a printer's webcams a camera view shows.
//
// Moonraker's `server.webcams.list` can carry several cameras. PrinterState
// keeps that whole list (see PrinterCapabilitiesState::set_webcams) and every
// camera view resolves its feed through the pure functions here: the auto-pick
// is the default, and a camera widget configured with a `source` (a webcam
// NAME, which survives a DHCP move where an absolute URL would not) gets that
// camera when it is present and usable, the auto-pick otherwise.
//
// Everything here is a pure function over WebcamInfo values: no printer, no
// network, no display, so the selection rule is testable on its own.

#include "json_fwd.h"
#include "moonraker_types.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Decide whether a webcam snapshot_url is a usable image endpoint.
 *
 * Moonraker webcam entries advertise a snapshot_url, but some "services"
 * (e.g. the Creality K2's "iframe" viewer) point it at an HTML page rather
 * than a JPEG. Polling such a URL would never yield a decodable frame, so
 * discovery rejects it up front instead of letting CameraStream spin on it.
 *
 * Rules:
 *   - empty            -> false
 *   - path ends .html  -> false (case-insensitive)
 *   - contains action=snapshot, or path ends .jpg/.jpeg/.png -> true
 *   - anything else    -> true (conservative: bare hosts, query-only URLs)
 */
[[nodiscard]] bool is_usable_snapshot_url(const std::string& snapshot_url);

/// The name the built-in screen server registers its webcam under (#1825).
constexpr const char* kRemoteScreenWebcamName = "HelixScreen Screen";
/// The `extra_data` key a screen server's entry carries.
constexpr const char* kRemoteScreenWebcamMarker = "helixscreen_screen";

namespace webcam {

/// A screen registered by a built-in screen server: ours, or another screen's on the
/// same Moonraker. Never a camera feed: a screen is not a view of the printer.
[[nodiscard]] bool is_own_screen_webcam(const WebcamInfo& cam);

/// Read one `server.webcams.list` entry. Unknown or missing fields keep the
/// WebcamInfo defaults; `unavailable_reason` is always empty here (discovery
/// fills it after its health checks).
[[nodiscard]] WebcamInfo parse_webcam_entry(const nlohmann::json& entry);

/// MJPEG-family service identifiers ("mjpegstreamer", "mjpegstreamer-adaptive",
/// "ustreamer", ...). These are the only families CameraStream decodes as a live
/// stream; every other service (webrtc-*, ipcamera, hlsstream, ...) returns
/// HTML/SDP/HLS from its stream_url and is only usable by snapshot polling.
[[nodiscard]] bool is_mjpeg_service(const std::string& service);

/// True when a camera view could show this entry: enabled, not flagged
/// unavailable, and offering either an MJPEG stream or a usable snapshot.
[[nodiscard]] bool is_usable(const WebcamInfo& cam);

/// The URLs CameraStream should be handed for this entry. A non-MJPEG service
/// comes back with `stream_url` cleared so the stream goes straight to
/// snapshot polling rather than burning its failover attempts on a WebRTC or
/// HLS endpoint.
[[nodiscard]] WebcamInfo feed_for(const WebcamInfo& cam);

/// The default camera: the first usable MJPEG entry, else the first usable
/// entry with a snapshot. Order is Moonraker's list order.
/// @return index into @p cams, or nullopt when nothing is usable.
[[nodiscard]] std::optional<size_t> auto_pick_index(const std::vector<WebcamInfo>& cams);

/// feed_for() of the auto-pick, or nullopt when nothing is usable.
[[nodiscard]] std::optional<WebcamInfo> auto_pick(const std::vector<WebcamInfo>& cams);

/// The feed for a configured source. @p source names a webcam (empty = no
/// preference). The named camera wins when it is in the list and usable;
/// otherwise — absent, disabled (never listed), service down, unreachable —
/// the auto-pick does, so a stale preference degrades to the default rather
/// than to "No Camera".
[[nodiscard]] std::optional<WebcamInfo> select_webcam(const std::vector<WebcamInfo>& cams,
                                                      const std::string& source);

/// The camera a print's photos are taken with: of the enabled cameras with a
/// usable snapshot URL (screens excluded), the first MJPEG one, else the
/// first. The HelixPrint plugin applies the same rule
/// (moonraker-plugin/helix_photos.py#pick_snapshot_url), so the plugin and
/// the screen photograph the same camera.
[[nodiscard]] std::optional<WebcamInfo> photo_pick(const std::vector<WebcamInfo>& cams);

/// The per-view config (rotation, flip_h, flip_v) that applies to a still of
/// the camera named @p cam_name: the camera widget's own @p widget_config when
/// the widget shows that camera (@p widget_feed), else none, which leaves
/// Moonraker's flips alone.
[[nodiscard]] nlohmann::json view_config_for(const std::string& cam_name,
                                             const std::optional<WebcamInfo>& widget_feed,
                                             const nlohmann::json& widget_config);

/// True when @p source names a camera that select_webcam() would honor. False
/// for an empty source and for a fallback to the auto-pick.
[[nodiscard]] bool source_is_honored(const std::vector<WebcamInfo>& cams,
                                     const std::string& source);

} // namespace webcam
} // namespace helix
