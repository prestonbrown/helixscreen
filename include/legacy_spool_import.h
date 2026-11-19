// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HelixScreen 1.0 kept per-tool spool links in tool_spools.json and the
// Moonraker key helix-screen/tool_spool_assignments, and the bypass spool in
// settings.json. A lane's identity now lives where Moonraker holds it, so these
// are read once, at the first connect after the upgrade, and imported. The
// reading here is pure; AmsState::run_legacy_spool_import() writes.

#include "ams_types.h"
#include "json_fwd.h"

#include <map>
#include <optional>
#include <string>

namespace helix::ams::legacy {

/// The 1.0 per-tool spool file, under the user config dir.
inline constexpr const char* kToolSpoolsFile = "tool_spools.json";
/// The 1.0 Moonraker database copy of the same.
inline constexpr const char* kToolSpoolsDbNamespace = "helix-screen";
inline constexpr const char* kToolSpoolsDbKey = "tool_spool_assignments";
/// Where the v34 config migration parks a printer's 1.0 bypass spool until
/// the import has written it to the bypass lane record.
inline constexpr const char* kLegacyExternalSpoolPath = "filament/legacy_external_spool";
/// Set under a printer's config once all of its 1.0 tool links are imported. The
/// restore script can re-publish a stale tool_spools.json, and its set for that
/// printer is then ignored, so a lane the user has since unlinked stays unlinked.
inline constexpr const char* kLegacyToolImportDonePath = "filament/legacy_tool_import_done";

/// Tool index -> Spoolman spool id from one assignment set ({"0": {"spoolman_id": 7, ...}}),
/// the shape the Moonraker key holds. Entries naming no spool are left out.
[[nodiscard]] std::map<int, int> tool_spools_from_set(const nlohmann::json& set);

/// The set @p printer_id owns in a tool_spools.json document: its entry under
/// "printers", or the whole document when it has no "printers" (the
/// single-printer layout).
[[nodiscard]] std::map<int, int> tool_spools_from_file(const nlohmann::json& file,
                                                       const std::string& printer_id);

/// @p set with only the entries of @p owed's tools left: the links not yet
/// imported. Pure.
[[nodiscard]] nlohmann::json set_keeping_tools(const nlohmann::json& set,
                                               const std::map<int, int>& owed);

/// @p file with @p printer_id's set cut to @p owed's tools, or nullopt when
/// nothing that names a spool remains and the file can go. A printer whose set
/// is left naming no spool leaves the file.
[[nodiscard]] std::optional<nlohmann::json> file_keeping_tools(const nlohmann::json& file,
                                                               const std::string& printer_id,
                                                               const std::map<int, int>& owed);

/// The bypass spool a 1.0 settings.json filament/external_spool block states,
/// or nullopt when it states none (no "assigned" and no colour).
[[nodiscard]] std::optional<SlotInfo> external_spool_from_settings(const nlohmann::json& block);

} // namespace helix::ams::legacy
