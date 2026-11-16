// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <string>
#include <utility>
#include <vector>

/// Shared between config.cpp and config_migrations.cpp only. Everything else
/// reaches config through Config.
namespace helix::config_detail {

using json = nlohmann::json;

/// The node @p root[json_pointer(ptr)] would assign to, creating missing
/// objects on the way, or nullptr (with @p why set) where nlohmann would throw.
json* node_for_write(json& root, const std::string& ptr, const char** why);

/// Move each {from, to} JSON pointer pair; a null at either end counts as absent.
/// @return true if any key moved or was dropped
bool migrate_config_keys(json& data,
                         const std::vector<std::pair<std::string, std::string>>& migrations);

/// The /printers key /active_printer_id resolves to, falling back to
/// @p preferred and then the first printer object; "" when there is none.
std::string find_active_printer_key(const json& config, const std::string& preferred = "");

/// The scroll_throw a pre-v27 document keeps: the old shipped default 25 was
/// written into every config but never chosen by a user, so it becomes
/// @p platform_default; any other value is the user's and stays.
int migrated_scroll_throw(int stored, int platform_default);

/// Upgrade @p config from its config_version to kCurrentConfigVersion and
/// stamp it. @p config_path locates sidecar files a migration folds in.
void run_versioned_migrations(json& config, const std::string& config_path);

} // namespace helix::config_detail
