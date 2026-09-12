// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file helix_install_roots.h
 * @brief Every directory HelixScreen is known to install into
 *
 * Anything that has to find an install it did not start from - a log tail, a
 * crash file, a local installer - searches these. They are the C++ side of
 * `HELIX_INSTALL_DIRS` in `scripts/lib/installer/common.sh` and of the
 * `storage.root` values in `assets/config/platforms.json`.
 *
 * One list, because three consumers each keeping their own is how a shipped
 * platform goes missing from one of them: a root absent here is a device whose
 * logs and crash reports cannot be recovered from a debug bundle, and nothing
 * about that failure is visible from the device. `scripts/check_platform_manifest.py`
 * checks this list against the manifest.
 *
 * Priority-ordered, but callers that care about freshness rank by mtime instead;
 * a stale tree at a higher-priority path must not shadow the live one.
 */

namespace helix {

/// Fixed install roots, one per platform layout.
inline constexpr const char* kInstallRoots[] = {
    "/opt/helixscreen",                   // Pi, AD5M Forge-X and KMod v00.06+; K2 until it migrates
    "/mnt/UDISK/helixscreen",             // K2, on the 27.5GB user partition
    "/usr/data/helixscreen",              // K1, K1C
    "/userdata/helixscreen",              // Snapmaker U1
    "/user-resource/helixscreen",         // CC1 (COSMOS)
    "/root/printer_software/helixscreen", // AD5M KMod v00.05 and earlier
    "/srv/helixscreen",                   // AD5M ZMOD, AD5X
};

/// Where a platform keeps cache/ and logs/. Never the payload root nor inside
/// it: an update replaces the payload, and a Moonraker `type: web` entry
/// rmtree()s it first, so state kept there is destroyed on every update.
/// A log tail or a debug bundle has to look here, not only under the payload.
inline constexpr const char* kStateRoots[] = {
    "/mnt/UDISK/helixscreen-state", // K2, beside its payload on the user partition
    "/mnt/UDISK/helixscreen",       // K2 state, on installs that predate the move
    // AD5M keeps cache and logs here and installs somewhere else entirely
    // (/opt, /srv or /root/printer_software by firmware), so this belongs in
    // this list and not in kInstallRoots: nothing ever puts a payload here, and
    // a payload root that no installer produces is a root no uninstall sweeps.
    "/data/helixscreen",                // AD5M, on the durable ext4 mount
    "/usr/data/helixscreen-state",      // K1, K1C
    "/user-resource/helixscreen-state", // CC1
    "/userdata/helixscreen-state",      // Snapmaker U1
    "/srv/helixscreen-state",           // AD5X
};
// AD5X and AD5M ZMOD put their LOG under the mod's own tree so the mod's
// archiver collects it. That path is gated on an AD5X layout actually being
// present (helix::logs::default_file_paths), so it is deliberately not listed
// here: an unconditional entry is a dead stat probe on every other host.

/// Home directories a Pi-class install is commonly found under. These are
/// `$KLIPPER_HOME/helixscreen` rather than a platform's fixed root, so they are
/// a fallback for when the canonical resolver cannot name the install itself.
inline constexpr const char* kHomeInstallRoots[] = {
    "/home/pi/helixscreen",
    "/home/biqu/helixscreen",
    "/home/mks/helixscreen",
    "/home/qidi/helixscreen",
};

} // namespace helix
