// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HelixScreen 1.0 kept per-tool spool links in tool_spools.json and the
// Moonraker key helix-screen/tool_spool_assignments, and the bypass spool in
// settings.json. A lane's identity now lives where Moonraker holds it, so these
// are read once, at the first connect after the upgrade, and imported. The
// reading here is pure; AmsState::run_legacy_spool_import() writes.

#include "ams_types.h"

#include <map>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix::ams::legacy {

/// The 1.0 per-tool spool file, under the user config dir.
inline constexpr const char* kToolSpoolsFile = "tool_spools.json";
/// The 1.0 Moonraker database copy of the same.
inline constexpr const char* kToolSpoolsDbNamespace = "helix-screen";
inline constexpr const char* kToolSpoolsDbKey = "tool_spool_assignments";
/// Where the v34 config migration parks a printer's 1.0 bypass spool until
/// the import has written it to the bypass lane record.
inline constexpr const char* kLegacyExternalSpoolPath = "filament/legacy_external_spool";

/// Tool index -> Spoolman spool id from one assignment set ({"0": {"spoolman_id": 7, ...}}),
/// the shape the Moonraker key holds. Entries naming no spool are left out.
[[nodiscard]] std::map<int, int> tool_spools_from_set(const nlohmann::json& set);

/// The set @p printer_id owns in a tool_spools.json document: its entry under
/// "printers", or the whole document when it has no "printers" (the
/// single-printer layout).
[[nodiscard]] std::map<int, int> tool_spools_from_file(const nlohmann::json& file,
                                                       const std::string& printer_id);

/// @p file without @p printer_id's set, or nullopt when nothing else remains and
/// the file can go.
[[nodiscard]] std::optional<nlohmann::json> file_without_printer(const nlohmann::json& file,
                                                                 const std::string& printer_id);

/// The bypass spool a 1.0 settings.json filament/external_spool block states,
/// or nullopt when it states none (no "assigned" and no colour).
[[nodiscard]] std::optional<SlotInfo> external_spool_from_settings(const nlohmann::json& block);

} // namespace helix::ams::legacy
