// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the printer-side converter says it made from each sliced .3mf, and the
// one decision drawn from it: which archives the file list can leave out
// because the G-code converted from them sits beside them, unchanged.

#include "json_fwd.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace helix::three_mf {

/// One entry of the converter's provenance (server.helix.3mf_conversions).
/// Paths are relative to the gcodes root.
struct Conversion {
    std::string source; ///< the .3mf
    double source_mtime = 0;
    uint64_t source_size = 0;
    int plate = 0;
    std::string plate_md5;
    std::string output; ///< the extracted .gcode
    std::string output_md5;
    std::string state; ///< "done", "pending" or "failed"
    std::string error;
};

/// Reads the endpoint's result object ({"conversions": [...]}). Malformed
/// entries are skipped; a malformed result yields none.
std::vector<Conversion> parse_conversions(const nlohmann::json& result);

/// A file as the listing reports it.
struct ListedFile {
    std::string path; ///< relative to the gcodes root
    uint64_t size = 0;
    double modified = 0;
};

/**
 * @brief The archives in @p listed the list can hide.
 *
 * An archive is hidden only when the conversion of every sliced plate in it
 * (one entry each) is done, was made from exactly the archive listed now (same
 * size, same modification time), and has its output listed too. Anything less (pending, failed, the
 * archive re-uploaded since, the output deleted or elsewhere) leaves the archive showing, because
 * then it is the only way to print that job.
 */
std::set<std::string> converted_archives(const std::vector<Conversion>& conversions,
                                         const std::vector<ListedFile>& listed);

/// The plates of the archive @p source whose conversion is finished, current
/// and listed, each with its output: those plates print from the output.
std::map<int, std::string> current_outputs(const std::vector<Conversion>& conversions,
                                           const std::vector<ListedFile>& listed,
                                           const std::string& source);

} // namespace helix::three_mf
