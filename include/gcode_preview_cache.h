// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::gcode {

/// Name of the local cache file for @p file_key, e.g. "print_view_<hash>.gcode".
/// Keyed by the connected printer's address as well as @p file_key: a file of
/// the same name on another printer is a different file, and a copy of it can
/// match the new one's size.
std::string preview_cache_file_name(const char* prefix, const std::string& file_key);

} // namespace helix::gcode
