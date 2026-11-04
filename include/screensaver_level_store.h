// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board_fingerprint.h"
#include "json_fwd.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace helix {
class Config;
}

/**
 * @file screensaver_level_store.h
 * @brief The quality level each screensaver settled on, per board and app version
 *
 * Stored in config at /display/screensaver_levels/<saver name>. An entry written by another
 * app version or on another board is ignored, so the saver is measured again from level 0.
 */

namespace helix::ui {

/// A saver's remembered level on one board and app version.
struct SaverLevelEntry {
    size_t level = 0;
    bool too_heavy = false;
    std::string version;
    std::string board;

    bool operator==(const SaverLevelEntry& o) const {
        return level == o.level && too_heavy == o.too_heavy && version == o.version &&
               board == o.board;
    }
};

/// "/display/screensaver_levels/<saver_name>"
std::string level_store_path(const char* saver_name);

/// The entry at `node`, or nullopt unless it is an object with an unsigned integer "level", a
/// boolean "too_heavy" and string "version" and "board".
std::optional<SaverLevelEntry> parse_level_entry(const nlohmann::json* node);

nlohmann::json level_entry_json(const SaverLevelEntry& entry);

/// Where a run starts: the stored level (clamped below `level_count`) and too-heavy mark when
/// `stored` was written by `version` on `board`, otherwise level 0 and not too heavy.
SaverLevelEntry start_entry(const std::optional<SaverLevelEntry>& stored,
                            const std::string& version, const std::string& board,
                            size_t level_count);

std::optional<SaverLevelEntry> load_level_entry(const helix::Config& config,
                                                const char* saver_name);

/// Writes the entry and saves the config file.
void save_level_entry(helix::Config& config, const char* saver_name, const SaverLevelEntry& entry);

} // namespace helix::ui
