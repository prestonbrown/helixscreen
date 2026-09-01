// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <sys/stat.h>

/// Runtime device-layout classification.
///
/// This module is the one place on the C++ side that knows the AD5X mod
/// layout markers: ZMOD (the /ZMOD marker file or FlashForge's /usr/prog dir)
/// or Forge-X (the mod git tree, proven by .shell/platform.sh under
/// /opt/config/mod in-chroot or /usr/data/config/mod host-side). Consumers ask
/// the capability question — log-collector path gating, the mips self-update
/// platform split — and never name the markers themselves.
/// scripts/helix-launcher.sh answers the same question in shell for its
/// heap-diag gate; the C++ and bats suites pin both to one truth table.
namespace helix::platform {

namespace detail {

/// Root-relative spelling of an absolute probe path: "/" (or empty) probes the
/// real path, a sandbox root nests it ("/tmp/xyz" + "/ZMOD"). Trailing slashes
/// on the root are collapsed so the join never doubles up.
inline std::string rooted(const std::string& probe_root, const char* abs_path) {
    if (probe_root.empty() || probe_root == "/") {
        return std::string(abs_path);
    }
    std::string root = probe_root;
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return root + abs_path;
}

} // namespace detail

/// True when the environment carries an AD5X mod tree: ZMOD (the /ZMOD marker
/// file or FlashForge's /usr/prog dir) or Forge-X — whose chroot has neither,
/// but whose mod git tree stays reachable (/opt/config/mod in-chroot,
/// /usr/data/config/mod host-side, proven by .shell/platform.sh). Same rule as
/// the launcher's heap-diag gate (scripts/helix-launcher.sh); the two test
/// suites pin both to one truth table. All probes resolve under `probe_root`
/// (default "/") so tests can feed a fake layout without touching the real
/// root filesystem. stat-only; never opens anything.
inline bool ad5x_mod_layout_present(const std::string& probe_root = "/") {
    // ZMOD hosts carry FlashForge's /usr/prog dir or the /ZMOD marker file. A
    // Forge-X chroot has neither — and not even /usr/data, which the chroot
    // binds at /opt — but the mod's git tree stays reachable, and
    // .shell/platform.sh in it is the same evidence the installer's
    // host_profile probe keys on. scripts/helix-launcher.sh answers this same
    // rule for its heap-diag gate; keep the two (and their tests) in step.
    struct ::stat st {};
    if ((::stat(detail::rooted(probe_root, "/ZMOD").c_str(), &st) == 0 && S_ISREG(st.st_mode)) ||
        (::stat(detail::rooted(probe_root, "/usr/prog").c_str(), &st) == 0 &&
         S_ISDIR(st.st_mode))) {
        return true;
    }
    for (const char* mod_tree : {"/opt/config/mod", "/usr/data/config/mod"}) {
        const std::string probe = detail::rooted(probe_root, mod_tree) + "/.shell/platform.sh";
        if (::stat(probe.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return true;
        }
    }
    return false;
}

} // namespace helix::platform
