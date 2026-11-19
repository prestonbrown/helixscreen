// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace helix::printer_db {

/// The bundled database's ids, as a printer_database.d entry is matched
/// against them.
struct BundledIds {
    /// id -> position in the "printers" array. Additions join it, so a later
    /// file overrides an earlier file's addition the same way.
    std::map<std::string, std::size_t> index;
    /// A bundled entry's "id_aliases" -> that entry's id. Another publisher's
    /// id for a printer the bundled entry already describes; an alias that is
    /// itself a bundled id is left out, since real ids win.
    std::map<std::string, std::string> superseding;
};

/// What one merge did, for the load status and the log.
struct MergeCounts {
    int overrides = 0;
    int additions = 0;
};

/// Index the bundled "printers" array.
[[nodiscard]] BundledIds index_bundled(const nlohmann::json& printers);

/// Merge one extension file's "printers" array into @p printers:
///  - an entry under a bundled id replaces it, or with "enabled": false
///    disables it;
///  - an entry under one of a bundled entry's id_aliases is skipped, so the
///    bundled entry is the only one of its name;
///  - any other enabled entry with a name is added.
/// Entries with no id, or additions with no name, are refused into @p errors.
void merge_extension_printers(nlohmann::json& printers, const nlohmann::json& extension_printers,
                              BundledIds& ids, const std::string& file_path, MergeCounts& counts,
                              std::vector<std::string>& errors);

} // namespace helix::printer_db
