// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::timezone_env {

// Apply an IANA timezone (e.g. "America/New_York") to the process by setting
// the TZ environment variable and calling tzset().
//
// On first call, checks whether the host has a usable zoneinfo database at
// /usr/share/zoneinfo/. If not (observed on Elegoo Centauri Carbon running
// OpenCentauri COSMOS), points TZDIR at the bundled assets/zoneinfo/ under the
// current working directory. Application::setup_data_root() ensures CWD is the
// install root by the time this is called.
//
// An unresolved zone name reads as UTC, producing times offset by the user's
// real timezone. musl does not read TZDIR, so with the bundled fallback active TZ
// is set to the zone file's absolute path (prestonbrown/helixscreen#2003).
//
// posix_tz is the zone's POSIX rule (e.g. "EST5EDT,M3.2.0,M11.1.0"). The ESP32
// firmware applies it instead of the IANA name, since newlib has no zoneinfo
// reader; elsewhere it is ignored.
void apply(const char* iana_id, const char* posix_tz = nullptr);

// Return the process to the zone it started with: the TZ it inherited, or the
// host's /etc/localtime when it inherited none. The ESP32 has no host zone and
// applies UTC.
void apply_system();

// The UTC offsets, in minutes east, that a POSIX TZ rule gives on 1 January and
// 1 July. Understands the M-form transition rules every offered zone uses;
// returns false for anything else.
[[nodiscard]] bool posix_offsets(const char* posix_tz, int& jan_minutes_east,
                                 int& jul_minutes_east);

// Test hook: exposes the TZDIR configuration step with injectable probe paths.
// system_probe: filesystem path that, if missing, triggers the bundled fallback
//   (default: "/usr/share/zoneinfo/UTC").
// bundled_subdir: path relative to CWD holding bundled zoneinfo
//   (default: "assets/zoneinfo").
// Returns the TZDIR value that was set, or empty string if no change was made
// (system zoneinfo present, or bundled fallback also missing).
//
// Idempotent across calls within one process: the probe runs at most once per
// process lifetime so the test must be the first caller to observe the probe.
// For repeated testing, use reset_probe_state().
[[nodiscard]] const char* configure_tzdir(const char* system_probe = nullptr,
                                          const char* bundled_subdir = nullptr);

// Test hook: resets the "probe has run" flag so the next configure_tzdir()
// call re-runs the probe, and forgets the startup zone so the next apply()
// records it again. Do not call from production code.
void reset_probe_state();

} // namespace helix::timezone_env
