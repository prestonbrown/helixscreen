// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

/**
 * @brief Which channel installed this copy, and which firmware mod hosts it.
 *
 * Both answers go only into the opt-in telemetry and are always one word from
 * a fixed list, so a corrupted or hand-edited marker can never send free text.
 */
namespace helix::install_provenance {

/// Written by install.sh into the config dir, one allowlisted word.
constexpr const char* kMarkerFile = ".install_source";

/// The marker's channel, or "unknown" for a missing, empty or unlisted marker.
const char* read_install_source(const std::string& config_dir);

/// The firmware mod whose files sit under @p root, or "none".
const char* detect_host_mod(const std::string& root = "");

} // namespace helix::install_provenance
