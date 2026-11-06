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
void apply(const char* iana_id);

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
// call re-runs the probe. Do not call from production code.
void reset_probe_state();

} // namespace helix::timezone_env
